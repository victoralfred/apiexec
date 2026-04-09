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

    // Adjust cursor window: grow on success, shrink on failure.
    virtual void adjust(Cursor& cursor, bool success) = 0;

    // Compute backoff duration for the given retry attempt.
    virtual Duration backoff(int retry_count) = 0;

    // Number of pages to prefetch ahead (0 = sequential, 1 = double-buffer).
    virtual std::size_t prefetch_depth() = 0;

    // Maximum number of retries before giving up.
    virtual int max_retries() const = 0;
};

} // namespace apiexec
