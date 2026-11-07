/*
 * groups.h — группы полей с одинаковым значением.
 */

#ifndef OZON_GROUPS_H
#define OZON_GROUPS_H

#include <stddef.h>

typedef struct {
    const char *label;       /* как показывать в настройках */
    const char *fields[2];   /* 1 или 2 поля PDF */
    int count;
} field_group_t;

/* Список групп (порядок отображения). */
const field_group_t *field_groups(size_t *count);

/* Группа, содержащая поле (по любому имени), или NULL. */
const field_group_t *field_group_for(const char *field);

#endif /* OZON_GROUPS_H */
