#include <xivrender/ngx_parameters.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <thread>

namespace {

template <class T> void round_trip(NVSDK_NGX_Parameter &parameters, const char *name, T value) {
    parameters.Set(name, value);
    T output{};
    assert(parameters.Get(name, &output) == NVSDK_NGX_Result_Success);
    assert(output == value);
}

template <class T> void missing(NVSDK_NGX_Parameter &parameters, T initial) {
    T output = initial;
    assert(parameters.Get("Missing", &output) == NVSDK_NGX_Result_FAIL_UnsupportedParameter);
    assert(output == T{});
}

} // namespace

int main() {
    using xivrender::NgxParameters;
    NgxParameters owned;
    NVSDK_NGX_Parameter &parameters = owned;
    // Exercise the public SDK's virtual calls, rather than concrete helpers.
    round_trip(parameters, "ULL", 0xfedcba9876543210ull);
    round_trip(parameters, "Float", .375f);
    round_trip(parameters, "Double", .123456789012345);
    round_trip(parameters, "Unsigned", 123456u);
    round_trip(parameters, "Signed", -73);

    // These are opaque addresses, never dereferenced as resource interfaces.
    // Their source is real storage; no fabricated integer address or pointer
    // alias into std::variant memory participates in the test.
    volatile std::uint64_t token = 0x0123456789abcdefull;
    auto *const source = const_cast<std::uint64_t *>(&token);
    void *const pointer = source;
    auto *const resource11 = reinterpret_cast<ID3D11Resource *>(source);
    auto *const resource12 = reinterpret_cast<ID3D12Resource *>(source);
    round_trip(parameters, "Pointer", pointer);
    round_trip(parameters, "Resource11", resource11);
    round_trip(parameters, "Resource12", resource12);
    assert(owned.size() == 8);

    for (const char *name : {"Pointer", "Resource11", "Resource12"}) {
        unsigned long long bits = 0;
        assert(parameters.Get(name, &bits) == NVSDK_NGX_Result_Success);
        assert(bits == reinterpret_cast<std::uintptr_t>(source));
        parameters.Set("PointerBits", bits);
        void *result = nullptr;
        ID3D11Resource *result11 = nullptr;
        ID3D12Resource *result12 = nullptr;
        assert(parameters.Get("PointerBits", &result) == NVSDK_NGX_Result_Success && result == pointer);
        assert(parameters.Get("PointerBits", &result11) == NVSDK_NGX_Result_Success &&
               result11 == resource11);
        assert(parameters.Get("PointerBits", &result12) == NVSDK_NGX_Result_Success &&
               result12 == resource12);
        assert(parameters.Get(name, &result11) == NVSDK_NGX_Result_Success && result11 == resource11);
        assert(parameters.Get(name, &result12) == NVSDK_NGX_Result_Success && result12 == resource12);
    }
    assert(token == 0x0123456789abcdefull);

    parameters.Set("NullResource", static_cast<ID3D12Resource *>(nullptr));
    unsigned long long null_bits = 99;
    assert(parameters.Get("NullResource", &null_bits) == NVSDK_NGX_Result_Success && null_bits == 0);
    parameters.Set("NullBits", 0ull);
    void *null_pointer = pointer;
    assert(parameters.Get("NullBits", &null_pointer) == NVSDK_NGX_Result_Success && null_pointer == nullptr);

    unsigned integer = 0;
    int signed_integer = 0;
    float single = 0;
    double dual = 0;
    unsigned long long wide = 0;
    assert(parameters.Get("Unsigned", &wide) == NVSDK_NGX_Result_Success && wide == 123456);
    assert(parameters.Get("Unsigned", &single) == NVSDK_NGX_Result_Success && single == 123456.0f);
    assert(parameters.Get("Float", &dual) == NVSDK_NGX_Result_Success && dual == .375);
    parameters.Set("Numeric", 42.75);
    assert(parameters.Get("Numeric", &integer) == NVSDK_NGX_Result_Success && integer == 42);
    assert(parameters.Get("Numeric", &signed_integer) == NVSDK_NGX_Result_Success && signed_integer == 42);
    parameters.Set("Numeric", -7);
    assert(parameters.Get("Numeric", &single) == NVSDK_NGX_Result_Success && single == -7.0f);
    assert(parameters.Get("Numeric", &integer) == NVSDK_NGX_Result_Success && integer == unsigned(-7));

    // Dangerous conversions reject instead of invoking float-to-integer UB.
    for (const double invalid : {std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN(), std::ldexp(1.0, 64)}) {
        parameters.Set("Invalid", invalid);
        wide = 99;
        assert(parameters.Get("Invalid", &wide) == NVSDK_NGX_Result_FAIL_UnsupportedParameter && wide == 0);
    }
    parameters.Set("Overflow", std::numeric_limits<double>::max());
    single = 99;
    assert(parameters.Get("Overflow", &single) == NVSDK_NGX_Result_FAIL_UnsupportedParameter && single == 0);
    single = 99;
    assert(parameters.Get("Pointer", &single) == NVSDK_NGX_Result_FAIL_UnsupportedParameter && single == 0);
    null_pointer = pointer;
    assert(parameters.Get("Float", &null_pointer) == NVSDK_NGX_Result_FAIL_UnsupportedParameter &&
           null_pointer == nullptr);

    missing(parameters, 1ull);
    missing(parameters, 1.0f);
    missing(parameters, 1.0);
    missing(parameters, 1u);
    missing(parameters, 1);
    missing(parameters, resource11);
    missing(parameters, resource12);
    missing(parameters, pointer);
    integer = 99;
    assert(parameters.Get(nullptr, &integer) == NVSDK_NGX_Result_FAIL_InvalidParameter && integer == 0);
    assert(parameters.Get("Unsigned", static_cast<unsigned *>(nullptr)) ==
           NVSDK_NGX_Result_FAIL_InvalidParameter);
    const auto before_null_set = owned.size();
    parameters.Set(nullptr, 1u);
    assert(owned.size() == before_null_set);

    // Independent features never share state, even if parameter names match.
    NgxParameters other;
    other.Set("Unsigned", 999u);
    assert(parameters.Get("Unsigned", &integer) == NVSDK_NGX_Result_Success && integer == 123456);
    assert(other.Get("Unsigned", &integer) == NVSDK_NGX_Result_Success && integer == 999);
    xivrender::NgxOverlay overlay(&parameters);
    overlay.Set("Unsigned",42u);
    assert(overlay.Get("Unsigned",&integer)==NVSDK_NGX_Result_Success && integer==42);
    assert(parameters.Get("Unsigned",&integer)==NVSDK_NGX_Result_Success && integer==123456);
    overlay.Set("OnlyOverlay",8u);overlay.Reset();
    assert(overlay.Get("Unsigned",&integer)==NVSDK_NGX_Result_Success && integer==123456);
    parameters.Reset();
    assert(owned.size() == 0 && other.size() == 1);
    missing(parameters, pointer);
    std::thread first([&] {
        for (unsigned i = 0; i < 1000; ++i)
            parameters.Set("Concurrent", i);
    });
    std::thread second([&] {
        for (unsigned i = 0; i < 1000; ++i) {
            unsigned output = 99;
            const auto result = parameters.Get("Concurrent", &output);
            assert(result == NVSDK_NGX_Result_Success ||
                   result == NVSDK_NGX_Result_FAIL_UnsupportedParameter);
        }
    });
    first.join();
    second.join();
    assert(parameters.Get("Concurrent", &integer) == NVSDK_NGX_Result_Success && integer == 999);
    std::puts("PASS: SDK-typed NGX parameters, all eight virtual overloads, numeric/pointer conversion, "
              "reset, missing outputs and isolated concurrent storage");
}
