// This test uses Microsoft's software WARP device and a native ABI mock. It
// never loads NVIDIA model files or submits a GPU command list.
#include "xivrender/native_nr_api.hpp"
#include "xivrender/ngx_parameters.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

// Keep in sync with the test-only mock_nr.cpp ABI.
struct MockNrMetrics {
    unsigned inits = 0, populates = 0, creates = 0, evaluations = 0;
    unsigned releases = 0, shutdowns = 0, callbacks = 0, live_handles = 0;
    unsigned api_version = 0, feature = 0, typed_mask = 0, width = 0, height = 0;
    unsigned init_parameters_null = 0, progress_null = 0;
    std::uint64_t application_id = 0;
    std::uintptr_t device = 0, commands = 0, parameters = 0, handle = 0;
};

namespace {
using namespace xivrender::nr;
unsigned checks = 0, failures = 0;
constexpr unsigned native_failure = 0xbad10002;
constexpr std::uint64_t private_application_id = 0x123456789abcdef0ull;

bool check(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
    return condition;
}
bool result(unsigned actual, unsigned expected, const char *label) {
    if (actual == expected)
        return check(true, label);
    std::cerr << "Result 0x" << std::hex << actual << ", expected 0x" << expected << std::dec << ": ";
    return check(false, label);
}
void must(bool condition, const char *label) {
    if (!check(condition, label))
        throw std::runtime_error(label);
}
void hr(HRESULT value, const char *label) {
    if (FAILED(value))
        throw std::runtime_error(std::string(label) + ": " + std::to_string(value));
}

struct Library {
    HMODULE handle = nullptr;
    explicit Library(const std::filesystem::path &path) {
        if (!path.is_absolute())
            throw std::runtime_error("Test library paths must be absolute");
        handle = LoadLibraryExW(path.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!handle)
            throw std::runtime_error("Test DLL load failed: " + std::to_string(GetLastError()));
    }
    ~Library() {
        if (handle)
            FreeLibrary(handle);
    }
    Library(const Library &) = delete;
    Library &operator=(const Library &) = delete;
    template <class T> static T function(HMODULE module, const char *name) {
        auto value = reinterpret_cast<T>(GetProcAddress(module, name));
        if (!value)
            throw std::runtime_error(std::string("Missing test export: ") + name);
        return value;
    }
    template <class T> T function(const char *name) const {
        return function<T>(handle, name);
    }
};
struct Bridge {
    decltype(&XIVNR_Open) open;
    decltype(&XIVNR_Create) create;
    decltype(&XIVNR_Evaluate) evaluate;
    decltype(&XIVNR_Release) release;
    decltype(&XIVNR_Close) close;
    explicit Bridge(const Library &library)
        : open(library.function<decltype(open)>("XIVNR_Open")),
          create(library.function<decltype(create)>("XIVNR_Create")),
          evaluate(library.function<decltype(evaluate)>("XIVNR_Evaluate")),
          release(library.function<decltype(release)>("XIVNR_Release")),
          close(library.function<decltype(close)>("XIVNR_Close")) {}
};
struct Mock {
    using Reset = void(__cdecl *)();
    using Snapshot = void(__cdecl *)(MockNrMetrics *);
    using Results = void(__cdecl *)(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
    Reset reset;
    Snapshot snapshot;
    Results results;
    MockNrMetrics &observed;
    Mock(HMODULE module, MockNrMetrics &sink)
        : reset(Library::function<Reset>(module, "MockNrReset")),
          snapshot(Library::function<Snapshot>(module, "MockNrSnapshot")),
          results(Library::function<Results>(module, "MockNrResults")), observed(sink) {
        snapshot(&observed);
    }
    // Native operations mirror metrics to this caller-owned sink. It remains
    // readable after Close unloads the mock DLL; no extra library reference or
    // dangling call through an unloaded export is needed.
    MockNrMetrics metrics() const {
        return observed;
    }
    void success_results() const {
        results(success, success, success, success, success, success, 0);
    }
};
struct Commands {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    explicit Commands(ID3D12Device *device) {
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
           "Create WARP allocator");
        hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                     IID_PPV_ARGS(&list)),
           "Create WARP commands");
    }
    ~Commands() {
        if (list)
            list->Close();
    }
};
struct Files {
    std::filesystem::path root;
    Files() {
        root = std::filesystem::temp_directory_path() /
               (L"xivrender-nr-mock-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()));
        std::filesystem::create_directory(root);
    }
    ~Files() {
        // This directory was created uniquely by this fixture. Remove only the
        // two named files; never recursively remove a computed path.
        std::error_code ignored;
        std::filesystem::remove(root / L"empty.dll", ignored);
        std::filesystem::remove(root / L"nr-alias.dll", ignored);
        std::filesystem::remove(root, ignored);
    }
};
struct Environment {
    const wchar_t *name;
    std::wstring previous;
    bool existed;
    Environment(const wchar_t *key, const wchar_t *value) : name(key) {
        const auto length = GetEnvironmentVariableW(key, nullptr, 0);
        existed = length != 0;
        if (existed) {
            previous.resize(length);
            const auto copied = GetEnvironmentVariableW(key, previous.data(), length);
            previous.resize(copied);
        }
        if (!SetEnvironmentVariableW(name, value))
            throw std::runtime_error("Set mock test environment failed");
    }
    ~Environment() {
        SetEnvironmentVariableW(name, existed ? previous.c_str() : nullptr);
    }
};

void seed(xivrender::NgxParameters &parameters, std::uint64_t &token) {
    parameters.Set("Mock.ULL", 0xfedcba9876543210ull);
    parameters.Set("Mock.Float", .375f);
    parameters.Set("Mock.Double", .123456789012345);
    parameters.Set("Mock.Unsigned", 123456u);
    parameters.Set("Mock.Signed", -73);
    parameters.Set("Mock.Resource11", reinterpret_cast<ID3D11Resource *>(&token));
    parameters.Set("Mock.Resource12", reinterpret_cast<ID3D12Resource *>(&token));
    parameters.Set("Mock.Pointer", static_cast<void *>(&token));
    parameters.Set("Width", 576u);
    parameters.Set("Height", 960u);
}
template <class T> void echo(const NVSDK_NGX_Parameter &parameters, const char *key, T expected) {
    T actual{};
    check(parameters.Get(key, &actual) == NVSDK_NGX_Result_Success && actual == expected, key);
}

ComPtr<ID3D12Device> independent_warp(IUnknown *adapter, ID3D12Device *original) {
    // D3D12CreateDevice is a per-adapter singleton. A second call to that API
    // would silently reuse the same WARP device and invalidate this regression
    // test. An independent device factory avoids process-global SDK changes.
    // https://microsoft.github.io/DirectX-Specs/d3d/IndependentDevices.html
    ComPtr<ID3D12DeviceFactory> factory;
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12GetInterface(CLSID_D3D12DeviceFactory, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->InitializeFromGlobalState()) ||
        FAILED(factory->SetFlags(D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON)) ||
        FAILED(factory->CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        return {};
    ComPtr<IUnknown> first, second;
    if (FAILED(original->QueryInterface(IID_PPV_ARGS(&first))) ||
        FAILED(device->QueryInterface(IID_PPV_ARGS(&second))) || first == second)
        return {};
    return device;
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 4) {
        std::cerr << "Usage: native_nr_bridge_tests <absolute bridge DLL> <absolute mock NR DLL> <absolute "
                     "mock DLSS DLL>\n";
        return 2;
    }
    try {
        const std::filesystem::path bridge_path(argv[1]), model_path(argv[2]), incomplete_path(argv[3]);
        must(bridge_path.is_absolute() && model_path.is_absolute() && incomplete_path.is_absolute(),
             "Only explicit absolute fixture paths are used");
        Library bridge_library(bridge_path);
        Bridge api(bridge_library);
        Files files;
        ComPtr<IDXGIFactory4> dxgi;
        ComPtr<IDXGIAdapter> warp;
        ComPtr<ID3D12Device> device;
        hr(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi)), "Create DXGI factory");
        hr(dxgi->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "Select WARP");
        hr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
           "Create WARP device");
        Commands commands(device.Get());
        auto other_device = independent_warp(warp.Get(), device.Get());
        std::uint64_t token = 0x0123456789abcdefull;
        xivrender::NgxParameters parameters;
        seed(parameters, token);
        MockNrMetrics observed;
        parameters.Set("Mock.Observer", static_cast<void *>(&observed));
        Session *session = nullptr;
        auto open = [&](const std::filesystem::path &path, Session **output) {
            return api.open(path.c_str(), files.root.c_str(), device.Get(), private_application_id,
                            &parameters, output);
        };

        result(open(L"relative-model.dll", &session), invalid_model, "Relative model path rejected");
        check(session == nullptr, "Invalid path clears the session output");
        result(open(files.root / L"missing.dll", &session), invalid_model, "Missing model path rejected");
        result(open(files.root, &session), invalid_model, "Directory rejected as model");
        {
            std::ofstream empty(files.root / L"empty.dll", std::ios::binary);
        }
        result(open(files.root / L"empty.dll", &session), invalid_model,
               "Zero-byte PE rejected before loading");
        result(open(incomplete_path, &session), missing_export,
               "Missing NR exports rejected as a complete set");
        result(api.open(nullptr, files.root.c_str(), device.Get(), 0, &parameters, &session),
               invalid_argument, "Null model rejected");
        result(api.open(model_path.c_str(), files.root.c_str(), device.Get(), 0, &parameters, nullptr),
               invalid_argument, "Null session output rejected");
        check(observed.inits == 0 && GetModuleHandleW(model_path.c_str()) == nullptr,
              "Invalid input never loads or enters the native NR runtime");

        must(result(open(model_path, &session), success, "Explicit mock model initializes"),
             "Open succeeded");
        must(session != nullptr, "Open returns an owned session");
        Mock mock(GetModuleHandleW(model_path.c_str()), observed);
        auto metrics = mock.metrics();
        check(metrics.inits == 1 && metrics.populates == 1, "Init and Populate each run once");
        check(metrics.api_version == 0x15 && metrics.init_parameters_null == 1,
              "Init ABI has SDK version 0x15 and null optional initialization block");
        check(metrics.application_id == private_application_id &&
                  metrics.device == reinterpret_cast<std::uintptr_t>(device.Get()),
              "Init forwards private application and device");
        check(metrics.typed_mask == 255 &&
                  metrics.parameters == reinterpret_cast<std::uintptr_t>(&parameters),
              "Loaded model reads every SDK virtual Get overload");
        echo(parameters, "Mock.EchoULL", 0xfedcba9876543210ull);
        echo(parameters, "Mock.EchoFloat", .375f);
        echo(parameters, "Mock.EchoDouble", .123456789012345);
        echo(parameters, "Mock.EchoUnsigned", 123456u);
        echo(parameters, "Mock.EchoSigned", -73);
        echo(parameters, "Mock.EchoResource11", reinterpret_cast<ID3D11Resource *>(&token));
        echo(parameters, "Mock.EchoResource12", reinterpret_cast<ID3D12Resource *>(&token));
        echo(parameters, "Mock.EchoPointer", static_cast<void *>(&token));
        unsigned long long callback_bits = 0;
        must(parameters.Get("DLSSNRComputeScalingRatioCallback", &callback_bits) ==
                     NVSDK_NGX_Result_Success &&
                 callback_bits,
             "Populate publishes a callback through typed parameters");
        using Callback = unsigned(__cdecl *)(NVSDK_NGX_Parameter *);
        const auto callback = reinterpret_cast<Callback>(static_cast<std::uintptr_t>(callback_bits));
        parameters.Set("PerfQualityValue", 2u);
        result(callback(&parameters), success, "Published NR ratio callback runs");
        float ratio = 0;
        check(parameters.Get("DLSSNR.ScalingRatio", &ratio) == NVSDK_NGX_Result_Success &&
                  std::fabs(ratio - 2.0f / 3.0f) < 1e-6f && mock.metrics().callbacks == 1,
              "Callback writes a float back through the public SDK interface");

        Session *duplicate = nullptr;
        result(open(model_path, &duplicate), busy, "Duplicate model session rejected");
        check(duplicate == nullptr && mock.metrics().inits == 1,
              "Duplicate Open does not reinitialize global native state");
        auto uppercase = model_path.native();
        for (wchar_t &letter : uppercase)
            letter = static_cast<wchar_t>(std::towupper(letter));
        if (std::filesystem::is_regular_file(uppercase))
            result(open(uppercase, &duplicate), busy, "Case-only model path alias rejected");
        else
            std::puts("SKIP: filesystem does not resolve case-only filename aliases");
        std::error_code link_error;
        std::filesystem::create_hard_link(model_path, files.root / L"nr-alias.dll", link_error);
        if (!link_error)
            result(open(files.root / L"nr-alias.dll", &duplicate), busy,
                   "Same-file hard-link alias rejected");
        else
            std::puts("SKIP: fixture volume does not support a same-file hard-link alias");

        unsigned foreign_storage = 0;
        void *foreign_handle = &foreign_storage;
        result(api.evaluate(session, commands.list.Get(), foreign_handle, &parameters), foreign_feature,
               "Foreign feature cannot be evaluated");
        result(api.release(session, foreign_handle), foreign_feature, "Foreign feature cannot be released");
        check(mock.metrics().evaluations == 0 && mock.metrics().releases == 0,
              "Foreign handles never cross the model ABI boundary");

        void *feature = nullptr;
        result(api.create(session, nullptr, &parameters, &feature), invalid_argument,
               "Null command list rejected");
        check(feature == nullptr && mock.metrics().creates == 0,
              "Invalid Create clears output without calling model");
        must(result(api.create(session, commands.list.Get(), &parameters, &feature), success,
                    "Create feature 18"),
             "Create succeeded");
        must(feature != nullptr, "Create returns an owned feature");
        metrics = mock.metrics();
        check(metrics.feature == 18 && metrics.width == 576 && metrics.height == 960 &&
                  metrics.live_handles == 1,
              "Create ABI forwards native NR type and typed dimensions");
        check(metrics.commands == reinterpret_cast<std::uintptr_t>(commands.list.Get()) &&
                  metrics.parameters == reinterpret_cast<std::uintptr_t>(&parameters),
              "Create forwards command list and parameter identity");
        result(api.close(session), busy, "Close refuses a live feature");
        check(mock.metrics().shutdowns == 0, "Busy Close leaves model and device initialized");
        result(api.evaluate(session, commands.list.Get(), feature, &parameters), success,
               "Evaluate owned feature");
        metrics = mock.metrics();
        check(metrics.evaluations == 1 && metrics.progress_null == 1 &&
                  metrics.handle == reinterpret_cast<std::uintptr_t>(feature),
              "Evaluate ABI forwards exact feature and null progress callback");

        if (other_device) {
            Commands other_commands(other_device.Get());
            const auto before = mock.metrics();
            void *wrong_feature = nullptr;
            const auto created = api.create(session, other_commands.list.Get(), &parameters, &wrong_feature);
            const auto after_create = mock.metrics();
            // Clean up a handle if an unguarded bridge accepted it, then report
            // the regression. The fixture itself performs no GPU commands.
            if (wrong_feature)
                api.release(session, wrong_feature);
            result(created, invalid_argument, "Create rejects another device's command list");
            check(after_create.creates == before.creates && wrong_feature == nullptr,
                  "Wrong-device Create never enters native model or returns a handle");
            const auto evaluated = api.evaluate(session, other_commands.list.Get(), feature, &parameters);
            result(evaluated, invalid_argument, "Evaluate rejects another device's command list");
            check(mock.metrics().evaluations == before.evaluations,
                  "Wrong-device Evaluate never enters native model");
        } else
            std::puts("SKIP: independent WARP devices unavailable; wrong-device regression not exercised");

        mock.results(success, success, success, native_failure, success, success, 0);
        result(api.evaluate(session, commands.list.Get(), feature, &parameters), native_failure,
               "Native Evaluate error is preserved");
        check(mock.metrics().live_handles == 1, "Evaluate failure retains feature ownership");
        mock.results(success, success, success, success, native_failure, success, 0);
        result(api.release(session, feature), native_failure, "Native Release failure is preserved");
        result(api.close(session), busy, "Failed Release keeps Close busy");
        check(mock.metrics().live_handles == 1 && GetModuleHandleW(model_path.c_str()) != nullptr,
              "Failed Release retains the model handle and loaded runtime");
        mock.success_results();
        result(api.release(session, feature), success, "Successful Release removes the handle");
        result(api.release(session, feature), xivrender::nr::foreign_feature, "Repeated Release rejected");

        mock.results(success, success, native_failure, success, success, success, 0);
        void *failed_feature = nullptr;
        result(api.create(session, commands.list.Get(), &parameters, &failed_feature), native_failure,
               "Create failure without a handle is preserved");
        check(failed_feature == nullptr && mock.metrics().live_handles == 0,
              "Failed Create leaves no phantom feature");
        mock.results(success, success, native_failure, success, success, success, 1);
        result(api.create(session, commands.list.Get(), &parameters, &failed_feature), native_failure,
               "Partial Create failure is preserved");
        must(failed_feature != nullptr && mock.metrics().live_handles == 1,
             "Partial Create returns a tracked cleanup handle");
        result(api.close(session), busy, "Partial failed Create retains session ownership");
        result(api.evaluate(session, commands.list.Get(), failed_feature, &parameters), invalid_argument,
               "Cleanup-only partial handle cannot be evaluated");
        mock.success_results();
        result(api.release(session, failed_feature), success,
               "Partial failed Create can be explicitly released");
        result(api.close(session), success, "Clean Close calls native Shutdown");
        check(mock.metrics().shutdowns == 1 && mock.metrics().live_handles == 0,
              "All handles released before native shutdown");
        check(GetModuleHandleW(model_path.c_str()) == nullptr,
              "Clean Close releases the private runtime library");
        result(api.close(session), invalid_argument, "Repeated Close rejected safely");
        result(api.create(session, commands.list.Get(), &parameters, &feature), invalid_argument,
               "Closed session cannot create a feature");
        check(feature == nullptr, "Closed-session Create clears the output");
        session = nullptr;

        observed = {};
        {
            Environment fail(L"XIVRENDER_TEST_NR_INIT_RESULT", L"0xbad10002");
            result(open(model_path, &session), native_failure, "Init failure returns the native error");
        }
        check(session == nullptr && observed.populates == 0 &&
                  GetModuleHandleW(model_path.c_str()) == nullptr,
              "Failed Init does not publish a session or leave a loaded runtime");
        observed = {};
        {
            Environment fail(L"XIVRENDER_TEST_NR_POPULATE_THROW", L"1");
            result(open(model_path, &session), internal_error, "Populate allocation exception is contained");
        }
        check(session == nullptr && observed.shutdowns == 1 &&
                  GetModuleHandleW(model_path.c_str()) == nullptr,
              "Populate exception shuts down the initialized model before unload");
        {
            Environment fail(L"XIVRENDER_TEST_NR_POPULATE_RESULT", L"0xbad10002");
            result(open(model_path, &session), native_failure, "Populate failure returns the native error");
        }
        metrics = observed;
        check(session == nullptr && metrics.inits == 1 && metrics.populates == 1 && metrics.shutdowns == 1,
              "Failed Populate shuts down initialized native state");
        check(GetModuleHandleW(model_path.c_str()) == nullptr,
              "Failed Populate releases the runtime after successful Shutdown");

        observed = {};
        {
            Environment fail_populate(L"XIVRENDER_TEST_NR_POPULATE_RESULT", L"0xbad10002");
            Environment fail_shutdown(L"XIVRENDER_TEST_NR_SHUTDOWN_RESULT", L"0xbad10002");
            result(open(model_path, &session), native_failure,
                   "Populate error survives a failed rollback Shutdown");
        }
        must(session != nullptr, "Failed rollback returns a session for explicit cleanup");
        void *rollback_feature = nullptr;
        result(api.create(session, commands.list.Get(), &parameters, &rollback_feature), invalid_argument,
               "Failed initialization session is cleanup-only");
        check(GetModuleHandleW(model_path.c_str()) != nullptr && observed.shutdowns == 1,
              "Failed rollback retains native runtime and its initialized device");
        result(open(model_path, &duplicate), busy, "Failed rollback session keeps the model exclusive");
        Mock failed_rollback(GetModuleHandleW(model_path.c_str()), observed);
        failed_rollback.success_results();
        result(api.close(session), success, "Retained failed rollback session can be closed successfully");
        check(GetModuleHandleW(model_path.c_str()) == nullptr && observed.shutdowns == 2,
              "Only successful cleanup unloads the failed initialization runtime");
        session = nullptr;

        must(result(open(model_path, &session), success, "Reopen after failed initialization"),
             "Reopen succeeded");
        Mock reopened_mock(GetModuleHandleW(model_path.c_str()), observed);
        reopened_mock.results(success, success, success, success, success, native_failure, 0);
        result(api.close(session), native_failure, "Shutdown failure is preserved");
        result(open(model_path, &duplicate), busy, "Failed Shutdown keeps the session exclusive");
        reopened_mock.success_results();
        result(api.close(session), success, "Shutdown can be retried without losing ownership");
        result(api.close(session), invalid_argument, "Successful retry invalidates the old session");
        check(observed.shutdowns == 2, "Only explicit Close attempts call native Shutdown");
        check(token == 0x0123456789abcdefull,
              "Opaque parameter pointers were never dereferenced as resources");

        std::cout << (failures ? "FAILED: " : "PASS: ") << checks << " native NR bridge checks; " << failures
                  << " failures. Software WARP and ABI fixture only; no NVIDIA model or GPU submission.\n";
        return failures ? 1 : 0;
    } catch (const std::exception &error) {
        std::cerr << "Native NR fixture error: " << error.what() << '\n';
        return 3;
    }
}
