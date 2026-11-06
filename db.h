/*
 * db.h — SQLite-хранилище настроек (per chat_id) и pending-ввода.
 */

#ifndef OZON_DB_H
#define OZON_DB_H

#include <stddef.h>

#include "ttn_config.h"

typedef struct db db_t;

typedef struct {
    long long chat_id;
    long long user_id;
    char field[128];
    int prompt_message_id;
    int menu_message_id;   /* 0 — нет */
} pending_t;

/* Открыть базу и создать таблицы. NULL при ошибке (err заполняется). */
db_t *db_open(const char *path, char *err, size_t errlen);
void db_close(db_t *db);

/* Создать/обновить значение поля. 0 — успех. */
int db_set(db_t *db, long long chat_id, const char *field, const char *value);

/* Удалить значение поля. 0 — успех. */
int db_clear(db_t *db, long long chat_id, const char *field);

/*
 * Получить значение поля. Возвращает 1 если строка есть (out — malloc-строка,
 * освободить free), 0 если нет, -1 при ошибке.
 */
int db_get_value(db_t *db, long long chat_id, const char *field, char **out);

/* Заполнить cfg всеми сохранёнными значениями чата. */
int db_load_config(db_t *db, long long chat_id, ttn_config_t *cfg);

/* pending: создать/обновить. */
int db_set_pending(db_t *db, long long chat_id, long long user_id,
                   const char *field, int prompt_message_id,
                   int menu_message_id);

/* pending: забрать и удалить. 1 — найден, 0 — нет, -1 — ошибка. */
int db_take_pending(db_t *db, long long chat_id, long long user_id,
                    pending_t *out);

/* pending: удалить без чтения. */
int db_clear_pending(db_t *db, long long chat_id, long long user_id);

#endif /* OZON_DB_H */
