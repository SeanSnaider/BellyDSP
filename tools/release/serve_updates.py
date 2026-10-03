#!/usr/bin/env python3
"""A stand-in for GitHub's release downloads, for testing updates locally (update_e2e.sh). Standard
library only.

    serve_updates.py <root> --latest v0.1.1 [--port 8765]

It serves files under <root>/releases/download/<tag>/ and imitates GitHub's redirects, so the test
exercises the same path a real update takes:

    /releases/latest/download/<file>   302 -> /releases/download/<latest tag>/<file>   (the "latest release" link)
    /releases/download/<tag>/<file>    302 -> /objects/<tag>/<file>                     (GitHub sends assets to its CDN)
    /objects/<tag>/<file>              200, the file

Every request is printed (method, path, status) so the test log shows what the app fetched.
"""

import argparse
import http.server
import os
import sys
import urllib.parse


def make_handler(root, latest):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            sys.stdout.write("serve_updates: " + (fmt % args) + "\n")
            sys.stdout.flush()

        def redirect(self, location):
            self.send_response(302)
            self.send_header("Location", location)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def do_GET(self):
            path = urllib.parse.unquote(urllib.parse.urlparse(self.path).path)
            parts = [p for p in path.split("/") if p]
            if len(parts) == 4 and parts[:3] == ["releases", "latest", "download"]:
                return self.redirect(f"/releases/download/{latest}/{urllib.parse.quote(parts[3])}")
            if len(parts) == 4 and parts[:2] == ["releases", "download"]:
                return self.redirect(f"/objects/{urllib.parse.quote(parts[2])}/{urllib.parse.quote(parts[3])}")
            if len(parts) == 3 and parts[0] == "objects" and ".." not in parts:
                file = os.path.join(root, "releases", "download", parts[1], parts[2])
                if os.path.isfile(file):
                    with open(file, "rb") as f:
                        data = f.read()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/xml" if file.endswith(".xml") else "application/octet-stream")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                    return
            self.send_error(404)

    return Handler


def main():
    parser = argparse.ArgumentParser(description="Serve release files the way GitHub does")
    parser.add_argument("root")
    parser.add_argument("--latest", required=True, help="the tag /releases/latest/ points at, like v0.1.1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(os.path.abspath(args.root), args.latest))
    print(f"serve_updates: http://127.0.0.1:{args.port}/ (latest = {args.latest})", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
