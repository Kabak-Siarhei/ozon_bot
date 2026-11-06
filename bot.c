/*
 * bot.c — Telegram-бот на C: long polling, SQLite-настройки, PDF через MuPDF.
 *
 * Полностью заменяет Rust-версию: зависит только от libcurl, sqlite3 и MuPDF.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "db.h"
#include "telegram.h"
#include "ttn_add_field.h"
#include "ttn_config.h"
#include "ui.h"

#define MAX_UPLOAD (20LL * 1024 * 1024)

typedef struct {
    tg_t *tg;
    db_t *db;
} bot_t;

/* -------------------------------------------------------------------------- */
/* Мелкие помощники                                                           */
/* -------------------------------------------------------------------------- */

static void load_dotenv(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[2048];

    if (!fp)
        return;

    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        char *eq;
        char *key;
        char *value;
        size_t len;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;

        eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        key = p;
        value = eq + 1;

        len = strlen(key);
        while (len && isspace((unsigned char)key[len - 1]))
            key[--len] = '\0';
        while (*value == ' ' || *value == '\t' || *value == '"')
            value++;
        len = strlen(value);
        while (len && (value[len - 1] == '\n' || value[len - 1] == '\r' ||
                       value[len - 1] == ' ' || value[len - 1] == '\t' ||
                       value[len - 1] == '"'))
            value[--len] = '\0';

        if (*key && !getenv(key))
            setenv(key, value, 0);
    }

    fclose(fp);
}

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *v;
    if (!obj)
        return NULL;
    v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static long long json_i64(cJSON *obj, const char *key)
{
    cJSON *v;
    if (!obj)
        return 0;
    v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? (long long)v->valuedouble : 0;
}

static cJSON *json_obj(cJSON *obj, const char *key)
{
    cJSON *v;
    if (!obj)
        return NULL;
    v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsObject(v) ? v : NULL;
}

static int has_prefix(const char *s, const char *prefix)
{
    return s && strncmp(s, prefix, strlen(prefix)) == 0;
}

static int has_suffix_ci(const char *s, const char *suffix)
{
    size_t ls, lf;
    if (!s)
        return 0;
    ls = strlen(s);
    lf = strlen(suffix);
    if (ls < lf)
        return 0;
    s += ls - lf;
    for (size_t i = 0; i < lf; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)suffix[i]))
            return 0;
    }
    return 1;
}

/* Извлечь команду: "/settings@bot" -> "/settings". */
static int is_command(const char *text, const char *cmd)
{
    char token[64];
    size_t i = 0;

    if (!text || text[0] != '/')
        return 0;
    while (text[i] && !isspace((unsigned char)text[i]) &&
           i + 1 < sizeof(token)) {
        token[i] = text[i];
        i++;
    }
    token[i] = '\0';

    {
        char *at = strchr(token, '@');
        if (at)
            *at = '\0';
    }

    return strcasecmp(token, cmd) == 0;
}

static void safe_filename(const char *src, char *dst, size_t dstlen)
{
    const char *base = src ? src : "document.pdf";
    const char *slash = strrchr(base, '/');
    const char *bslash = strrchr(base, '\\');
    if (slash && (!bslash || slash > bslash))
        base = slash + 1;
    else if (bslash)
        base = bslash + 1;

    if (!*base)
        base = "document.pdf";

    size_t j = 0;
    for (size_t i = 0; base[i] && j + 1 < dstlen; i++) {
        unsigned char c = (unsigned char)base[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || c < 32)
            dst[j++] = '_';
        else
            dst[j++] = (char)c;
    }
    dst[j] = '\0';
}

static int is_pdf_document(cJSON *doc)
{
    const char *mime = json_str(doc, "mime_type");
    const char *name = json_str(doc, "file_name");
    if (mime && strcmp(mime, "application/pdf") == 0)
        return 1;
    return has_suffix_ci(name, ".pdf");
}

static int field_is_known(const char *name)
{
    const ttn_field_t *fields;
    size_t count = 0;
    if (!name)
        return 0;
    fields = ttn_fields(&count);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(fields[i].name, name) == 0)
            return 1;
    }
    return 0;
}

static const char *chat_type(cJSON *message)
{
    cJSON *chat = json_obj(message, "chat");
    return json_str(chat, "type");
}

/* -------------------------------------------------------------------------- */
/* Действия бота                                                              */
/* -------------------------------------------------------------------------- */

static void send_help(bot_t *bot, long long chat_id)
{
    char *text = ui_help();
    if (text) {
        tg_send_message(bot->tg, chat_id, text, NULL, NULL);
        free(text);
    }
}

static void send_settings(bot_t *bot, long long chat_id)
{
    char *text = ui_settings_text(bot->db, chat_id);
    cJSON *kb = ui_settings_keyboard(bot->db, chat_id);
    if (text) {
        tg_send_message(bot->tg, chat_id, text, kb, NULL);
        free(text);
    } else {
        cJSON_Delete(kb);
    }
}

static void refresh_menu(bot_t *bot, long long chat_id, int message_id)
{
    char *text;
    cJSON *kb;

    if (message_id <= 0)
        return;

    text = ui_settings_text(bot->db, chat_id);
    kb = ui_settings_keyboard(bot->db, chat_id);
    if (text) {
        tg_edit_message(bot->tg, chat_id, message_id, text, kb);
        free(text);
    } else {
        cJSON_Delete(kb);
    }
}

static void edit_or_send(bot_t *bot, long long chat_id, int message_id,
                         const char *text)
{
    if (message_id > 0 &&
        tg_edit_message(bot->tg, chat_id, message_id, text, NULL) == 0)
        return;
    tg_send_message(bot->tg, chat_id, text, NULL, NULL);
}

static void handle_command(bot_t *bot, cJSON *message, long long chat_id,
                           long long user_id, const char *text)
{
    if (is_command(text, "/start")) {
        send_help(bot, chat_id);
        return;
    }

    if (is_command(text, "/settings")) {
        db_clear_pending(bot->db, chat_id, user_id);
        send_settings(bot, chat_id);
        return;
    }

    if (is_command(text, "/cancel")) {
        pending_t p;
        if (db_take_pending(bot->db, chat_id, user_id, &p) == 1) {
            edit_or_send(bot, chat_id, p.prompt_message_id,
                         "🚫 Ввод значения отменён.");
        }
        return;
    }

    (void)message;
}

static void handle_text(bot_t *bot, cJSON *message, long long chat_id,
                        long long user_id, const char *text)
{
    pending_t p;

    if (db_take_pending(bot->db, chat_id, user_id, &p) == 1) {
        char confirm[2048];

        db_set(bot->db, chat_id, p.field, text);
        snprintf(confirm, sizeof(confirm), "✅ «%s» сохранено: %s",
                 p.field, text);
        edit_or_send(bot, chat_id, p.prompt_message_id, confirm);
        if (p.menu_message_id > 0)
            refresh_menu(bot, chat_id, p.menu_message_id);
        return;
    }

    {
        const char *ct = chat_type(message);
        if (ct && strcmp(ct, "private") == 0)
            send_help(bot, chat_id);
    }
}

static void handle_document(bot_t *bot, cJSON *message, long long chat_id,
                            int message_id)
{
    cJSON *doc = json_obj(message, "document");
    const char *file_id;
    const char *file_name;
    long long file_size;
    char safe[256];
    char processed[300];
    char in_path[512];
    char out_path[512];
    char *remote_path = NULL;
    long long status_id = 0;
    char *status_text;
    ttn_config_t cfg;

    if (!doc)
        return;

    if (!is_pdf_document(doc)) {
        tg_send_message(bot->tg, chat_id, "Нужен PDF-файл (.pdf).", NULL, NULL);
        return;
    }

    file_size = json_i64(doc, "file_size");
    if (file_size > MAX_UPLOAD) {
        tg_send_message(bot->tg, chat_id,
                        "Файл больше 20 МБ — Telegram не позволяет его скачать.",
                        NULL, NULL);
        return;
    }

    file_id = json_str(doc, "file_id");
    if (!file_id) {
        tg_send_message(bot->tg, chat_id, "Не удалось получить файл.", NULL, NULL);
        return;
    }

    file_name = json_str(doc, "file_name");
    safe_filename(file_name, safe, sizeof(safe));
    snprintf(processed, sizeof(processed), "processed_%s", safe);

    snprintf(in_path, sizeof(in_path), "/tmp/ttn_in_%lld_%d.pdf", chat_id,
             message_id);
    snprintf(out_path, sizeof(out_path), "/tmp/ttn_out_%lld_%d.pdf", chat_id,
             message_id);

    status_text = strdup("⏳ Обрабатываю PDF…");
    if (status_text) {
        tg_send_message(bot->tg, chat_id, status_text, NULL, &status_id);
        free(status_text);
    }

    remote_path = tg_get_file_path(bot->tg, file_id);
    if (!remote_path) {
        edit_or_send(bot, chat_id, (int)status_id,
                     "❌ Ошибка обработки: не удалось получить файл");
        return;
    }

    if (tg_download_file(bot->tg, remote_path, in_path) != 0) {
        edit_or_send(bot, chat_id, (int)status_id,
                     "❌ Ошибка обработки: не удалось скачать файл");
        free(remote_path);
        return;
    }
    free(remote_path);

    db_load_config(bot->db, chat_id, &cfg);

    if (process_pdf_file(in_path, out_path, &cfg) != 0) {
        edit_or_send(bot, chat_id, (int)status_id,
                     "❌ Ошибка обработки PDF (MuPDF)");
        remove(in_path);
        return;
    }

    if (tg_send_document(bot->tg, chat_id, out_path, processed,
                         "✅ Поля добавлены", NULL) != 0) {
        edit_or_send(bot, chat_id, (int)status_id,
                     "❌ Не удалось отправить PDF");
    } else {
        edit_or_send(bot, chat_id, (int)status_id, "✅ Готово");
    }

    remove(in_path);
    remove(out_path);
}

static void handle_message(bot_t *bot, cJSON *message)
{
    cJSON *chat = json_obj(message, "chat");
    cJSON *from = json_obj(message, "from");
    long long chat_id = json_i64(chat, "id");
    long long user_id = from ? json_i64(from, "id") : chat_id;
    const char *text = json_str(message, "text");

    if (!chat_id)
        return;

    if (text && text[0] == '/') {
        handle_command(bot, message, chat_id, user_id, text);
        return;
    }

    if (text && *text) {
        handle_text(bot, message, chat_id, user_id, text);
        return;
    }

    if (json_obj(message, "document")) {
        handle_document(bot, message, chat_id,
                        (int)json_i64(message, "message_id"));
    }
}

static void handle_callback(bot_t *bot, cJSON *query)
{
    const char *data = json_str(query, "data");
    cJSON *from = json_obj(query, "from");
    cJSON *message = json_obj(query, "message");
    const char *id = json_str(query, "id");
    long long chat_id;
    long long user_id;
    int message_id;

    if (!message || !from || !id) {
        tg_answer_callback(bot->tg, id ? id : "", NULL);
        return;
    }

    chat_id = json_i64(json_obj(message, "chat"), "id");
    user_id = json_i64(from, "id");
    message_id = (int)json_i64(message, "message_id");

    if (has_prefix(data, "set:")) {
        const char *field = data + 4;
        char prompt[512];
        long long prompt_id = 0;

        if (!field_is_known(field)) {
            tg_answer_callback(bot->tg, id, NULL);
            return;
        }

        snprintf(prompt, sizeof(prompt),
                 "Введите значение для «%s»", field);
        tg_answer_callback(bot->tg, id, prompt);

        snprintf(prompt, sizeof(prompt),
                 "✏️ Отправьте новое значение для «%s» текстом.\n"
                 "Отмена — /cancel",
                 field);
        if (tg_send_message(bot->tg, chat_id, prompt, NULL, &prompt_id) == 0) {
            db_set_pending(bot->db, chat_id, user_id, field, (int)prompt_id,
                           message_id);
        }
        return;
    }

    if (has_prefix(data, "clr:")) {
        const char *field = data + 4;

        if (!field_is_known(field)) {
            tg_answer_callback(bot->tg, id, NULL);
            return;
        }

        db_clear(bot->db, chat_id, field);
        tg_answer_callback(bot->tg, id, "Очищено ✅");
        refresh_menu(bot, chat_id, message_id);
        return;
    }

    tg_answer_callback(bot->tg, id, NULL);
}

/* -------------------------------------------------------------------------- */
/* HTTP health-сервер для Render Web Service                                  */
/* -------------------------------------------------------------------------- */

static void *health_worker(void *arg)
{
    int port = (int)(intptr_t)arg;
    int server;
    int opt = 1;
    struct sockaddr_in addr;

    server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0)
        return NULL;

    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "health: bind 0.0.0.0:%d failed: %s\n", port,
                strerror(errno));
        close(server);
        return NULL;
    }

    if (listen(server, 16) < 0) {
        close(server);
        return NULL;
    }

    fprintf(stderr, "health: слушаю 0.0.0.0:%d (GET / -> 200 ok)\n", port);

    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0)
            continue;

        {
            char buf[1024];
            (void)read(client, buf, sizeof(buf));
            const char *resp =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 2\r\n"
                "Connection: close\r\n"
                "\r\n"
                "ok";
            (void)write(client, resp, strlen(resp));
        }
        close(client);
    }

    return NULL;
}

static void start_health_server(void)
{
    const char *port_str = getenv("PORT");
    long port;
    pthread_t th;

    if (!port_str || !*port_str)
        return;

    port = strtol(port_str, NULL, 10);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "health: некорректный PORT=%s\n", port_str);
        return;
    }

    if (pthread_create(&th, NULL, health_worker, (void *)(intptr_t)port) != 0) {
        fprintf(stderr, "health: не удалось создать поток\n");
        return;
    }
    pthread_detach(th);
}

/* -------------------------------------------------------------------------- */
/* main                                                                       */
/* -------------------------------------------------------------------------- */

int main(void)
{
    const char *token;
    const char *db_path;
    char db_err[256] = {0};
    bot_t bot;
    long long offset = 0;

    load_dotenv(".env");

    token = getenv("TELEGRAM_BOT_TOKEN");
    if (!token || !*token)
        token = getenv("TELOXIDE_TOKEN");
    if (!token || !*token) {
        fprintf(stderr, "❌ Не задан TELEGRAM_BOT_TOKEN.\n");
        return 1;
    }

    db_path = getenv("BOT_DB");
    if (!db_path || !*db_path)
        db_path = "bot_settings.db";

    {
        char cwd[4096];
        fprintf(stderr, "старт: cwd=%s, BOT_DB=%s\n",
                getcwd(cwd, sizeof(cwd)) ? cwd : "?", db_path);
    }
    fprintf(stderr, "токен: %.8s… (%zu символов)\n", token, strlen(token));

    bot.db = db_open(db_path, db_err, sizeof(db_err));
    if (!bot.db) {
        fprintf(stderr, "❌ Не удалось открыть базу '%s': %s\n", db_path, db_err);
        return 1;
    }

    bot.tg = tg_new(token);
    if (!bot.tg) {
        fprintf(stderr, "❌ Не удалось создать Telegram-клиент\n");
        db_close(bot.db);
        return 1;
    }

    start_health_server();
    fprintf(stderr, "Бот запущен (база: %s).\n", db_path);

    for (;;) {
        cJSON *updates = tg_get_updates(bot.tg, offset);

        if (!updates) {
            sleep(3);
            continue;
        }

        if (cJSON_IsArray(updates)) {
            int n = cJSON_GetArraySize(updates);
            for (int i = 0; i < n; i++) {
                cJSON *update = cJSON_GetArrayItem(updates, i);
                long long update_id = json_i64(update, "update_id");

                if (update_id >= offset)
                    offset = update_id + 1;

                {
                    cJSON *message = json_obj(update, "message");
                    cJSON *callback = json_obj(update, "callback_query");
                    if (message)
                        handle_message(&bot, message);
                    if (callback)
                        handle_callback(&bot, callback);
                }
            }
        }

        cJSON_Delete(updates);
    }

    tg_free(bot.tg);
    db_close(bot.db);
    return 0;
}
