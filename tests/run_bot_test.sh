#!/usr/bin/env bash
# Полный интеграционный тест: C-бот против mock Telegram.
set -euo pipefail

cd "$(dirname "$0")/.."

# macOS SIP вырезает унаследованный DYLD_LIBRARY_PATH у shell-скриптов,
# поэтому выставляем его уже внутри скрипта (для локального теста с PyMuPDF).
if [ -n "${MUPDF_DYLIB_DIR:-}" ]; then
  export DYLD_LIBRARY_PATH="$MUPDF_DYLIB_DIR${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
fi

export TTN_FIXTURE="tests/fixtures/ttn.pdf"
export SENT_FILE="/tmp/ozon_bot_sent.pdf"
export LOG_FILE="/tmp/ozon_bot_calls.jsonl"
export BOT_DB="/tmp/ozon_bot_flow.db"
rm -f "$BOT_DB" "$SENT_FILE" "$LOG_FILE"

${PYTHON:-python3} tests/mock_bot_flow.py 0 > /tmp/ozon_bot_port.txt &
MOCK_PID=$!
BOT_PID=""
cleanup() {
  [ -n "$BOT_PID" ] && kill "$BOT_PID" 2>/dev/null || true
  kill "$MOCK_PID" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 50); do
  [ -s /tmp/ozon_bot_port.txt ] && break
  sleep 0.1
done
PORT=$(head -n1 /tmp/ozon_bot_port.txt)
[ -n "$PORT" ] || { echo "mock server did not start" >&2; exit 1; }

TELEGRAM_BOT_TOKEN="123:test" \
TELEGRAM_API_BASE="http://127.0.0.1:$PORT" \
BOT_DB="$BOT_DB" \
./ozon_bot > /tmp/ozon_bot_flow.log 2>&1 &
BOT_PID=$!

for _ in $(seq 1 100); do
  [ -s "$SENT_FILE" ] && break
  sleep 0.1
done

if [ ! -s "$SENT_FILE" ]; then
  echo "FAIL: bot did not send document" >&2
  cat /tmp/ozon_bot_flow.log >&2
  exit 1
fi

${PYTHON:-python3} - << 'PY'
import json
import os
import sqlite3
import fitz

db = sqlite3.connect(os.environ["BOT_DB"])
for field in ("Водитель", "Товар к перевозке принял"):
    row = db.execute(
        "SELECT value FROM settings WHERE chat_id=42 AND field=?", (field,)
    ).fetchone()
    assert row and row[0] == "Иванов Иван Иванович", (field, row)
print("ok: grouped settings saved to SQLite for both fields")

calls = [json.loads(line)["method"] for line in open(os.environ["LOG_FILE"], encoding="utf-8")]
for method in ("answerCallbackQuery", "sendMessage", "getFile", "sendDocument", "editMessageText"):
    assert method in calls, (method, calls)
print("ok: Bot API calls:", ", ".join(dict.fromkeys(calls)))

doc = fitz.open(os.environ["SENT_FILE"])
widgets = {w.field_name: w.field_value for w in doc[0].widgets()}
assert len(widgets) == 5, widgets
assert widgets["Водитель"] == "Иванов Иван Иванович", widgets
assert widgets["Товар к перевозке принял"] == "Иванов Иван Иванович", widgets
assert widgets["Автомобиль"] == "", widgets
assert widgets["Сдал грузоотправитель"] == "", widgets
print("ok: returned PDF has grouped values and no defaults")
PY

echo "ALL OK"
