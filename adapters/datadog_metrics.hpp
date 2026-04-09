#pragma once

#include "../core/vendor_adapter.hpp"
#include "generic_rest.hpp"  // for JsonBatch

#include <nlohmann/json.hpp>
#include <string>

namespace apiexec {

// DatadogMetricsAdapter — VendorAdapter strategy for the Datadog Metrics API.
//
// Uses time-window cursor advancement: each request queries a time range,
// and the cursor advances by shifting the window forward.
// Returns JsonBatch (records = series array, count = total data points).
//
// Configuration JSON:
//   {
//     "base_url": "https://api.datadoghq.com/api/v1/query",
//     "api_key": "<key>",
//     "app_key": "<key>",
//     "query": "avg:system.cpu.user{*}",
//     "window_ms": 3600000
//   }
class DatadogMetricsAdapter : public VendorAdapter<JsonBatch> {
public:
    struct Config {
        std::string base_url = "https://api.datadoghq.com/api/v1/query";
        std::string api_key;
        std::string app_key;
        std::string query;
        int64_t window_ms = 3'600'000;  // 1 hour default
    };

    explicit DatadogMetricsAdapter(Config cfg);

    auto build_request(const Cursor& cursor) -> Request override;
    auto parse_response(const Response& resp, JsonBatch& out) -> bool override;
    auto next_cursor(const Cursor& current, const Response& resp) -> Cursor override;
    auto is_retryable(const Response& resp) -> bool override;
    auto retry_after(const Response& resp) -> std::optional<int> override;

    static auto from_json(const std::string& config_json) -> Config;

private:
    Config config_;
    nlohmann::json last_parsed_;
};

} // namespace apiexec
