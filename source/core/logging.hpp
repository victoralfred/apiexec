#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace apiexec {

enum class LogLevel : int {
    TRACE = 0,
    DEBUG = 1,
    INFO  = 2,
    WARN  = 3,
    ERROR = 4,
    NONE  = 5,
};

// Structured log entry — all fields populated by the engine.
// API keys and secrets are never included.
struct LogEntry {
    LogLevel level = LogLevel::INFO;
    std::string adapter_name;
    std::string message;
    int32_t http_status = 0;
    int retry_count = 0;
    int64_t cursor_start_ms = 0;
    int64_t cursor_end_ms = 0;
    std::string page_token_redacted;  // first 8 chars + "..."
    bool cursor_exhausted = false;
};

// Callback type for log output. The engine calls this for every log-worthy event.
using LogCallback = std::function<void(const LogEntry&)>;

// Global log level — default INFO. Engine checks this before constructing LogEntry.
inline auto global_log_level() -> std::atomic<LogLevel>& {
    static std::atomic<LogLevel> level{LogLevel::INFO};
    return level;
}

inline auto set_log_level(LogLevel level) -> void {
    global_log_level().store(level, std::memory_order_relaxed);
}

inline auto should_log(LogLevel level) -> bool {
    return level >= global_log_level().load(std::memory_order_relaxed);
}

// Redact a page token for logging: show first 8 chars, mask rest.
inline auto redact_token(const std::string& token) -> std::string {
    constexpr std::size_t kVisibleChars = 8;
    if (token.size() <= kVisibleChars) return token;
    return token.substr(0, kVisibleChars) + "...";
}

} // namespace apiexec
