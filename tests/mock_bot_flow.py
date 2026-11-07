#!/usr/bin/env python3
"""Mock Telegram Bot API для полного сценария C-бота.

Сценарий: нажатие «Автомобиль» -> ввод значения -> отправка PDF.
Сохраняет полученный sendDocument в SENT_FILE, логирует методы в LOG_FILE.
"""
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

FIXTURE = os.environ.get(
    "TTN_FIXTURE",
    "/Users/rayan/development/ozon_pdf/ozon_bot/tests/fixtures/ttn.pdf",
)
SENT_FILE = os.environ.get("SENT_FILE", "/tmp/ozon_bot_sent.pdf")
LOG_FILE = os.environ.get("LOG_FILE", "/tmp/ozon_bot_calls.jsonl")

UPDATES = [
    {
        "update_id": 1,
        "callback_query": {
            "id": "cb1",
            "from": {"id": 1},
            "data": "set:Водитель",
            "message": {
                "message_id": 5,
                "chat": {"id": 42, "type": "private"},
            },
        },
    },
    {
        "update_id": 2,
        "message": {
            "message_id": 6,
            "chat": {"id": 42, "type": "private"},
            "from": {"id": 1},
            "text": "Иванов Иван Иванович",
        },
    },
    {
        "update_id": 3,
        "message": {
            "message_id": 7,
            "chat": {"id": 42, "type": "private"},
            "from": {"id": 1},
            "document": {
                "file_id": "F1",
                "file_name": "ttn.pdf",
                "mime_type": "application/pdf",
                "file_size": os.path.getsize(FIXTURE),
            },
        },
    },
]

served = False


def log_call(method, body):
    with open(LOG_FILE, "a", encoding="utf-8") as f:
        f.write(json.dumps({"method": method, "body": body}, ensure_ascii=False) + "\n")


def save_multipart_document(body, content_type):
    boundary = content_type.split("boundary=", 1)[-1].strip().strip('"')
    marker = ("--" + boundary).encode()
    for part in body.split(marker):
        if b'name="document"' not in part:
            continue
        header_end = part.find(b"\r\n\r\n")
        if header_end < 0:
            continue
        content = part[header_end + 4:]
        if content.endswith(b"\r\n"):
            content = content[:-2]
        with open(SENT_FILE, "wb") as f:
            f.write(content)
        return True
    return False


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _json(self, payload):
        body = json.dumps(payload, ensure_ascii=False).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/file/"):
            with open(FIXTURE, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        global served
        method = self.path.rsplit("/", 1)[-1]
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b""

        if method == "sendDocument":
            ok = save_multipart_document(raw, self.headers.get("Content-Type", ""))
            log_call(method, {"saved": ok})
            self._json({"ok": True, "result": {"message_id": 43}})
            return

        try:
            body = json.loads(raw.decode()) if raw else {}
        except Exception:
            body = {"raw": True}
        log_call(method, body)

        if method == "getUpdates":
            if not served:
                served = True
                self._json({"ok": True, "result": UPDATES})
            else:
                time.sleep(1)
                self._json({"ok": True, "result": []})
        elif method == "sendMessage":
            self._json({"ok": True, "result": {"message_id": 100}})
        elif method == "getFile":
            self._json({"ok": True, "result": {"file_path": "docs/ttn.pdf"}})
        else:
            self._json({"ok": True, "result": True})


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    for path in (SENT_FILE, LOG_FILE):
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
    server = HTTPServer(("127.0.0.1", port), Handler)
    print(server.server_port, flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
