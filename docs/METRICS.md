# Metrics

apiexec exposes runtime metrics for observability. The metrics interface is **callback-based** rather than embedded — the engine maintains thread-safe atomic counters and invokes a user-supplied callback after every successful fetch with a read-only snapshot. This keeps the library footprint small and lets applications integrate with whichever metrics system they already use (Prometheus, StatsD, Datadog, OpenTelemetry, etc.).

**Header:** `source/core/metrics.hpp`
**Owned by:** `ExecutionEngine<T>` (one `Metrics` instance per stream)
**Thread safety:** All counters are `std::atomic`; `snapshot()` is lock-free.

---

## Available metrics

| Metric | Type | Description | Incremented when |
|---|---|---|---|
| `request_count` | counter (int64) | Total HTTP requests made by the transport | Every call to `transport->execute()` |
| `retry_count` | counter (int64) | Total retry attempts (429 + 5xx + network errors) | Every time `handle_error` decides to retry |
| `success_count` | counter (int64) | Successful fetches (response parsed OK) | After `parse_response()` returns true |
| `records_total` | counter (int64) | Records delivered to the caller | Once per successful batch (tracks batch count; adapters return 1 batch per fetch) |
| `error_rate_limit` | counter (int64) | Terminal 429 errors after retries exhausted | `StreamErrorCode::RATE_LIMIT` returned to caller |
| `error_server` | counter (int64) | Terminal 5xx errors after retries exhausted | `StreamErrorCode::SERVER` returned to caller |
| `error_client` | counter (int64) | Terminal 4xx errors (non-429, no retry) | `StreamErrorCode::CLIENT` returned to caller |
| `error_network` | counter (int64) | Terminal network errors after retries exhausted | `StreamErrorCode::NETWORK` returned to caller |
| `error_parse` | counter (int64) | Response parse failures | `parse_response()` returned false |
| `window_size_ms` | gauge (double) | Current cursor time window size in milliseconds | After every cursor advancement |
| `cumulative_cost` | gauge (double) | Cumulative cost units reported by adapter (e.g., tokens) | After every `adapter->response_cost()` call |

### What is NOT exposed

- **Per-adapter labels**: metrics are per-stream. If you need per-adapter aggregation, create one stream per adapter and aggregate externally.
- **Latency histograms**: the library does not measure request duration. Track this yourself in your callback if needed.
- **Prefetch queue depth**: prefetch is depth 0 or 1 (double-buffer) — no queue.

---

## MetricsSnapshot

A read-only snapshot is returned by `engine.metrics_snapshot()` and passed to the callback. It has the fields listed above. Snapshots are cheap (one atomic load per field) and lock-free.

```cpp
struct MetricsSnapshot {
    int64_t request_count;
    int64_t retry_count;
    int64_t success_count;
    int64_t error_rate_limit;
    int64_t error_server;
    int64_t error_client;
    int64_t error_network;
    int64_t error_parse;
    int64_t records_total;
    double  window_size_ms;
    double  cumulative_cost;
};
```

---

## Usage patterns

### 1. Poll the snapshot on demand

Query metrics at any time — for example, to log periodically or expose on a `/metrics` HTTP endpoint.

```cpp
#include "core/engine.hpp"

ExecutionEngine<JsonBatch> engine(/* ... */);

// Run the stream, then inspect metrics
while (engine.has_next()) {
    auto result = engine.next_batch();
    // ... process result ...
}

auto snap = engine.metrics_snapshot();
std::cout << "Requests: " << snap.request_count
          << " Retries: " << snap.retry_count
          << " Errors (429): " << snap.error_rate_limit << "\n";
```

### 2. Receive a callback on every successful fetch

Register a `MetricsCallback` — it fires once per successful `next_batch()` call, letting you update your metrics backend live.

```cpp
engine.set_metrics_callback([](const apiexec::MetricsSnapshot& s) {
    // Update Prometheus counter
    my_counter_requests.Increment(s.request_count);
    my_counter_retries.Increment(s.retry_count);

    // Or send to StatsD
    statsd.gauge("apiexec.window_size_ms", s.window_size_ms);
});
```

The callback runs on the thread calling `next_batch()` (or the prefetch thread when prefetch is enabled). Keep it fast — do not perform I/O synchronously inside it.

### 3. Export to Prometheus text format

The `Metrics` class has a built-in Prometheus text exposition formatter:

```cpp
auto prom_text = engine.metrics().to_prometheus("apiexec");
// Write prom_text to your HTTP /metrics endpoint
```

**Sample output:**

```
apiexec_requests_total 1250
apiexec_retries_total 47
apiexec_successes_total 1200
apiexec_errors_rate_limit_total 5
apiexec_errors_server_total 2
apiexec_errors_client_total 0
apiexec_errors_network_total 1
apiexec_errors_parse_total 0
apiexec_records_total 1200
apiexec_window_size_ms 3600000.000000
apiexec_cumulative_cost_units 0.000000
```

The prefix is configurable (default `"apiexec"`). Use a different prefix per stream if you run multiple streams in the same process.

### 4. Expose `/metrics` over HTTP

Pair the Prometheus formatter with any small HTTP server:

```cpp
#include <httplib.h>  // or any HTTP server

httplib::Server svr;
svr.Get("/metrics", [&engine](const auto& req, auto& res) {
    res.set_content(engine.metrics().to_prometheus("apiexec"), "text/plain");
});
svr.listen("0.0.0.0", 9090);
```

---

## Interpreting the metrics

### Healthy stream

```
request_count   = 100
success_count   = 100
retry_count     = 0
error_*         = 0
```

Every request succeeds on the first try. The window may be growing toward `max_window_ms`.

### Stream under moderate rate-limit pressure

```
request_count     = 150
success_count     = 100
retry_count       = 50
error_rate_limit  = 0
window_size_ms    = 1800000  (shrunk from 3600000)
```

50 retries were needed to complete 100 successful fetches — a 50% retry rate. The window shrunk once on the first 429 and has stabilized. Consider reducing `window_grow_factor` or increasing `min_window_ms` to reduce oscillation.

### Stream hitting budget cap

```
request_count     = 10
success_count     = 10
cumulative_cost   = 1000.0  (equal to budget_tokens)
```

Budget exhausted. Next call to `next_batch()` will return `BUDGET_EXHAUSTED`. See [docs/c_api.md](./c_api.md) for the error code.

### Stream with persistent network issues

```
request_count   = 60
success_count   = 10
retry_count     = 50
error_network   = 0        # still retrying
```

Versus:

```
request_count   = 60
success_count   = 10
retry_count     = 50
error_network   = 5        # 5 terminal network errors
```

In the second case, 5 batches hit `max_retries` and were returned to the caller as terminal network failures. Check the log callback output for the specific failure modes.

---

## Using metrics alongside logging

Metrics tell you **how many** things happened. Logs tell you **what specifically** happened. They are complementary:

- `error_network = 5` → there were 5 network failures (metric)
- Log output → "connection refused to api.example.com:443" (log)

See [docs/c_api.md](./c_api.md) for the structured logging interface.

---

## Metrics and the C API

The C API does not currently expose per-stream metrics directly. Two workarounds:

1. **Use the C++ engine directly** if you're in a C++ host process.
2. **Use the `stream_cost_info_v1` C API** for budget/cost queries (bindings expose this as `CostInfo()` in Go, `cost_info()` in Rust, etc.).

A future ABI version (v2) may add `stream_metrics_snapshot_v2` for full metric access from C callers. This is tracked in the backlog.

---

## Metrics in the language bindings

| Binding | Metrics access |
|---|---|
| **C++ (direct)** | `engine.metrics_snapshot()` / `engine.set_metrics_callback()` |
| **C API** | `stream_cost_info_v1()` — budget/cost only, not yet full metrics |
| **Go** | `stream.CostInfo()` — budget/cost only |
| **Rust** | `stream.cost_info()` — budget/cost only |
| **Python** | Not yet exposed beyond cost |
| **Java** | Not yet exposed beyond cost |
| **JavaScript** | Not yet exposed beyond cost |

If you need full metrics access from a binding, the recommended pattern is to host the engine in a C++ process and expose metrics via `/metrics` HTTP, then have the binding consumer poll that endpoint.

---

## Implementation notes

- **Thread safety**: every counter is `std::atomic<int64_t>` or `std::atomic<double>`, updated with `memory_order_relaxed`. This is correct because the counters are monotonically incrementing; no happens-before ordering between counter updates is required for correct observation.
- **Cost**: incrementing a counter is a single atomic add. Snapshot is 11 atomic loads. Negligible overhead even at high throughput (the prefetch benchmark shows identical sequential-mode performance with metrics enabled vs. disabled).
- **No dependency**: `metrics.hpp` is header-only and has zero external dependencies — it's part of the `core/` layer.
- **Callback ordering**: the metrics callback fires AFTER the record is parsed and the cursor is advanced, so `records_total` in the snapshot includes the record being delivered in the current call.

---

## See also

- [c_api.md](./c_api.md) — C API reference including error codes and structured logging
- [source/core/metrics.hpp](../source/core/metrics.hpp) — the implementation
- [tests/unit/metrics_test.cpp](../tests/unit/metrics_test.cpp) — test suite demonstrating all usage patterns
