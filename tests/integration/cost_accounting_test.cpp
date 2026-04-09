// T6.7: Cost accounting integration test.
// Verifies CostAwarePolicy halts execution at the correct token budget.

#include "c_api.h"
#include "core/engine.hpp"
#include "core/vendor_adapter.hpp"
#include "core/itransport.hpp"
#include "policy/cost_aware_policy.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <queue>

using namespace apiexec;

constexpr double TOKENS_PER_RESPONSE = 100.0;
constexpr double BUDGET_TOKENS = 1000.0;
constexpr int EXPECTED_FETCHES = 10;  // 1000 / 100 = 10

struct TestRecord { std::string data; };

class MockTransport : public ITransport {
public:
    auto execute(const Request&, std::atomic<bool>&) -> Response override {
        return Response{200, "data", {}};
    }
};

class CostAdapter : public VendorAdapter<TestRecord> {
public:
    int page = 0;

    auto build_request(const Cursor&) -> Request override { return {}; }
    auto parse_response(const Response& r, TestRecord& out) -> bool override {
        out.data = r.body; return true;
    }
    auto next_cursor(const Cursor& c, const Response&) -> Cursor override {
        Cursor n = c; ++page; n.page_token = std::to_string(page); return n;
    }
    auto is_retryable(const Response& r) -> bool override {
        return r.is_rate_limited() || r.is_server_error();
    }
    auto retry_after(const Response&) -> std::optional<int> override { return std::nullopt; }

    // Report fixed token cost per response
    auto response_cost(const Response&) const -> std::optional<double> override {
        return TOKENS_PER_RESPONSE;
    }
};

void test_budget_halts_at_correct_count() {
    DefaultPolicy::Config base_cfg;
    base_cfg.prefetch_depth_val = 0;

    CostAwarePolicy::CostConfig cost_cfg;
    cost_cfg.budget_tokens = BUDGET_TOKENS;
    cost_cfg.price_per_token = 0.00003;  // ~$0.03/1K tokens

    auto* policy_ptr = new CostAwarePolicy(base_cfg, cost_cfg);

    ExecutionEngine<TestRecord> engine(
        std::make_unique<CostAdapter>(),
        std::unique_ptr<ExecutionPolicy>(policy_ptr),
        std::make_unique<MockTransport>(),
        Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    int fetched = 0;
    StreamErrorCode last_error = StreamErrorCode::OK;
    while (engine.has_next()) {
        auto result = engine.next_batch();
        last_error = result.error;
        if (result.error != StreamErrorCode::OK) break;
        ++fetched;
    }

    // Should halt after exactly EXPECTED_FETCHES (budget / cost_per_response)
    assert(fetched == EXPECTED_FETCHES);
    assert(last_error == StreamErrorCode::BUDGET_EXHAUSTED);
    assert(policy_ptr->cumulative_tokens() == BUDGET_TOKENS);
    assert(!engine.has_next());  // stream_done_ should be set

    double expected_dollars = BUDGET_TOKENS * cost_cfg.price_per_token;
    double actual_dollars = policy_ptr->cumulative_dollars();
    assert(std::abs(actual_dollars - expected_dollars) < 1e-9);

    std::cout << "  PASS: budget_halts_at_correct_count"
              << " (fetched=" << fetched
              << " tokens=" << policy_ptr->cumulative_tokens()
              << " cost=$" << policy_ptr->cumulative_dollars() << ")\n";
}

void test_no_budget_runs_unlimited() {
    DefaultPolicy::Config base_cfg;
    base_cfg.prefetch_depth_val = 0;

    CostAwarePolicy::CostConfig cost_cfg;
    cost_cfg.budget_tokens = 0;  // no budget

    auto adapter = std::make_unique<CostAdapter>();
    adapter->page = 0;

    auto* policy_ptr = new CostAwarePolicy(base_cfg, cost_cfg);

    ExecutionEngine<TestRecord> engine(
        std::move(adapter),
        std::unique_ptr<ExecutionPolicy>(policy_ptr),
        std::make_unique<MockTransport>(),
        Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    constexpr int LIMIT = 50;
    int fetched = 0;
    while (engine.has_next() && fetched < LIMIT) {
        auto result = engine.next_batch();
        if (result.error != StreamErrorCode::OK) break;
        ++fetched;
    }

    assert(fetched == LIMIT);
    assert(!policy_ptr->budget_exceeded());
    std::cout << "  PASS: no_budget_runs_unlimited (fetched=" << fetched << ")\n";
}

void test_remaining_budget_decreases() {
    DefaultPolicy::Config base_cfg;
    base_cfg.prefetch_depth_val = 0;

    CostAwarePolicy::CostConfig cost_cfg;
    cost_cfg.budget_tokens = 500.0;

    auto* policy_ptr = new CostAwarePolicy(base_cfg, cost_cfg);

    ExecutionEngine<TestRecord> engine(
        std::make_unique<CostAdapter>(),
        std::unique_ptr<ExecutionPolicy>(policy_ptr),
        std::make_unique<MockTransport>(),
        Cursor{}
    );
    engine.set_sleep_fn([](Duration) {});

    // Fetch 3 batches (300 tokens consumed)
    for (int i = 0; i < 3; ++i) {
        engine.next_batch();
    }

    auto remaining = policy_ptr->remaining_budget();
    assert(remaining.has_value());
    assert(remaining.value() == 200.0);  // 500 - 300
    std::cout << "  PASS: remaining_budget_decreases (remaining=" << remaining.value() << ")\n";
}

auto main() -> int {
    std::cout << "cost_accounting_test:\n";
    test_budget_halts_at_correct_count();
    test_no_budget_runs_unlimited();
    test_remaining_budget_decreases();
    std::cout << "All cost accounting tests passed.\n";
    return 0;
}
