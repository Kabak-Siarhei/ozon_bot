# syntax=docker/dockerfile:1
# C-версия Telegram-бота: libcurl + SQLite + MuPDF. Без Rust и Python.

# ---------- Этап 1: статические библиотеки MuPDF ----------
FROM debian:bookworm-slim AS mupdf-builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential pkg-config wget ca-certificates libssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp
# Официальный source-tarball содержит bundled thirdparty.
RUN wget -q https://mupdf.com/downloads/archive/mupdf-1.23.2-source.tar.gz \
    && tar xzf mupdf-1.23.2-source.tar.gz \
    && cd mupdf-1.23.2-source \
    && make -j"$(nproc)" build=release HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no USE_SYSTEM_ZLIB=yes libs \
    && make build=release HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no USE_SYSTEM_ZLIB=yes \
            prefix=/usr/local install-libs

# ---------- Этап 2: сборка C-бота ----------
FROM debian:bookworm-slim AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential pkg-config \
        libcurl4-openssl-dev libsqlite3-dev libssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

COPY --from=mupdf-builder /usr/local/include /usr/local/include
COPY --from=mupdf-builder /usr/local/lib /usr/local/lib

WORKDIR /app
COPY *.c *.h Makefile ./
RUN make ozon_bot \
        MUPDF_INC=/usr/local/include \
        MUPDF_LIB=/usr/local/lib \
        SYS_LIBS="-lpthread -ldl -lz -lcrypto -lstdc++" \
    && test -x ozon_bot

# ---------- Этап 3: runtime ----------
FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates libcurl4 libsqlite3-0 libssl3 zlib1g libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --uid 1001 app

WORKDIR /app
COPY --from=builder /app/ozon_bot /usr/local/bin/ozon_bot

# Файловая система Render эфемерна: /tmp живёт до рестарта/деплоя.
ENV BOT_DB=/tmp/bot_settings.db
# TELEGRAM_BOT_TOKEN задаётся в дашборде Render (см. render.yaml).

USER app

# Обёртка — детектор диагностики (если [wrapper] нет в логах, падение
# произошло на уровне запуска контейнера, а не внутри бота).
CMD ["/bin/sh", "-c", "echo \"[wrapper] запуск ozon_bot, PATH=$PATH\"; exec /usr/local/bin/ozon_bot"]
