/*
 * ui.h — тексты и inline-клавиатура настроек.
 */

#ifndef OZON_UI_H
#define OZON_UI_H

#include "cJSON.h"
#include "db.h"

/* Справка (/start). Возвращает malloc-строку. */
char *ui_help(void);

/* Текст меню настроек. Возвращает malloc-строку. */
char *ui_settings_text(db_t *db, long long chat_id);

/* reply_markup для меню настроек (владеет вызывающий). */
cJSON *ui_settings_keyboard(db_t *db, long long chat_id);

#endif /* OZON_UI_H */
