#pragma once

#include "cursor.hpp"
#include "types.hpp"

#include <optional>

namespace apiexec {

// VendorAdapter<T> — Strategy interface for API-specific behaviour.
//
// Every vendor adapter must implement all five methods.
// parse_response returns false on parse failure (fail-fast, no retry).
// is_retryable governs 429/5xx vs 4xx behaviour.
template <typename T>
struct VendorAdapter {
    virtual ~VendorAdapter() = default;

    // Build the HTTP request for the current cursor position.
    virtual Request build_request(const Cursor& cursor) = 0;

    // Parse the HTTP response into output T. Return false on parse failure.
    virtual bool parse_response(const Response& resp, T& out) = 0;

    // Compute the next cursor position given current cursor and response.
    virtual Cursor next_cursor(const Cursor& current, const Response& resp) = 0;

    // Should this response be retried? (true for 429, 5xx)
    virtual bool is_retryable(const Response& resp) = 0;

    // If the server sent a Retry-After hint, return the delay in seconds.
    virtual std::optional<int> retry_after(const Response& resp) = 0;
};

} // namespace apiexec
