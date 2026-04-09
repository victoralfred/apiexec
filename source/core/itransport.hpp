#pragma once

#include "types.hpp"

#include <atomic>
#include <cstddef>
#include <functional>

namespace apiexec {

// ITransport — abstract transport interface.
// Decouples the execution engine from any specific HTTP client implementation.
// Mock this for unit testing without a live network.
class ITransport {
public:
    virtual ~ITransport() = default;

    // Execute an HTTP request. Checks cancel_flag between I/O waits.
    // Returns a Response; status_code 0 indicates network error or cancellation.
    virtual auto execute(const Request& req, std::atomic<bool>& cancel_flag) -> Response = 0;

    // --- Streaming hook (defaulted — non-breaking) ---

    // Streaming execute: calls data_cb for each chunk of response body as it arrives.
    // Default implementation calls execute() then delivers the body as one chunk.
    // Override in CurlTransport (or SSE transport) for true incremental delivery.
    using DataCallback = std::function<void(const char* data, std::size_t len)>;

    virtual auto execute_streaming(const Request& req,
                                   std::atomic<bool>& cancel_flag,
                                   DataCallback data_cb) -> Response {
        Response resp = execute(req, cancel_flag);
        if (!resp.body.empty()) {
            data_cb(resp.body.data(), resp.body.size());
        }
        return resp;
    }
};

} // namespace apiexec
