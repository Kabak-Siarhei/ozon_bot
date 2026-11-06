//! Telegram-бот — клон веб-сервера `ozon`.
//!
//! Сценарий: пользователь настраивает значения полей (/settings),
//! отправляет PDF-документ и получает тот же файл с заполненными полями.

mod db;
mod pdf;

use std::collections::HashMap;
use std::error::Error;
use std::sync::{Arc, Mutex};

use dotenvy::dotenv;
use teloxide::net::Download;
use teloxide::prelude::*;
use teloxide::types::{
    CallbackQuery, InlineKeyboardButton, InlineKeyboardMarkup, InputFile, Me, MessageId,
};
use teloxide::utils::command::BotCommands;

use db::{Db, DbHandle};
use pdf::FIELDS;

/// Состояние «ожидаем ввода значения»: (chat_id, user_id) -> текущая правка.
/// `user_id` — `UserId.0` (u64), `chat_id` — `ChatId.0` (i64).
type PendingMap = Arc<Mutex<HashMap<(i64, u64), PendingEdit>>>;

/// Ждём, пока пользователь пришлёт новое значение текстом.
struct PendingEdit {
    /// Название поля (одно из [`FIELDS`]).
    field: String,
    /// Сообщение-просьба «введите значение» (его отредактируем подтверждением).
    prompt_id: MessageId,
    /// Сообщение с меню настроек (его обновим после сохранения).
    menu_id: Option<MessageId>,
}

type HandlerResult = Result<(), Box<dyn Error + Send + Sync>>;

#[derive(BotCommands)]
#[command(rename_rule = "lowercase", description = "Команды:")]
enum Command {
    /// Приветствие и справка
    Start,
    /// Настройка значений полей
    Settings,
    /// Отменить ввод значения
    Cancel,
}

/// Минималистичный логгер: без него сообщения teloxide (`log::…`) теряются,
/// и причина падения/ретраев в логах Render не видна.
/// Пишем в **stdout** — в логах Render.stdout виден гарантированно.
struct StdLogger;

impl log::Log for StdLogger {
    fn enabled(&self, metadata: &log::Metadata) -> bool {
        metadata.level() <= log::Level::Info
    }

    fn log(&self, record: &log::Record) {
        if self.enabled(record.metadata()) {
            println!("[{}] {}: {}", record.level(), record.target(), record.args());
        }
    }

    fn flush(&self) {}
}

fn init_logger() {
    static LOGGER: StdLogger = StdLogger;
    let _ = log::set_logger(&LOGGER);
    log::set_max_level(log::LevelFilter::Info);
}

/// Служебная строка диагностики — только stdout (надёжно попадает в логи).
fn diag(msg: &str) {
    println!("{msg}");
}

/// Фатальная ошибка: в оба потока (чтобы не потерять ни при каком
/// перехвате stderr) и выход с кодом 1.
fn fatal(msg: &str) -> ! {
    println!("{msg}");
    eprintln!("{msg}");
    std::process::exit(1)
}

/// Токен в логах не показываем целиком — только начало для сверки.
fn mask_token(token: &str) -> String {
    let head: String = token.chars().take(8).collect();
    format!("{head}… ({} символов)", token.chars().count())
}

#[tokio::main]
async fn main() {
    // Логгер — до всего остального, чтобы ошибки teloxide были видны.
    init_logger();

    dotenv().ok();
    diag(&format!(
        "старт: cwd={:?}, BOT_DB={:?}",
        std::env::current_dir().unwrap_or_default(),
        std::env::var("BOT_DB").unwrap_or_default()
    ));

    // Токен: TELEGRAM_BOT_TOKEN (наш .env) либо TELOXIDE_TOKEN (стандарт teloxide).
    let token = std::env::var("TELEGRAM_BOT_TOKEN")
        .or_else(|_| std::env::var("TELOXIDE_TOKEN"));
    let token = match token {
        Ok(t) if !t.trim().is_empty() => t,
        _ => fatal(
            "❌ Не задан TELEGRAM_BOT_TOKEN (ни в окружении, ни в .env).\n\
             \x20  На Render: сервис → Environment → добавьте TELEGRAM_BOT_TOKEN.",
        ),
    };
    diag(&format!("токен: {}", mask_token(&token)));

    let db_path = std::env::var("BOT_DB").unwrap_or_else(|_| "bot_settings.db".to_string());
    let db: DbHandle = match Db::open(&db_path) {
        Ok(db) => Arc::new(db),
        Err(e) => fatal(&format!("❌ Не удалось открыть базу настроек '{db_path}': {e}")),
    };

    let pending: PendingMap = Arc::new(Mutex::new(HashMap::new()));

    let bot = Bot::new(token);

    // Предварительная проверка: токен валиден и api.telegram.org достижим.
    // Именно здесь падает большинство «тихих» деплоев — поэтому сообщаем явно.
    match bot.get_me().send().await {
        Ok(me) => diag(&format!("✅ Telegram API доступен, бот: @{}", me.username())),
        Err(e) => fatal(&format!(
            "❌ getMe не прошёл: {e}\n\
             \x20  Причины: неверный TELEGRAM_BOT_TOKEN либо нет доступа к api.telegram.org."
        )),
    }

    let handler = dptree::entry()
        .branch(Update::filter_message().endpoint(handle_message))
        .branch(Update::filter_callback_query().endpoint(handle_callback));

    diag(&format!("Бот запущен (база: {db_path})."));
    Dispatcher::builder(bot, handler)
        .dependencies(dptree::deps![db, pending])
        .enable_ctrlc_handler()
        .build()
        .dispatch()
        .await;
    diag("Dispatcher остановлен — выход.");
}

// ---------------------------------------------------------------------------
// Обработка сообщений: команды, ввод значения, документы
// ---------------------------------------------------------------------------

async fn handle_message(
    bot: Bot,
    msg: Message,
    me: Me,
    db: DbHandle,
    pending: PendingMap,
) -> HandlerResult {
    let chat = msg.chat.id;
    let chat_id = chat.0;
    let user_id = msg.from.as_ref().map(|u| u.id.0).unwrap_or(chat_id as u64);
    let key = (chat_id, user_id);

    // ---------- команды и текст ----------
    if let Some(text) = msg.text() {
        if text.starts_with('/') {
            match Command::parse(text, me.username()) {
                Ok(Command::Start) => {
                    bot.send_message(chat, help_text()).await?;
                }
                Ok(Command::Settings) => {
                    pending.lock().unwrap().remove(&key);
                    send_settings(&bot, chat, chat_id, &db).await?;
                }
                Ok(Command::Cancel) => {
                    // Важно: guard не должен жить через .await — сначала извлекаем.
                    let editing = pending.lock().unwrap().remove(&key);
                    if let Some(p) = editing {
                        edit_or_send(&bot, chat, p.prompt_id, "🚫 Ввод значения отменён.").await?;
                    }
                }
                Err(_) => {} // неизвестная команда — игнорируем
            }
            return Ok(());
        }

        // Обычный текст: возможно, это новое значение поля.
        let editing = pending.lock().unwrap().remove(&key);
        if let Some(p) = editing {
            db.set(chat_id, &p.field, text)?;
            let confirm = format!("✅ «{}» сохранено: {}", p.field, text);
            edit_or_send(&bot, chat, p.prompt_id, &confirm).await?;
            if let Some(menu_id) = p.menu_id {
                refresh_menu(&bot, chat, chat_id, &db, menu_id).await;
            }
            return Ok(());
        }

        // Подсказка только в личке, чтобы не спамить в группах.
        if msg.chat.is_private() {
            bot.send_message(chat, help_text()).await?;
        }
        return Ok(());
    }

    // ---------- документ ----------
    if let Some(document) = msg.document() {
        let is_pdf = document
            .mime_type
            .as_ref()
            .map(|m| m.essence_str() == "application/pdf")
            .unwrap_or(false)
            || document
                .file_name
                .as_deref()
                .map(|n| n.to_ascii_lowercase().ends_with(".pdf"))
                .unwrap_or(false);

        if !is_pdf {
            bot.send_message(chat, "Нужен PDF-файл (.pdf).").await?;
            return Ok(());
        }

        // Лимит Bot API на скачивание — 20 МБ.
        if document.file.size > 20 * 1024 * 1024 {
            bot.send_message(
                chat,
                "Файл больше 20 МБ — Telegram не позволяет его скачать.",
            )
            .await?;
            return Ok(());
        }

        let status = bot.send_message(chat, "⏳ Обрабатываю PDF…").await?;

        let file = bot.get_file(document.file.id.clone()).await?;
        let mut input: Vec<u8> = Vec::new();
        bot.download_file(&file.path, &mut input).await?;

        let values = db.values(chat_id);
        match pdf::process_pdf(&input, &values) {
            Ok(out) => {
                let name = match document.file_name.as_deref() {
                    Some(n) => format!("processed_{n}"),
                    None => "processed.pdf".to_string(),
                };
                bot.send_document(chat, InputFile::memory(out).file_name(name))
                    .caption("✅ Поля добавлены")
                    .await?;
                let _ = bot.edit_message_text(chat, status.id, "✅ Готово").await;
            }
            Err(e) => {
                // Форматируем до await: текст сообщения не должен зависеть от `e`.
                let text = format!("❌ Ошибка обработки: {e}");
                let _ = bot.edit_message_text(chat, status.id, text).await;
            }
        }
        return Ok(());
    }

    Ok(())
}

// ---------------------------------------------------------------------------
// Обработка нажатий кнопок настроек
// ---------------------------------------------------------------------------

async fn handle_callback(
    bot: Bot,
    q: CallbackQuery,
    db: DbHandle,
    pending: PendingMap,
) -> HandlerResult {
    let data = q.data.clone().unwrap_or_default();

    let Some(message) = q.regular_message().cloned() else {
        bot.answer_callback_query(q.id).await?;
        return Ok(());
    };

    let chat = message.chat.id;
    let chat_id = chat.0;
    let user_id = q.from.id.0;
    let key = (chat_id, user_id);

    if let Some(field) = data.strip_prefix("set:") {
        // Кнопка поля: просим прислать новое значение текстом.
        if !FIELDS.contains(&field) {
            bot.answer_callback_query(q.id).await?;
            return Ok(());
        }
        bot.answer_callback_query(q.id)
            .text(format!("Введите значение для «{field}»"))
            .await?;

        let prompt = bot
            .send_message(
                chat,
                format!(
                    "✏️ Отправьте новое значение для «{field}» текстом.\nОтмена — /cancel"
                ),
            )
            .await?;

        pending.lock().unwrap().insert(
            key,
            PendingEdit {
                field: field.to_string(),
                prompt_id: prompt.id,
                menu_id: Some(message.id),
            },
        );
    } else if let Some(field) = data.strip_prefix("clr:") {
        // Кнопка «✖»: очистить значение поля.
        if FIELDS.contains(&field) {
            db.clear(chat_id, field)?;
            bot.answer_callback_query(q.id).text("Очищено ✅").await?;
            refresh_menu(&bot, chat, chat_id, &db, message.id).await;
        } else {
            bot.answer_callback_query(q.id).await?;
        }
    } else {
        bot.answer_callback_query(q.id).await?;
    }

    Ok(())
}

// ---------------------------------------------------------------------------
// UI: справка, меню настроек, клавиатура
// ---------------------------------------------------------------------------

fn help_text() -> String {
    [
        "🤖 Я обрабатываю PDF-документы и добавляю поля в бланки (ТТН).",
        "",
        "Просто отправьте PDF-файл — я верну его с заполненными полями.",
        "",
        "Команды:",
        "/settings — настроить значения полей",
        "/cancel — отменить ввод значения",
        "/start — эта справка",
        "",
        "Поля: Автомобиль, Водитель, Отпуск разрешил, Сдал грузоотправитель, Товар к перевозке принял.",
    ]
    .join("\n")
}

fn settings_text(chat_id: i64, db: &Db) -> String {
    let mut out = String::from("⚙️ Настройки полей\n\n");
    for field in FIELDS {
        let value = db.display_value(chat_id, field);
        let value = if value.is_empty() { "—".to_string() } else { value };
        out.push_str(&format!("• {field}: {value}\n"));
    }
    out.push_str(
        "\nНажмите поле, чтобы изменить значение, ✖ — чтобы очистить.\nКогда настроите — отправьте PDF-документ.",
    );
    out
}

fn settings_keyboard(chat_id: i64, db: &Db) -> InlineKeyboardMarkup {
    let mut rows: Vec<Vec<InlineKeyboardButton>> = Vec::new();
    for field in FIELDS {
        let value = db.display_value(chat_id, field);
        let value = if value.is_empty() {
            "не задано".to_string()
        } else {
            value
        };
        let label = shorten(&format!("{field}: {value}"), 40);
        rows.push(vec![
            InlineKeyboardButton::callback(label, format!("set:{field}")),
            InlineKeyboardButton::callback("✖", format!("clr:{field}")),
        ]);
    }
    InlineKeyboardMarkup::new(rows)
}

/// Обрезка строки до `max` символов (по символам, не по байтам).
fn shorten(s: &str, max: usize) -> String {
    if s.chars().count() <= max {
        return s.to_string();
    }
    let mut out: String = s.chars().take(max.saturating_sub(1)).collect();
    out.push('…');
    out
}

async fn send_settings(bot: &Bot, chat: ChatId, chat_id: i64, db: &Db) -> HandlerResult {
    bot.send_message(chat, settings_text(chat_id, db))
        .reply_markup(settings_keyboard(chat_id, db))
        .await?;
    Ok(())
}

/// Обновляет текст и клавиатуру уже отправленного меню настроек.
/// Ошибки редактирования не должны ронять обработку — поэтому `let _`.
async fn refresh_menu(bot: &Bot, chat: ChatId, chat_id: i64, db: &Db, menu_id: MessageId) {
    let _ = bot
        .edit_message_text(chat, menu_id, settings_text(chat_id, db))
        .reply_markup(settings_keyboard(chat_id, db))
        .await;
}

/// Редактирует сообщение; если редактирование невозможно — шлёт новое.
async fn edit_or_send(bot: &Bot, chat: ChatId, id: MessageId, text: &str) -> HandlerResult {
    if bot.edit_message_text(chat, id, text).await.is_err() {
        bot.send_message(chat, text).await?;
    }
    Ok(())
}
