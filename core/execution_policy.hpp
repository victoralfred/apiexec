#pragma once

#include "cursor.hpp"

#include <chrono>
#include <cstddef>

namespace apiexec {

using Duration = std::chrono::milliseconds;

// ExecutionPolicy — Strategy interface for runtime behaviour decisions.
//
// Controls backoff timing, adaptive window sizing, prefetch depth,
// and retry limits. Injected into ExecutionEngine<T> at construction.
struct ExecutionPolicy {
    virtual ~ExecutionPolicy() = default;

    virtual auto adjust(Cursor& cursor, bool success) -> void = 0;
    virtual auto backoff(int retry_count) -> Duration = 0;
    virtual auto prefetch_depth() -> std::size_t = 0;
    virtual auto max_retries() const -> int = 0;
};

} // namespace apiexec
