/*
 * telegram.h — минимальный клиент Telegram Bot API на libcurl + cJSON.
 */

#ifndef OZON_TELEGRAM_H
#define OZON_TELEGRAM_H

#include "cJSON.h"

typedef struct tg tg_t;

/* Создать клиент. token — токен Bot API. NULL при ошибке. */
tg_t *tg_new(const char *token);
void tg_free(tg_t *tg);

/* getUpdates; возвращает result-массив (владеет вызывающий) или NULL. */
cJSON *tg_get_updates(tg_t *tg, long long offset);

/* sendMessage; reply_markup может быть NULL (забирает ownership). */
int tg_send_message(tg_t *tg, long long chat_id, const char *text,
                    cJSON *reply_markup, long long *out_message_id);

/* editMessageText; reply_markup может быть NULL (забирает ownership). */
int tg_edit_message(tg_t *tg, long long chat_id, int message_id,
                    const char *text, cJSON *reply_markup);

/* answerCallbackQuery. */
int tg_answer_callback(tg_t *tg, const char *callback_id, const char *text);

/* sendDocument (multipart). Возвращает 0 при успехе. */
int tg_send_document(tg_t *tg, long long chat_id, const char *path,
                     const char *filename, const char *caption,
                     long long *out_message_id);

/* getFile -> file_path (malloc, освободить free). NULL при ошибке. */
char *tg_get_file_path(tg_t *tg, const char *file_id);

/* Скачать файл по file_path в dest_path. 0 — успех. */
int tg_download_file(tg_t *tg, const char *file_path, const char *dest_path);

#endif /* OZON_TELEGRAM_H */
