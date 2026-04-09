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

constexpr int MAX_RETRY_AFTER_SECS = 3600;  // 1 hour cap on Retry-After
constexpr int MS_PER_SECOND        = 1000;

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

    auto cursor() const -> const Cursor& { return cursor_; }

    auto set_sleep_fn(std::function<void(Duration)> fn) -> void { sleep_fn_ = std::move(fn); }

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

            if (result.error == StreamErrorCode::OK && !cursor_.exhausted) {
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
        prefetch_cv_.wait(lock, [this] { return prefetch_ready_; });
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
                // Parse failure does not trigger window shrink — the server
                // responded successfully; the issue is data format, not load.
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

    auto handle_error(const Response& resp, int& retry_count, int max_retries) -> ErrorAction {
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
                    int clamped = std::min(retry_after.value(), MAX_RETRY_AFTER_SECS);
                    sleep_for(Duration(clamped * MS_PER_SECOND));
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
