#include "c_api.h"

#include "../adapters/generic_rest.hpp"
#include "../adapters/registry.hpp"
#include "../core/engine.hpp"
#include "../core/limits.hpp"
#include "../policy/default_policy.hpp"
#include "../transport/curl_transport.hpp"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace apiexec;

// --- Type-erased handle wrapping ExecutionEngine<JsonBatch> ---

struct StreamHandle {
    std::unique_ptr<ExecutionEngine<JsonBatch>> engine;

    // Cached result from the last fetch — shared across v1/v2_ts/v2_sc/foreach.
    // Populated by ensure_fetched(). Cleared after consumption or on next fetch.
    bool has_cached_result = false;
    std::string cached_json;                      // full batch as JSON array
    std::vector<std::string> cached_record_jsons; // per-record JSON strings
    int32_t cached_count = 0;
    int64_t cached_ts_ms = 0;
};

// --- Helpers ---

static auto is_null_handle(StreamHandle* h) -> bool {
    return h == nullptr || h->engine == nullptr;
}

// Fetch the next batch if no cached result is available.
// Returns STREAM_OK if a cached result is ready, or an error code.
static auto ensure_fetched(StreamHandle* h) -> int32_t {
    if (h->has_cached_result) {
        return STREAM_OK;
    }

    auto result = h->engine->next_batch();
    if (result.error != StreamErrorCode::OK) {
        return static_cast<int32_t>(result.error);
    }

    // Serialise records to JSON — both as a full array and per-record strings
    nlohmann::json output = nlohmann::json::array();
    h->cached_record_jsons.clear();
    int32_t total_records = 0;
    for (const auto& batch : result.records) {
        for (const auto& record : batch.records) {
            output.push_back(record);
            h->cached_record_jsons.push_back(record.dump());
            ++total_records;
        }
    }

    h->cached_json = output.dump();
    h->cached_count = total_records;
    h->cached_ts_ms = h->engine->cursor().time_window_start;
    h->has_cached_result = true;

    return STREAM_OK;
}

// Mark cached result as consumed so the next call fetches a new batch.
static auto consume_cache(StreamHandle* h) -> void {
    h->has_cached_result = false;
    h->cached_json.clear();
    h->cached_record_jsons.clear();
    h->cached_count = 0;
    h->cached_ts_ms = 0;
}

// --- Lifecycle ---

extern "C" auto stream_create(const char* adapter,
                               const char* config_json,
                               const char* policy_json) -> StreamHandle* {
    if (adapter == nullptr || config_json == nullptr) {
        return nullptr;
    }

    // Length caps at the C boundary before constructing std::string
    constexpr size_t MAX_ADAPTER_NAME_LEN = 64;
    if (std::strlen(adapter) > MAX_ADAPTER_NAME_LEN) {
        return nullptr;
    }
    if (std::strlen(config_json) > MAX_CONFIG_JSON_SIZE) {
        return nullptr;
    }

    std::string adapter_name(adapter);
    std::string config(config_json);

    try {
        // Dispatch through the adapter registry
        auto factory = Registry::instance().find(adapter_name);
        if (!factory) {
            return nullptr;
        }

        auto* raw_adapter = factory(config);
        if (!raw_adapter) {
            return nullptr;
        }

        // The factory returns a VendorAdapter<JsonBatch>* as void*.
        // Transfer ownership to a unique_ptr.
        auto adapter_ptr = std::unique_ptr<VendorAdapter<JsonBatch>>(
            static_cast<VendorAdapter<JsonBatch>*>(raw_adapter));

        std::unique_ptr<DefaultPolicy> policy;
        if (policy_json != nullptr && std::strlen(policy_json) > 0) {
            if (std::strlen(policy_json) > MAX_CONFIG_JSON_SIZE) {
                return nullptr;
            }
            policy = std::make_unique<DefaultPolicy>(
                DefaultPolicy::from_json(std::string(policy_json)));
        } else {
            policy = std::make_unique<DefaultPolicy>();
        }

        auto transport = std::make_unique<CurlTransport>();
        Cursor cursor;

        auto engine = std::make_unique<ExecutionEngine<JsonBatch>>(
            std::move(adapter_ptr),
            std::move(policy),
            std::move(transport),
            cursor
        );

        auto* handle = new StreamHandle();
        handle->engine = std::move(engine);
        return handle;

    } catch (...) {
        return nullptr;
    }
}

extern "C" auto stream_destroy(StreamHandle* handle) -> void {
    delete handle;
}

// --- Iteration ---

extern "C" auto stream_has_next(StreamHandle* handle) -> int32_t {
    if (is_null_handle(handle)) {
        return STREAM_ERROR_INVALID_ARG;
    }
    // If there's a cached result, there's data to read
    if (handle->has_cached_result) return 1;
    return handle->engine->has_next() ? 1 : 0;
}

extern "C" auto stream_next_batch_v1(StreamHandle* handle,
                                     void* buf,
                                     int32_t buf_len,
                                     int32_t* out_count) -> int32_t {
    if (is_null_handle(handle) || buf == nullptr || out_count == nullptr) {
        return STREAM_ERROR_INVALID_ARG;
    }
    if (buf_len <= 0) {
        return STREAM_ERROR_INVALID_ARG;
    }

    *out_count = 0;

    int32_t rc = ensure_fetched(handle);
    if (rc != STREAM_OK) return rc;

    // Check buffer size (need size+1 for null terminator)
    if (static_cast<int32_t>(handle->cached_json.size() + 1) > buf_len) {
        return STREAM_ERROR_CLIENT;  // buffer too small; cache preserved for retry
    }

    std::memcpy(buf, handle->cached_json.data(), handle->cached_json.size());
    static_cast<char*>(buf)[handle->cached_json.size()] = '\0';
    *out_count = handle->cached_count;

    consume_cache(handle);
    return STREAM_OK;
}

extern "C" auto stream_next_v2_ts(StreamHandle* handle,
                                  int64_t* out_ts_ms) -> int32_t {
    if (is_null_handle(handle) || out_ts_ms == nullptr) {
        return STREAM_ERROR_INVALID_ARG;
    }

    int32_t rc = ensure_fetched(handle);
    if (rc != STREAM_OK) return rc;

    // Returns cursor time_window_start (0 for cursor-based adapters without time windows)
    *out_ts_ms = handle->cached_ts_ms;

    // Do NOT consume cache — allow a follow-up stream_next_v2_sc to read the same batch
    return STREAM_OK;
}

extern "C" auto stream_next_v2_sc(StreamHandle* handle,
                                  char* out_buf,
                                  int32_t buf_len) -> int32_t {
    if (is_null_handle(handle) || out_buf == nullptr) {
        return STREAM_ERROR_INVALID_ARG;
    }
    if (buf_len <= 0) {
        return STREAM_ERROR_INVALID_ARG;
    }

    int32_t rc = ensure_fetched(handle);
    if (rc != STREAM_OK) return rc;

    if (static_cast<int32_t>(handle->cached_json.size() + 1) > buf_len) {
        return STREAM_ERROR_CLIENT;  // buffer too small; cache preserved for retry
    }

    std::memcpy(out_buf, handle->cached_json.c_str(), handle->cached_json.size() + 1);

    consume_cache(handle);
    return STREAM_OK;
}

// --- Record-level streaming ---

extern "C" auto stream_foreach_v1(StreamHandle* handle,
                                   stream_record_cb cb,
                                   void* user_data) -> int32_t {
    if (is_null_handle(handle) || cb == nullptr) {
        return STREAM_ERROR_INVALID_ARG;
    }

    while (true) {
        int32_t rc = ensure_fetched(handle);
        if (rc == STREAM_EXHAUSTED) return STREAM_OK;
        if (rc != STREAM_OK) return rc;

        // Deliver pre-serialised per-record JSON strings (no re-parse)
        for (const auto& json_str : handle->cached_record_jsons) {
            int32_t cb_rc = cb(json_str.c_str(),
                                static_cast<int32_t>(json_str.size()),
                                user_data);
            if (cb_rc != STREAM_OK) return cb_rc;
        }

        consume_cache(handle);
    }
}

// --- Cost / Budget ---

extern "C" auto stream_cost_info_v1(StreamHandle* handle,
                                     double* out_remaining_budget,
                                     int32_t* out_budget_exceeded) -> int32_t {
    if (is_null_handle(handle)) {
        return STREAM_ERROR_INVALID_ARG;
    }

    if (out_remaining_budget != nullptr) {
        auto budget = handle->engine->remaining_budget();
        *out_remaining_budget = budget.has_value() ? budget.value() : -1.0;
    }

    if (out_budget_exceeded != nullptr) {
        *out_budget_exceeded = handle->engine->budget_exceeded() ? 1 : 0;
    }

    return STREAM_OK;
}

// --- Cancellation ---

extern "C" auto stream_cancel(StreamHandle* handle) -> void {
    if (handle != nullptr && handle->engine != nullptr) {
        handle->engine->cancel();
    }
}
