"""Example: runtime adapter selection via the registry.

The same Stream(adapter_name, config) API dispatches to any of the 4
registered adapters. This shows openai and datadog_metrics.

Run: LD_LIBRARY_PATH=../../build python3 examples/multi_adapter.py
"""

import http.server
import json
import sys
import threading

sys.path.insert(0, str(__import__("pathlib").Path(__file__).parent.parent))
from apiexec import Stream

openai_port = None
dd_port = None


class OpenAIMock(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        content_length = int(self.headers.get("Content-Length", 0))
        _ = self.rfile.read(content_length)

        resp = {
            "choices": [{"index": 0, "message": {
                "role": "assistant", "content": "Hello from mock OpenAI"
            }}],
            "usage": {"prompt_tokens": 10, "completion_tokens": 20, "total_tokens": 30},
        }
        body = json.dumps(resp).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class DatadogMock(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        resp = {
            "series": [{
                "metric": "system.cpu.user",
                "pointlist": [[1700000000, 42.5], [1700000060, 43.1]],
            }]
        }
        body = json.dumps(resp).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


openai_server = http.server.HTTPServer(("127.0.0.1", 0), OpenAIMock)
openai_port = openai_server.server_address[1]
threading.Thread(target=openai_server.serve_forever, daemon=True).start()

dd_server = http.server.HTTPServer(("127.0.0.1", 0), DatadogMock)
dd_port = dd_server.server_address[1]
threading.Thread(target=dd_server.serve_forever, daemon=True).start()


def run_adapter(name, config, policy):
    print(f"--- {name} ---")
    try:
        with Stream(name, json.dumps(config), policy) as s:
            if not s.has_next():
                print("  No data available")
                return
            data, count = s.next_batch()
            print(f"  Got {count} records ({len(data)} bytes)")
            preview = data[:120].decode("utf-8", errors="replace")
            print(f"  Preview: {preview}{'...' if len(data) > 120 else ''}")
    except Exception as e:
        print(f"  Error: {e}")
    print()


run_adapter("openai", {
    "base_url": f"http://127.0.0.1:{openai_port}/v1/chat/completions",
    "api_key": "fake",
    "model": "gpt-4",
    "prompts": ["Say hello"],
    "max_tokens": 50,
}, '{"prefetch_depth": 0}')

run_adapter("datadog_metrics", {
    "base_url": f"http://127.0.0.1:{dd_port}/api/v1/query",
    "api_key": "fake",
    "app_key": "fake",
    "query": "avg:system.cpu.user{*}",
    "window_ms": 3600000,
}, '{"prefetch_depth": 0}')

print("Same Stream API drives any adapter — runtime dispatch via registry.")

openai_server.shutdown()
dd_server.shutdown()
