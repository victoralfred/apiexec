#pragma once

#include "ai_adapter.hpp"
#include "generic_rest.hpp"  // for JsonBatch

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace apiexec {

constexpr std::size_t ANTHROPIC_DEFAULT_MAX_TOKENS = 4096;
constexpr std::size_t ANTHROPIC_DEFAULT_CONTEXT = 200000;
constexpr std::size_t ANTHROPIC_CHARS_PER_TOKEN_ESTIMATE = 4;

// AnthropicAdapter — AIAdapter for the Anthropic Messages API.
//
// Supports batch completions. Cursor advances by prompt index.
// Token counting is adapter-provided from response usage stats.
//
// Configuration JSON:
//   {
//     "base_url": "https://api.anthropic.com/v1/messages",
//     "api_key": "<key>",
//     "model": "claude-sonnet-4-20250514",
//     "prompts": ["prompt1", "prompt2", ...],
//     "max_tokens": 4096
//   }
class AnthropicAdapter : public AIAdapter<JsonBatch> {
public:
    struct Config {
        std::string base_url = "https://api.anthropic.com/v1/messages";
        std::string api_key;
        std::string model = "claude-sonnet-4-20250514";
        std::vector<std::string> prompts;
        std::size_t max_tokens = ANTHROPIC_DEFAULT_MAX_TOKENS;
    };

    explicit AnthropicAdapter(Config cfg);

    // VendorAdapter core
    auto build_request(const Cursor& cursor) -> Request override;
    auto parse_response(const Response& resp, JsonBatch& out) -> bool override;
    auto next_cursor(const Cursor& current, const Response& resp) -> Cursor override;
    auto is_retryable(const Response& resp) -> bool override;
    auto retry_after(const Response& resp) -> std::optional<int> override;

    // AIAdapter extensions
    auto prompt_chunk_for_cursor(const Cursor& cursor,
                                  const std::string& full_prompt) -> std::string override;
    auto token_count(const Response& resp) -> std::size_t override;
    auto max_context_tokens() const -> std::size_t override;

    static auto from_json(const std::string& config_json) -> Config;

private:
    Config config_;
    std::size_t last_token_count_ = 0;
    nlohmann::json last_parsed_;
};

} // namespace apiexec
