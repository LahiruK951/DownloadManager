"""Throttled HTTP server with Range support, for manually testing FluxDM.

    python scripts/range_server.py <directory> [--port 8765] [--rate-kbps 2048] [--no-range]

Every connection is limited to --rate-kbps, so multi-connection downloads are
visibly faster, and there is time to pause / kill the app mid-download.
"""
import argparse
import os
import re
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

RANGE_RE = re.compile(r"bytes=(\d*)-(\d*)")


class Handler(SimpleHTTPRequestHandler):
    rate = 2048 * 1024
    ranges = True
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        path = self.translate_path(self.path)
        if not os.path.isfile(path):
            self.send_error(404)
            return
        size = os.path.getsize(path)
        start, end = 0, size - 1
        m = RANGE_RE.match(self.headers.get("Range", "")) if self.ranges else None
        if m and (m.group(1) or m.group(2)):
            if m.group(1):
                start = int(m.group(1))
                if m.group(2):
                    end = min(int(m.group(2)), size - 1)
            else:  # suffix range: last N bytes
                start = max(0, size - int(m.group(2)))
            if start > end:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        else:
            self.send_response(200)
        if self.ranges:
            self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Connection", "close")
        self.end_headers()

        chunk = 16 * 1024
        delay = chunk / self.rate
        with open(path, "rb") as f:
            f.seek(start)
            left = end - start + 1
            try:
                while left > 0:
                    data = f.read(min(chunk, left))
                    if not data:
                        break
                    self.wfile.write(data)
                    left -= len(data)
                    time.sleep(delay)
            except (ConnectionResetError, BrokenPipeError, ConnectionAbortedError):
                pass
        self.close_connection = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("directory")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--rate-kbps", type=int, default=2048, help="per-connection limit")
    ap.add_argument("--no-range", action="store_true")
    a = ap.parse_args()
    Handler.rate = a.rate_kbps * 1024
    Handler.ranges = not a.no_range
    os.chdir(a.directory)
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"serving {os.getcwd()} on http://127.0.0.1:{a.port} ({a.rate_kbps} KB/s per connection)", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
