# ozon_bot (C)

Telegram-бот для заполнения PDF-документов ТТН. Полностью на **C11**:

- Telegram Bot API — long polling через **libcurl** + **cJSON**;
- настройки полей — **SQLite** (per `chat_id`);
- заполнение AcroForm — **MuPDF** (`ttn_add_field.c`);
- один исполняемый файл, без Rust, Python и subprocess-вызовов.

## Возможности

- `/start` — справка.
- `/settings` — меню из 5 отдельных полей ТТН с кнопками «изменить» и «✖ очистить».
- Значений по умолчанию нет: пустое поле остаётся пустым.
- Значения хранятся отдельно для каждого чата/пользователя.
- Пользователь отправляет PDF — бот возвращает `processed_<имя>.pdf`
  с заполненными полями.
- Кириллица в значениях (UTF-16BE + BOM), поля AcroForm с корректным `/Rect`.
- Если задан `PORT` (Render Web Service) — поднимается HTTP health-сервер
  `GET / -> 200 ok`, чтобы проходил port scan. Для Background Worker порт
  не нужен и сервер не стартует.

## Архитектура

```
bot.c            long polling, команды, кнопки, приём/отправка PDF
telegram.c/.h    Bot API: JSON-методы, multipart sendDocument, getFile/download
db.c/.h          SQLite: settings (chat_id, field, value) + pending
ui.c/.h          тексты и inline-клавиатура
ttn_add_field.c/.h   заполнение AcroForm через MuPDF (+ CLI)
ttn_config.c/.h  чтение INI (для CLI)
cJSON.c/.h       вендоренный JSON-парсер (MIT)
```

## Зависимости

- `libcurl` (TLS, multipart)
- `sqlite3`
- MuPDF (`libmupdf.a` + `libmupdf-third.a` или `libmupdf.dylib`)
- `pkg-config`, компилятор C11

## Переменные окружения

| Переменная | Назначение | По умолчанию |
|------------|------------|--------------|
| `TELEGRAM_BOT_TOKEN` | токен Bot API | обязателен (или `TELOXIDE_TOKEN`) |
| `BOT_DB` | путь к SQLite | `bot_settings.db` |
| `PORT` | если задан — включить health-сервер | не задан |
| `TELEGRAM_API_BASE` | подмена API (тесты/локальный Bot API) | `https://api.telegram.org` |

`.env` читается автоматически при наличии.

## Сборка

### Linux (как в Docker)

```bash
# Debian/Ubuntu
sudo apt-get install build-essential pkg-config \
    libcurl4-openssl-dev libsqlite3-dev libssl-dev zlib1g-dev libmupdf-dev

make
```

Если MuPDF собран из исходников в `/usr/local`:

```bash
make MUPDF_INC=/usr/local/include MUPDF_LIB=/usr/local/lib
```

### macOS (локальный тест с MuPDF из PyMuPDF)

```bash
MUPDF_INC="$HOME/development/local-llm-pdf-ocr/.venv/lib/python3.12/site-packages/pymupdf/mupdf-devel/include"
MUPDF_LIB="$HOME/development/local-llm-pdf-ocr/.venv/lib/python3.12/site-packages/pymupdf"

make MUPDF_INC="$MUPDF_INC" MUPDF_LIB="$MUPDF_LIB" \
     MUPDF_LIBS="$MUPDF_LIB/libmupdf.dylib" \
     SYS_LIBS="-lpthread -lz" \
     EXTRA_LIBS="-framework CoreGraphics -framework CoreText -framework CoreFoundation -undefined dynamic_lookup"

DYLD_LIBRARY_PATH="$MUPDF_LIB" ./ozon_bot
```

## Запуск

```bash
export TELEGRAM_BOT_TOKEN="123456:ABC..."
export BOT_DB="/tmp/bot_settings.db"
./ozon_bot
```

CLI-версия для одного PDF (без Telegram):

```bash
./ttn_add_field tests/fixtures/ttn.pdf /tmp/out.pdf -c settings.example.ini
```

## Тесты

```bash
# SQLite + UI + заполнение PDF
make test-core

# Telegram-клиент против mock Bot API
make test-tg

# Полный сценарий: кнопка -> ввод -> PDF -> sendDocument
./tests/run_bot_test.sh
```

Для macOS перед тестами:

```bash
export DYLD_LIBRARY_PATH="$MUPDF_LIB"
export PYTHON="$HOME/development/local-llm-pdf-ocr/.venv/bin/python3"  # нужен PyMuPDF для проверки
export MUPDF_DYLIB_DIR="$MUPDF_LIB"
```

## Docker / Render

```bash
docker build -t ozon-ttn-bot .
docker run --rm -e TELEGRAM_BOT_TOKEN="..." ozon-ttn-bot
```

`Dockerfile` собирает MuPDF из официального source-tarball (static),
затем C-бота; runtime содержит только `libcurl4`, `libsqlite3-0`,
`libssl3`, `zlib1g`, `libstdc++6`.

`render.yaml` описывает Background Worker. Если сервис создан как
**Web Service**, health-сервер автоматически слушает `PORT`; но на free-плане
web service засыпает без входящего трафика — нужен внешний пинг или Worker.

## Ограничения

- `getFile` Bot API — до 20 МБ (проверяется до скачивания).
- Отправка документа — до 50 МБ.
- SQLite на Render эфемерна (`/tmp`): настройки сбрасываются при рестарте.
  Для постоянного хранения подключите диск или внешнюю БД.

## Serverless-вариант

В соседней папке `../tg_serverless` лежит версия для Telegram Serverless
(JavaScript + pdf-lib), если Telegram включит доступ. Она не использует C/MuPDF
и деплоится через `tgcloud`.
