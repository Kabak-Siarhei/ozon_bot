# syntax=docker/dockerfile:1
# Сборка Telegram-бота ozon_bot для Render (runtime: Docker).
# В образ включена C-утилита ttn_add_field (MuPDF) — без зависимостей от Python.

# ---------- Этап 1: сборка MuPDF (статические библиотеки) ----------
FROM debian:bookworm-slim AS mupdf-builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential pkg-config wget ca-certificates libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp
# Собираем MuPDF с встроенными thirdparty-зависимостями (без X11/GLUT).
# Результат: libmupdf.a + libmupdf-third.a в /usr/local/lib.
RUN wget -q https://github.com/ArtifexSoftware/mupdf/archive/refs/tags/1.23.2.tar.gz \
    && tar xzf 1.23.2.tar.gz \
    && cd mupdf-1.23.2 \
    && make -j"$(nproc)" build=release HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no mupdf \
    && make install build=release prefix=/usr/local

# ---------- Этап 2: сборка Rust-бота и C-утилиты ----------
FROM rust:1-slim-bookworm AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        pkg-config libssl-dev build-essential \
    && rm -rf /var/lib/apt/lists/*

# Статические библиотеки MuPDF (заголовки + .a)
COPY --from=mupdf-builder /usr/local/include /usr/local/include
COPY --from=mupdf-builder /usr/local/lib /usr/local/lib

WORKDIR /app

# Сначала только манифесты — слой с зависимостями кэшируется,
# пока не меняются Cargo.toml / Cargo.lock.
COPY Cargo.toml Cargo.lock ./
RUN mkdir -p src \
    && echo 'fn main() {}' > src/main.rs \
    && cargo build --release --locked \
       --config 'profile.release.lto="off"' \
       --config 'profile.release.codegen-units=16' \
    && rm -rf src

# Затем исходники: пересобирается только наш крейт, зависимости из кэша.
COPY src ./src
RUN cargo build --release --locked \
    --config 'profile.release.lto="off"' \
    --config 'profile.release.codegen-units=16'

# C-утилита ttn_add_field (MuPDF, без Python).
# Статическая линковка (libmupdf.a + libmupdf-third.a) — в runtime-образ
# не нужно тащить библиотеки MuPDF и их зависимости.
COPY ttn_add_field.c ttn_config.c ttn_config.h ./
RUN cc -std=c11 -Wall -Wextra -O2 -I/usr/local/include \
       -o ttn_add_field ttn_add_field.c ttn_config.c \
       -L/usr/local/lib -lmupdf -lmupdf-third \
       -static -lm -lpthread -ldl \
    && test -x ttn_add_field

# ---------- Этап 3: исполнение ----------
FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates libssl3 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --uid 1001 app

WORKDIR /app
COPY --from=builder /app/target/release/ozon_bot /usr/local/bin/ozon_bot
COPY --from=builder /app/ttn_add_field /usr/local/bin/ttn_add_field

# Файловая система Render эфемерна: /tmp живёт до рестарта/деплоя.
ENV BOT_DB=/tmp/bot_settings.db
ENV TTN_BIN_PATH=/usr/local/bin/ttn_add_field
# TELEGRAM_BOT_TOKEN задаётся в дашборде Render (см. render.yaml).

USER app

# Обёртка — детектор диагностики:
#  * строка "[wrapper]" в логах означает, что контейнер реально выполнил
#    команду и stdout доходит до логов Render;
#  * её отсутствие при "Application exited early" = падение на уровне
#    запуска (путь/команда), а не внутри бота;
#  * `exec` передаёт сигналы (SIGTERM при рестарте) самому боту.
CMD ["/bin/sh", "-c", "echo \"[wrapper] запуск ozon_bot, PATH=$PATH\"; exec /usr/local/bin/ozon_bot"]
