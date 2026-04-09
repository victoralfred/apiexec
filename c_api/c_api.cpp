#include "c_api.h"

#include "../adapters/generic_rest.hpp"
#include "../core/engine.hpp"
#include "../policy/default_policy.hpp"
#include "../transport/curl_transport.hpp"

#include <cstring>
#include <memory>
#include <string>

using namespace apiexec;

// --- Type-erased handle wrapping ExecutionEngine<JsonBatch> ---

struct StreamHandle {
    std::unique_ptr<ExecutionEngine<JsonBatch>> engine;
    std::string last_json;   // cached serialised output for v2 functions
    int32_t last_count = 0;  // record count from last fetch
    int64_t last_ts_ms = 0;  // timestamp from last fetch (cursor start)
};

// --- Helpers ---

static auto is_null_handle(StreamHandle* h) -> bool {
    return h == nullptr || h->engine == nullptr;
}

// --- Lifecycle ---

extern "C" StreamHandle* stream_create(const char* adapter,
                                       const char* config_json,
                                       const char* policy_json) {
    if (adapter == nullptr || config_json == nullptr) {
        return nullptr;
    }

    std::string adapter_name(adapter);
    std::string config(config_json);

    try {
        // Currently only "generic_rest" is supported; adapter registry
        // dispatch will be added in M4.
        if (adapter_name != "generic_rest") {
            return nullptr;
        }

        auto adapter_cfg = GenericRestAdapter::from_json(config);
        auto adapter_ptr = std::make_unique<GenericRestAdapter>(adapter_cfg);

        std::unique_ptr<DefaultPolicy> policy;
        if (policy_json != nullptr && std::strlen(policy_json) > 0) {
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
        // No C++ exceptions propagate across the C boundary
        return nullptr;
    }
}

extern "C" void stream_destroy(StreamHandle* handle) {
    delete handle;
}

// --- Iteration ---

extern "C" auto stream_has_next(StreamHandle* handle) -> int32_t {
    if (is_null_handle(handle)) {
        return STREAM_ERROR_INVALID_ARG;
    }
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

    auto result = handle->engine->next_batch();
    if (result.error != StreamErrorCode::OK) {
        return static_cast<int32_t>(result.error);
    }

    // Serialise records to JSON
    nlohmann::json output = nlohmann::json::array();
    int32_t total_records = 0;
    for (const auto& batch : result.records) {
        for (const auto& record : batch.records) {
            output.push_back(record);
            ++total_records;
        }
    }

    std::string json_str = output.dump();

    // Check buffer size
    if (static_cast<int32_t>(json_str.size()) >= buf_len) {
        return STREAM_ERROR_CLIENT;  // buffer too small
    }

    std::memcpy(buf, json_str.data(), json_str.size());
    static_cast<char*>(buf)[json_str.size()] = '\0';
    *out_count = total_records;

    // Cache for v2 functions
    handle->last_json = std::move(json_str);
    handle->last_count = total_records;
    handle->last_ts_ms = handle->engine->cursor().time_window_start;

    return STREAM_OK;
}

extern "C" auto stream_next_v2_ts(StreamHandle* handle,
                                  int64_t* out_ts_ms) -> int32_t {
    if (is_null_handle(handle) || out_ts_ms == nullptr) {
        return STREAM_ERROR_INVALID_ARG;
    }

    auto result = handle->engine->next_batch();
    if (result.error != StreamErrorCode::OK) {
        return static_cast<int32_t>(result.error);
    }

    // Return the cursor's time_window_start as the timestamp
    *out_ts_ms = handle->engine->cursor().time_window_start;
    handle->last_ts_ms = *out_ts_ms;

    // Cache serialised JSON for a potential follow-up v2_sc call
    nlohmann::json output = nlohmann::json::array();
    for (const auto& batch : result.records) {
        for (const auto& record : batch.records) {
            output.push_back(record);
        }
    }
    handle->last_json = output.dump();

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

    auto result = handle->engine->next_batch();
    if (result.error != StreamErrorCode::OK) {
        return static_cast<int32_t>(result.error);
    }

    // Serialise records to JSON string
    nlohmann::json output = nlohmann::json::array();
    for (const auto& batch : result.records) {
        for (const auto& record : batch.records) {
            output.push_back(record);
        }
    }
    std::string json_str = output.dump();

    if (static_cast<int32_t>(json_str.size() + 1) > buf_len) {
        return STREAM_ERROR_CLIENT;  // buffer too small
    }

    std::memcpy(out_buf, json_str.c_str(), json_str.size() + 1);
    return STREAM_OK;
}

// --- Cancellation ---

extern "C" void stream_cancel(StreamHandle* handle) {
    if (handle != nullptr && handle->engine != nullptr) {
        handle->engine->cancel();
    }
}
