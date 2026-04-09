#pragma once

#include "cursor.hpp"

#include <chrono>
#include <cstddef>
#include <optional>

namespace apiexec {

using Duration = std::chrono::milliseconds;

// ExecutionPolicy — Strategy interface for runtime behaviour decisions.
//
// Controls backoff timing, adaptive window sizing, prefetch depth,
// retry limits, and cost budgets. Injected into ExecutionEngine<T> at construction.
struct ExecutionPolicy {
    virtual ~ExecutionPolicy() = default;

    // --- Core (pure virtual) ---
    virtual auto adjust(Cursor& cursor, bool success) -> void = 0;
    virtual auto backoff(int retry_count) -> Duration = 0;
    virtual auto prefetch_depth() -> std::size_t = 0;
    virtual auto max_retries() const -> int = 0;

    // --- Cost hooks (defaulted — non-breaking for existing implementations) ---

    // Called after each successful fetch with the cost from the adapter.
    // Override in a CostAwarePolicy to accumulate costs and enforce budgets.
    virtual auto record_cost(const Cursor& /*cursor*/, double /*cost_units*/) -> void {}

    // Remaining budget. nullopt means no budget constraint is active.
    virtual auto remaining_budget() const -> std::optional<double> { return std::nullopt; }

    // Returns true if the engine should stop due to budget exhaustion.
    virtual auto budget_exceeded() const -> bool { return false; }
};

} // namespace apiexec
