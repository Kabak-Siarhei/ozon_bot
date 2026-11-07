/*
 * groups.c — группировка полей ТТН с общим значением.
 */

#include "groups.h"

#include <string.h>

static const field_group_t GROUPS[] = {
    {
        "Автомобиль",
        {"Автомобиль", NULL},
        1,
    },
    {
        "Водитель / Товар к перевозке принял",
        {"Водитель", "Товар к перевозке принял"},
        2,
    },
    {
        "Отпуск разрешил / Сдал грузоотправитель",
        {"Отпуск разрешил", "Сдал грузоотправитель"},
        2,
    },
};

#define NUM_GROUPS (sizeof(GROUPS) / sizeof(GROUPS[0]))

const field_group_t *field_groups(size_t *count)
{
    if (count)
        *count = NUM_GROUPS;
    return GROUPS;
}

const field_group_t *field_group_for(const char *field)
{
    if (!field)
        return NULL;

    for (size_t i = 0; i < NUM_GROUPS; i++) {
        for (int j = 0; j < GROUPS[i].count; j++) {
            if (GROUPS[i].fields[j] &&
                strcmp(GROUPS[i].fields[j], field) == 0)
                return &GROUPS[i];
        }
    }
    return NULL;
}
