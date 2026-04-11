"""Integration test: stream paginated data from a local mock server.

Starts a simple HTTP server that returns paginated JSON in the format
GenericRestAdapter expects: {"data": [...], "next": "token"}.

Run: LD_LIBRARY_PATH=../../build python3 test_api.py
"""

import http.server
import json
import threading
import urllib.parse
from apiexec import Stream

TOTAL_PAGES = 5
RECORDS_PER_PAGE = 10


class MockHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(parsed.query)
        page = int(params.get("cursor", ["0"])[0])

        data = [{"id": page * RECORDS_PER_PAGE + i, "value": f"record_{page}_{i}"}
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

    def log_message(self, format, *args):
        pass  # suppress request logs


# Start mock server on a random port
server = http.server.HTTPServer(("127.0.0.1", 0), MockHandler)
port = server.server_address[1]
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()

print(f"Mock server running on port {port}")
print(f"Fetching {TOTAL_PAGES} pages x {RECORDS_PER_PAGE} records\n")

total_records = 0
pages = 0

with Stream("generic_rest",
            json.dumps({"base_url": f"http://127.0.0.1:{port}/api"}),
            '{"prefetch_depth": 1}') as s:
    for batch_json, count in s:
        pages += 1
        total_records += count
        print(f"  Page {pages}: {count} records ({len(batch_json)} bytes)")

print(f"\nTotal: {pages} pages, {total_records} records")
assert pages == TOTAL_PAGES, f"Expected {TOTAL_PAGES} pages, got {pages}"
assert total_records == TOTAL_PAGES * RECORDS_PER_PAGE, \
    f"Expected {TOTAL_PAGES * RECORDS_PER_PAGE} records, got {total_records}"
print("PASS")

server.shutdown()
