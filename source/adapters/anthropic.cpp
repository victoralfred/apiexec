#include "anthropic.hpp"
#include "registry.hpp"
#include "../core/limits.hpp"

#include <stdexcept>

namespace apiexec {

APIEXEC_REGISTER_ADAPTER(anthropic, [](const std::string& config_json) -> void* {
    try {
        auto cfg = AnthropicAdapter::from_json(config_json);
        return new AnthropicAdapter(cfg);
    } catch (...) {
        return nullptr;
    }
});

AnthropicAdapter::AnthropicAdapter(Config cfg) : config_(std::move(cfg)) {}

auto AnthropicAdapter::from_json(const std::string& config_json) -> Config {
    if (config_json.size() > MAX_CONFIG_JSON_SIZE) {
        throw std::invalid_argument("config_json exceeds 64KB limit");
    }

    auto j = nlohmann::json::parse(config_json);
    Config cfg;
    cfg.base_url   = j.value("base_url", cfg.base_url);
    cfg.api_key    = j.at("api_key").get<std::string>();
    cfg.model      = j.value("model", cfg.model);
    cfg.max_tokens = j.value("max_tokens", cfg.max_tokens);

    if (j.contains("prompts") && j["prompts"].is_array()) {
        for (const auto& p : j["prompts"]) {
            cfg.prompts.push_back(p.get<std::string>());
        }
    }

    return cfg;
}

auto AnthropicAdapter::build_request(const Cursor& cursor) -> Request {
    Request req;
    req.method = Request::Method::POST;
    req.url = config_.base_url;

    int prompt_idx = 0;
    if (!cursor.page_token.empty()) {
        try { prompt_idx = std::stoi(cursor.page_token); } catch (...) {}
    }

    std::string content;
    if (prompt_idx < static_cast<int>(config_.prompts.size())) {
        content = config_.prompts[prompt_idx];
    }

    nlohmann::json body;
    body["model"] = config_.model;
    body["max_tokens"] = config_.max_tokens;
    body["messages"] = nlohmann::json::array({
        {{"role", "user"}, {"content", content}}
    });

    req.body = body.dump();
    req.headers["x-api-key"] = config_.api_key;
    req.headers["anthropic-version"] = "2023-06-01";
    req.headers["Content-Type"] = "application/json";

    return req;
}

auto AnthropicAdapter::parse_response(const Response& resp, JsonBatch& out) -> bool {
    try {
        last_parsed_ = nlohmann::json::parse(resp.body);

        if (!last_parsed_.contains("content") || !last_parsed_["content"].is_array()) {
            return false;
        }

        out.records = last_parsed_["content"];
        out.count = out.records.size();

        // Extract token usage
        last_token_count_ = 0;
        if (last_parsed_.contains("usage")) {
            auto& usage = last_parsed_["usage"];
            std::size_t input = usage.value("input_tokens", 0);
            std::size_t output = usage.value("output_tokens", 0);
            last_token_count_ = input + output;
        }

        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

auto AnthropicAdapter::next_cursor(const Cursor& current, const Response& /*resp*/) -> Cursor {
    Cursor next = current;

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

    // Store token count in cursor extra for cost tracking
    if (last_token_count_ > 0) {
        next.set_extra(kCursorExtraTokenCount, std::to_string(last_token_count_));
    }

    return next;
}

auto AnthropicAdapter::is_retryable(const Response& resp) -> bool {
    return resp.is_rate_limited() || resp.is_server_error();
}

auto AnthropicAdapter::retry_after(const Response& resp) -> std::optional<int> {
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

auto AnthropicAdapter::prompt_chunk_for_cursor(const Cursor& cursor,
                                                const std::string& full_prompt) -> std::string {
    // Estimate max chars per chunk based on model context and chars-per-token ratio
    std::size_t max_chars = max_context_tokens() * ANTHROPIC_CHARS_PER_TOKEN_ESTIMATE;
    auto chunks = chunk_text(full_prompt, max_chars);

    int idx = 0;
    if (!cursor.page_token.empty()) {
        try { idx = std::stoi(cursor.page_token); } catch (...) {}
    }

    if (idx < static_cast<int>(chunks.size())) {
        return chunks[idx];
    }
    return "";
}

auto AnthropicAdapter::token_count(const Response& /*resp*/) -> std::size_t {
    return last_token_count_;
}

auto AnthropicAdapter::max_context_tokens() const -> std::size_t {
    return ANTHROPIC_DEFAULT_CONTEXT;
}

} // namespace apiexec
