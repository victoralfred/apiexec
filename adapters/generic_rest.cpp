#include "generic_rest.hpp"

#include <stdexcept>

namespace apiexec {

constexpr size_t MAX_CONFIG_JSON_SIZE   = 65536;  // 64 KB
constexpr int    MAX_RETRY_AFTER_SECS   = 3600;   // 1 hour

GenericRestAdapter::GenericRestAdapter(Config cfg) : config_(std::move(cfg)) {}

auto GenericRestAdapter::from_json(const std::string& config_json) -> Config {
    if (config_json.size() > MAX_CONFIG_JSON_SIZE) {
        throw std::invalid_argument("config_json exceeds 64KB limit");
    }

    auto j = nlohmann::json::parse(config_json);
    Config cfg;
    cfg.base_url         = j.at("base_url").get<std::string>();
    cfg.auth_header      = j.value("auth_header", "");
    cfg.data_field       = j.value("data_field", "data");
    cfg.next_token_field = j.value("next_token_field", "next");
    cfg.page_param       = j.value("page_param", "cursor");
    cfg.page_size        = j.value("page_size", DEFAULT_PAGE_SIZE);
    return cfg;
}

auto GenericRestAdapter::build_request(const Cursor& cursor) -> Request {
    Request req;
    req.method = Request::Method::GET;

    // Build URL with page token if present
    std::string url = config_.base_url;
    std::string sep = (url.find('?') != std::string::npos) ? "&" : "?";

    if (!cursor.page_token.empty()) {
        // Percent-encode the token to prevent query string injection.
        char* escaped = curl_easy_escape(nullptr, cursor.page_token.c_str(),
                                          static_cast<int>(cursor.page_token.size()));
        if (!escaped) {
            // OOM or invalid input — return empty request to signal failure
            return Request{};
        }
        url += sep + config_.page_param + "=" + std::string(escaped);
        curl_free(escaped);
        sep = "&";
    }

    // Add page size
    url += sep + "limit=" + std::to_string(config_.page_size);

    req.url = url;

    // Auth header
    if (!config_.auth_header.empty()) {
        req.headers["Authorization"] = config_.auth_header;
    }
    req.headers["Accept"] = "application/json";

    return req;
}

auto GenericRestAdapter::parse_response(const Response& resp, JsonBatch& out) -> bool {
    try {
        last_parsed_ = nlohmann::json::parse(resp.body);

        if (!last_parsed_.contains(config_.data_field) ||
            !last_parsed_[config_.data_field].is_array()) {
            return false;
        }

        out.records = last_parsed_[config_.data_field];
        out.count = out.records.size();
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

auto GenericRestAdapter::next_cursor(const Cursor& current, const Response& /*resp*/) -> Cursor {
    Cursor next = current;

    if (last_parsed_.contains(config_.next_token_field) &&
        !last_parsed_[config_.next_token_field].is_null()) {
        next.page_token = last_parsed_[config_.next_token_field].get<std::string>();
        next.exhausted = false;
    } else {
        next.page_token.clear();
        next.exhausted = true;
    }

    if (last_parsed_.contains(config_.data_field) &&
        last_parsed_[config_.data_field].empty()) {
        next.exhausted = true;
    }

    return next;
}

auto GenericRestAdapter::is_retryable(const Response& resp) -> bool {
    return resp.is_rate_limited() || resp.is_server_error();
}

auto GenericRestAdapter::retry_after(const Response& resp) -> std::optional<int> {
    std::string val = resp.header("retry-after");
    if (val.empty()) {
        return std::nullopt;
    }

    try {
        int seconds = std::stoi(val);
        if (seconds <= 0 || seconds > MAX_RETRY_AFTER_SECS) return std::nullopt;
        return seconds;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace apiexec
