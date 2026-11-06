/*
 * ui.h — тексты и inline-клавиатура настроек.
 */

#ifndef OZON_UI_H
#define OZON_UI_H

#include <stddef.h>

#include "cJSON.h"
#include "db.h"

/* Справка (/start). Возвращает malloc-строку. */
char *ui_help(void);

/* Текст меню настроек. Возвращает malloc-строку. */
char *ui_settings_text(db_t *db, long long chat_id);

/* reply_markup для меню настроек (владеет вызывающий). */
cJSON *ui_settings_keyboard(db_t *db, long long chat_id);

/* Значение поля для отображения (malloc): сохранённое или default. */
char *ui_display_value(db_t *db, long long chat_id, const char *field);

#endif /* OZON_UI_H */
