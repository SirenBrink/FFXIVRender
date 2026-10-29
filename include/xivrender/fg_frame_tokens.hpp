#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace xivrender::fg {
// SDK tokens are borrowed handles, not copyable frame objects. Issue once at
// simulation start and carry that handle through asynchronous render/Present.
// Capacity must not exceed the SDK's token pool. No lookup can allocate a new
// token or silently turn an old frame's constants into a newer frame's data.
template <class Token, std::size_t Capacity> class FrameTokens {
    static_assert(Capacity > 0);
    struct Entry {
        std::uint64_t id = 0;
        Token *token = nullptr;
    };
    std::array<Entry, Capacity> entries_{};
    std::size_t cursor_ = 0;
    std::uint64_t last_issued_ = 0;
    std::mutex mutex_;

    Token *find_locked(std::uint64_t id) const {
        if (!id)
            return nullptr;
        for (const auto &entry : entries_)
            if (entry.id == id && entry.token &&
                static_cast<std::uint32_t>(*entry.token) == static_cast<std::uint32_t>(id))
                return entry.token;
        return nullptr;
    }

  public:
    template <class Issue> Token *issue(std::uint64_t id, Issue &&issue_sdk_token) {
        std::lock_guard lock(mutex_);
        if (auto *token = find_locked(id))
            return token;
        // Only the simulation owner may advance issuance. Out-of-order render
        // or marker requests must use find(), including after a live handoff.
        if (!id || id <= last_issued_)
            return nullptr;
        auto *token = issue_sdk_token(static_cast<std::uint32_t>(id));
        if (!token || static_cast<std::uint32_t>(*token) != static_cast<std::uint32_t>(id))
            return nullptr;
        entries_[cursor_] = {id, token};
        cursor_ = (cursor_ + 1) % Capacity;
        last_issued_ = id;
        return token;
    }
    Token *find(std::uint64_t id) {
        std::lock_guard lock(mutex_);
        return find_locked(id);
    }
    void clear() {
        std::lock_guard lock(mutex_);
        entries_ = {};
        cursor_ = 0;
        last_issued_ = 0;
    }
};
} // namespace xivrender::fg
