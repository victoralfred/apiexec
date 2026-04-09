#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace apiexec {

// HTTP status code boundaries.
constexpr int32_t HTTP_OK_MIN            = 200;
constexpr int32_t HTTP_OK_MAX            = 299;
constexpr int32_t HTTP_RATE_LIMITED      = 429;
constexpr int32_t HTTP_CLIENT_ERROR_MIN  = 400;
constexpr int32_t HTTP_CLIENT_ERROR_MAX  = 499;
constexpr int32_t HTTP_SERVER_ERROR_MIN  = 500;
constexpr int32_t HTTP_SERVER_ERROR_MAX  = 599;

// HTTP request to be sent by a transport implementation.
struct Request {
    enum class Method { GET, POST };

    Method method = Method::GET;
    std::string url;
    std::string body;
    std::map<std::string, std::string> headers;
};

// HTTP response returned by a transport implementation.
struct Response {
    int32_t status_code = 0;
    std::string body;
    std::map<std::string, std::string> headers;

    auto ok() const -> bool {
        return status_code >= HTTP_OK_MIN && status_code <= HTTP_OK_MAX;
    }

    auto is_rate_limited() const -> bool {
        return status_code == HTTP_RATE_LIMITED;
    }

    auto is_server_error() const -> bool {
        return status_code >= HTTP_SERVER_ERROR_MIN && status_code <= HTTP_SERVER_ERROR_MAX;
    }

    auto is_client_error() const -> bool {
        return status_code >= HTTP_CLIENT_ERROR_MIN
            && status_code <= HTTP_CLIENT_ERROR_MAX
            && status_code != HTTP_RATE_LIMITED;
    }

    // Get a header value (keys stored lowercase for case-insensitive lookup).
    auto header(const std::string& key) const -> std::string {
        auto it = headers.find(key);
        return it != headers.end() ? it->second : "";
    }
};

} // namespace apiexec
