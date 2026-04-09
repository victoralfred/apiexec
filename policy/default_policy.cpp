#include "default_policy.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace apiexec {

DefaultPolicy::DefaultPolicy(Config cfg) : config_(std::move(cfg)) {}

void DefaultPolicy::adjust(Cursor& cursor, bool success) {
    if (!cursor.has_time_window()) return;

    int64_t window = cursor.time_window_end - cursor.time_window_start;
    if (window <= 0) return;

    if (success) {
        int64_t new_window = static_cast<int64_t>(window * config_.window_grow_factor);
        new_window = std::min(new_window, config_.max_window_ms);
        cursor.time_window_end = cursor.time_window_start + new_window;
    } else {
        int64_t new_window = static_cast<int64_t>(window * config_.window_shrink_factor);
        new_window = std::max(new_window, config_.min_window_ms);
        cursor.time_window_end = cursor.time_window_start + new_window;
    }
}

Duration DefaultPolicy::backoff(int retry_count) {
    // Exponential: base * 2^retry
    double base = static_cast<double>(config_.base_backoff_ms);
    double delay = base * std::pow(2.0, retry_count);

    // Cap at 32 seconds
    delay = std::min(delay, 32000.0);

    // Jitter: +/- 25%
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<double> jitter(0.75, 1.25);
    delay *= jitter(rng);

    return Duration(static_cast<int64_t>(delay));
}

std::size_t DefaultPolicy::prefetch_depth() {
    // M1: sequential only (0). Double-buffer (1) comes in M2.
    return 0;
}

} // namespace apiexec
