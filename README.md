# ozon_bot

Telegram-бот — клон веб-сервера `ozon`: обрабатывает PDF-документы (ТТН)
и добавляет AcroForm-поля с настраиваемыми значениями.

## Настройка

1. Получите токен у [@BotFather](https://t.me/BotFather).
2. Скопируйте конфиг и впишите токен:
   ```bash
   cp .env.example .env
   ```
3. Запустите:
   ```bash
   cargo run
   ```

## Команды

| Команда     | Назначение                                        |
|-------------|---------------------------------------------------|
| `/start`    | Справка                                           |
| `/settings` | Настройка значений полей (кнопками в чате)       |
| `/cancel`   | Отменить ввод значения                            |

Настраиваемые поля: **Автомобиль**, **Водитель**, **Отпуск разрешил**,
**Сдал грузоотправитель**, **Товар к перевозке принял**.

## Работа

1. `/settings` → нажать поле → прислать новое значение текстом
   (кнопка `✖` очищает значение).
2. Отправить PDF-документ → бот вернёт тот же файл с заполненными полями.

Настройки хранятся в SQLite (`bot_settings.db`), отдельно для каждого чата.

## Тесты

```bash
cargo test
```

Покрывают логику PDF (5 полей, дефолты, кастомные значения, очистка)
и базу настроек (CRUD, изоляция по чатам) —7 тестов.

## Деплой на Render (free)

Проект готов к Render: есть `Dockerfile` и `render.yaml` (blueprint).

1. Залейте репозиторий на GitHub.
2. В Render: **New → Background Worker** → подключите репозиторий.
   Имено **Worker**, а не Web Service: бот работает через long polling
   (исходящие запросы), а Web Service на free засыпал бы после простоя.
3. Render подхватит `render.yaml` (runtime Docker, plan free).
4. В **Environment** впишите `TELEGRAM_BOT_TOKEN` (в yaml он помечен
   `sync: false`, в репозитории его нет).
5. **Deploy** — в логах появится «Бот запущен (база: /tmp/bot_settings.db)».

Проверка локальной сборки образа (если есть Docker):

```bash
docker build -t ozon_bot .
docker run --rm -e TELEGRAM_BOT_TOKEN=... ozon_bot
```

### Нюансы free-плана

- **Настройки полей эфемерны**: база лежит в `/tmp` и стирается при каждом
  рестарте/деплое — persistent disks на free недоступны. После деплоя
  заново задайте значения через `/settings`.
- **Сборка**: LTO в Dockerfile отключён (`--config profile.release.lto="off"`),
  иначе релиз с LTO собирается очень долго и может упереться в лимиты
  free-сборки.
- **Сигналы**: при деплое Render шлёт SIGTERM — процесс завершается,
  неподтверждённые обновления Telegram доставит заново при старте.
- Если в вашем аккаунте недоступен тип Worker — альтернатива: развернуть
  как Web Service и держать его живым внешним пингером health-endpoint'а
  каждые 5 минут (на free он засыпает без входящих запросов).

## Ограничения

- Скачивание файлов Telegram Bot API — до 20 МБ.
- Обрабатывается первая страница документа.

## Обработка PDF через C-утилиту (MuPDF)

Начиная с этой версии PDF обрабатывается внешней утилитой `ttn_add_field`
на чистом Си (MuPDF), а не встроенным Rust-кодом. Это не меняет интерфейс
бота: `/settings` и отправка PDF работают как раньше.

Как это устроено:

1. Бот скачивает присланный PDF во временный файл в `/tmp`.
2. Берёт настройки полей **текущего пользователя** из SQLite (`chat_id`).
3. Пишет временный `ttn_settings_<chat_id>.ini`.
4. Запускает `ttn_add_field in.pdf out.pdf -c ttn_settings_<chat_id>.ini`.
5. Отправляет пользователю `processed_<имя>.pdf` и удаляет временные файлы.

Путь к бинарнику задаётся переменной окружения `TTN_BIN_PATH`
(по умолчанию `./ttn_add_field`; в Docker-образе — `/usr/local/bin/ttn_add_field`).

### Локальная сборка C-утилиты

Требуется установленный MuPDF (без Python). Например:

```bash
# Debian/Ubuntu
sudo apt-get install libmupdf-dev
make -f Makefile.c_ttn

# macOS (Homebrew)
brew install mupdf
make -f Makefile.c_ttn \
  MUPDF_INC="$(brew --prefix mupdf)/include" \
  MUPDF_LIB="$(brew --prefix mupdf)/lib"
```

Проверка:

```bash
./ttn_add_field tests/fixtures/ttn.pdf /tmp/out.pdf -c settings.example.ini
```

### Файлы

| Файл | Назначение |
|------|------------|
| `ttn_add_field.c` | CLI-утилита: добавляет поля AcroForm в PDF через MuPDF |
| `ttn_config.c/.h` | Парсер INI-конфигурации (`[fields]`) |
| `Makefile.c_ttn` | Сборка C-утилиты |
| `settings.example.ini` | Пример конфигурации полей |
| `src/ttn_processor.rs` | Генерация ini из SQLite и вызов C-утилиты |
| `README_C_TTN.md` | Подробная документация по C-утилите |

Docker-образ (см. `Dockerfile`) собирает MuPDF и `ttn_add_field` автоматически.
