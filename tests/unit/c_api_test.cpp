#include "c_api.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

constexpr int BUF_SIZE = 65536;

void test_create_destroy() {
    const char* config = R"({"base_url": "http://localhost:9999/api"})";
    StreamHandle* h = stream_create("generic_rest", config, nullptr);
    assert(h != nullptr);
    stream_destroy(h);
    std::cout << "  PASS: create_destroy\n";
}

void test_create_null_args() {
    assert(stream_create(nullptr, "{}", nullptr) == nullptr);
    assert(stream_create("generic_rest", nullptr, nullptr) == nullptr);
    std::cout << "  PASS: create_null_args\n";
}

void test_create_unknown_adapter() {
    assert(stream_create("nonexistent", "{}", nullptr) == nullptr);
    std::cout << "  PASS: create_unknown_adapter\n";
}

void test_create_invalid_json() {
    assert(stream_create("generic_rest", "not json", nullptr) == nullptr);
    std::cout << "  PASS: create_invalid_json\n";
}

void test_destroy_null() {
    // Should not crash
    stream_destroy(nullptr);
    std::cout << "  PASS: destroy_null\n";
}

void test_has_next_null() {
    assert(stream_has_next(nullptr) == STREAM_ERROR_INVALID_ARG);
    std::cout << "  PASS: has_next_null\n";
}

void test_next_batch_null() {
    char buf[64];
    int32_t count = 0;
    assert(stream_next_batch_v1(nullptr, buf, 64, &count) == STREAM_ERROR_INVALID_ARG);

    const char* config = R"({"base_url": "http://localhost:9999/api"})";
    StreamHandle* h = stream_create("generic_rest", config, nullptr);
    assert(h != nullptr);

    // Null buffer
    assert(stream_next_batch_v1(h, nullptr, 64, &count) == STREAM_ERROR_INVALID_ARG);
    // Null out_count
    assert(stream_next_batch_v1(h, buf, 64, nullptr) == STREAM_ERROR_INVALID_ARG);
    // Zero buf_len
    assert(stream_next_batch_v1(h, buf, 0, &count) == STREAM_ERROR_INVALID_ARG);

    stream_destroy(h);
    std::cout << "  PASS: next_batch_null\n";
}

void test_v2_ts_null() {
    assert(stream_next_v2_ts(nullptr, nullptr) == STREAM_ERROR_INVALID_ARG);

    int64_t ts = 0;
    assert(stream_next_v2_ts(nullptr, &ts) == STREAM_ERROR_INVALID_ARG);
    std::cout << "  PASS: v2_ts_null\n";
}

void test_v2_sc_null() {
    assert(stream_next_v2_sc(nullptr, nullptr, 0) == STREAM_ERROR_INVALID_ARG);

    char buf[64];
    assert(stream_next_v2_sc(nullptr, buf, 64) == STREAM_ERROR_INVALID_ARG);
    std::cout << "  PASS: v2_sc_null\n";
}

void test_cancel_null() {
    // Should not crash
    stream_cancel(nullptr);
    std::cout << "  PASS: cancel_null\n";
}

void test_cancel_stops_stream() {
    // Use sequential mode to avoid prefetch-in-flight complications
    const char* config = R"({"base_url": "http://localhost:9999/api"})";
    const char* policy = R"({"prefetch_depth": 0})";
    StreamHandle* h = stream_create("generic_rest", config, policy);
    assert(h != nullptr);
    assert(stream_has_next(h) == 1);

    stream_cancel(h);
    assert(stream_has_next(h) == 0);

    char buf[BUF_SIZE];
    int32_t count = 0;
    int32_t rc = stream_next_batch_v1(h, buf, BUF_SIZE, &count);
    assert(rc == STREAM_ERROR_CANCELLED || rc == STREAM_EXHAUSTED);

    stream_destroy(h);
    std::cout << "  PASS: cancel_stops_stream\n";
}

void test_with_policy_json() {
    const char* config = R"({"base_url": "http://localhost:9999/api"})";
    const char* policy = R"({"max_retries": 2, "prefetch_depth": 0})";
    StreamHandle* h = stream_create("generic_rest", config, policy);
    assert(h != nullptr);
    stream_destroy(h);
    std::cout << "  PASS: with_policy_json\n";
}

void test_abi_version() {
    assert(APIEXEC_ABI_VERSION == 1);
    std::cout << "  PASS: abi_version\n";
}

auto main() -> int {
    std::cout << "c_api_test:\n";
    test_create_destroy();
    test_create_null_args();
    test_create_unknown_adapter();
    test_create_invalid_json();
    test_destroy_null();
    test_has_next_null();
    test_next_batch_null();
    test_v2_ts_null();
    test_v2_sc_null();
    test_cancel_null();
    test_cancel_stops_stream();
    test_with_policy_json();
    test_abi_version();
    std::cout << "All C API tests passed.\n";
    return 0;
}
