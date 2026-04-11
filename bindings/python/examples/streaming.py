"""Example: streaming batches with prefetch for I/O overlap.

Demonstrates the iterator protocol with prefetch enabled. The next batch
is fetched in the background while Python code processes the current one,
halving wall-clock time compared to sequential mode.

Run: LD_LIBRARY_PATH=../../build python3 examples/streaming.py
"""

import http.server
import json
import sys
import threading
import time
import urllib.parse

sys.path.insert(0, str(__import__("pathlib").Path(__file__).parent.parent))
from apiexec import Stream

TOTAL_PAGES = 10
RECORDS_PER_PAGE = 5


class MockHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        time.sleep(0.02)  # simulate 20ms API latency

        parsed = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(parsed.query)
        page = int(params.get("cursor", ["0"])[0])

        data = [{"id": page * RECORDS_PER_PAGE + i, "value": f"r_{page}_{i}"}
                for i in range(RECORDS_PER_PAGE)]
        body = {
            "data": data,
            "next": str(page + 1) if page + 1 < TOTAL_PAGES else None,
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
# Prefetch enabled for I/O overlap
policy = json.dumps({"prefetch_depth": 1})

start = time.time()
batches = 0
total = 0

with Stream("generic_rest", config, policy) as s:
    for batch_json, count in s:
        batches += 1
        total += count
        # Simulate 20ms processing — overlapped with background prefetch
        time.sleep(0.02)
        print(f"Batch {batches}: {count} records")

elapsed = time.time() - start
print(f"\nProcessed {batches} batches, {total} records in {elapsed:.3f}s")
print("With prefetch, ~max(fetch, process) x N, not fetch + process.")

server.shutdown()
