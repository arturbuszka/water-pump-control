#!/usr/bin/env python3
"""Work on the phone UI without reflashing.

Serves web/index.html straight from disk and forwards /api/* to a real board,
so an edit shows up on the next browser refresh, running against live data.
Flash only once the page is done.

    tools/dev_server.py http://192.168.1.42        # then open http://localhost:8000
    tools/dev_server.py http://waterpump.local --port 8080

Standard library only. Open it from the phone too, via the computer's LAN IP
(--host 0.0.0.0).
"""

import argparse
import http.server
import pathlib
import urllib.error
import urllib.request

PAGE = pathlib.Path(__file__).resolve().parent.parent / "web" / "index.html"


def make_handler(board):
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path.startswith("/api/"):
                self.forward()
            elif self.path in ("/", "/index.html"):
                # Read on every request - that is the point of this server.
                self.reply(200, "text/html; charset=utf-8", PAGE.read_bytes())
            else:
                self.reply(404, "text/plain", b"not found")

        def do_POST(self):
            if self.path.startswith("/api/"):
                self.forward()
            else:
                self.reply(404, "text/plain", b"not found")

        def forward(self):
            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length) if length else None
            headers = {}
            if self.headers.get("Content-Type"):
                # Passed through as-is, so the board's Content-Type check is
                # exercised exactly as from the real page.
                headers["Content-Type"] = self.headers["Content-Type"]
            req = urllib.request.Request(board + self.path, data=body, headers=headers,
                                         method=self.command)
            try:
                with urllib.request.urlopen(req, timeout=8) as resp:
                    self.reply(resp.status, resp.headers.get("Content-Type", ""), resp.read())
            except urllib.error.HTTPError as e:
                # The board's 4xx answers carry the JSON error the page shows.
                self.reply(e.code, e.headers.get("Content-Type", ""), e.read())
            except OSError as e:
                self.reply(502, "application/json",
                           ('{"ok":false,"error":"board unreachable: %s"}' % e).encode())

        def reply(self, status, content_type, data):
            self.send_response(status)
            if content_type:
                self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("board", help="board base URL, e.g. http://192.168.1.42")
    ap.add_argument("--host", default="127.0.0.1", help="address to listen on (default 127.0.0.1)")
    ap.add_argument("--port", type=int, default=8000)
    args = ap.parse_args()

    server = http.server.ThreadingHTTPServer((args.host, args.port),
                                             make_handler(args.board.rstrip("/")))
    print(f"serving {PAGE} at http://{args.host}:{args.port}, API -> {args.board}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
