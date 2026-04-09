#pragma once

#include "cursor.hpp"
#include "execution_policy.hpp"
#include "itransport.hpp"
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
    ExecutionEngine& operator=(const ExecutionEngine&) = delete;

    bool has_next() const {
        // If a prefetch is in flight, there's a result to collect
        if (prefetch_in_flight_) return true;
        return !stream_done_.load(std::memory_order_acquire)
            && !cancel_flag_.load(std::memory_order_relaxed);
    }

    FetchResult<T> next_batch() {
        if (prefetch_enabled_) {
            return next_batch_prefetch();
        }
        return fetch_one();
    }

    void cancel() {
        cancel_flag_.store(true, std::memory_order_relaxed);
        stream_done_.store(true, std::memory_order_release);
    }

    const Cursor& cursor() const { return cursor_; }

    void set_sleep_fn(std::function<void(Duration)> fn) { sleep_fn_ = std::move(fn); }

private:
    enum class LoopSignal { PROCEED, RETRY, TERMINAL };

    struct ErrorAction {
        LoopSignal signal;
        StreamErrorCode error;
    };

    // --- Prefetch mode ---

    FetchResult<T> next_batch_prefetch() {
        // If a prefetch is in flight, always collect its result first
        if (prefetch_in_flight_) {
            FetchResult<T> result = collect_prefetch();
            prefetch_in_flight_ = false;

            // If this result is successful and there's more data, start next prefetch
            if (result.error == StreamErrorCode::OK && !cursor_.exhausted) {
                start_prefetch();
            } else {
                stream_done_.store(true, std::memory_order_release);
            }

            return result;
        }

        // No prefetch in flight and stream is done
        FetchResult<T> r;
        r.error = cancel_flag_.load() ? StreamErrorCode::CANCELLED
                                      : StreamErrorCode::EXHAUSTED;
        return r;
    }

    void start_prefetch() {
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

    FetchResult<T> collect_prefetch() {
        std::unique_lock<std::mutex> lock(prefetch_mutex_);
        prefetch_cv_.wait(lock, [this] { return prefetch_ready_; });
        return std::move(prefetch_result_);
    }

    void join_prefetch() {
        if (prefetch_thread_.joinable()) {
            prefetch_thread_.join();
        }
    }

    // --- Template method: single fetch cycle ---

    FetchResult<T> fetch_one() {
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

        int retry_count = 0;
        const int max_retries = policy_->max_retries();

        while (true) {
            if (is_cancelled()) {
                result.error = StreamErrorCode::CANCELLED;
                return result;
            }

            Request req = adapter_->build_request(cursor_);
            Response resp = transport_->execute(req, cancel_flag_);

            ErrorAction action = handle_error(resp, retry_count, max_retries);
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

            cursor_ = adapter_->next_cursor(cursor_, resp);
            policy_->adjust(cursor_, true);

            if (cursor_.exhausted) {
                stream_done_.store(true, std::memory_order_release);
            }

            result.records.push_back(std::move(parsed));
            result.error = StreamErrorCode::OK;
            return result;
        }
    }

    // --- Error handling ---

    ErrorAction handle_error(const Response& resp, int& retry_count, int max_retries) {
        if (resp.status_code == 0) {
            if (is_cancelled()) return {LoopSignal::TERMINAL, StreamErrorCode::CANCELLED};
            if (retry_count < max_retries) {
                sleep_for(policy_->backoff(retry_count));
                ++retry_count;
                return {LoopSignal::RETRY, {}};
            }
            return {LoopSignal::TERMINAL, StreamErrorCode::NETWORK};
        }

        if (resp.is_client_error()) {
            policy_->adjust(cursor_, false);
            return {LoopSignal::TERMINAL, StreamErrorCode::CLIENT};
        }

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
            policy_->adjust(cursor_, false);
            auto code = resp.is_rate_limited() ? StreamErrorCode::RATE_LIMIT
                                               : StreamErrorCode::SERVER;
            return {LoopSignal::TERMINAL, code};
        }

        return {LoopSignal::PROCEED, {}};
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

    // --- Prefetch ---
    bool prefetch_enabled_;
    bool prefetch_in_flight_ = false;
    std::atomic<bool> stream_done_;
    std::thread prefetch_thread_;
    std::mutex prefetch_mutex_;
    std::condition_variable prefetch_cv_;
    bool prefetch_ready_ = false;
    FetchResult<T> prefetch_result_;
};

} // namespace apiexec
