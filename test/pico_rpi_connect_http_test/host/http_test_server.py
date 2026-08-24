#!/usr/bin/env python3
# HTTP test server for pico_rpi_connect_http_test.
#
# Endpoints (matching the expectations in pico_rpi_connect_http_test.c):
#   GET  /get         fixed body plus an X-Test-Server response header
#   POST /echo        echoes the request body back
#   GET  /headers     returns the request headers as "Name: value" lines
#   GET  /status/<n>  responds with HTTP status <n> (204 sends no body)
#   GET  /data/<n>    <n> bytes where byte i is 'A' + (i % 26)
#   GET  /lowercase   raw response with a lowercase content-length header and
#                     the connection held open: completion depends on the
#                     client parsing the header case-insensitively

import argparse
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAX_DATA_LEN = 1024 * 1024


class TestHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    quiet = False

    def log_message(self, fmt, *args):
        if not self.quiet:
            super().log_message(fmt, *args)

    def send_body(self, body, status=200, headers=()):
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/get":
            self.send_body(b"pico-http-test-get\n",
                           headers=[("X-Test-Server", "pico-http-test")])
        elif self.path == "/headers":
            body = "".join(f"{name}: {value}\n" for name, value in self.headers.items())
            self.send_body(body.encode())
        elif self.path.startswith("/status/"):
            status = int(self.path[len("/status/"):])
            if status == 204:
                self.send_response(status)
                self.end_headers()
            else:
                self.send_body(f"status {status}\n".encode(), status=status)
        elif self.path.startswith("/data/"):
            length = int(self.path[len("/data/"):])
            if length > MAX_DATA_LEN:
                self.send_body(b"too large\n", status=400)
                return
            body = bytes(ord("A") + (i % 26) for i in range(length))
            self.send_body(body)
        elif self.path == "/lowercase":
            # Raw response with a lowercase content-length; keep the
            # connection open so the client cannot rely on close-to-complete.
            body = b"lowercase-content-length\n"
            self.wfile.write(b"HTTP/1.1 200 OK\r\n")
            self.wfile.write(b"content-type: text/plain\r\n")
            self.wfile.write(b"content-length: %d\r\n\r\n" % len(body))
            self.wfile.write(body)
            self.close_connection = False
        else:
            self.send_body(b"not found\n", status=404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        if self.path == "/echo":
            self.send_body(body)
        else:
            self.send_body(b"not found\n", status=404)


def main():
    parser = argparse.ArgumentParser(description="HTTP test server for pico_rpi_connect_http_test")
    parser.add_argument("-p", "--port", type=int, default=8080)
    parser.add_argument("-b", "--bind", default="0.0.0.0")
    parser.add_argument("-q", "--quiet", action="store_true", help="suppress request logging")
    args = parser.parse_args()

    TestHandler.quiet = args.quiet
    server = ThreadingHTTPServer((args.bind, args.port), TestHandler)
    print(f"Serving on {args.bind}:{args.port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
