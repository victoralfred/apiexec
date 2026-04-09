#pragma once

#include <cstddef>
#include <cstdint>

namespace apiexec {

// Shared constants used across layers.
constexpr int      MAX_RETRY_AFTER_SECS  = 3600;       // 1 hour cap on Retry-After
constexpr int      MS_PER_SECOND         = 1000;
constexpr size_t   MAX_CONFIG_JSON_SIZE  = 65536;       // 64 KB

} // namespace apiexec
