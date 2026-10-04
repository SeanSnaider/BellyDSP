# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""A local stand-in for Hugging Face, for the separation installer's download tests
(tests/ToneMatchSeparationTests.cpp, with AMPSIM_HTTP_TESTS=1).

It serves one file (the pinned weights) the way huggingface.co does: the model URL answers 302 with a
Location on another host name (127.0.0.1 -> localhost) whose query string is long and percent-escaped
like the real signed CDN URL (about 1 KB, with %2F, %3D, %7E, and a literal ~). The CDN path refuses a
query that arrives changed (400), so a client that re-escapes the redirect fails visibly. Range requests
get 206. The first path segment picks how the server misbehaves:

  /ok/...        plain
Each distinct first segment keeps its own count (/flaky-a/ and /flaky-b/ are separate), so one server can
run several tests.

  /drop/...      a request from byte 0 gets the full Content-Length, 20 MB of body, then a closed socket
  /flaky/...     the first two requests to the model URL get 503 Service Unavailable
  /stall/...     the first CDN request waits 25 s before answering (longer than the old 20 s timeout)
  /missing/...   404 Not Found
  /corrupt/...   one byte of the body flipped (the SHA-256 check must catch it)

Usage: python tests/model_server.py --file <weights> --port-file <path>   (prints and writes the port)
"""

import argparse
import http.server
import os
import socketserver
import sys
import threading
import time
import urllib.parse

SIGNED_QUERY = (
    "xip=kCdUoZ9JLMc&user_id=public&X-Xet-Cas-Uid=public"
    "&response-content-disposition=inline%3B+filename*%3DUTF-8%27%275c90dfd2.safetensors%3B+filename%3D%225c90dfd2.safetensors%22%3B"
    "&x-amz-checksum-mode=ENABLED&X-Amz-Algorithm=AWS4-HMAC-SHA256"
    "&X-Amz-Credential=cas%2F20261004%2Fus-east-1%2Fs3%2Faws4_request&X-Amz-Date=20261004T151800Z&X-Amz-Expires=3600"
    "&X-Amz-Signature=" + "8f2c" * 16 + "&X-Amz-SignedHeaders=host"
    "&Policy=eyJTdGF0ZW1lbnQiOlt7IlJlc291cmNlIjoiaHR0cHM6Ly9jYXMtYnJpZGdlLnhldGh1Yi5oZi5jby94ZXQtYnJpZGdlLXVzLzZhNTI0ZDgyMTdlMzllYTUyYzliZWU5ZS80YTA4Y2E4MjMxZGE0YmQ5NDMzMTkxYTk1ZWU3MDBjYzhiYThhNjkzZTk4MGFjNWI0NDRmNjNlZmYzOGM4MDdlMSoiLCJDb25kaXRpb24iOnsiRGF0ZUxlc3NUaGFuIjp7IkFXUzpFcG9jaFRpbWUiOjE3OTEyMTc0ODB9fX1dfQ__"
    "&Signature=Ab~cD%7Eef" + "x9Y" * 40 + "&Key-Pair-Id=K2L8F4GPSG1IFC"
)

state = {}
lock = threading.Lock()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"  # one request per connection, so a dropped body ends the connection

    def log_message(self, fmt, *args):
        sys.stdout.write("[server] " + (fmt % args) + "\n")
        sys.stdout.flush()

    def do_GET(self):
        parts = self.path.split("?", 1)
        path, query = parts[0], (parts[1] if len(parts) > 1 else "")
        segments = path.strip("/").split("/")
        mode = segments[0] if segments else ""
        if len(segments) >= 2 and segments[1] == "resolve":
            return self.model_url(mode)
        if len(segments) >= 2 and segments[0] == "cdn":
            return self.cdn(segments[1], query)
        self.send_error(404)

    def model_url(self, mode):
        if mode.startswith("missing"):
            return self.send_error(404, "Entry not found")
        if mode.startswith("flaky"):
            with lock:
                state[mode] = state.get(mode, 0) + 1
                n = state[mode]
            if n <= 2:
                return self.send_error(503, "Service Unavailable")
        port = self.server.server_address[1]
        self.send_response(302)
        self.send_header("Location", f"http://localhost:{port}/cdn/{mode}/4a08ca8231da4bd9433191a95ee700cc?{SIGNED_QUERY}")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def cdn(self, mode, query):
        if query != SIGNED_QUERY:
            body = b"query mangled"
            self.send_response(400)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            sys.stdout.write("[server] query arrived changed:\n  " + query[:300] + "\n")
            return
        if mode.startswith("stall"):
            with lock:
                state[mode] = state.get(mode, 0) + 1
                n = state[mode]
            if n == 1:
                time.sleep(25.0)
        data = self.server.data
        start = 0
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes="):
            start = int(rng[len("bytes="):].split("-")[0])
        body = data[start:]
        if mode.startswith("corrupt"):
            body = bytearray(body)
            body[len(body) // 2] ^= 0xFF
            body = bytes(body)
        if start > 0:
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{len(data) - 1}/{len(data)}")
        else:
            self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        if mode.startswith("drop") and start == 0:
            self.wfile.write(body[: 20 * 1024 * 1024])
            self.wfile.flush()
            self.connection.shutdown(2)
            return
        self.wfile.write(body)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", required=True)
    ap.add_argument("--port-file", required=True)
    args = ap.parse_args()
    server = Server(("127.0.0.1", 0), Handler)
    with open(args.file, "rb") as f:
        server.data = f.read()
    port = server.server_address[1]
    tmp = args.port_file + ".tmp"
    with open(tmp, "w") as f:
        f.write(str(port))
    os.replace(tmp, args.port_file)
    print(f"[server] serving {args.file} on port {port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
