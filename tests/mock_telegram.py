#!/usr/bin/env python3
"""Минимальный mock Telegram Bot API для tests/test_tg."""
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _json(self, payload):
        body = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/file/"):
            body = b"PDFDATA"
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b""
        method = self.path.rsplit("/", 1)[-1]

        if method == "sendMessage":
            self._json({"ok": True, "result": {"message_id": 42}})
        elif method == "sendDocument":
            self._json({"ok": True, "result": {"message_id": 43}})
        elif method == "editMessageText" or method == "answerCallbackQuery":
            self._json({"ok": True, "result": True})
        elif method == "getFile":
            self._json({"ok": True, "result": {"file_path": "docs/file.pdf"}})
        elif method == "getUpdates":
            self._json({
                "ok": True,
                "result": [{
                    "update_id": 7,
                    "message": {
                        "message_id": 1,
                        "chat": {"id": 42, "type": "private"},
                        "from": {"id": 1},
                        "text": "/start",
                    },
                }],
            })
        else:
            self._json({"ok": False, "description": "unknown method"})


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    server = HTTPServer(("127.0.0.1", port), Handler)
    print(server.server_port, flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
