#!/usr/bin/env bash
# Запускает mock Telegram API и tests/test_tg против него.
set -euo pipefail

cd "$(dirname "$0")/.."

python3 tests/mock_telegram.py 0 > /tmp/ozon_mock_port.txt &
MOCK_PID=$!
trap 'kill "$MOCK_PID" 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
  if [ -s /tmp/ozon_mock_port.txt ]; then break; fi
  sleep 0.1
done

PORT=$(head -n1 /tmp/ozon_mock_port.txt)
if [ -z "$PORT" ]; then
  echo "mock server did not start" >&2
  exit 1
fi

TELEGRAM_API_BASE="http://127.0.0.1:$PORT" ./test_tg
