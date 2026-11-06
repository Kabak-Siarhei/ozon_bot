# syntax=docker/dockerfile:1
# Сборка Telegram-бота ozon_bot для Render (runtime: Docker).
# В образ включена C-утилита ttn_add_field (MuPDF) — без зависимостей от Python.

# ---------- Этап 1: сборка MuPDF ----------
FROM debian:bookworm-slim AS mupdf-builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config wget ca-certificates \
        zlib1g-dev libjpeg62-turbo-dev libfreetype6-dev libharfbuzz-dev \
        libjbig2dec0-dev libopenjp2-7-dev libssl-dev liblcms2-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp
RUN wget -q https://github.com/ArtifexSoftware/mupdf/archive/refs/tags/mupdf-1.23.2.tar.gz \
    && tar xzf mupdf-1.23.2.tar.gz \
    && cd mupdf-mupdf-1.23.2 \
    && make -j"$(nproc)" build=release prefix=/usr/local shared=yes mupdf \
    && make install build=release prefix=/usr/local shared=yes

# ---------- Этап 2: сборка Rust-бота и C-утилиты ----------
FROM rust:1-slim-bookworm AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        pkg-config libssl-dev \
        build-essential zlib1g-dev libjpeg62-turbo-dev libfreetype6-dev \
        libharfbuzz-dev libjbig2dec0-dev libopenjp2-7-dev liblcms2-dev \
    && rm -rf /var/lib/apt/lists/*

# Заголовки и библиотека MuPDF из первого этапа
COPY --from=mupdf-builder /usr/local/include /usr/local/include
COPY --from=mupdf-builder /usr/local/lib /usr/local/lib
RUN ldconfig

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

# C-утилита ttn_add_field (MuPDF, без Python)
COPY ttn_add_field.c ttn_config.c ttn_config.h Makefile.c_ttn ./
RUN cc -std=c11 -Wall -Wextra -O2 -I/usr/local/include \
       -o ttn_add_field ttn_add_field.c ttn_config.c \
       -L/usr/local/lib -lmupdf -lm -lz

# ---------- Этап 3: исполнение ----------
FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates libssl3 \
        zlib1g libjpeg62-turbo libfreetype6 libharfbuzz0b \
        libjbig2dec0 libopenjp2-7 liblcms2-2 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --uid 1001 app

# Разделяемая библиотека MuPDF
COPY --from=mupdf-builder /usr/local/lib/libmupdf*.so* /usr/local/lib/
RUN ldconfig

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
