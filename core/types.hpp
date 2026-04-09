#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace apiexec {

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

    bool ok() const { return status_code >= 200 && status_code < 300; }
    bool is_rate_limited() const { return status_code == 429; }
    bool is_server_error() const { return status_code >= 500 && status_code < 600; }
    bool is_client_error() const {
        return status_code >= 400 && status_code < 500 && status_code != 429;
    }

    // Get a header value (keys stored lowercase for case-insensitive lookup).
    std::string header(const std::string& key) const {
        auto it = headers.find(key);
        return it != headers.end() ? it->second : "";
    }
};

} // namespace apiexec
