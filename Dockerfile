# syntax=docker/dockerfile:1
# Сборка Telegram-бота ozon_bot для Render (runtime: Docker).

# ---------- Этап сборки ----------
# `rust:1-slim-bookworm` — последний стабильный Rust 1.x на Debian bookworm.
FROM rust:1-slim-bookworm AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends pkg-config libssl-dev \
    && rm -rf /var/lib/apt/lists/*

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

# ---------- Этап исполнения ----------
FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates libssl3 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --uid 1001 app

WORKDIR /app
COPY --from=builder /app/target/release/ozon_bot /usr/local/bin/ozon_bot

# Файловая система Render эфемерна: /tmp живёт до рестарта/деплоя.
ENV BOT_DB=/tmp/bot_settings.db
# TELEGRAM_BOT_TOKEN задаётся в дашборде Render (см. render.yaml).

USER app

# Обёртка — детектор диагностики:
#  * строка "[wrapper]" в логах означает, что контейнер реально выполнил
#    команду и stdout доходит до логов Render;
#  * её отсутствие при "Application exited early" = падение на уровне
#    запуска (путь/команда), а не внутри бота;
#  * `exec` передаёт сигналы (SIGTERM при рестарте) самому боту.
CMD ["/bin/sh", "-c", "echo \"[wrapper] запуск ozon_bot, PATH=$PATH\"; exec /usr/local/bin/ozon_bot"]
