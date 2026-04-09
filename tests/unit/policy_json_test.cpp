#include "policy/default_policy.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

using namespace apiexec;

void test_from_json_defaults() {
    auto policy = DefaultPolicy::from_json("{}");
    assert(policy.max_retries() == 5);
    assert(policy.config().base_backoff_ms == 100);
    assert(policy.config().window_grow_factor == 1.5);
    assert(policy.config().window_shrink_factor == 0.5);
    assert(policy.config().min_window_ms == 60'000);
    assert(policy.config().max_window_ms == 86'400'000);
    assert(policy.prefetch_depth() == 1);
    std::cout << "  PASS: from_json_defaults\n";
}

void test_from_json_custom() {
    auto policy = DefaultPolicy::from_json(R"({
        "max_retries": 10,
        "base_backoff_ms": 200,
        "window_grow_factor": 2.0,
        "window_shrink_factor": 0.25,
        "min_window_ms": 30000,
        "max_window_ms": 43200000,
        "prefetch_depth": 0
    })");
    assert(policy.max_retries() == 10);
    assert(policy.config().base_backoff_ms == 200);
    assert(policy.config().window_grow_factor == 2.0);
    assert(policy.config().window_shrink_factor == 0.25);
    assert(policy.config().min_window_ms == 30'000);
    assert(policy.config().max_window_ms == 43'200'000);
    assert(policy.prefetch_depth() == 0);
    std::cout << "  PASS: from_json_custom\n";
}

void test_from_json_reflects_in_backoff() {
    auto policy = DefaultPolicy::from_json(R"({"base_backoff_ms": 500})");
    // backoff(0) = 500 * 2^0 = 500ms (±25% jitter → 375..625)
    auto d = policy.backoff(0);
    assert(d.count() >= 375 && d.count() <= 625);
    std::cout << "  PASS: from_json_reflects_in_backoff (delay=" << d.count() << "ms)\n";
}

void test_from_json_reflects_in_adjust() {
    auto policy = DefaultPolicy::from_json(R"({
        "window_grow_factor": 3.0,
        "max_window_ms": 86400000
    })");
    Cursor c;
    c.time_window_start = 0;
    c.time_window_end = 1'000'000;  // 1M ms
    policy.adjust(c, true);
    assert(c.time_window_end == 3'000'000);  // 1M * 3.0
    std::cout << "  PASS: from_json_reflects_in_adjust\n";
}

void test_from_json_rejects_unknown_keys() {
    bool caught = false;
    try {
        DefaultPolicy::from_json(R"({"max_retries": 5, "unknown_key": true})");
    } catch (const std::invalid_argument& e) {
        caught = true;
        assert(std::string(e.what()).find("unknown_key") != std::string::npos);
    }
    assert(caught);
    std::cout << "  PASS: from_json_rejects_unknown_keys\n";
}

void test_from_json_validates_ranges() {
    // max_retries out of range
    bool caught = false;
    try { DefaultPolicy::from_json(R"({"max_retries": -1})"); }
    catch (const std::invalid_argument&) { caught = true; }
    assert(caught);

    // window_grow_factor must be > 1.0
    caught = false;
    try { DefaultPolicy::from_json(R"({"window_grow_factor": 0.5})"); }
    catch (const std::invalid_argument&) { caught = true; }
    assert(caught);

    // window_shrink_factor must be in (0, 1)
    caught = false;
    try { DefaultPolicy::from_json(R"({"window_shrink_factor": 1.5})"); }
    catch (const std::invalid_argument&) { caught = true; }
    assert(caught);

    std::cout << "  PASS: from_json_validates_ranges\n";
}

int main() {
    std::cout << "policy_json_test:\n";
    test_from_json_defaults();
    test_from_json_custom();
    test_from_json_reflects_in_backoff();
    test_from_json_reflects_in_adjust();
    test_from_json_rejects_unknown_keys();
    test_from_json_validates_ranges();
    std::cout << "All policy JSON tests passed.\n";
    return 0;
}
