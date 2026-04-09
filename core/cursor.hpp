#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace apiexec {

// Maximum entries in Cursor::extra to prevent unbounded growth.
inline constexpr std::size_t kCursorExtraMaxEntries = 16;
// Maximum byte length of a single extra value.
inline constexpr std::size_t kCursorExtraMaxValueLen = 256;

struct Cursor {
    // Time-window boundaries (epoch milliseconds). 0 means unset.
    int64_t time_window_start = 0;
    int64_t time_window_end   = 0;

    // Opaque page token for cursor-based pagination.
    std::string page_token;

    // True when no more data is available.
    bool exhausted = false;

    // Adapter-specific key/value metadata with bounded size.
    std::map<std::string, std::string> extra;

    // Try to insert into extra. Returns false if the entry would violate
    // size caps (max entries or max value length).
    auto set_extra(const std::string& key, const std::string& value) -> bool {
        if (value.size() > kCursorExtraMaxValueLen) return false;
        if (extra.count(key) == 0 && extra.size() >= kCursorExtraMaxEntries) return false;
        extra[key] = value;
        return true;
    }

    auto clear_extra() -> void { extra.clear(); }

    auto has_time_window() const -> bool {
        return time_window_start != 0 || time_window_end != 0;
    }
};

} // namespace apiexec
