# apiexec

A vendor-agnostic streaming execution engine that turns unreliable API calls into a deterministic, streaming data retrieval model. Handles retry, backoff, adaptive chunking, pagination, and cost budgets internally -- exposing a clean `Stream<T>` interface upward and delegating API-specific logic to swappable `VendorAdapter<T>` implementations downward.

## Prerequisites

### Linux (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libcurl4-openssl-dev nlohmann-json3-dev
```

### Linux (Fedora/RHEL)

```bash
sudo dnf install -y gcc-c++ cmake libcurl-devel nlohmann-json-devel
```

### macOS

```bash
brew install cmake curl nlohmann-json
```

### Required versions

| Dependency | Minimum version |
|---|---|
| CMake | 3.16 |
| C++ compiler | C++17 (GCC 9+, Clang 10+, MSVC 19.14+) |
| libcurl | 7.68+ |
| nlohmann/json | 3.11+ |

## Build

All commands run from the `codebase/` directory.

### Release build

```bash
cmake -S source -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Debug build

```bash
cmake -S source -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Build with address and undefined behaviour sanitizers

```bash
cmake -S source -B build_asan -DSANITIZE=ON
cmake --build build_asan
```

### CMake options

| Option | Default | Description |
|---|---|---|
| `CMAKE_BUILD_TYPE` | (none) | `Release`, `Debug`, `RelWithDebInfo` |
| `SANITIZE` | `OFF` | Enable `-fsanitize=address,undefined` |

### Build outputs

| Target | Type | Description |
|---|---|---|
| `libapiexec.so` | Shared library | For language bindings (Go, Rust, Python, Java, JS) |
| `libapiexec_capi.a` | Static library | C API for static linking |
| `libapiexec_core.a` | INTERFACE | Header-only core interfaces |
| `libapiexec_transport.a` | Static library | CurlTransport |
| `libapiexec_policy.a` | Static library | DefaultPolicy + CostAwarePolicy |
| `libapiexec_adapters.a` | Static library | All vendor adapters |
| `apiexec` | Executable | CLI smoke test driver |
| `summarise` | Executable | Example: chunked summarisation with budget |

## Running tests

### All tests

```bash
cd build && ctest --output-on-failure
```

### Individual test

```bash
./build/cursor_test
./build/retry_test
./build/metrics_test
./build/multi_adapter_test
./build/prefetch_bench
```

### Tests under ASAN + UBSAN

```bash
cmake -S source -B build_asan -DSANITIZE=ON
cmake --build build_asan
cd build_asan && ctest --output-on-failure
```

### Soak test (configurable duration)

```bash
# Quick (10 seconds, default in ctest)
./build/soak_test 10

# Full 24-hour soak
./build/soak_test 86400
```

### Prefetch throughput benchmark

```bash
./build/prefetch_bench
```

Output shows sequential vs prefetch timings and the percentage improvement.

## Test suite

| Test | What it covers |
|---|---|
| `cursor_test` | Cursor struct, extra map bounds |
| `policy_test` | Backoff, window grow/shrink, prefetch depth |
| `policy_json_test` | JSON-configurable policy, validation, unknown keys |
| `metrics_test` | Atomic counters, Prometheus format, callback |
| `retry_test` | 429/5xx retry, Retry-After, 4xx fail-fast, rate-limit adaptation |
| `exhaustion_test` | Stream termination, cancellation |
| `prefetch_test` | Double-buffer overlap, timing, cancellation |
| `streaming_test` | Record-level callback, early stop |
| `cost_hooks_test` | Budget enforcement, cost recording |
| `c_api_test` | C API null safety, lifecycle, error codes |
| `engine_test` | GenericRestAdapter + engine integration |
| `e2e_test` | Full HTTP over libcurl against mock server |
| `multi_adapter_test` | All adapters (REST, Datadog, OpenAI) x 10k records |
| `rate_limit_test` | 429-storm convergence, window recovery |
| `cost_accounting_test` | CostAwarePolicy halts at budget, dollar tracking |
| `soak_test` | Long-running stability with rate limiting |
| `prefetch_bench` | Throughput benchmark (sequential vs prefetch) |

## Using as a library

### C/C++ (static linking)

Link against `libapiexec_capi.a` and its dependencies:

```bash
g++ -std=c++17 my_app.cpp \
    -I/path/to/apiexec/source/c_api \
    -L/path/to/apiexec/build \
    -lapiexec_capi \
    -Wl,--whole-archive -lapiexec_adapters -Wl,--no-whole-archive \
    -lapiexec_transport -lapiexec_policy \
    -lcurl -lstdc++ -lpthread
```

The `--whole-archive` flag on `apiexec_adapters` is required to preserve adapter self-registration static initialisers.

### C/C++ (shared library)

```bash
g++ -std=c++17 my_app.cpp \
    -I/path/to/apiexec/source/c_api \
    -L/path/to/apiexec/build \
    -lapiexec \
    -Wl,-rpath,/path/to/apiexec/build
```

### CMake (as a subdirectory)

```cmake
add_subdirectory(path/to/apiexec/source)
target_link_libraries(my_app PRIVATE apiexec_all)
```

### CMake (installed)

```bash
# Install
cmake --install build --prefix /usr/local

# In your CMakeLists.txt
find_package(apiexec REQUIRED)
target_link_libraries(my_app PRIVATE apiexec::apiexec)
```

### C API usage

```c
#include "c_api.h"

StreamHandle* stream = stream_create("generic_rest",
    "{\"base_url\": \"https://api.example.com/v1/data\"}",
    "{\"max_retries\": 3, \"prefetch_depth\": 1}");

char buf[1048576];
int32_t count;

while (stream_has_next(stream) == 1) {
    int32_t rc = stream_next_batch_v1(stream, buf, sizeof(buf), &count);
    if (rc == STREAM_EXHAUSTED) break;
    if (rc != STREAM_OK) { /* handle error */ break; }
    printf("Got %d records: %s\n", count, buf);
}

stream_destroy(stream);
```

## Language bindings

### Go

```bash
cd bindings/go
CGO_ENABLED=1 go test -v -race ./apiexec/...
```

```go
import "github.com/voseghale/apiexec/bindings/go/apiexec"

stream, _ := apiexec.NewStream("generic_rest",
    `{"base_url": "https://api.example.com/v1/data"}`, "")
defer stream.Close()

for stream.HasNext() {
    data, count, _ := stream.NextBatch(0)
    fmt.Printf("Got %d records (%d bytes)\n", count, len(data))
}
```

### Rust

```bash
cd bindings/rust
LD_LIBRARY_PATH=../../build cargo test
```

```rust
use apiexec::Stream;

let stream = Stream::new("generic_rest",
    r#"{"base_url": "https://api.example.com/v1/data"}"#, "")?;

stream.for_each(|data, count| {
    println!("Got {} records", count);
    true
})?;
```

### Python

```bash
cd bindings/python
LD_LIBRARY_PATH=../../build python3 -m pytest test_apiexec.py -v
```

```python
from apiexec import Stream

with Stream("generic_rest", '{"base_url": "https://api.example.com/v1/data"}') as s:
    for batch_json, count in s:
        print(f"Got {count} records")
```

### Java (requires JNA)

```java
import com.apiexec.Stream;

try (var stream = new Stream("generic_rest",
        "{\"base_url\": \"https://api.example.com/v1/data\"}")) {
    while (stream.hasNext()) {
        var batch = stream.nextBatch();
        System.out.printf("Got %d records%n", batch.count());
    }
}
```

### JavaScript/Node.js (requires ffi-napi)

```javascript
const { Stream } = require('apiexec');

const stream = new Stream('generic_rest',
    '{"base_url": "https://api.example.com/v1/data"}');

while (stream.hasNext()) {
    const { json, count } = stream.nextBatch();
    console.log(`Got ${count} records`);
}

stream.close();
```

## Available adapters

| Name | Description | Config keys |
|---|---|---|
| `generic_rest` | Cursor-paginated REST APIs | `base_url`, `auth_header`, `data_field`, `next_token_field`, `page_param`, `page_size` |
| `datadog_metrics` | Datadog Metrics API (time-window) | `base_url`, `api_key`, `app_key`, `query`, `window_ms` |
| `openai` | OpenAI Chat Completions | `base_url`, `api_key`, `model`, `prompts`, `max_tokens`, `temperature` |
| `anthropic` | Anthropic Messages API | `base_url`, `api_key`, `model`, `prompts`, `max_tokens` |

## Policy configuration (JSON)

```json
{
    "max_retries": 5,
    "base_backoff_ms": 100,
    "window_grow_factor": 1.5,
    "window_shrink_factor": 0.5,
    "min_window_ms": 60000,
    "max_window_ms": 86400000,
    "prefetch_depth": 1
}
```

## Architecture

```
source/
  core/          Zero-dep interfaces (Strategy + Template Method patterns)
  transport/     CurlTransport behind ITransport
  policy/        DefaultPolicy + CostAwarePolicy
  adapters/      4 vendor adapters + AIAdapter base + registry
  c_api/         ABI-stable C surface (APIEXEC_ABI_VERSION=1)
  examples/      Summarise example with cost budget
bindings/
  go/            cgo wrapper + tests
  rust/          Safe FFI wrapper + tests
  python/        ctypes binding + tests
  java/          JNA binding
  js/            ffi-napi binding
tests/
  unit/          12 unit test suites
  integration/   5 integration tests + benchmark
  fuzz/          libFuzzer targets for adapter parse paths
  soak/          Configurable-duration stability test
```

## License

See LICENSE file.
