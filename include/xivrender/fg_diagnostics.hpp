#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <chrono>
namespace xivrender::fg::diagnostics {
// No graphics locks, SDK calls, or file I/O on a recording thread. The writer
// owns disk operations; queue loss and write failures are explicit telemetry.
void start(const std::filesystem::path &directory) noexcept;
void context(unsigned provider, std::uint64_t generation) noexcept;
void event(const char *kind, std::uint64_t frame = 0, double a = 0, double b = 0,
           double c = 0, double d = 0, const char *message = nullptr) noexcept;
void flush() noexcept;
void shutdown() noexcept;
std::string path();
unsigned error() noexcept;
// Sparse timing for hot-path stages: no queue entry unless the call stalls.
class SlowScope {
    const char *name_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
public:
    explicit SlowScope(const char *name) noexcept : name_(name) {}
    ~SlowScope() {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
        if (ms >= 2) event("phase.slow", 0, ms, 0, 0, 0, name_);
    }
    SlowScope(const SlowScope &) = delete;
    SlowScope &operator=(const SlowScope &) = delete;
};
class Scope {
    const char *name_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
public:
    explicit Scope(const char *name) noexcept : name_(name) {
        event("scope.begin", 0, 0, 0, 0, 0, name_);
    }
    ~Scope() {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
        event("scope.end", 0, ms, 0, 0, 0, name_);
    }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
};
}
