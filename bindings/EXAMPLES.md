# Binding Examples

Comprehensive examples for each language binding. Every binding has parallel examples that demonstrate the same core library features.

## Features covered

| Example | What it demonstrates |
|---|---|
| **retry** | Automatic 429/5xx retry, exponential backoff, Retry-After honored, transparent recovery |
| **streaming** | Batch iteration with prefetch enabled, I/O overlap with processing (~50% wall-time reduction) |
| **cancellation** | Thread-safe cancel from a watchdog thread, clean termination on next iteration |
| **multi_adapter** | Runtime adapter dispatch via registry (openai, datadog_metrics) |
| **cost_budget** | Budget info query via the C API cost hooks |

## Running

### Go

```bash
cd bindings/go
CGO_ENABLED=1 go run ./examples/retry/
CGO_ENABLED=1 go run ./examples/streaming/
CGO_ENABLED=1 go run ./examples/cancellation/
CGO_ENABLED=1 go run ./examples/multi_adapter/
CGO_ENABLED=1 go run ./examples/cost_budget/
```

The Go binding now links against `libapiexec.so` via rpath — no `LD_LIBRARY_PATH` required.

### Rust

```bash
cd bindings/rust
LD_LIBRARY_PATH=../../build cargo run --example retry
LD_LIBRARY_PATH=../../build cargo run --example streaming
LD_LIBRARY_PATH=../../build cargo run --example cancellation
```

### Python

```bash
cd bindings/python
LD_LIBRARY_PATH=../../build python3 examples/retry.py
LD_LIBRARY_PATH=../../build python3 examples/streaming.py
LD_LIBRARY_PATH=../../build python3 examples/cancellation.py
LD_LIBRARY_PATH=../../build python3 examples/multi_adapter.py
```

### Java (requires JNA)

```bash
cd bindings/java/examples
javac -cp ".:jna.jar:../src/main/java" RetryExample.java StreamingExample.java
java -cp ".:jna.jar:../src/main/java" -Djna.library.path=../../../build RetryExample
java -cp ".:jna.jar:../src/main/java" -Djna.library.path=../../../build StreamingExample
```

### JavaScript/Node.js (requires ffi-napi)

```bash
cd bindings/js
LD_LIBRARY_PATH=../../build node examples/retry.js
LD_LIBRARY_PATH=../../build node examples/streaming.js
```

## Example anatomy

Each example follows the same pattern:

1. **Start a mock server** embedded in the example (no external dependencies)
2. **Create a Stream** via `stream_create(adapter, config, policy)`
3. **Iterate** — each example shows a different iteration pattern
4. **Demonstrate a feature** — retry, cancel, prefetch, etc.
5. **Clean up** — close stream, stop server

The mock servers serve the `{"data":[...], "next":"token"}` format that `generic_rest` expects, or the adapter-specific format for `openai`/`datadog_metrics`.

## Notes

- **Go examples** use `httptest.NewServer` for inline mock servers.
- **Rust examples** use a minimal raw TCP listener to avoid pulling in a web framework.
- **Python examples** use `http.server` from the stdlib.
- **Java examples** use `com.sun.net.httpserver` from the JDK.
- **JavaScript examples** use the built-in `http` module.

All examples print timing or counter info so you can verify the behavior matches the library's documented contract.
