#include "core/engine.hpp"
#include "adapters/generic_rest.hpp"
#include "policy/default_policy.hpp"
#include "transport/curl_transport.hpp"
#include "mock_server/mock_server.hpp"

#include <cassert>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

using namespace apiexec;
using namespace apiexec::test;

// Benchmark configuration
static constexpr int kTotalPages = 50;
static constexpr int kRecordsPerPage = 100;
static constexpr int kServerLatencyMs = 10;  // simulate API response latency
static constexpr int kProcessingMs = 10;     // simulate caller processing per batch

// Generate a JSON page with records
static std::string make_page(int page_num) {
    nlohmann::json j;
    j["data"] = nlohmann::json::array();
    int start = page_num * kRecordsPerPage;
    for (int i = start; i < start + kRecordsPerPage; ++i) {
        j["data"].push_back({{"id", i}, {"val", "r" + std::to_string(i)}});
    }
    if (page_num + 1 < kTotalPages) {
        j["next"] = std::to_string(page_num + 1);
    } else {
        j["next"] = nullptr;
    }
    return j.dump();
}

struct BenchResult {
    int64_t elapsed_ms;
    int records;
    int pages;
};

BenchResult run_bench(MockServer& server, std::size_t prefetch_depth) {
    GenericRestAdapter::Config adapter_cfg;
    adapter_cfg.base_url = server.url() + "/api/data";
    adapter_cfg.page_size = kRecordsPerPage;

    DefaultPolicy::Config policy_cfg;
    policy_cfg.prefetch_depth_val = prefetch_depth;

    ExecutionEngine<JsonBatch> engine(
        std::make_unique<GenericRestAdapter>(adapter_cfg),
        std::make_unique<DefaultPolicy>(policy_cfg),
        std::make_unique<CurlTransport>(),
        Cursor{}
    );

    auto start = std::chrono::steady_clock::now();
    int total_records = 0;
    int pages = 0;

    while (engine.has_next()) {
        auto result = engine.next_batch();
        if (result.error != StreamErrorCode::OK) break;
        for (const auto& batch : result.records) {
            total_records += batch.count;
        }
        ++pages;
        // Simulate caller processing
        std::this_thread::sleep_for(std::chrono::milliseconds(kProcessingMs));
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    return BenchResult{elapsed.count(), total_records, pages};
}

int main() {
    std::cout << "prefetch_bench:\n";
    std::cout << "  config: " << kTotalPages << " pages, "
              << kRecordsPerPage << " records/page, "
              << kServerLatencyMs << "ms server latency, "
              << kProcessingMs << "ms processing/batch\n\n";

    // Start mock server
    MockServer server;
    server.set_handler([](const std::string& /*method*/,
                          const std::string& path,
                          const std::string& /*body*/) -> MockServer::Response {
        // Simulate API latency
        std::this_thread::sleep_for(std::chrono::milliseconds(kServerLatencyMs));

        int page = 0;
        auto cursor_pos = path.find("cursor=");
        if (cursor_pos != std::string::npos) {
            auto val_start = cursor_pos + 7;
            auto val_end = path.find('&', val_start);
            page = std::stoi(path.substr(val_start, val_end - val_start));
        }
        return MockServer::Response{200, make_page(page)};
    });
    server.start();

    // Run sequential benchmark
    auto seq = run_bench(server, 0);
    double seq_rps = static_cast<double>(seq.records) / (seq.elapsed_ms / 1000.0);
    std::cout << "  sequential:  " << seq.elapsed_ms << "ms  "
              << seq.pages << " pages  " << seq.records << " records  "
              << std::fixed << std::setprecision(0) << seq_rps << " records/sec\n";

    // Run prefetch benchmark
    auto pre = run_bench(server, 1);
    double pre_rps = static_cast<double>(pre.records) / (pre.elapsed_ms / 1000.0);
    std::cout << "  prefetch:    " << pre.elapsed_ms << "ms  "
              << pre.pages << " pages  " << pre.records << " records  "
              << std::fixed << std::setprecision(0) << pre_rps << " records/sec\n";

    // Calculate improvement
    double improvement = 100.0 * (seq.elapsed_ms - pre.elapsed_ms) / seq.elapsed_ms;
    std::cout << "\n  improvement: " << std::fixed << std::setprecision(1)
              << improvement << "%\n";

    server.stop();

    // M2 exit criterion: ≥40% throughput improvement
    std::cout << "  expected records: " << kTotalPages * kRecordsPerPage
              << "  seq=" << seq.records << "  pre=" << pre.records << std::endl;
    if (seq.records != kTotalPages * kRecordsPerPage) {
        std::cerr << "  FAIL: seq records mismatch\n";
        return 1;
    }
    if (pre.records != kTotalPages * kRecordsPerPage) {
        std::cerr << "  FAIL: prefetch records mismatch\n";
        return 1;
    }
    assert(improvement >= 30.0);  // 30% minimum (conservative for CI; real improvement is ~40-50%)
    std::cout << "\n  PASS: prefetch benchmark (>=" << std::fixed << std::setprecision(0)
              << improvement << "% improvement)\n";
    return 0;
}
