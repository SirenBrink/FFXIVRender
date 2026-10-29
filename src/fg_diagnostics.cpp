#include "xivrender/fg_diagnostics.hpp"
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <cmath>
namespace xivrender::fg::diagnostics {
namespace {
struct Record {
    std::uint64_t sequence = 0, qpc = 0, frame = 0, generation = 0;
    unsigned thread = 0, provider = 0;
    double a = 0, b = 0, c = 0, d = 0;
    bool truncated = false;
    std::array<char, 48> kind{};
    std::array<char, 1536> message{};
};
struct Writer {
    std::mutex mutex;
    std::condition_variable wake, written;
    std::deque<Record> queue;
    std::thread worker;
    std::filesystem::path file;
    std::atomic<bool> active{false};
    std::atomic<unsigned> provider{0}, failure{0};
    std::atomic<std::uint64_t> generation{0};
    std::uint64_t next = 0, completed = 0, dropped = 0;
    bool stopping = false;
};
Writer &state() { static auto *value = new Writer; return *value; }
std::string escape(const char *text) {
    std::string out;
    for (auto *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
        if (*p == '"' || *p == '\\') { out += '\\'; out += char(*p); }
        else if (*p < 32) {
            char encoded[7]{};
            sprintf_s(encoded, "\\u%04x", unsigned(*p));
            out += encoded;
        } else if (*p < 128) out += char(*p);
        else {
            // Vendor messages may contain invalid bytes, or the bounded
            // record may cut a UTF-8 sequence. Keep every JSONL record valid.
            unsigned count = *p >= 0xf0 ? 4 : *p >= 0xe0 ? 3 : *p >= 0xc2 ? 2 : 0;
            unsigned code = count ? *p & ((1u << (7 - count)) - 1) : 0;
            bool valid = count && *p <= 0xf4;
            for (unsigned i = 1; valid && i < count; ++i) {
                valid = p[i] && (p[i] & 0xc0) == 0x80;
                if (valid) code = (code << 6) | (p[i] & 0x3f);
            }
            valid = valid && code <= 0x10ffff && !(code >= 0xd800 && code <= 0xdfff) &&
                    code >= (count == 2 ? 0x80u : count == 3 ? 0x800u : 0x10000u);
            if (valid) { out.append(reinterpret_cast<const char *>(p), count); p += count - 1; }
            else out += "\\ufffd";
        }
    }
    return out;
}
double number(double x) { return std::isfinite(x) ? x : 0; }
void write_loop() noexcept {
    auto &s = state();
    try {
        std::ofstream output(s.file, std::ios::binary | std::ios::trunc);
        if (!output) { s.failure = ERROR_OPEN_FAILED; return; }
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
        SYSTEMTIME utc{}; GetSystemTime(&utc);
        output << "{\"schema\":1,\"kind\":\"session\",\"pid\":" << GetCurrentProcessId()
               << ",\"qpc_frequency\":" << frequency.QuadPart << ",\"utc\":\""
               << utc.wYear << '-' << utc.wMonth << '-' << utc.wDay << 'T' << utc.wHour << ':'
               << utc.wMinute << ':' << utc.wSecond << "Z\",\"version\":\"0.4.16\"}\n";
        std::uint64_t bytes = 0;
        for (;;) {
            std::deque<Record> batch;
            std::uint64_t lost = 0;
            bool done = false;
            {
                std::unique_lock lock(s.mutex);
                s.wake.wait_for(lock, std::chrono::milliseconds(100), [&] { return s.stopping || s.queue.size() >= 256; });
                batch.swap(s.queue);
                lost = std::exchange(s.dropped, 0);
                done = s.stopping;
            }
            if (lost) output << "{\"kind\":\"records_dropped\",\"count\":" << lost << "}\n";
            for (const auto &r : batch) {
                output << std::setprecision(12) << "{\"seq\":" << r.sequence << ",\"qpc\":" << r.qpc
                       << ",\"tid\":" << r.thread << ",\"frame\":" << r.frame
                       << ",\"generation\":" << r.generation << ",\"provider\":" << r.provider
                       << ",\"kind\":\"" << escape(r.kind.data()) << "\",\"a\":" << r.a
                       << ",\"b\":" << r.b << ",\"c\":" << r.c << ",\"d\":" << r.d
                       << ",\"truncated\":" << (r.truncated ? "true" : "false")
                       << ",\"message\":\"" << escape(r.message.data()) << "\"}\n";
            }
            output.flush();
            if (!output) { s.failure = ERROR_WRITE_FAULT; break; }
            bytes = std::uint64_t(output.tellp());
            {
                std::lock_guard lock(s.mutex);
                if (!batch.empty()) s.completed = batch.back().sequence;
                s.written.notify_all();
            }
            if (done) break;
            if (bytes >= 32ull * 1024 * 1024) {
                output.close();
                // Rotate only this session's own files (256 MiB maximum).
                std::error_code ec;
                for (int i = 7; i >= 1; --i) {
                    auto to = s.file; to += L"." + std::to_wstring(i);
                    auto from = s.file; if (i > 1) from += L"." + std::to_wstring(i - 1);
                    std::filesystem::remove(to, ec); ec.clear();
                    if (std::filesystem::exists(from)) std::filesystem::rename(from, to, ec);
                    if (ec) { s.failure = unsigned(ec.value()); return; }
                }
                output.open(s.file, std::ios::binary | std::ios::trunc);
                output << "{\"kind\":\"continuation\",\"schema\":1,\"qpc_frequency\":" << frequency.QuadPart << "}\n";
            }
        }
    } catch (...) { s.failure = ERROR_UNHANDLED_EXCEPTION; }
    s.written.notify_all();
}
}
void start(const std::filesystem::path &directory) noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.active || directory.empty()) return;
    try {
        std::filesystem::create_directories(directory);
        SYSTEMTIME utc{}; GetSystemTime(&utc);
        wchar_t name[100]{};
        swprintf_s(name, L"fg-%04u%02u%02u-%02u%02u%02u-%03u-%lu.jsonl", utc.wYear, utc.wMonth,
                   utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds, GetCurrentProcessId());
        s.file = directory / name;
        s.stopping = false; s.failure = 0;
        s.queue.clear(); s.next = s.completed = s.dropped = 0;
        s.worker = std::thread(write_loop);
        s.active = true;
    } catch (...) { s.failure = ERROR_OPEN_FAILED; }
}
void context(unsigned provider, std::uint64_t generation) noexcept {
    state().provider = provider; state().generation = generation;
}
void event(const char *kind, std::uint64_t frame, double a, double b, double c, double d, const char *message) noexcept {
    auto &s = state();
    if (!s.active || s.failure) return;
    Record r;
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    r.qpc = now.QuadPart; r.thread = GetCurrentThreadId(); r.frame = frame;
    r.provider = s.provider.load(); r.generation = s.generation.load();
    r.a = number(a); r.b = number(b); r.c = number(c); r.d = number(d);
    if (kind) strncpy_s(r.kind.data(), r.kind.size(), kind, _TRUNCATE);
    if (message) {
        r.truncated = strlen(message) >= r.message.size();
        strncpy_s(r.message.data(), r.message.size(), message, _TRUNCATE);
    }
    try {
        std::lock_guard lock(s.mutex);
        if (s.stopping) return;
        if (s.queue.size() >= 8192) { ++s.dropped; return; }
        r.sequence = ++s.next;
        s.queue.push_back(r);
        s.wake.notify_one();
    } catch (...) { s.failure = ERROR_NOT_ENOUGH_MEMORY; }
}
void flush() noexcept {
    auto &s = state();
    std::unique_lock lock(s.mutex);
    const auto target = s.next;
    s.wake.notify_one();
    s.written.wait_for(lock, std::chrono::seconds(2), [&] { return s.completed >= target || s.failure || !s.active; });
}
void shutdown() noexcept {
    event("session.end");
    auto &s = state();
    {
        std::lock_guard lock(s.mutex);
        if (!s.active) return;
        s.stopping = true; s.active = false; s.wake.notify_one();
    }
    if (s.worker.joinable()) s.worker.join();
}
std::string path() { std::lock_guard lock(state().mutex); return state().file.string(); }
unsigned error() noexcept { return state().failure.load(); }
}
