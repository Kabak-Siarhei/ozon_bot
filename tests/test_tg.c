/*
 * test_tg.c — тест Telegram-клиента против локального mock-сервера.
 *
 * TELEGRAM_API_BASE и TEST_PORT задаёт tests/run_tg_test.sh.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "telegram.h"

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
    tg_t *tg;
    long long message_id = 0;
    cJSON *updates;
    char *file_path;
    FILE *fp;
    char data[64] = {0};

    tg = tg_new("123:test");
    CHECK(tg != NULL, "tg_new");
    if (!tg)
        return 1;

    CHECK(tg_send_message(tg, 42, "hello", NULL, &message_id) == 0,
          "sendMessage");
    CHECK(message_id == 42, "sendMessage message_id");

    CHECK(tg_edit_message(tg, 42, 7, "edited", NULL) == 0, "editMessageText");
    CHECK(tg_answer_callback(tg, "cb1", "ok") == 0, "answerCallbackQuery");

    updates = tg_get_updates(tg, 0);
    CHECK(updates && cJSON_IsArray(updates), "getUpdates array");
    if (updates) {
        cJSON *first = cJSON_GetArrayItem(updates, 0);
        cJSON *uid = cJSON_GetObjectItemCaseSensitive(first, "update_id");
        CHECK(cJSON_IsNumber(uid) && (long long)uid->valuedouble == 7,
              "update_id=7");
        cJSON_Delete(updates);
    }

    file_path = tg_get_file_path(tg, "FILE1");
    CHECK(file_path && strcmp(file_path, "docs/file.pdf") == 0, "getFile path");

    if (file_path) {
        remove("/tmp/ozon_tg_download.bin");
        CHECK(tg_download_file(tg, file_path, "/tmp/ozon_tg_download.bin") == 0,
              "downloadFile");
        fp = fopen("/tmp/ozon_tg_download.bin", "rb");
        CHECK(fp != NULL, "downloaded file exists");
        if (fp) {
            size_t n = fread(data, 1, sizeof(data) - 1, fp);
            data[n] = '\0';
            fclose(fp);
            CHECK(strcmp(data, "PDFDATA") == 0, "downloaded content");
        }
        remove("/tmp/ozon_tg_download.bin");
        free(file_path);
    }

    message_id = 0;
    CHECK(tg_send_document(tg, 42, "tests/fixtures/ttn.pdf", "ttn.pdf",
                           "caption", &message_id) == 0,
          "sendDocument");
    CHECK(message_id == 43, "sendDocument message_id");

    tg_free(tg);
    printf(failures ? "\nFAILURES: %d\n" : "\nALL OK\n", failures);
    return failures ? 1 : 0;
}
