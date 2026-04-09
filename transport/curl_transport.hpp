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
    CurlTransport& operator=(const CurlTransport&) = delete;
    CurlTransport(CurlTransport&& other) noexcept;
    CurlTransport& operator=(CurlTransport&& other) noexcept;

    Response execute(const Request& req, std::atomic<bool>& cancel_flag) override;

private:
    void* curl_handle_ = nullptr;  // CURL* — opaque to avoid leaking curl.h
};

} // namespace apiexec
