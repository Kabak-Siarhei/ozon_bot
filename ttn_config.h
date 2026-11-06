/*
 * ttn_config.h — Чтение INI-конфигурации полей ТТН.
 *
 * Формат ini:
 *   [fields]
 *   Автомобиль = BMW X5 A123BC
 *   Водитель = Иванов Иван Иванович
 *   Отпуск разрешил =
 *   (пустое значение после '=' считается заданным пустым значением)
 */

#ifndef TTN_CONFIG_H
#define TTN_CONFIG_H

#include <stddef.h>

#define MAX_FIELDS   32
#define MAX_NAME_LEN 128
#define MAX_VAL_LEN  512

typedef struct {
    char name[MAX_NAME_LEN];
    char value[MAX_VAL_LEN];
    int has_value;   /* 1 — ключ присутствует в ini (даже если значение пустое), 0 — отсутствует */
} ttn_field_entry_t;

typedef struct {
    ttn_field_entry_t entries[MAX_FIELDS];
    int count;
} ttn_config_t;

/*
 * parse_ini — прочитать ini-файл и заполнить cfg.
 * Возвращает 0 при успехе, -1 при ошибке.
 */
int parse_ini(const char *path, ttn_config_t *cfg);

/*
 * config_get_value — вернуть значение для поля name, или NULL если ключ absent.
 */
const char *config_get_value(const ttn_config_t *cfg, const char *name);

/*
 * config_has_field — вернуть 1 если поле присутствует в ini, иначе 0.
 */
int config_has_field(const ttn_config_t *cfg, const char *name);

#endif /* TTN_CONFIG_H */
