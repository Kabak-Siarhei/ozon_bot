/*
 * telegram.c — Telegram Bot API через libcurl и cJSON.
 *
 * Текстовые методы отправляются как JSON (Bot API это поддерживает),
 * sendDocument — multipart/form-data.
 */

#define _POSIX_C_SOURCE 200809L

#include "telegram.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct tg {
    CURL *curl;
    char *token;
    char *base;      /* https://api.telegram.org/bot<token>/ */
    char *file_base; /* https://api.telegram.org/file/bot<token>/ */
};

typedef struct {
    char *data;
    size_t len;
} membuf;

static int curl_global_ready = 0;

static void log_curl_error(CURLcode rc, const char *what)
{
    fprintf(stderr, "telegram: %s: %s\n", what, curl_easy_strerror(rc));
}

static size_t write_membuf(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t total = size * nmemb;
    membuf *buf = userdata;
    char *p = realloc(buf->data, buf->len + total + 1);
    if (!p)
        return 0;
    buf->data = p;
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static size_t write_file(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    return fwrite(ptr, size, nmemb, (FILE *)userdata);
}

tg_t *tg_new(const char *token)
{
    tg_t *tg;
    char *token_root = NULL;
    size_t n;

    if (!token || !*token)
        return NULL;

    if (!curl_global_ready) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            return NULL;
        curl_global_ready = 1;
    }

    tg = calloc(1, sizeof(*tg));
    if (!tg)
        return NULL;

    {
        const char *root = getenv("TELEGRAM_API_BASE");
        if (!root || !*root)
            root = "https://api.telegram.org";
        /* Убираем завершающий слэш, чтобы не было двойного. */
        char root_buf[512];
        snprintf(root_buf, sizeof(root_buf), "%s", root);
        size_t rl = strlen(root_buf);
        while (rl > 0 && root_buf[rl - 1] == '/')
            root_buf[--rl] = '\0';
        token_root = strdup(root_buf);
    }
    if (!token_root) {
        tg_free(tg);
        return NULL;
    }

    tg->token = strdup(token);
    if (!tg->token) {
        free(tg);
        return NULL;
    }

    n = strlen(token) + strlen(token_root) + 64;
    tg->base = malloc(n);
    tg->file_base = malloc(n);
    if (!tg->base || !tg->file_base) {
        free(token_root);
        tg_free(tg);
        return NULL;
    }

    snprintf(tg->base, n, "%s/bot%s/", token_root, token);
    snprintf(tg->file_base, n, "%s/file/bot%s/", token_root, token);
    free(token_root);
    token_root = NULL;

    tg->curl = curl_easy_init();
    if (!tg->curl) {
        tg_free(tg);
        return NULL;
    }

    return tg;
}

void tg_free(tg_t *tg)
{
    if (!tg)
        return;
    if (tg->curl)
        curl_easy_cleanup(tg->curl);
    free(tg->token);
    free(tg->base);
    free(tg->file_base);
    free(tg);
}

/*
 * Выполнить метод с JSON-телом. Возвращает result (владеет вызывающий)
 * или NULL. timeout_sec <= 0 — значение по умолчанию.
 */
static cJSON *call_json(tg_t *tg, const char *method, cJSON *params,
                        long timeout_sec)
{
    CURL *curl = tg->curl;
    struct curl_slist *headers = NULL;
    char *url = NULL;
    char *body = NULL;
    membuf response = {0};
    CURLcode rc;
    cJSON *root = NULL;
    cJSON *result = NULL;
    size_t url_len;

    if (!tg || !method) {
        cJSON_Delete(params);
        return NULL;
    }

    body = params ? cJSON_PrintUnformatted(params) : strdup("{}");
    cJSON_Delete(params);
    if (!body) {
        fprintf(stderr, "telegram: out of memory\n");
        return NULL;
    }

    url_len = strlen(tg->base) + strlen(method) + 1;
    url = malloc(url_len);
    if (!url) {
        free(body);
        return NULL;
    }
    snprintf(url, url_len, "%s%s", tg->base, method);

    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_membuf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                     timeout_sec > 0 ? timeout_sec : 60L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ozon-ttn-c/1.0");

    rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    free(url);
    free(body);

    if (rc != CURLE_OK) {
        log_curl_error(rc, method);
        free(response.data);
        return NULL;
    }

    root = cJSON_Parse(response.data ? response.data : "");
    free(response.data);
    if (!root) {
        fprintf(stderr, "telegram: %s: invalid JSON\n", method);
        return NULL;
    }

    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"))) {
        cJSON *desc = cJSON_GetObjectItemCaseSensitive(root, "description");
        cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "error_code");
        fprintf(stderr, "telegram: %s failed (%d): %s\n", method,
                cJSON_IsNumber(code) ? code->valueint : 0,
                cJSON_IsString(desc) ? desc->valuestring : "unknown error");
        cJSON_Delete(root);
        return NULL;
    }

    result = cJSON_DetachItemFromObjectCaseSensitive(root, "result");
    cJSON_Delete(root);
    return result;
}

cJSON *tg_get_updates(tg_t *tg, long long offset)
{
    cJSON *params = cJSON_CreateObject();
    cJSON *allowed;

    if (!params)
        return NULL;

    cJSON_AddNumberToObject(params, "offset", (double)offset);
    cJSON_AddNumberToObject(params, "limit", 100);
    cJSON_AddNumberToObject(params, "timeout", 25);

    allowed = cJSON_AddArrayToObject(params, "allowed_updates");
    if (allowed) {
        cJSON_AddItemToArray(allowed, cJSON_CreateString("message"));
        cJSON_AddItemToArray(allowed, cJSON_CreateString("callback_query"));
    }

    return call_json(tg, "getUpdates", params, 40);
}

int tg_send_message(tg_t *tg, long long chat_id, const char *text,
                    cJSON *reply_markup, long long *out_message_id)
{
    cJSON *params = cJSON_CreateObject();
    cJSON *result;
    int ok = -1;

    if (out_message_id)
        *out_message_id = 0;
    if (!params)
        return -1;

    {
        char chat_buf[32];
        snprintf(chat_buf, sizeof(chat_buf), "%lld", chat_id);
        cJSON_AddStringToObject(params, "chat_id", chat_buf);
    }
    cJSON_AddStringToObject(params, "text", text ? text : "");
    if (reply_markup)
        cJSON_AddItemToObject(params, "reply_markup", reply_markup);

    result = call_json(tg, "sendMessage", params, 0);
    if (result) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(result, "message_id");
        if (out_message_id && cJSON_IsNumber(id))
            *out_message_id = (long long)id->valuedouble;
        cJSON_Delete(result);
        ok = 0;
    }
    return ok;
}

int tg_edit_message(tg_t *tg, long long chat_id, int message_id,
                    const char *text, cJSON *reply_markup)
{
    cJSON *params = cJSON_CreateObject();
    cJSON *result;

    if (!params) {
        cJSON_Delete(reply_markup);
        return -1;
    }

    {
        char chat_buf[32];
        snprintf(chat_buf, sizeof(chat_buf), "%lld", chat_id);
        cJSON_AddStringToObject(params, "chat_id", chat_buf);
    }
    cJSON_AddNumberToObject(params, "message_id", message_id);
    cJSON_AddStringToObject(params, "text", text ? text : "");
    if (reply_markup)
        cJSON_AddItemToObject(params, "reply_markup", reply_markup);

    result = call_json(tg, "editMessageText", params, 0);
    if (!result)
        return -1;
    cJSON_Delete(result);
    return 0;
}

int tg_answer_callback(tg_t *tg, const char *callback_id, const char *text)
{
    cJSON *params = cJSON_CreateObject();
    cJSON *result;

    if (!params)
        return -1;

    cJSON_AddStringToObject(params, "callback_query_id",
                            callback_id ? callback_id : "");
    if (text && *text)
        cJSON_AddStringToObject(params, "text", text);

    result = call_json(tg, "answerCallbackQuery", params, 0);
    if (!result)
        return -1;
    cJSON_Delete(result);
    return 0;
}

int tg_send_document(tg_t *tg, long long chat_id, const char *path,
                     const char *filename, const char *caption,
                     long long *out_message_id)
{
    CURL *curl;
    struct curl_slist *headers = NULL;
    curl_mime *mime = NULL;
    curl_mimepart *part;
    membuf response = {0};
    char url[1024];
    char chat_buf[32];
    CURLcode rc;
    cJSON *root = NULL;
    int ok = -1;

    if (!tg || !path)
        return -1;
    if (out_message_id)
        *out_message_id = 0;

    curl = tg->curl;
    snprintf(url, sizeof(url), "%ssendDocument", tg->base);
    snprintf(chat_buf, sizeof(chat_buf), "%lld", chat_id);

    headers = curl_slist_append(headers, "Expect:");

    mime = curl_mime_init(curl);
    if (!mime) {
        curl_slist_free_all(headers);
        return -1;
    }

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "chat_id");
    curl_mime_data(part, chat_buf, CURL_ZERO_TERMINATED);

    if (caption && *caption) {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "caption");
        curl_mime_data(part, caption, CURL_ZERO_TERMINATED);
    }

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "document");
    if (curl_mime_filedata(part, path) != CURLE_OK) {
        fprintf(stderr, "telegram: cannot attach %s\n", path);
        curl_mime_free(mime);
        return -1;
    }
    curl_mime_filename(part, filename ? filename : "document.pdf");
    curl_mime_type(part, "application/pdf");

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_membuf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ozon-ttn-c/1.0");

    rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_mime_free(mime);

    if (rc != CURLE_OK) {
        log_curl_error(rc, "sendDocument");
        free(response.data);
        return -1;
    }

    root = cJSON_Parse(response.data ? response.data : "");
    free(response.data);
    if (!root) {
        fprintf(stderr, "telegram: sendDocument: invalid JSON\n");
        return -1;
    }

    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"))) {
        cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
        cJSON *id = cJSON_GetObjectItemCaseSensitive(result, "message_id");
        if (out_message_id && cJSON_IsNumber(id))
            *out_message_id = (long long)id->valuedouble;
        ok = 0;
    } else {
        cJSON *desc = cJSON_GetObjectItemCaseSensitive(root, "description");
        fprintf(stderr, "telegram: sendDocument failed: %s\n",
                cJSON_IsString(desc) ? desc->valuestring : "unknown error");
    }

    cJSON_Delete(root);
    return ok;
}

char *tg_get_file_path(tg_t *tg, const char *file_id)
{
    cJSON *params = cJSON_CreateObject();
    cJSON *result;
    char *path = NULL;

    if (!params)
        return NULL;

    cJSON_AddStringToObject(params, "file_id", file_id ? file_id : "");
    result = call_json(tg, "getFile", params, 0);
    if (!result)
        return NULL;

    {
        cJSON *fp = cJSON_GetObjectItemCaseSensitive(result, "file_path");
        if (cJSON_IsString(fp) && fp->valuestring)
            path = strdup(fp->valuestring);
    }
    cJSON_Delete(result);
    return path;
}

int tg_download_file(tg_t *tg, const char *file_path, const char *dest_path)
{
    CURL *curl;
    FILE *fp;
    char url[2048];
    CURLcode rc;
    long http_code = 0;

    if (!tg || !file_path || !dest_path)
        return -1;

    fp = fopen(dest_path, "wb");
    if (!fp) {
        fprintf(stderr, "telegram: cannot create %s\n", dest_path);
        return -1;
    }

    snprintf(url, sizeof(url), "%s%s", tg->file_base, file_path);
    curl = tg->curl;

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ozon-ttn-c/1.0");

    rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    fclose(fp);

    if (rc != CURLE_OK) {
        log_curl_error(rc, "downloadFile");
        remove(dest_path);
        return -1;
    }
    if (http_code != 200) {
        fprintf(stderr, "telegram: downloadFile: HTTP %ld\n", http_code);
        remove(dest_path);
        return -1;
    }

    return 0;
}
