"""Example: retry and rate-limit adaptation.

Demonstrates automatic 429/5xx retry with exponential backoff. The mock
server returns 3 x 429 before succeeding; the engine retries transparently
and the caller never sees the rate-limit errors.

Run: LD_LIBRARY_PATH=../../build python3 examples/retry.py
"""

import http.server
import json
import sys
import threading
import urllib.parse

sys.path.insert(0, str(__import__("pathlib").Path(__file__).parent.parent))
from apiexec import Stream, ApiExecError


class MockHandler(http.server.BaseHTTPRequestHandler):
    request_count = 0

    def do_GET(self):
        MockHandler.request_count += 1
        count = MockHandler.request_count

        if count <= 3:
            body = b'{"error":"rate limited"}'
            self.send_response(429)
            self.send_header("Retry-After", "0")
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        parsed = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(parsed.query)
        page = int(params.get("cursor", ["0"])[0])

        body = {
            "data": [{"id": page, "value": f"record_{page}"}],
            "next": str(page + 1) if page < 2 else None,
        }
        response = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)

    def log_message(self, *args):
        pass


server = http.server.HTTPServer(("127.0.0.1", 0), MockHandler)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()

config = json.dumps({"base_url": f"http://127.0.0.1:{port}"})
policy = json.dumps({"max_retries": 5, "base_backoff_ms": 10, "prefetch_depth": 0})

with Stream("generic_rest", config, policy) as s:
    pages = 0
    records = 0
    for batch_json, count in s:
        pages += 1
        records += count
        print(f"Page {pages}: {count} records ({len(batch_json)} bytes)")

print(f"\nResult: {pages} pages, {records} records")
print(f"Mock server served {MockHandler.request_count} requests (3 x 429 + 3 successes)")
print("The engine transparently retried past all 429s.")

server.shutdown()
