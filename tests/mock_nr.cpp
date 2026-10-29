// Test-only native NR ABI fixture. No NVIDIA algorithm, GPU work, or model data.
// The .def file maps these separate names to the snippet's actual exports.
#define NVSDK_NGX
#include <nvsdk_ngx.h>
#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <unordered_set>
#include <new>
#include <vector>

// Kept byte-for-byte in native_nr_bridge_tests.cpp. Do not expose this test ABI
// as part of the addon or the native bridge's public interface.
struct MockNrMetrics {
    unsigned inits = 0, populates = 0, creates = 0, evaluations = 0;
    unsigned releases = 0, shutdowns = 0, callbacks = 0, live_handles = 0;
    unsigned api_version = 0, feature = 0, typed_mask = 0, width = 0, height = 0;
    unsigned init_parameters_null = 0, progress_null = 0;
    std::uint64_t application_id = 0;
    std::uintptr_t device = 0, commands = 0, parameters = 0, handle = 0;
};

namespace {
constexpr unsigned success = 1;
constexpr unsigned invalid = 0xbad10001;
MockNrMetrics metrics;
MockNrMetrics *observer = nullptr;
unsigned init_result = success, populate_result = success, create_result = success;
unsigned evaluate_result = success, release_result = success, shutdown_result = success;
bool partial_create = false;
unsigned next_id = 1;
std::unordered_set<void *> handles;
std::vector<NVSDK_NGX_Parameter *> created_parameters;

struct Publish {
    ~Publish() {
        if (observer)
            *observer = metrics;
    }
};
unsigned environment_result(const wchar_t *key, unsigned fallback) {
    wchar_t value[32]{};
    const auto length = GetEnvironmentVariableW(key, value, 32);
    if (!length || length >= 32)
        return fallback;
    wchar_t *end = nullptr;
    const auto parsed = std::wcstoul(value, &end, 0);
    return end && *end == L'\0' ? parsed : fallback;
}
__declspec(noinline) void allocation_failure() {
    throw std::bad_alloc{};
}

unsigned typed_parameters(NVSDK_NGX_Parameter *parameters) {
    unsigned mask = 0;
    unsigned long long wide = 0;
    float single = 0;
    double dual = 0;
    unsigned integer = 0;
    int signed_integer = 0;
    ID3D11Resource *resource11 = nullptr;
    ID3D12Resource *resource12 = nullptr;
    void *pointer = nullptr;
    if (parameters->Get("Mock.ULL", &wide) == NVSDK_NGX_Result_Success && wide == 0xfedcba9876543210ull)
        mask |= 1u;
    if (parameters->Get("Mock.Float", &single) == NVSDK_NGX_Result_Success && single == .375f)
        mask |= 2u;
    if (parameters->Get("Mock.Double", &dual) == NVSDK_NGX_Result_Success && dual == .123456789012345)
        mask |= 4u;
    if (parameters->Get("Mock.Unsigned", &integer) == NVSDK_NGX_Result_Success && integer == 123456u)
        mask |= 8u;
    if (parameters->Get("Mock.Signed", &signed_integer) == NVSDK_NGX_Result_Success && signed_integer == -73)
        mask |= 16u;
    if (parameters->Get("Mock.Resource11", &resource11) == NVSDK_NGX_Result_Success && resource11)
        mask |= 32u;
    if (parameters->Get("Mock.Resource12", &resource12) == NVSDK_NGX_Result_Success && resource12)
        mask |= 64u;
    if (parameters->Get("Mock.Pointer", &pointer) == NVSDK_NGX_Result_Success && pointer &&
        reinterpret_cast<void *>(resource11) == pointer && reinterpret_cast<void *>(resource12) == pointer)
        mask |= 128u;

    // Call every Set overload through the SDK's virtual interface as a loaded
    // model would. These calls must not use guessed vtable indices.
    parameters->Set("Mock.EchoULL", wide);
    parameters->Set("Mock.EchoFloat", single);
    parameters->Set("Mock.EchoDouble", dual);
    parameters->Set("Mock.EchoUnsigned", integer);
    parameters->Set("Mock.EchoSigned", signed_integer);
    parameters->Set("Mock.EchoResource11", resource11);
    parameters->Set("Mock.EchoResource12", resource12);
    parameters->Set("Mock.EchoPointer", pointer);
    return mask;
}

unsigned __cdecl scaling_ratio(NVSDK_NGX_Parameter *parameters) {
    Publish publish;
    ++metrics.callbacks;
    if (!parameters)
        return invalid;
    unsigned quality = 99;
    if (parameters->Get("PerfQualityValue", &quality) != NVSDK_NGX_Result_Success)
        return invalid;
    if (quality != 2)
        return invalid;
    parameters->Set("DLSSNR.ScalingRatio", 2.0f / 3.0f);
    return success;
}
} // namespace

extern "C" __declspec(dllexport) void __cdecl MockNrReset() {
    for (void *handle : handles)
        delete static_cast<unsigned *>(handle);
    handles.clear();
    metrics = {};
    observer = nullptr;
    next_id = 1;
    init_result = populate_result = create_result = evaluate_result = release_result = shutdown_result =
        success;
    partial_create = false;
}
extern "C" __declspec(dllexport) void __cdecl MockNrSnapshot(MockNrMetrics *output) {
    if (output)
        *output = metrics;
}
extern "C" __declspec(dllexport) void __cdecl MockNrResults(unsigned init, unsigned populate, unsigned create,
                                                            unsigned evaluate, unsigned release,
                                                            unsigned shutdown, unsigned partial) {
    init_result = init;
    populate_result = populate;
    create_result = create;
    evaluate_result = evaluate;
    release_result = release;
    shutdown_result = shutdown;
    partial_create = partial != 0;
}

extern "C" __declspec(noinline) unsigned __cdecl MockNrInit(std::uint64_t application_id, const wchar_t *data,
                                                            ID3D12Device *device, unsigned api_version,
                                                            NVSDK_NGX_Parameter *parameters) {
    Publish publish;
    // Failure injection before Open cannot rely on preloading this DLL: the
    // bridge correctly refuses native runtimes already owned by another loader.
    init_result = environment_result(L"XIVRENDER_TEST_NR_INIT_RESULT", init_result);
    populate_result = environment_result(L"XIVRENDER_TEST_NR_POPULATE_RESULT", populate_result);
    shutdown_result = environment_result(L"XIVRENDER_TEST_NR_SHUTDOWN_RESULT", shutdown_result);
    ++metrics.inits;
    metrics.application_id = application_id;
    metrics.device = reinterpret_cast<std::uintptr_t>(device);
    metrics.api_version = api_version;
    metrics.init_parameters_null = parameters == nullptr;
    if (!data || !*data || !device)
        return invalid;
    return init_result;
}
extern "C" __declspec(noinline) unsigned __cdecl MockNrPopulate(NVSDK_NGX_Parameter *parameters) {
    Publish publish;
    ++metrics.populates;
    metrics.parameters = reinterpret_cast<std::uintptr_t>(parameters);
    if (!parameters)
        return invalid;
    void *sink = nullptr;
    if (parameters->Get("Mock.Observer", &sink) == NVSDK_NGX_Result_Success)
        observer = static_cast<MockNrMetrics *>(sink);
    if (environment_result(L"XIVRENDER_TEST_NR_POPULATE_THROW", 0))
        allocation_failure();
    metrics.typed_mask = typed_parameters(parameters);
    parameters->Set("DLSSNRComputeScalingRatioCallback",
                    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&scaling_ratio)));
    return populate_result;
}
extern "C" __declspec(dllexport) NVSDK_NGX_Parameter *__cdecl MockNrParameters(unsigned index) {
    return index < created_parameters.size() ? created_parameters[index] : nullptr;
}
extern "C" __declspec(noinline) unsigned __cdecl MockNrCreate(ID3D12GraphicsCommandList *commands,
                                                              unsigned feature,
                                                              NVSDK_NGX_Parameter *parameters,
                                                              void **output) {
    Publish publish;
    ++metrics.creates;
    created_parameters.push_back(parameters);
    metrics.commands = reinterpret_cast<std::uintptr_t>(commands);
    metrics.parameters = reinterpret_cast<std::uintptr_t>(parameters);
    metrics.feature = feature;
    if (!commands || !parameters || !output)
        return invalid;
    parameters->Get("Width", &metrics.width);
    parameters->Get("Height", &metrics.height);
    *output = nullptr;
    if (create_result == success || partial_create) {
        *output = new unsigned(next_id++);
        handles.insert(*output);
        metrics.handle = reinterpret_cast<std::uintptr_t>(*output);
        metrics.live_handles = static_cast<unsigned>(handles.size());
    }
    return create_result;
}
extern "C" __declspec(noinline) unsigned __cdecl MockNrEvaluate(ID3D12GraphicsCommandList *commands,
                                                                const void *feature,
                                                                NVSDK_NGX_Parameter *parameters,
                                                                void *progress) {
    Publish publish;
    ++metrics.evaluations;
    metrics.commands = reinterpret_cast<std::uintptr_t>(commands);
    metrics.parameters = reinterpret_cast<std::uintptr_t>(parameters);
    metrics.handle = reinterpret_cast<std::uintptr_t>(feature);
    metrics.progress_null = progress == nullptr;
    if (!commands || !parameters || !handles.contains(const_cast<void *>(feature)))
        return invalid;
    return evaluate_result;
}
extern "C" __declspec(noinline) unsigned __cdecl MockNrRelease(void *feature) {
    Publish publish;
    ++metrics.releases;
    metrics.handle = reinterpret_cast<std::uintptr_t>(feature);
    if (!handles.contains(feature))
        return invalid;
    if (release_result == success) {
        handles.erase(feature);
        delete static_cast<unsigned *>(feature);
        metrics.live_handles = static_cast<unsigned>(handles.size());
    }
    return release_result;
}
extern "C" __declspec(noinline) unsigned __cdecl MockNrShutdown(ID3D12Device *device) {
    Publish publish;
    ++metrics.shutdowns;
    if (reinterpret_cast<std::uintptr_t>(device) != metrics.device)
        return invalid;
    if (!handles.empty())
        return invalid;
    return shutdown_result;
}
