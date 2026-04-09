#pragma once

#include "../core/execution_policy.hpp"

namespace apiexec {

// DefaultPolicy — concrete ExecutionPolicy strategy.
// Exponential backoff with jitter, simple window grow/shrink.
class DefaultPolicy : public ExecutionPolicy {
public:
    struct Config {
        int max_retries = 5;
        int base_backoff_ms = 100;
        double window_grow_factor = 1.5;
        double window_shrink_factor = 0.5;
        int64_t min_window_ms = 60'000;        // 1 minute
        int64_t max_window_ms = 86'400'000;     // 24 hours
    };

    DefaultPolicy() : DefaultPolicy(Config{}) {}
    explicit DefaultPolicy(Config cfg);

    void adjust(Cursor& cursor, bool success) override;
    Duration backoff(int retry_count) override;
    std::size_t prefetch_depth() override;
    int max_retries() const override { return config_.max_retries; }

private:
    Config config_;
};

} // namespace apiexec
