/*
 * ttn_config.c — Реализация чтения INI-конфигурации.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "ttn_config.h"

/* Убирает пробелы по краям строки (in-place). */
static char *trim(char *s)
{
    char *end;
    if (!s)
        return NULL;
    while (isspace((unsigned char)*s))
        s++;
    if (*s == '\0')
        return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        end--;
    *(end + 1) = '\0';
    return s;
}

int parse_ini(const char *path, ttn_config_t *cfg)
{
    FILE *fp;
    char line[1024];
    int in_fields = 0;
    int count = 0;

    if (!path || !cfg)
        return -1;

    memset(cfg, 0, sizeof(*cfg));

    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "ttn_config: не удалось открыть ini-файл '%s'\n", path);
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *p = trim(line);

        /* Пустые строки и комментарии */
        if (*p == '\0' || *p == '#' || *p == ';')
            continue;

        /* Заголовок секции [fields] */
        if (*p == '[') {
            char *sec_end = strchr(p + 1, ']');
            if (sec_end) {
                *sec_end = '\0';
                char *sec = trim(p + 1);
                in_fields = (strcmp(sec, "fields") == 0);
            }
            continue;
        }

        /* Пара ключ=значение внутри [fields] */
        if (in_fields && count < MAX_FIELDS) {
            char *eq = strchr(p, '=');
            if (eq) {
                *eq = '\0';
                char *key = trim(p);
                char *val = trim(eq + 1);

                if (key[0] != '\0') {
                    strncpy(cfg->entries[count].name, key,
                            sizeof(cfg->entries[count].name) - 1);
                    cfg->entries[count].name[sizeof(cfg->entries[count].name) - 1] = '\0';

                    if (val[0] != '\0') {
                        strncpy(cfg->entries[count].value, val,
                                sizeof(cfg->entries[count].value) - 1);
                        cfg->entries[count].value[sizeof(cfg->entries[count].value) - 1] = '\0';
                        cfg->entries[count].has_value = 1;
                    } else {
                        cfg->entries[count].value[0] = '\0';
                        cfg->entries[count].has_value = 1; /* ключ есть, но значение пустое */
                    }
                    count++;
                }
            }
        }
    }

    if (ferror(fp)) {
        fprintf(stderr, "ttn_config: ошибка чтения ini-файла '%s'\n", path);
        fclose(fp);
        return -1;
    }

    cfg->count = count;
    fclose(fp);
    return 0;
}

const char *config_get_value(const ttn_config_t *cfg, const char *name)
{
    int i;
    if (!cfg || !name)
        return NULL;
    for (i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].name, name) == 0)
            return cfg->entries[i].value;
    }
    return NULL;
}

int config_has_field(const ttn_config_t *cfg, const char *name)
{
    int i;
    if (!cfg || !name)
        return 0;
    for (i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].name, name) == 0)
            return 1;
    }
    return 0;
}
