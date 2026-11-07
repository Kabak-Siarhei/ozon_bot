/*
 * ui.c — тексты и клавиатура меню настроек (5 отдельных полей).
 */

#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ttn_add_field.h"

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} sbuf;

static int sb_reserve(sbuf *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    if (need <= b->cap)
        return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need)
        cap *= 2;
    char *p = realloc(b->data, cap);
    if (!p)
        return -1;
    b->data = p;
    b->cap = cap;
    return 0;
}

static int sb_append(sbuf *b, const char *s)
{
    size_t n = strlen(s);
    if (sb_reserve(b, n) != 0)
        return -1;
    memcpy(b->data + b->len, s, n + 1);
    b->len += n;
    return 0;
}

static size_t utf8_char_len(unsigned char c)
{
    if ((c & 0x80) == 0)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1;
}

/* Обрезать UTF-8 строку до max символов, добавив «…». malloc-строка. */
static char *utf8_shorten(const char *s, size_t max)
{
    size_t len = strlen(s);
    size_t i = 0;
    size_t chars = 0;
    size_t cut;

    while (i < len && chars < max) {
        size_t n = utf8_char_len((unsigned char)s[i]);
        if (i + n > len)
            n = 1;
        i += n;
        chars++;
    }

    if (i >= len)
        return strdup(s);

    i = 0;
    chars = 0;
    cut = 0;
    while (i < len && chars + 1 < max) {
        size_t n = utf8_char_len((unsigned char)s[i]);
        if (i + n > len)
            n = 1;
        i += n;
        chars++;
        cut = i;
    }

    {
        char *out = malloc(cut + 4);
        if (!out)
            return NULL;
        memcpy(out, s, cut);
        memcpy(out + cut, "…", 4);
        return out;
    }
}

/* Сохранённое значение поля, иначе пустая строка (дефолтов нет). */
static char *field_value(db_t *db, long long chat_id, const char *field)
{
    char *stored = NULL;
    int rc = db_get_value(db, chat_id, field, &stored);

    if (rc == 1)
        return stored;
    free(stored);
    return strdup("");
}

char *ui_help(void)
{
    const ttn_field_t *fields;
    size_t count = 0;
    sbuf b = {0};

    fields = ttn_fields(&count);

    sb_append(&b,
        "🤖 Я обрабатываю PDF-документы и добавляю поля в бланки (ТТН).\n"
        "\n"
        "Просто отправьте PDF-файл — я верну его с заполненными полями.\n"
        "\n"
        "Команды:\n"
        "/settings — настроить значения полей\n"
        "/cancel — отменить ввод значения\n"
        "/start — эта справка\n"
        "\n"
        "Поля: ");

    for (size_t i = 0; i < count; i++) {
        if (i)
            sb_append(&b, ", ");
        sb_append(&b, fields[i].name);
    }
    sb_append(&b, ".");

    return b.data;
}

char *ui_settings_text(db_t *db, long long chat_id)
{
    const ttn_field_t *fields;
    size_t count = 0;
    sbuf b = {0};
    char line[1024];

    fields = ttn_fields(&count);

    sb_append(&b, "⚙️ Настройки полей\n\n");

    for (size_t i = 0; i < count; i++) {
        char *value = field_value(db, chat_id, fields[i].name);
        snprintf(line, sizeof(line), "• %s: %s\n", fields[i].name,
                 (value && *value) ? value : "—");
        sb_append(&b, line);
        free(value);
    }

    sb_append(&b,
        "\nНажмите поле, чтобы изменить значение, ✖ — чтобы очистить.\n"
        "Когда настроите — отправьте PDF-документ.");

    return b.data;
}

cJSON *ui_settings_keyboard(db_t *db, long long chat_id)
{
    const ttn_field_t *fields;
    size_t count = 0;
    cJSON *kb = cJSON_CreateObject();
    cJSON *rows;

    if (!kb)
        return NULL;

    rows = cJSON_AddArrayToObject(kb, "inline_keyboard");
    if (!rows) {
        cJSON_Delete(kb);
        return NULL;
    }

    fields = ttn_fields(&count);

    for (size_t i = 0; i < count; i++) {
        char *value = field_value(db, chat_id, fields[i].name);
        char label[1024];
        char set_data[192];
        char clr_data[192];
        char *short_label;
        cJSON *row;
        cJSON *btn_set;
        cJSON *btn_clr;

        snprintf(label, sizeof(label), "%s: %s", fields[i].name,
                 (value && *value) ? value : "не задано");
        free(value);

        short_label = utf8_shorten(label, 40);
        if (!short_label)
            short_label = strdup("");

        snprintf(set_data, sizeof(set_data), "set:%s", fields[i].name);
        snprintf(clr_data, sizeof(clr_data), "clr:%s", fields[i].name);

        row = cJSON_CreateArray();
        btn_set = cJSON_CreateObject();
        btn_clr = cJSON_CreateObject();

        cJSON_AddStringToObject(btn_set, "text", short_label ? short_label : "");
        cJSON_AddStringToObject(btn_set, "callback_data", set_data);
        cJSON_AddStringToObject(btn_clr, "text", "✖");
        cJSON_AddStringToObject(btn_clr, "callback_data", clr_data);

        cJSON_AddItemToArray(row, btn_set);
        cJSON_AddItemToArray(row, btn_clr);
        cJSON_AddItemToArray(rows, row);
        free(short_label);
    }

    return kb;
}
