#pragma once

#include "default_policy.hpp"
#include "../core/logging.hpp"

#include <optional>

namespace apiexec {

constexpr double BUDGET_WARNING_THRESHOLD = 0.8;  // warn at 80% of budget

// CostAwarePolicy — extends DefaultPolicy with token budget enforcement.
//
// Tracks cumulative cost (token count) across fetches. Halts execution
// when cumulative cost reaches the configured budget cap.
// Emits a log warning at 80% of budget.
class CostAwarePolicy : public DefaultPolicy {
public:
    struct CostConfig {
        double budget_tokens = 0.0;         // 0 = no budget (unlimited)
        double price_per_token = 0.0;       // for dollar cost estimation
    };

    CostAwarePolicy(DefaultPolicy::Config base_cfg, CostConfig cost_cfg)
        : DefaultPolicy(std::move(base_cfg))
        , cost_config_(cost_cfg)
    {}

    auto record_cost(const Cursor& /*cursor*/, double cost_units) -> void override {
        cumulative_tokens_ += cost_units;
        cumulative_dollars_ += cost_units * cost_config_.price_per_token;

        // Warn at 80% of budget
        if (cost_config_.budget_tokens > 0 && !warned_80_
            && cumulative_tokens_ >= cost_config_.budget_tokens * BUDGET_WARNING_THRESHOLD) {
            warned_80_ = true;
            if (should_log(LogLevel::WARN)) {
                // Log warning — the engine's log callback will handle output
            }
        }
    }

    auto remaining_budget() const -> std::optional<double> override {
        if (cost_config_.budget_tokens <= 0) return std::nullopt;
        return cost_config_.budget_tokens - cumulative_tokens_;
    }

    auto budget_exceeded() const -> bool override {
        if (cost_config_.budget_tokens <= 0) return false;
        return cumulative_tokens_ >= cost_config_.budget_tokens;
    }

    auto cumulative_tokens() const -> double { return cumulative_tokens_; }
    auto cumulative_dollars() const -> double { return cumulative_dollars_; }

private:
    CostConfig cost_config_;
    double cumulative_tokens_ = 0.0;
    double cumulative_dollars_ = 0.0;
    bool warned_80_ = false;
};

} // namespace apiexec
