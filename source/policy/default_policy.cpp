#include "default_policy.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace apiexec {
constexpr double MAX_TIMEOUT_MS = 32000.0;
constexpr double JITTER_FACTOR = 0.25; // +/- 25% jitter
constexpr double JITTER_MIN = 1.0 - JITTER_FACTOR;
constexpr double JITTER_MAX = 1.0 + JITTER_FACTOR;
constexpr double DELAY_MULTIPLIER = 2.0; // Exponential backoff multiplier


DefaultPolicy::DefaultPolicy(Config cfg) : config_(std::move(cfg)) {}

void DefaultPolicy::adjust(Cursor& cursor, bool success) {
    if (!cursor.has_time_window()){
        return;
    }

    const int64_t window = cursor.time_window_end - cursor.time_window_start;
    if (window <= 0){
        return;
    }

    const double factor = success
        ? config_.window_grow_factor
        : config_.window_shrink_factor;

    auto new_window = static_cast<int64_t>(
        static_cast<double>(window) * factor
    );

    if (success) {
        new_window = std::min(new_window, config_.max_window_ms);
    } else {
        new_window = std::max(new_window, config_.min_window_ms);
    }

    cursor.time_window_end = cursor.time_window_start + new_window;
}

auto DefaultPolicy::backoff(int retry_count) -> Duration {
    // Exponential: base * 2^retry
    auto base = static_cast<double>(config_.base_backoff_ms);
    double delay = base * std::pow(DELAY_MULTIPLIER, retry_count);

    // Cap at 32 seconds
    delay = std::min(delay, MAX_TIMEOUT_MS);

    // Jitter: +/- 25%
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<double> jitter(JITTER_MIN, JITTER_MAX);
    delay *= jitter(rng);

    return Duration(static_cast<int64_t>(delay));
}

auto DefaultPolicy::prefetch_depth() -> std::size_t  {
    return config_.prefetch_depth_val;
}

} // namespace apiexec
