#pragma once

#include "../core/vendor_adapter.hpp"
#include "generic_rest.hpp"  // for JsonBatch

#include <string>
#include <vector>

namespace apiexec {

// Token counting is adapter-provided (OQ3 resolved). Each AI API uses a
// different tokeniser; the adapter is the correct owner of counting logic.
// No vendored tiktoken — each adapter implements its own estimation.

// AIAdapter<T> — extension of VendorAdapter for AI API patterns.
//
// Adds:
//   - prompt_chunk_for_cursor: split large prompts into model-sized chunks
//   - token_count: estimate tokens consumed by a response
//   - max_context_tokens: the model's context window size
//
// Concrete implementations: OpenAIAdapter (existing), AnthropicAdapter (new).
template <typename T>
struct AIAdapter : public VendorAdapter<T> {
    // Split input into a chunk suitable for the current cursor position.
    // Returns the prompt text for this chunk. The cursor's page_token
    // tracks the chunk index (0, 1, 2...).
    virtual auto prompt_chunk_for_cursor(const Cursor& cursor,
                                          const std::string& full_prompt) -> std::string = 0;

    // Estimate the number of tokens in the response.
    // Adapter-provided: each AI API returns usage stats differently.
    virtual auto token_count(const Response& resp) -> std::size_t = 0;

    // Maximum context window for this model (tokens).
    virtual auto max_context_tokens() const -> std::size_t = 0;

    // Default response_cost delegates to token_count.
    auto response_cost(const Response& resp) const -> std::optional<double> override {
        auto count = const_cast<AIAdapter*>(this)->token_count(resp);
        return count > 0 ? std::optional<double>(static_cast<double>(count))
                         : std::nullopt;
    }
};

// Utility: split text into chunks of approximately max_tokens size.
// Splits at sentence boundaries ('. ') when possible, otherwise at word boundaries.
inline auto chunk_text(const std::string& text, std::size_t max_chars) -> std::vector<std::string> {
    std::vector<std::string> chunks;
    if (text.empty() || max_chars == 0) return chunks;

    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = std::min(pos + max_chars, text.size());

        if (end < text.size()) {
            // Try to find a sentence boundary
            auto sentence_break = text.rfind(". ", end);
            if (sentence_break != std::string::npos && sentence_break > pos) {
                end = sentence_break + 2;  // include ". "
            } else {
                // Fall back to word boundary
                auto space = text.rfind(' ', end);
                if (space != std::string::npos && space > pos) {
                    end = space + 1;
                }
            }
        }

        chunks.push_back(text.substr(pos, end - pos));
        pos = end;
    }

    return chunks;
}

} // namespace apiexec
