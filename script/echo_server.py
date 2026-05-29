#!/usr/bin/env python3
"""Tiny HTTP echo server for testing RestClient's POST path.

POST  -> reflects the request body back verbatim (200, same Content-Type).
GET   -> returns a short plaintext confirmation.

HTTP/1.0 so the connection closes after each response, matching RestClient's
v1 `Connection: close` / read-until-EOF framing.

Run:  python3 script/echo_server.py        # listens on 127.0.0.1:8900
"""

from http.server import BaseHTTPRequestHandler, HTTPServer

HOST, PORT = "127.0.0.1", 8900


class EchoHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"  # close after each response

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        ctype = self.headers.get("Content-Type", "application/octet-stream")
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        msg = b"GET ok\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(msg)))
        self.end_headers()
        self.wfile.write(msg)


if __name__ == "__main__":
    print(f"echo server on http://{HOST}:{PORT}  (Ctrl-C to stop)")
    HTTPServer((HOST, PORT), EchoHandler).serve_forever()
