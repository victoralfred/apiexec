#pragma once

#include "cursor.hpp"
#include "types.hpp"

#include <functional>
#include <optional>

namespace apiexec {

// VendorAdapter<T> — Strategy interface for API-specific behaviour.
//
// Every vendor adapter must implement the 5 core methods.
// parse_response returns false on parse failure (fail-fast, no retry).
// is_retryable governs 429/5xx vs 4xx behaviour.
template <typename T>
struct VendorAdapter {
    virtual ~VendorAdapter() = default;

    // --- Core (pure virtual) ---
    virtual auto build_request(const Cursor& cursor) -> Request = 0;
    virtual auto parse_response(const Response& resp, T& out) -> bool = 0;
    virtual auto next_cursor(const Cursor& current, const Response& resp) -> Cursor = 0;
    virtual auto is_retryable(const Response& resp) -> bool = 0;
    virtual auto retry_after(const Response& resp) -> std::optional<int> = 0;

    // --- Cost hook (defaulted — non-breaking for existing adapters) ---

    // Estimate the cost of the last response (e.g., token count for AI APIs,
    // record count for REST APIs). Returns nullopt if the adapter does not
    // track costs. AI adapters will override to return token counts or dollar costs.
    virtual auto response_cost(const Response& /*resp*/) const -> std::optional<double> {
        return std::nullopt;
    }

    // --- Streaming hooks (defaulted — non-breaking) ---

    // Streaming parse: calls record_cb for each record as it is parsed.
    // Default: delegates to parse_response and delivers the batch as one call.
    // SSE/AI adapters override to deliver tokens or events incrementally.
    using ChunkCallback = std::function<bool(const T& partial)>;

    virtual auto parse_streaming(const Response& resp, ChunkCallback record_cb) -> bool {
        T out;
        if (!parse_response(resp, out)) return false;
        return record_cb(out);
    }

    // Does this adapter support streaming/SSE responses?
    // Default: false. AI adapters override to return true.
    virtual auto supports_streaming() const -> bool { return false; }
};

} // namespace apiexec
