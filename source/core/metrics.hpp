#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace apiexec {

// Metrics snapshot — immutable point-in-time view of engine counters.
// Passed to the metrics callback for external consumption (Prometheus, etc.).
struct MetricsSnapshot {
    int64_t request_count = 0;       // Total HTTP requests made
    int64_t retry_count = 0;         // Total retries (429 + 5xx + network)
    int64_t success_count = 0;       // Successful fetches
    int64_t error_rate_limit = 0;    // Terminal 429 errors
    int64_t error_server = 0;        // Terminal 5xx errors
    int64_t error_client = 0;        // Terminal 4xx errors
    int64_t error_network = 0;       // Terminal network errors
    int64_t error_parse = 0;         // Parse failures
    int64_t records_total = 0;       // Total records delivered to caller
    double  window_size_ms = 0.0;    // Current cursor window size in ms
    double  cumulative_cost = 0.0;   // Cumulative cost units (from adapter)
};

// Callback type: receives a snapshot whenever the engine emits metrics.
using MetricsCallback = std::function<void(const MetricsSnapshot&)>;

// Metrics collector — thread-safe atomic counters.
// Owned by ExecutionEngine, updated on every fetch cycle.
class Metrics {
public:
    auto inc_request() -> void { request_count_.fetch_add(1, std::memory_order_relaxed); }
    auto inc_retry() -> void { retry_count_.fetch_add(1, std::memory_order_relaxed); }
    auto inc_success() -> void { success_count_.fetch_add(1, std::memory_order_relaxed); }
    auto inc_error_rate_limit() -> void { error_rate_limit_.fetch_add(1, std::memory_order_relaxed); }
    auto inc_error_server() -> void { error_server_.fetch_add(1, std::memory_order_relaxed); }
    auto inc_error_client() -> void { error_client_.fetch_add(1, std::memory_order_relaxed); }
    auto inc_error_network() -> void { error_network_.fetch_add(1, std::memory_order_relaxed); }
    auto inc_error_parse() -> void { error_parse_.fetch_add(1, std::memory_order_relaxed); }
    auto add_records(int64_t n) -> void { records_total_.fetch_add(n, std::memory_order_relaxed); }

    auto set_window_size_ms(double ms) -> void { window_size_ms_.store(ms, std::memory_order_relaxed); }
    auto set_cumulative_cost(double c) -> void { cumulative_cost_.store(c, std::memory_order_relaxed); }

    auto snapshot() const -> MetricsSnapshot {
        MetricsSnapshot s;
        s.request_count    = request_count_.load(std::memory_order_relaxed);
        s.retry_count      = retry_count_.load(std::memory_order_relaxed);
        s.success_count    = success_count_.load(std::memory_order_relaxed);
        s.error_rate_limit = error_rate_limit_.load(std::memory_order_relaxed);
        s.error_server     = error_server_.load(std::memory_order_relaxed);
        s.error_client     = error_client_.load(std::memory_order_relaxed);
        s.error_network    = error_network_.load(std::memory_order_relaxed);
        s.error_parse      = error_parse_.load(std::memory_order_relaxed);
        s.records_total    = records_total_.load(std::memory_order_relaxed);
        s.window_size_ms   = window_size_ms_.load(std::memory_order_relaxed);
        s.cumulative_cost  = cumulative_cost_.load(std::memory_order_relaxed);
        return s;
    }

    // Format as Prometheus text exposition format.
    auto to_prometheus(const std::string& prefix = "apiexec") const -> std::string {
        auto s = snapshot();
        std::string out;
        auto line = [&](const char* name, int64_t val) {
            out += prefix + "_" + name + " " + std::to_string(val) + "\n";
        };
        auto fline = [&](const char* name, double val) {
            out += prefix + "_" + name + " " + std::to_string(val) + "\n";
        };
        line("requests_total", s.request_count);
        line("retries_total", s.retry_count);
        line("successes_total", s.success_count);
        line("errors_rate_limit_total", s.error_rate_limit);
        line("errors_server_total", s.error_server);
        line("errors_client_total", s.error_client);
        line("errors_network_total", s.error_network);
        line("errors_parse_total", s.error_parse);
        line("records_total", s.records_total);
        fline("window_size_ms", s.window_size_ms);
        fline("cumulative_cost_units", s.cumulative_cost);
        return out;
    }

private:
    std::atomic<int64_t> request_count_{0};
    std::atomic<int64_t> retry_count_{0};
    std::atomic<int64_t> success_count_{0};
    std::atomic<int64_t> error_rate_limit_{0};
    std::atomic<int64_t> error_server_{0};
    std::atomic<int64_t> error_client_{0};
    std::atomic<int64_t> error_network_{0};
    std::atomic<int64_t> error_parse_{0};
    std::atomic<int64_t> records_total_{0};
    std::atomic<double>  window_size_ms_{0.0};
    std::atomic<double>  cumulative_cost_{0.0};
};

} // namespace apiexec
