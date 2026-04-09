#pragma once

#include "cursor.hpp"
#include "execution_policy.hpp"
#include "itransport.hpp"
#include "limits.hpp"
#include "stream_error.hpp"
#include "vendor_adapter.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace apiexec {

template <typename T>
struct FetchResult {
    StreamErrorCode error = StreamErrorCode::OK;
    std::vector<T> records;
};

// ---------------------------------------------------------------------------
// ExecutionEngine<T>
//
// Strategy slots (injected at construction):
//   - VendorAdapter<T>   : API-specific request/response behaviour
//   - ExecutionPolicy    : backoff, window sizing, retry limits
//   - ITransport         : HTTP execution (mockable for unit tests)
//
// Template method (fetch_one): invariant sequence of
//   check → build → execute → handle_error → parse → advance → adjust → yield
//
// Double-buffer prefetch: when prefetch_depth >= 1, the engine kicks off the
// next fetch on a background thread after yielding a result. The next
// next_batch() call collects the prefetched result. Overlaps network I/O
// with the caller's processing of the current batch.
//
// Threading: single-consumer. next_batch() must not be called concurrently.
// cancel() is safe from any thread.
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
        , prefetch_enabled_(policy_->prefetch_depth() >= 1)
        , stream_done_(cursor_.exhausted)
    {
        if (prefetch_enabled_ && !stream_done_) {
            start_prefetch();
        }
    }

    ~ExecutionEngine() {
        cancel();
        join_prefetch();
    }

    ExecutionEngine(const ExecutionEngine&) = delete;
    auto operator=(const ExecutionEngine&) -> ExecutionEngine& = delete;

    auto has_next() const -> bool {
        if (prefetch_in_flight_) return true;
        return !stream_done_.load(std::memory_order_acquire)
            && !cancel_flag_.load(std::memory_order_relaxed);
    }

    auto next_batch() -> FetchResult<T> {
        if (prefetch_enabled_) {
            return next_batch_prefetch();
        }
        return fetch_one();
    }

    auto cancel() -> void {
        cancel_flag_.store(true, std::memory_order_relaxed);
        stream_done_.store(true, std::memory_order_release);
    }

    auto cursor() const -> Cursor {
        std::lock_guard<std::mutex> lock(cursor_mutex_);
        return cursor_;
    }

    auto set_sleep_fn(std::function<void(Duration)> fn) -> void { sleep_fn_ = std::move(fn); }

    // --- Record-level streaming interface ---

    // Per-record callback. Return false to stop iteration.
    using RecordCallback = std::function<bool(const T& record)>;

    // Stream mode: calls cb for each record in each batch until exhaustion,
    // error, or the callback returns false. Returns the terminal error code
    // (OK if fully exhausted, CANCELLED if cb returned false).
    auto stream(RecordCallback cb) -> StreamErrorCode {
        while (has_next()) {
            auto result = next_batch();
            if (result.error == StreamErrorCode::EXHAUSTED) return StreamErrorCode::OK;
            if (result.error != StreamErrorCode::OK) return result.error;
            for (auto& record : result.records) {
                if (!cb(record)) return StreamErrorCode::CANCELLED;
            }
        }
        return is_cancelled() ? StreamErrorCode::CANCELLED : StreamErrorCode::OK;
    }

    // Cost info for the C API boundary.
    auto remaining_budget() const -> std::optional<double> {
        return policy_->remaining_budget();
    }

    auto budget_exceeded() const -> bool {
        return policy_->budget_exceeded();
    }

private:
    enum class LoopSignal { PROCEED, RETRY, TERMINAL };

    struct ErrorAction {
        LoopSignal signal;
        StreamErrorCode error;
    };

    // --- Prefetch mode ---

    auto next_batch_prefetch() -> FetchResult<T> {
        if (prefetch_in_flight_) {
            FetchResult<T> result = collect_prefetch();
            prefetch_in_flight_ = false;

            // Read cursor_.exhausted under lock to avoid race with prefetch thread
            bool exhausted;
            {
                std::lock_guard<std::mutex> lock(cursor_mutex_);
                exhausted = cursor_.exhausted;
            }

            if (result.error == StreamErrorCode::OK) {
                prefetch_consecutive_failures_ = 0;
                if (!exhausted) {
                    start_prefetch();
                } else {
                    stream_done_.store(true, std::memory_order_release);
                }
            } else if (!exhausted && !is_cancelled()
                       && prefetch_consecutive_failures_ < kMaxPrefetchRetries) {
                ++prefetch_consecutive_failures_;
                start_prefetch();
            } else {
                stream_done_.store(true, std::memory_order_release);
            }

            return result;
        }

        FetchResult<T> r;
        r.error = cancel_flag_.load() ? StreamErrorCode::CANCELLED
                                      : StreamErrorCode::EXHAUSTED;
        return r;
    }

    auto start_prefetch() -> void {
        join_prefetch();

        {
            std::lock_guard<std::mutex> lock(prefetch_mutex_);
            prefetch_ready_ = false;
        }

        prefetch_in_flight_ = true;
        prefetch_thread_ = std::thread([this] {
            FetchResult<T> result = fetch_one();
            {
                std::lock_guard<std::mutex> lock(prefetch_mutex_);
                prefetch_result_ = std::move(result);
                prefetch_ready_ = true;
            }
            prefetch_cv_.notify_one();
        });
    }

    auto collect_prefetch() -> FetchResult<T> {
        std::unique_lock<std::mutex> lock(prefetch_mutex_);
        bool completed = prefetch_cv_.wait_for(lock, kPrefetchTimeout,
                                                [this] { return prefetch_ready_; });
        if (!completed) {
            cancel_flag_.store(true, std::memory_order_relaxed);
            FetchResult<T> timeout_result;
            timeout_result.error = StreamErrorCode::NETWORK;
            return timeout_result;
        }
        return std::move(prefetch_result_);
    }

    auto join_prefetch() -> void {
        if (prefetch_thread_.joinable()) {
            prefetch_thread_.join();
        }
    }

    // --- Template method: single fetch cycle ---

    auto fetch_one() -> FetchResult<T> {
        FetchResult<T> result;

        if (cursor_.exhausted) {
            result.error = StreamErrorCode::EXHAUSTED;
            stream_done_.store(true, std::memory_order_release);
            return result;
        }
        if (is_cancelled()) {
            result.error = StreamErrorCode::CANCELLED;
            return result;
        }
        if (policy_->budget_exceeded()) {
            stream_done_.store(true, std::memory_order_release);
            result.error = StreamErrorCode::BUDGET_EXHAUSTED;
            return result;
        }

        int retry_count = 0;
        const int max_retries = policy_->max_retries();
        bool saw_rate_limit = false;

        while (true) {
            if (is_cancelled()) {
                result.error = StreamErrorCode::CANCELLED;
                return result;
            }

            Request req = adapter_->build_request(cursor_);
            Response resp = transport_->execute(req, cancel_flag_);

            ErrorAction action = handle_error(resp, retry_count, max_retries, saw_rate_limit);
            switch (action.signal) {
                case LoopSignal::PROCEED:
                    break;
                case LoopSignal::RETRY:
                    continue;
                case LoopSignal::TERMINAL:
                    result.error = action.error;
                    return result;
            }

            T parsed;
            if (!adapter_->parse_response(resp, parsed)) {
                result.error = StreamErrorCode::PARSE;
                return result;
            }

            {
                std::lock_guard<std::mutex> lock(cursor_mutex_);
                cursor_ = adapter_->next_cursor(cursor_, resp);
                // Only grow the window if no rate-limit pressure was seen this cycle.
                if (!saw_rate_limit) {
                    policy_->adjust(cursor_, true);
                }
            }

            // Record cost if the adapter provides it
            auto cost = adapter_->response_cost(resp);
            if (cost.has_value()) {
                policy_->record_cost(cursor_, cost.value());
            }

            bool exhausted;
            {
                std::lock_guard<std::mutex> lock(cursor_mutex_);
                exhausted = cursor_.exhausted;
            }
            if (exhausted) {
                stream_done_.store(true, std::memory_order_release);
            }

            result.records.push_back(std::move(parsed));
            result.error = StreamErrorCode::OK;
            return result;
        }
    }

    // --- Error handling ---

    auto handle_error(const Response& resp, int& retry_count, int max_retries,
                      bool& saw_rate_limit) -> ErrorAction {
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

        // 4xx non-429: fail immediately. No window adjustment — auth/validation
        // errors are not load signals.
        if (resp.is_client_error()) {
            return {LoopSignal::TERMINAL, StreamErrorCode::CLIENT};
        }

        // Retryable errors (429, 5xx)
        if (adapter_->is_retryable(resp)) {
            // 429: signal rate-limit pressure on first encounter this cycle.
            // This shrinks the window immediately so the next request scope is smaller.
            if (resp.is_rate_limited() && !saw_rate_limit) {
                {
                    std::lock_guard<std::mutex> lock(cursor_mutex_);
                    policy_->adjust(cursor_, false);
                }
                saw_rate_limit = true;
            }

            if (retry_count < max_retries) {
                auto retry_after = adapter_->retry_after(resp);
                if (retry_after.has_value() && retry_after.value() > 0) {
                    int clamped = std::min(retry_after.value(), MAX_RETRY_AFTER_SECS);
                    sleep_for(Duration(clamped * MS_PER_SECOND));
                } else {
                    sleep_for(policy_->backoff(retry_count));
                }
                ++retry_count;
                return {LoopSignal::RETRY, {}};
            }

            // Terminal: retries exhausted. No additional adjust — 429 already
            // adjusted on first encounter; 5xx is an infrastructure issue,
            // not a load signal.
            auto code = resp.is_rate_limited() ? StreamErrorCode::RATE_LIMIT
                                               : StreamErrorCode::SERVER;
            return {LoopSignal::TERMINAL, code};
        }

        return {LoopSignal::PROCEED, {}};
    }

    // --- Utilities ---

    auto is_cancelled() const -> bool {
        return cancel_flag_.load(std::memory_order_relaxed);
    }

    auto sleep_for(Duration d) -> void {
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

    // --- Cursor synchronisation ---
    mutable std::mutex cursor_mutex_;

    // --- Prefetch ---
    static constexpr int kMaxPrefetchRetries = 1;
    static constexpr auto kPrefetchTimeout = std::chrono::minutes(10);
    bool prefetch_enabled_;
    int prefetch_consecutive_failures_ = 0;
    std::atomic<bool> prefetch_in_flight_{false};
    std::atomic<bool> stream_done_;
    std::thread prefetch_thread_;
    std::mutex prefetch_mutex_;
    std::condition_variable prefetch_cv_;
    bool prefetch_ready_ = false;
    FetchResult<T> prefetch_result_;
};

} // namespace apiexec
