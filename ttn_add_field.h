/*
 * ttn_add_field.h — публичный API заполнения AcroForm-полей ТТН.
 */

#ifndef TTN_ADD_FIELD_H
#define TTN_ADD_FIELD_H

#include <stddef.h>

#include "ttn_config.h"

typedef struct {
    const char *name;  /* имя поля AcroForm (/T) */
    float x;           /* левая координата (от левого края страницы) */
    float y;           /* верхняя координата (от верхнего края страницы) */
    float w;           /* ширина */
    float h;           /* высота */
} ttn_field_t;

/* Список стандартных полей ТТН. Возвращает указатель на статический массив. */
const ttn_field_t *ttn_fields(size_t *count);

/* Значение поля по умолчанию. */
const char *ttn_default_value(const char *field);

/*
 * Добавить 5 полей ТТН в PDF-файл.
 * in_path/out_path — пути к файлам; cfg — значения (может быть NULL).
 * Возвращает 0 при успехе, 1 при ошибке.
 */
int process_pdf_file(const char *in_path, const char *out_path,
                     const ttn_config_t *cfg);

/* Добавить одно поле с заданными координатами. */
int process_pdf_file_single(const char *in_path, const char *out_path,
                            const char *field_name, const char *field_value,
                            float x0, float y0, float x1, float y1);

#endif /* TTN_ADD_FIELD_H */
