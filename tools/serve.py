#!/usr/bin/env python3
"""A test source over HTTPS, with Range requests (docs/client.md §6).

    tools/serve.py <dir> <cert.pem> <key.pem> [--port 8443] [--bind 0.0.0.0]
                   [--redirect PREFIX=URL] [--drop-after BYTES]

Serves <dir> as the source's root. --redirect answers paths under PREFIX
with a 302 to URL + the rest (as GitHub does for downloads);
--drop-after closes each connection after that many body bytes, to test
resuming. For tests only.
"""
import argparse
import http.server
import os
import ssl


def handler(root, redirects, drop_after):
    class H(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **k):
            super().__init__(*a, directory=root, **k)

        def do_GET(self):
            for prefix, url in redirects:
                if self.path.startswith(prefix):
                    self.send_response(302)
                    self.send_header("Location", url + self.path[len(prefix):])
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
            path = self.translate_path(self.path)
            if not os.path.isfile(path):
                self.send_error(404)
                return
            size = os.path.getsize(path)
            start, end, status = 0, size - 1, 200
            rng = self.headers.get("Range", "")
            if rng.startswith("bytes="):
                a, _, b = rng[6:].partition("-")
                start = int(a) if a else 0
                end = int(b) if b else size - 1
                if start >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", "bytes */%d" % size)
                    self.end_headers()
                    return
                status = 206
            self.send_response(status)
            self.send_header("Content-Length", str(end - start + 1))
            self.send_header("Accept-Ranges", "bytes")
            if status == 206:
                self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
            self.end_headers()
            with open(path, "rb") as f:
                f.seek(start)
                left = end - start + 1
                if drop_after:
                    left = min(left, drop_after)
                while left > 0:
                    chunk = f.read(min(65536, left))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    left -= len(chunk)
            if drop_after:
                self.close_connection = True
    return H


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("cert")
    ap.add_argument("key")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--redirect", action="append", default=[])
    ap.add_argument("--drop-after", type=int, default=0)
    a = ap.parse_args()
    redirects = [tuple(r.split("=", 1)) for r in a.redirect]
    srv = http.server.ThreadingHTTPServer((a.bind, a.port), handler(os.path.abspath(a.dir), redirects, a.drop_after))
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(a.cert, a.key)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print("serving %s on https://%s:%d/" % (a.dir, a.bind, a.port), flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
