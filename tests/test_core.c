/*
 * test_core.c — локальные тесты SQLite, UI и заполнения PDF (без Telegram).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "db.h"
#include "ttn_add_field.h"
#include "ui.h"

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            printf("ok: %s\n", msg);                                        \
        } else {                                                            \
            printf("FAIL: %s\n", msg);                                      \
            failures++;                                                     \
        }                                                                   \
    } while (0)

int main(void)
{
    const char *db_path = "/tmp/ozon_c_test.db";
    char err[256] = {0};
    db_t *db;
    char *value = NULL;
    ttn_config_t cfg;
    char *text;
    cJSON *kb;
    pending_t p;
    struct stat st;

    remove(db_path);
    db = db_open(db_path, err, sizeof(err));
    CHECK(db != NULL, "db_open");
    if (!db) {
        printf("db error: %s\n", err);
        return 1;
    }

    CHECK(db_set(db, 42, "Автомобиль", "BMW X5 A123BC") == 0, "db_set");
    CHECK(db_get_value(db, 42, "Автомобиль", &value) == 1, "db_get_value exists");
    CHECK(value && strcmp(value, "BMW X5 A123BC") == 0, "stored value matches");
    free(value);

    CHECK(db_get_value(db, 42, "Водитель", &value) == 0, "missing value");
    CHECK(value == NULL, "missing value is NULL");

    memset(&cfg, 0, sizeof(cfg));
    CHECK(db_load_config(db, 42, &cfg) == 0, "db_load_config");
    CHECK(cfg.count == 1, "config has one field");
    CHECK(strcmp(config_get_value(&cfg, "Автомобиль"), "BMW X5 A123BC") == 0,
          "config value matches");
    CHECK(config_get_value(&cfg, "Водитель") == NULL, "config missing field");

    text = ui_settings_text(db, 42);
    CHECK(text && strstr(text, "Автомобиль: BMW X5 A123BC") != NULL,
          "settings text has stored value");
    CHECK(text && strstr(text, "Сдал грузоотправитель: —") != NULL,
          "settings text has no default value");
    free(text);

    kb = ui_settings_keyboard(db, 42);
    CHECK(kb != NULL, "settings keyboard built");
    if (kb) {
        char *json = cJSON_PrintUnformatted(kb);
        CHECK(json && strstr(json, "set:Автомобиль") != NULL,
              "keyboard has set callback");
        CHECK(json && strstr(json, "clr:Автомобиль") != NULL,
              "keyboard has clear callback");
        free(json);
        cJSON_Delete(kb);
    }

    CHECK(db_set_pending(db, 42, 1, "Водитель", 100, 5) == 0, "set pending");
    memset(&p, 0, sizeof(p));
    CHECK(db_take_pending(db, 42, 1, &p) == 1, "take pending");
    CHECK(strcmp(p.field, "Водитель") == 0, "pending field");
    CHECK(p.prompt_message_id == 100 && p.menu_message_id == 5, "pending ids");
    CHECK(db_take_pending(db, 42, 1, &p) == 0, "pending removed after take");

    CHECK(db_clear(db, 42, "Автомобиль") == 0, "db_clear");
    CHECK(db_get_value(db, 42, "Автомобиль", &value) == 0, "cleared value missing");

    /* PDF: заполняем реальный бланк значениями из БД */
    db_set(db, 42, "Автомобиль", "BMW X5 A123BC");
    db_load_config(db, 42, &cfg);
    remove("/tmp/ozon_c_test_out.pdf");
    CHECK(process_pdf_file("tests/fixtures/ttn.pdf", "/tmp/ozon_c_test_out.pdf",
                           &cfg) == 0,
          "process_pdf_file");
    CHECK(stat("/tmp/ozon_c_test_out.pdf", &st) == 0 && st.st_size > 1000,
          "output PDF created");

    db_close(db);
    remove(db_path);
    remove("/tmp/ozon_c_test_out.pdf");

    printf(failures ? "\nFAILURES: %d\n" : "\nALL OK\n", failures);
    return failures ? 1 : 0;
}
