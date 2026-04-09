#pragma once

#include "cursor.hpp"
#include "execution_policy.hpp"
#include "itransport.hpp"
#include "stream_error.hpp"
#include "vendor_adapter.hpp"

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace apiexec {

// Batch result from a single fetch cycle.
template <typename T>
struct FetchResult {
    StreamErrorCode error = StreamErrorCode::OK;
    std::vector<T> records;
};

// ---------------------------------------------------------------------------
// ExecutionEngine<T>
//
// Core data plane. Combines the Strategy pattern with the Template Method
// pattern:
//
//   Strategy slots (injected at construction):
//     - VendorAdapter<T>   : API-specific request/response behaviour
//     - ExecutionPolicy    : backoff, window sizing, retry limits
//     - ITransport         : HTTP execution (mockable for unit tests)
//
//   Template method (next_batch):
//     The invariant sequence is:
//       1. check_preconditions  — cancel / exhaustion guard
//       2. build_request        — delegates to VendorAdapter<T>
//       3. execute_transport    — delegates to ITransport
//       4. handle_error         — retry / fail decisions via Policy + Adapter
//       5. parse_response       — delegates to VendorAdapter<T>
//       6. advance_cursor       — delegates to VendorAdapter<T>
//       7. adjust_policy        — delegates to ExecutionPolicy
//       8. yield                — return parsed batch to consumer
//
//   Each step is a named private method. The loop structure is fixed;
//   the behaviour at each step is determined entirely by the strategies.
//
// Threading model: single-consumer. next_batch() must not be called
// concurrently. cancel() is safe to call from any thread.
// ---------------------------------------------------------------------------
template <typename T>
class ExecutionEngine {
public:
    ExecutionEngine(std::unique_ptr<VendorAdapter<T>> adapter,
                    std::unique_ptr<ExecutionPolicy> policy,
                    std::unique_ptr<ITransport> transport,
                    Cursor initial_cursor)
        : adapter_(std::move(adapter))
        , policy_(std::move(policy))
        , transport_(std::move(transport))
        , cursor_(std::move(initial_cursor))
        , cancel_flag_(false)
    {}

    // Is there more data to fetch?
    bool has_next() const {
        return !cursor_.exhausted && !cancel_flag_.load(std::memory_order_relaxed);
    }

    // Template method: fetch the next batch of records.
    // Drives the invariant sequence, delegating each variable step
    // to the injected strategies.
    FetchResult<T> next_batch() {
        FetchResult<T> result;

        // Step 1: check preconditions
        if (!check_preconditions(result)) return result;

        int retry_count = 0;
        const int max_retries = policy_->max_retries();

        while (true) {
            if (is_cancelled()) {
                result.error = StreamErrorCode::CANCELLED;
                return result;
            }

            // Step 2: build request (adapter strategy)
            Request req = build_request();

            // Step 3: execute transport (transport strategy)
            Response resp = execute_transport(req);

            // Step 4: handle errors (policy + adapter strategies)
            ErrorAction action = handle_error(resp, retry_count, max_retries);
            switch (action.signal) {
                case LoopSignal::PROCEED:
                    break;  // fall through to parse
                case LoopSignal::RETRY:
                    continue;
                case LoopSignal::TERMINAL:
                    result.error = action.error;
                    return result;
            }

            // Step 5: parse response (adapter strategy)
            T parsed;
            if (!parse_response(resp, parsed)) {
                // Parse failure does not trigger window shrink — the server
                // responded successfully; the issue is data format, not load.
                result.error = StreamErrorCode::PARSE;
                return result;
            }

            // Step 6: advance cursor (adapter strategy)
            advance_cursor(resp);

            // Step 7: adjust policy on success (policy strategy)
            adjust_policy(true);

            // Step 8: yield to consumer
            result.records.push_back(std::move(parsed));
            result.error = StreamErrorCode::OK;
            return result;
        }
    }

    // Cancel the stream. Safe to call from any thread.
    void cancel() {
        cancel_flag_.store(true, std::memory_order_relaxed);
    }

    // Access the current cursor (for observability).
    const Cursor& cursor() const { return cursor_; }

    // Inject a sleep function for testing (avoids real delays).
    void set_sleep_fn(std::function<void(Duration)> fn) { sleep_fn_ = std::move(fn); }

private:
    // Private loop control — never exposed to callers.
    enum class LoopSignal { PROCEED, RETRY, TERMINAL };

    struct ErrorAction {
        LoopSignal signal;
        StreamErrorCode error;  // only meaningful when signal == TERMINAL
    };

    // --- Template method steps ---

    // Step 1: Guard against exhaustion and cancellation.
    bool check_preconditions(FetchResult<T>& result) {
        if (cursor_.exhausted) {
            result.error = StreamErrorCode::EXHAUSTED;
            return false;
        }
        if (is_cancelled()) {
            result.error = StreamErrorCode::CANCELLED;
            return false;
        }
        return true;
    }

    // Step 2: Delegate request construction to the adapter strategy.
    Request build_request() {
        return adapter_->build_request(cursor_);
    }

    // Step 3: Delegate HTTP execution to the transport strategy.
    Response execute_transport(const Request& req) {
        return transport_->execute(req, cancel_flag_);
    }

    // Step 4: Classify the response and decide retry / fail / proceed.
    // Policy adjustment for failure happens only on terminal errors (not
    // per-retry), to avoid compounding window shrink across retries.
    ErrorAction handle_error(const Response& resp, int& retry_count, int max_retries) {
        // Network error (status 0)
        if (resp.status_code == 0) {
            if (is_cancelled()) return {LoopSignal::TERMINAL, StreamErrorCode::CANCELLED};
            if (retry_count < max_retries) {
                sleep_for(policy_->backoff(retry_count));
                ++retry_count;
                return {LoopSignal::RETRY, {}};
            }
            return {LoopSignal::TERMINAL, StreamErrorCode::NETWORK};
        }

        // Client error (4xx non-429): fail immediately, adjust policy once
        if (resp.is_client_error()) {
            adjust_policy(false);
            return {LoopSignal::TERMINAL, StreamErrorCode::CLIENT};
        }

        // Retryable errors (429, 5xx): delegate to adapter + policy
        if (adapter_->is_retryable(resp)) {
            if (retry_count < max_retries) {
                auto retry_after = adapter_->retry_after(resp);
                if (retry_after.has_value() && retry_after.value() > 0) {
                    int clamped = std::min(retry_after.value(), 3600);
                    sleep_for(Duration(clamped * 1000));
                } else {
                    sleep_for(policy_->backoff(retry_count));
                }
                ++retry_count;
                return {LoopSignal::RETRY, {}};
            }
            // Terminal: all retries exhausted — adjust policy once
            adjust_policy(false);
            auto code = resp.is_rate_limited() ? StreamErrorCode::RATE_LIMIT
                                               : StreamErrorCode::SERVER;
            return {LoopSignal::TERMINAL, code};
        }

        // Success
        return {LoopSignal::PROCEED, {}};
    }

    // Step 5: Delegate response parsing to the adapter strategy.
    bool parse_response(const Response& resp, T& out) {
        return adapter_->parse_response(resp, out);
    }

    // Step 6: Delegate cursor advancement to the adapter strategy.
    void advance_cursor(const Response& resp) {
        cursor_ = adapter_->next_cursor(cursor_, resp);
    }

    // Step 7: Delegate policy adjustment to the policy strategy.
    void adjust_policy(bool success) {
        policy_->adjust(cursor_, success);
    }

    // --- Utilities ---

    bool is_cancelled() const {
        return cancel_flag_.load(std::memory_order_relaxed);
    }

    void sleep_for(Duration d) {
        if (sleep_fn_) {
            sleep_fn_(d);
        } else {
            std::this_thread::sleep_for(d);
        }
    }

    // --- Strategy slots ---
    std::unique_ptr<VendorAdapter<T>> adapter_;
    std::unique_ptr<ExecutionPolicy>  policy_;
    std::unique_ptr<ITransport>       transport_;

    // --- State ---
    Cursor cursor_;
    std::atomic<bool> cancel_flag_;
    std::function<void(Duration)> sleep_fn_;
};

} // namespace apiexec
