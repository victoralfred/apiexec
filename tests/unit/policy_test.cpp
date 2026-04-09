#include "policy/default_policy.hpp"

#include <cassert>
#include <iostream>

using namespace apiexec;

void test_backoff_base() {
    DefaultPolicy::Config cfg;
    cfg.base_backoff_ms = 100;
    DefaultPolicy policy(cfg);
    auto d = policy.backoff(0);
    assert(d.count() >= 75 && d.count() <= 125);
    std::cout << "  PASS: backoff_base (delay=" << d.count() << "ms)\n";
}

void test_backoff_exponential() {
    DefaultPolicy::Config cfg;
    cfg.base_backoff_ms = 100;
    DefaultPolicy policy(cfg);
    auto d = policy.backoff(5);
    assert(d.count() >= 2400 && d.count() <= 4000);
    std::cout << "  PASS: backoff_exponential (delay=" << d.count() << "ms)\n";
}

void test_backoff_cap() {
    DefaultPolicy::Config cfg;
    cfg.base_backoff_ms = 100;
    DefaultPolicy policy(cfg);
    auto d = policy.backoff(10);
    assert(d.count() <= 40000);
    std::cout << "  PASS: backoff_cap (delay=" << d.count() << "ms)\n";
}

void test_adjust_grow_on_success() {
    DefaultPolicy::Config cfg;
    cfg.window_grow_factor = 1.5;
    cfg.max_window_ms = 86'400'000;
    DefaultPolicy policy(cfg);
    Cursor c;
    c.time_window_start = 0;
    c.time_window_end = 3'600'000;
    policy.adjust(c, true);
    assert(c.time_window_end == 5'400'000);
    std::cout << "  PASS: adjust_grow_on_success\n";
}

void test_adjust_shrink_on_failure() {
    DefaultPolicy::Config cfg;
    cfg.window_shrink_factor = 0.5;
    cfg.min_window_ms = 60'000;
    DefaultPolicy policy(cfg);
    Cursor c;
    c.time_window_start = 0;
    c.time_window_end = 3'600'000;
    policy.adjust(c, false);
    assert(c.time_window_end == 1'800'000);
    std::cout << "  PASS: adjust_shrink_on_failure\n";
}

void test_adjust_respects_min_window() {
    DefaultPolicy::Config cfg;
    cfg.window_shrink_factor = 0.5;
    cfg.min_window_ms = 60'000;
    DefaultPolicy policy(cfg);
    Cursor c;
    c.time_window_start = 0;
    c.time_window_end = 60'000;
    policy.adjust(c, false);
    assert(c.time_window_end == 60'000);
    std::cout << "  PASS: adjust_respects_min_window\n";
}

void test_adjust_respects_max_window() {
    DefaultPolicy::Config cfg;
    cfg.window_grow_factor = 2.0;
    cfg.max_window_ms = 86'400'000;
    DefaultPolicy policy(cfg);
    Cursor c;
    c.time_window_start = 0;
    c.time_window_end = 50'000'000;
    policy.adjust(c, true);
    assert(c.time_window_end == 86'400'000);
    std::cout << "  PASS: adjust_respects_max_window\n";
}

void test_adjust_no_time_window() {
    DefaultPolicy policy;
    Cursor c;
    policy.adjust(c, true);
    assert(c.time_window_end == 0);
    std::cout << "  PASS: adjust_no_time_window\n";
}

void test_prefetch_depth_default() {
    DefaultPolicy policy;
    assert(policy.prefetch_depth() == 1);  // M2: double-buffer is the default
    std::cout << "  PASS: prefetch_depth_default\n";
}

void test_prefetch_depth_sequential() {
    DefaultPolicy::Config cfg;
    cfg.prefetch_depth_val = 0;
    DefaultPolicy policy(cfg);
    assert(policy.prefetch_depth() == 0);
    std::cout << "  PASS: prefetch_depth_sequential\n";
}

int main() {
    std::cout << "policy_test:\n";
    test_backoff_base();
    test_backoff_exponential();
    test_backoff_cap();
    test_adjust_grow_on_success();
    test_adjust_shrink_on_failure();
    test_adjust_respects_min_window();
    test_adjust_respects_max_window();
    test_adjust_no_time_window();
    test_prefetch_depth_default();
    test_prefetch_depth_sequential();
    std::cout << "All policy tests passed.\n";
    return 0;
}
