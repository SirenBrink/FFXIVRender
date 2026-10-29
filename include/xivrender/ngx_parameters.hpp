#pragma once

// This is our implementation of the licensed SDK's declared interface. Keep
// its eight overloads in the SDK's declaration order; never call guessed vtable
// indices or borrow the driver's private capability-parameter implementation.
#include <nvsdk_ngx_params.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <shared_mutex>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>

namespace xivrender {

class NgxParameters final : public NVSDK_NGX_Parameter {
    using Value = std::variant<unsigned long long, float, double, unsigned int, int, ID3D11Resource *,
                               ID3D12Resource *, void *>;
    struct KeyHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view key) const noexcept {
            return std::hash<std::string_view>{}(key);
        }
    };
    struct KeyEqual {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const noexcept {
            return a == b;
        }
    };
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Value, KeyHash, KeyEqual> values_;

    template <class To, class From> static bool convert(From value, To &output) {
        if constexpr (std::is_same_v<To, From>) {
            output = value;
            return true;
        } else if constexpr (std::is_pointer_v<To> && std::is_pointer_v<From>) {
            // Opaque resource identity only. No resource is dereferenced or
            // reference-counted by a parameter block.
            output = reinterpret_cast<To>(value);
            return true;
        } else if constexpr (std::is_pointer_v<To> && std::is_integral_v<From>) {
            if constexpr (sizeof(From) > sizeof(std::uintptr_t))
                if (value > std::numeric_limits<std::uintptr_t>::max())
                    return false;
            output = reinterpret_cast<To>(static_cast<std::uintptr_t>(value));
            return true;
        } else if constexpr (std::is_integral_v<To> && std::is_pointer_v<From>) {
            const auto bits = reinterpret_cast<std::uintptr_t>(value);
            if constexpr (sizeof(To) < sizeof(std::uintptr_t) || std::is_signed_v<To>)
                if (bits > static_cast<std::uintptr_t>(std::numeric_limits<To>::max()))
                    return false;
            output = static_cast<To>(bits);
            return true;
        } else if constexpr (std::is_arithmetic_v<To> && std::is_arithmetic_v<From>) {
            if constexpr (std::is_integral_v<To> && std::is_floating_point_v<From>) {
                // Upper-exclusive powers of two avoid UINT64_MAX rounding up
                // to 2^64 on MSVC, where long double has double precision.
                const long double number = static_cast<long double>(value);
                const long double upper = std::ldexp(1.0L, std::numeric_limits<To>::digits);
                const long double lower = std::is_signed_v<To> ? -upper : 0.0L;
                if (!std::isfinite(value) || number < lower || number >= upper)
                    return false;
            } else if constexpr (std::is_floating_point_v<To>) {
                if (std::isfinite(value) && std::fabs(static_cast<long double>(value)) >
                                                static_cast<long double>(std::numeric_limits<To>::max()))
                    return false;
            }
            output = static_cast<To>(value);
            return true;
        } else {
            // Converting a pointer through floating point would lose address
            // bits. Only pointer/integer interchange has an NGX meaning.
            return false;
        }
    }

    template <class T> void write(const char *name, T value) {
        if (!name)
            return;
        std::unique_lock lock(mutex_);
        const auto found = values_.find(std::string_view{name});
        if (found != values_.end())
            found->second = Value{value};
        else
            values_.emplace(name, Value{value});
    }

    template <class T> NVSDK_NGX_Result read(const char *name, T *output) const {
        if (!output)
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        *output = T{};
        if (!name)
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        std::shared_lock lock(mutex_);
        const auto found = values_.find(std::string_view{name});
        if (found == values_.end())
            return NVSDK_NGX_Result_FAIL_UnsupportedParameter;
        return std::visit(
            [&](const auto value) {
                return convert(value, *output) ? NVSDK_NGX_Result_Success
                                               : NVSDK_NGX_Result_FAIL_UnsupportedParameter;
            },
            found->second);
    }

  public:
    // The SDK base deliberately has no virtual destructor. Own this exact
    // concrete type; do not delete it through NVSDK_NGX_Parameter*.
    NgxParameters() = default;
    NgxParameters(const NgxParameters &other) {
        std::shared_lock lock(other.mutex_);
        values_ = other.values_;
    }
    NgxParameters &operator=(const NgxParameters &) = delete;

    void Set(const char *name, unsigned long long value) override {
        write(name, value);
    }
    void Set(const char *name, float value) override {
        write(name, value);
    }
    void Set(const char *name, double value) override {
        write(name, value);
    }
    void Set(const char *name, unsigned int value) override {
        write(name, value);
    }
    void Set(const char *name, int value) override {
        write(name, value);
    }
    void Set(const char *name, ID3D11Resource *value) override {
        write(name, value);
    }
    void Set(const char *name, ID3D12Resource *value) override {
        write(name, value);
    }
    void Set(const char *name, void *value) override {
        write(name, value);
    }

    NVSDK_NGX_Result Get(const char *name, unsigned long long *value) const override {
        return read(name, value);
    }
    NVSDK_NGX_Result Get(const char *name, float *value) const override {
        return read(name, value);
    }
    NVSDK_NGX_Result Get(const char *name, double *value) const override {
        return read(name, value);
    }
    NVSDK_NGX_Result Get(const char *name, unsigned int *value) const override {
        return read(name, value);
    }
    NVSDK_NGX_Result Get(const char *name, int *value) const override {
        return read(name, value);
    }
    NVSDK_NGX_Result Get(const char *name, ID3D11Resource **value) const override {
        return read(name, value);
    }
    NVSDK_NGX_Result Get(const char *name, ID3D12Resource **value) const override {
        return read(name, value);
    }
    NVSDK_NGX_Result Get(const char *name, void **value) const override {
        return read(name, value);
    }

    void Reset() override {
        std::unique_lock lock(mutex_);
        values_.clear();
    }
    std::size_t size() const {
        std::shared_lock lock(mutex_);
        return values_.size();
    }
};

// A call-local overlay: reads fall through to the original NGX block, while
// overrides and SDK writes remain private. No game/NR parameter mutation.
class NgxOverlay final : public NVSDK_NGX_Parameter {
    const NVSDK_NGX_Parameter *source_;
    NgxParameters values_;
public:
    explicit NgxOverlay(const NVSDK_NGX_Parameter *p):source_(p) {}
#define XIV_NGX_OVERLOAD(T) \
    void Set(const char *key,T value) override { values_.Set(key,value); } \
    NVSDK_NGX_Result Get(const char *key,T *out) const override { \
        const auto result=values_.Get(key,out); \
        return result==NVSDK_NGX_Result_Success || !source_ ? result : source_->Get(key,out); }
    XIV_NGX_OVERLOAD(unsigned long long)
    XIV_NGX_OVERLOAD(float)
    XIV_NGX_OVERLOAD(double)
    XIV_NGX_OVERLOAD(unsigned int)
    XIV_NGX_OVERLOAD(int)
    XIV_NGX_OVERLOAD(ID3D11Resource *)
    XIV_NGX_OVERLOAD(ID3D12Resource *)
    XIV_NGX_OVERLOAD(void *)
#undef XIV_NGX_OVERLOAD
    void Reset() override {values_.Reset();}
};

} // namespace xivrender

namespace xivrender::ngx {
using Parameters = ::xivrender::NgxParameters;
}
