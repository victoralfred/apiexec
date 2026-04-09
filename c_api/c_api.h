/*
 * apiexec C API — ABI-stable public interface
 *
 * Contract:
 *   - No C++ types, no STL, no exceptions across this boundary
 *   - Structs are versioned; layout changes require a new _v{N} suffix
 *   - All pointer parameters are null-checked
 *   - Caller owns StreamHandle*; stream_destroy is the only deallocation path
 *   - Unrecognised adapter name returns NULL from stream_create
 *
 * ABI version: bump APIEXEC_ABI_VERSION when any function signature changes.
 * Callers must not call a function whose version they did not compile against.
 */

#ifndef APIEXEC_C_API_H
#define APIEXEC_C_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- ABI version --- */

#define APIEXEC_ABI_VERSION 1

/* --- Error codes (match StreamErrorCode in core/stream_error.hpp) --- */

#define STREAM_OK                  0
#define STREAM_EXHAUSTED           1
#define STREAM_ERROR_RATE_LIMIT   -1
#define STREAM_ERROR_SERVER       -2
#define STREAM_ERROR_CLIENT       -3
#define STREAM_ERROR_PARSE        -4
#define STREAM_ERROR_NETWORK      -5
#define STREAM_ERROR_CANCELLED    -6
#define STREAM_ERROR_INVALID_ARG  -7

/* --- Opaque handle --- */

typedef struct StreamHandle StreamHandle;

/* --- Lifecycle --- */

/*
 * Create a new stream for the named adapter with JSON configuration.
 *
 * adapter:     Adapter name (e.g., "generic_rest"). Must not be NULL.
 * config_json: JSON configuration string. Must not be NULL.
 * policy_json: JSON policy configuration string. NULL for defaults.
 *
 * Returns: opaque handle, or NULL on failure (unknown adapter, parse error).
 *          Caller must call stream_destroy() to free the handle.
 */
StreamHandle* stream_create(const char* adapter,
                            const char* config_json,
                            const char* policy_json);

/*
 * Destroy a stream handle and free all associated resources.
 * Safe to call with NULL (no-op).
 */
void stream_destroy(StreamHandle* handle);

/* --- Iteration --- */

/*
 * Check if more data is available.
 *
 * Returns: 1 if more data, 0 if exhausted,
 *          STREAM_ERROR_INVALID_ARG for NULL handle.
 */
int32_t stream_has_next(StreamHandle* handle);

/*
 * Fetch the next batch of raw JSON bytes into a caller-supplied buffer.
 *
 * handle:    Stream handle. Must not be NULL.
 * buf:       Caller-allocated output buffer. Must not be NULL.
 * buf_len:   Size of buf in bytes.
 * out_count: On success, set to the number of records in this batch.
 *            Must not be NULL.
 *
 * Returns: STREAM_OK on success (buf contains JSON array, out_count set),
 *          STREAM_EXHAUSTED if no more data,
 *          or a negative error code.
 *
 * If the JSON output exceeds buf_len, returns STREAM_ERROR_CLIENT and
 * sets *out_count to 0. The caller should retry with a larger buffer.
 */
int32_t stream_next_batch_v1(StreamHandle* handle,
                             void* buf,
                             int32_t buf_len,
                             int32_t* out_count);

/*
 * Fetch the next record's timestamp.
 *
 * handle:     Stream handle. Must not be NULL.
 * out_ts_ms:  On success, set to the record's epoch timestamp in milliseconds.
 *             Must not be NULL.
 *
 * Returns: STREAM_OK on success, STREAM_EXHAUSTED, or a negative error code.
 */
int32_t stream_next_v2_ts(StreamHandle* handle,
                          int64_t* out_ts_ms);

/*
 * Fetch the next record as a string/JSON into a caller-supplied buffer.
 *
 * handle:   Stream handle. Must not be NULL.
 * out_buf:  Caller-allocated output buffer. Must not be NULL.
 * buf_len:  Size of out_buf in bytes.
 *
 * Returns: STREAM_OK on success (out_buf contains null-terminated JSON string),
 *          STREAM_EXHAUSTED, or a negative error code.
 *          If output exceeds buf_len, returns STREAM_ERROR_CLIENT.
 */
int32_t stream_next_v2_sc(StreamHandle* handle,
                          char* out_buf,
                          int32_t buf_len);

/* --- Cancellation --- */

/*
 * Cancel an in-progress stream. Safe to call from any thread.
 * Subsequent calls to stream_has_next/stream_next_* will return
 * STREAM_ERROR_CANCELLED. Safe to call with NULL (no-op).
 */
void stream_cancel(StreamHandle* handle);

#ifdef __cplusplus
}
#endif

#endif /* APIEXEC_C_API_H */
