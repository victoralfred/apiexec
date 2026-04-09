#pragma once

#include "types.hpp"

#include <atomic>

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
};

} // namespace apiexec
