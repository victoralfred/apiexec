#pragma once

#include "../core/itransport.hpp"

namespace apiexec {

// CurlTransport — libcurl-based ITransport implementation.
//
// Owns a single CURL easy handle that is reused across requests
// (connection pooling via libcurl's internal cache).
// Not thread-safe: a single CurlTransport must not be shared
// across threads without synchronisation.
class CurlTransport : public ITransport {
public:
    CurlTransport();
    ~CurlTransport() override;

    // Non-copyable, movable.
    CurlTransport(const CurlTransport&) = delete;
    auto operator=(const CurlTransport&) -> CurlTransport& = delete;
    CurlTransport(CurlTransport&& other) noexcept;
    auto operator=(CurlTransport&& other) noexcept -> CurlTransport&;

    auto execute(const Request& req, std::atomic<bool>& cancel_flag) -> Response override;

private:
    void* curl_handle_ = nullptr;  // CURL* — opaque to avoid leaking curl.h
};

} // namespace apiexec
