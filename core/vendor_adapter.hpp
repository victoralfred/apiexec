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

    virtual auto build_request(const Cursor& cursor) -> Request = 0;
    virtual auto parse_response(const Response& resp, T& out) -> bool = 0;
    virtual auto next_cursor(const Cursor& current, const Response& resp) -> Cursor = 0;
    virtual auto is_retryable(const Response& resp) -> bool = 0;
    virtual auto retry_after(const Response& resp) -> std::optional<int> = 0;
};

} // namespace apiexec
