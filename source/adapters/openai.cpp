#include "openai.hpp"
#include "registry.hpp"
#include "../core/limits.hpp"

#include <stdexcept>

namespace apiexec {

constexpr int DEFAULT_MAX_TOKENS = 1000;
constexpr double DEFAULT_TEMPERATURE = 0.7;

// Self-register with the adapter registry.
APIEXEC_REGISTER_ADAPTER(openai, [](const std::string& config_json) -> void* {
    try {
        auto cfg = OpenAIAdapter::from_json(config_json);
        return new OpenAIAdapter(cfg);
    } catch (...) {
        return nullptr;
    }
});

OpenAIAdapter::OpenAIAdapter(Config cfg) : config_(std::move(cfg)) {}

auto OpenAIAdapter::from_json(const std::string& config_json) -> Config {
    if (config_json.size() > MAX_CONFIG_JSON_SIZE) {
        throw std::invalid_argument("config_json exceeds 64KB limit");
    }

    auto j = nlohmann::json::parse(config_json);
    Config cfg;
    cfg.base_url    = j.value("base_url", cfg.base_url);
    cfg.api_key     = j.at("api_key").get<std::string>();
    cfg.model       = j.value("model", cfg.model);
    cfg.max_tokens  = j.value("max_tokens", DEFAULT_MAX_TOKENS);
    cfg.temperature = j.value("temperature", DEFAULT_TEMPERATURE);

    if (j.contains("prompts") && j["prompts"].is_array()) {
        for (const auto& p : j["prompts"]) {
            cfg.prompts.push_back(p.get<std::string>());
        }
    }

    return cfg;
}

auto OpenAIAdapter::build_request(const Cursor& cursor) -> Request {
    Request req;
    req.method = Request::Method::POST;
    req.url = config_.base_url;

    // Determine which prompt to send based on cursor position
    int prompt_idx = 0;
    if (!cursor.page_token.empty()) {
        try { prompt_idx = std::stoi(cursor.page_token); } catch (...) {}
    }

    // Build the chat completion request body
    nlohmann::json body;
    body["model"] = config_.model;
    body["max_tokens"] = config_.max_tokens;
    body["temperature"] = config_.temperature;

    std::string content;
    if (prompt_idx < static_cast<int>(config_.prompts.size())) {
        content = config_.prompts[prompt_idx];
    } else {
        content = "";  // exhaustion will be detected in next_cursor
    }

    body["messages"] = nlohmann::json::array({
        {{"role", "user"}, {"content", content}}
    });

    req.body = body.dump();
    req.headers["Authorization"] = "Bearer " + config_.api_key;
    req.headers["Content-Type"] = "application/json";

    return req;
}

auto OpenAIAdapter::parse_response(const Response& resp, JsonBatch& out) -> bool {
    try {
        last_parsed_ = nlohmann::json::parse(resp.body);

        if (!last_parsed_.contains("choices") || !last_parsed_["choices"].is_array()) {
            return false;
        }

        out.records = last_parsed_["choices"];
        out.count = out.records.size();

        // Extract token usage for cost reporting
        last_total_tokens_ = 0;
        if (last_parsed_.contains("usage") && last_parsed_["usage"].contains("total_tokens")) {
            last_total_tokens_ = last_parsed_["usage"]["total_tokens"].get<int>();
        }

        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

auto OpenAIAdapter::next_cursor(const Cursor& current, const Response& /*resp*/) -> Cursor {
    Cursor next = current;

    // Advance to the next prompt index
    int prompt_idx = 0;
    if (!current.page_token.empty()) {
        try { prompt_idx = std::stoi(current.page_token); } catch (...) {}
    }
    ++prompt_idx;

    if (prompt_idx >= static_cast<int>(config_.prompts.size())) {
        next.exhausted = true;
        next.page_token.clear();
    } else {
        next.page_token = std::to_string(prompt_idx);
        next.exhausted = false;
    }

    return next;
}

auto OpenAIAdapter::is_retryable(const Response& resp) -> bool {
    return resp.is_rate_limited() || resp.is_server_error();
}

auto OpenAIAdapter::retry_after(const Response& resp) -> std::optional<int> {
    std::string val = resp.header("retry-after");
    if (val.empty()) return std::nullopt;

    try {
        int seconds = std::stoi(val);
        if (seconds <= 0 || seconds > MAX_RETRY_AFTER_SECS) return std::nullopt;
        return seconds;
    } catch (...) {
        return std::nullopt;
    }
}

auto OpenAIAdapter::response_cost(const Response& /*resp*/) const -> std::optional<double> {
    // Report token count as cost units — CostAwarePolicy can convert to dollars
    if (last_total_tokens_ > 0) {
        return static_cast<double>(last_total_tokens_);
    }
    return std::nullopt;
}

} // namespace apiexec
