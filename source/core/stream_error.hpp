#pragma once

#include <cstdint>

namespace apiexec {

// Error codes matching the C API contract.
// Positive = informational, zero = success, negative = error.
enum class StreamErrorCode : int32_t {
    OK             =  0,
    EXHAUSTED      =  1,
    RATE_LIMIT     = -1,
    SERVER         = -2,
    CLIENT         = -3,
    PARSE          = -4,
    NETWORK        = -5,
    CANCELLED      = -6,
    INVALID_ARG    = -7,
    BUDGET_EXHAUSTED = -8,
};

} // namespace apiexec
