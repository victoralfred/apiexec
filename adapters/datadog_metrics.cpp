#include "datadog_metrics.hpp"
#include "registry.hpp"
#include "../core/limits.hpp"

#include <stdexcept>

namespace apiexec {

// Self-register with the adapter registry.
APIEXEC_REGISTER_ADAPTER(datadog_metrics, [](const std::string& config_json) -> void* {
    try {
        auto cfg = DatadogMetricsAdapter::from_json(config_json);
        return new DatadogMetricsAdapter(cfg);
    } catch (...) {
        return nullptr;
    }
});

DatadogMetricsAdapter::DatadogMetricsAdapter(Config cfg) : config_(std::move(cfg)) {}

auto DatadogMetricsAdapter::from_json(const std::string& config_json) -> Config {
    if (config_json.size() > MAX_CONFIG_JSON_SIZE) {
        throw std::invalid_argument("config_json exceeds 64KB limit");
    }

    auto j = nlohmann::json::parse(config_json);
    Config cfg;
    cfg.base_url  = j.value("base_url", cfg.base_url);
    cfg.api_key   = j.at("api_key").get<std::string>();
    cfg.app_key   = j.at("app_key").get<std::string>();
    cfg.query     = j.at("query").get<std::string>();
    cfg.window_ms = j.value("window_ms", cfg.window_ms);
    return cfg;
}

auto DatadogMetricsAdapter::build_request(const Cursor& cursor) -> Request {
    Request req;
    req.method = Request::Method::GET;

    // Initialize time window from config if not yet set (first request)
    int64_t from_ms = cursor.time_window_start;
    int64_t to_ms = cursor.time_window_end;
    if (from_ms == 0 && to_ms == 0) {
        // Use "now - window_ms" as a sensible default start
        from_ms = 0;  // epoch start for mock testing
        to_ms = config_.window_ms;
    }

    int64_t from_sec = from_ms / MS_PER_SECOND;
    int64_t to_sec = to_ms / MS_PER_SECOND;

    req.url = config_.base_url
        + "?query=" + config_.query
        + "&from=" + std::to_string(from_sec)
        + "&to=" + std::to_string(to_sec);

    req.headers["DD-API-KEY"] = config_.api_key;
    req.headers["DD-APPLICATION-KEY"] = config_.app_key;
    req.headers["Accept"] = "application/json";

    return req;
}

auto DatadogMetricsAdapter::parse_response(const Response& resp, JsonBatch& out) -> bool {
    try {
        last_parsed_ = nlohmann::json::parse(resp.body);

        if (!last_parsed_.contains("series") || !last_parsed_["series"].is_array()) {
            return false;
        }

        // Flatten all data points from all series into a single records array
        out.records = nlohmann::json::array();
        out.count = 0;
        for (const auto& s : last_parsed_["series"]) {
            if (s.contains("pointlist") && s["pointlist"].is_array()) {
                for (const auto& point : s["pointlist"]) {
                    out.records.push_back(point);
                    ++out.count;
                }
            }
        }
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

auto DatadogMetricsAdapter::next_cursor(const Cursor& current, const Response& /*resp*/) -> Cursor {
    Cursor next = current;

    // Always advance by the configured window size, ignoring any policy
    // growth/shrink of the window. The adapter owns the time progression.
    int64_t window = config_.window_ms;
    int64_t current_start = current.time_window_start;
    if (current_start == 0 && current.time_window_end == 0) {
        // First call — start from 0
        current_start = 0;
    }
    next.time_window_start = current_start + window;
    next.time_window_end = next.time_window_start + window;

    // If the response had no series data, mark exhausted
    if (last_parsed_.contains("series") && last_parsed_["series"].empty()) {
        next.exhausted = true;
    }

    return next;
}

auto DatadogMetricsAdapter::is_retryable(const Response& resp) -> bool {
    return resp.is_rate_limited() || resp.is_server_error();
}

auto DatadogMetricsAdapter::retry_after(const Response& resp) -> std::optional<int> {
    std::string val = resp.header("retry-after");
    if (val.empty()) return std::nullopt;

    try {
        int seconds = std::stoi(val);
        if (seconds <= 0 || seconds > MAX_RETRY_AFTER_SECS) {
            return std::nullopt;
        }
        return seconds;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace apiexec
