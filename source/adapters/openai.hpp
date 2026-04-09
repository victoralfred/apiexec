#pragma once

#include "../core/vendor_adapter.hpp"
#include "generic_rest.hpp"  // for JsonBatch

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace apiexec {

// OpenAIAdapter — VendorAdapter strategy for OpenAI Chat Completions API.
//
// Supports batch completions (non-streaming). Cursor advances by prompt
// index for multi-prompt workloads. Reports token cost via response_cost().
// Returns JsonBatch (records = choices array, count = number of choices).
//
// Configuration JSON:
//   {
//     "base_url": "https://api.openai.com/v1/chat/completions",
//     "api_key": "<key>",
//     "model": "gpt-4",
//     "prompts": ["prompt1", "prompt2", ...],
//     "max_tokens": 1000,
//     "temperature": 0.7
//   }
class OpenAIAdapter : public VendorAdapter<JsonBatch> {
public:
    struct Config {
        std::string base_url = "https://api.openai.com/v1/chat/completions";
        std::string api_key;
        std::string model = "gpt-4";
        std::vector<std::string> prompts;
        int max_tokens = 1000;
        double temperature = 0.7;
    };

    explicit OpenAIAdapter(Config cfg);

    auto build_request(const Cursor& cursor) -> Request override;
    auto parse_response(const Response& resp, JsonBatch& out) -> bool override;
    auto next_cursor(const Cursor& current, const Response& resp) -> Cursor override;
    auto is_retryable(const Response& resp) -> bool override;
    auto retry_after(const Response& resp) -> std::optional<int> override;
    auto response_cost(const Response& resp) const -> std::optional<double> override;

    static auto from_json(const std::string& config_json) -> Config;

private:
    Config config_;
    int last_total_tokens_ = 0;
    nlohmann::json last_parsed_;
};

} // namespace apiexec
