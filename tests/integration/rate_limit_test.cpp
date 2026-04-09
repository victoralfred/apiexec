#include "core/engine.hpp"
#include "adapters/generic_rest.hpp"
#include "policy/default_policy.hpp"
#include "transport/curl_transport.hpp"
#include "mock_server/mock_server.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace apiexec;
using namespace apiexec::test;

void test_429_storm_window_recovery() {
    // Mock server issues 5 consecutive 429s with Retry-After: 0, then succeeds.
    // After recovery, engine serves 10 successful fetches.
    // Verify: window shrinks during storm and recovers to near-original after successes.

    int request_count = 0;
    int storm_count = 5;

    MockServer server;
    server.set_handler([&](const std::string& /*method*/,
                           const std::string& /*path*/,
                           const std::string& /*body*/) -> MockServer::Response {
        ++request_count;

        // First 5 requests: 429 storm
        if (request_count <= storm_count) {
            return MockServer::Response{
                429, R"({"error":"rate limited"})", "application/json",
                "Retry-After: 0\r\n"
            };
        }

        // Subsequent requests: success with pagination
        int page = request_count - storm_count - 1;
        nlohmann::json j;
        j["data"] = {{{"id", page}, {"value", "record"}}};
        if (page < 14) {
            j["next"] = std::to_string(page + 1);
        } else {
            j["next"] = nullptr;
        }
        return MockServer::Response{200, j.dump()};
    });
    server.start();

    GenericRestAdapter::Config adapter_cfg;
    adapter_cfg.base_url = server.url() + "/api/data";
    adapter_cfg.page_size = 10;

    DefaultPolicy::Config policy_cfg;
    policy_cfg.max_retries = 10;
    policy_cfg.window_grow_factor = 1.5;
    policy_cfg.window_shrink_factor = 0.5;
    policy_cfg.min_window_ms = 60'000;
    policy_cfg.max_window_ms = 86'400'000;
    policy_cfg.prefetch_depth_val = 0;  // sequential for deterministic testing

    Cursor cursor;
    cursor.time_window_start = 0;
    cursor.time_window_end = 3'600'000;  // 1 hour initial window
    int64_t initial_window = cursor.time_window_end - cursor.time_window_start;

    ExecutionEngine<JsonBatch> engine(
        std::make_unique<GenericRestAdapter>(adapter_cfg),
        std::make_unique<DefaultPolicy>(policy_cfg),
        std::make_unique<CurlTransport>(),
        cursor
    );
    engine.set_sleep_fn([](Duration) {});  // no real delays

    // Fetch pages — the storm happens on the first fetch attempt (retries internally)
    int success_count = 0;
    while (engine.has_next() && success_count < 15) {
        auto result = engine.next_batch();
        if (result.error == StreamErrorCode::EXHAUSTED) break;
        if (result.error != StreamErrorCode::OK) {
            std::cout << "  ERROR: unexpected error code " << static_cast<int>(result.error) << "\n";
            break;
        }
        ++success_count;
    }

    // Should have fetched all 15 pages
    assert(success_count == 15);

    // After the 429 storm (terminal failure adjust), and then 15 successful adjusts,
    // window should have grown back. Check it's within reasonable range.
    int64_t final_window = engine.cursor().time_window_end - engine.cursor().time_window_start;
    double ratio = static_cast<double>(final_window) / initial_window;

    std::cout << "  initial_window=" << initial_window
              << "ms  final_window=" << final_window
              << "ms  ratio=" << ratio << "\n";

    // After 1 shrink (0.5x) then 15 grows (1.5x each), window should be much larger.
    // We just verify it recovered past the initial value.
    assert(final_window > initial_window);

    server.stop();
    std::cout << "  PASS: 429_storm_window_recovery (requests=" << request_count
              << " successes=" << success_count << ")\n";
}

void test_429_storm_convergence() {
    // Unit test: verify that after shrink + N grows, window converges.
    // Start at 3600s, shrink once (0.5x → 1800s), then grow 10 times (1.5x each).
    // After 10 iterations: 1800 * 1.5^10 = 1800 * 57.66 ≈ 103,788s
    // Capped at max_window (86400s = 24h).

    DefaultPolicy::Config cfg;
    cfg.window_grow_factor = 1.5;
    cfg.window_shrink_factor = 0.5;
    cfg.min_window_ms = 60'000;
    cfg.max_window_ms = 86'400'000;
    DefaultPolicy policy(cfg);

    Cursor c;
    c.time_window_start = 0;
    c.time_window_end = 3'600'000;  // 1h

    // Shrink once (simulating a terminal 429 failure)
    policy.adjust(c, false);
    int64_t post_shrink = c.time_window_end;
    assert(post_shrink == 1'800'000);  // 1h * 0.5

    // Grow 10 times (simulating recovery)
    for (int i = 0; i < 10; ++i) {
        policy.adjust(c, true);
    }
    int64_t post_recovery = c.time_window_end;

    // Should have hit the max window cap
    assert(post_recovery == cfg.max_window_ms);
    std::cout << "  PASS: 429_storm_convergence (post_shrink=" << post_shrink
              << "ms post_recovery=" << post_recovery << "ms)\n";
}

int main() {
    std::cout << "rate_limit_test (integration):\n";
    test_429_storm_convergence();
    test_429_storm_window_recovery();
    std::cout << "All rate limit tests passed.\n";
    return 0;
}
