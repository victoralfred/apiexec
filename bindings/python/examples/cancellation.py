"""Example: cancelling an in-progress stream from another thread.

cancel() is thread-safe. A watchdog thread can cancel the stream based
on a timeout; the main thread sees the cancellation on its next iteration.

Run: LD_LIBRARY_PATH=../../build python3 examples/cancellation.py
"""

import http.server
import json
import sys
import threading
import time
import urllib.parse

sys.path.insert(0, str(__import__("pathlib").Path(__file__).parent.parent))
from apiexec import Stream, ApiExecError


class MockHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        time.sleep(0.01)

        parsed = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(parsed.query)
        page = int(params.get("cursor", ["0"])[0])

        # Infinite stream
        body = {"data": [{"id": page}], "next": str(page + 1)}
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
stream = Stream("generic_rest", config, '{"prefetch_depth": 0}')


def watchdog():
    time.sleep(0.2)
    print("\n[watchdog] cancelling stream")
    stream.cancel()


threading.Thread(target=watchdog, daemon=True).start()

count = 0
try:
    for _ in stream:
        count += 1
except ApiExecError as e:
    if e.code == -6:  # CANCELLED
        print(f"Stream cancelled cleanly after {count} batches")
    else:
        print(f"Error: {e}")

stream.close()
server.shutdown()
