#define _POSIX_C_SOURCE 200809L

/*
 * db.c — SQLite-хранилище настроек и pending-ввода.
 */

#include "db.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct db {
    sqlite3 *conn;
};

static void set_err(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
}

db_t *db_open(const char *path, char *err, size_t errlen)
{
    db_t *db;
    char *sql_err = NULL;
    const char *schema =
        "CREATE TABLE IF NOT EXISTS settings ("
        "  chat_id INTEGER NOT NULL,"
        "  field   TEXT    NOT NULL,"
        "  value   TEXT    NOT NULL,"
        "  PRIMARY KEY (chat_id, field)"
        ");"
        "CREATE TABLE IF NOT EXISTS pending ("
        "  chat_id           INTEGER NOT NULL,"
        "  user_id           INTEGER NOT NULL,"
        "  field             TEXT    NOT NULL,"
        "  prompt_message_id INTEGER NOT NULL,"
        "  menu_message_id   INTEGER,"
        "  PRIMARY KEY (chat_id, user_id)"
        ");";

    db = calloc(1, sizeof(*db));
    if (!db) {
        set_err(err, errlen, "out of memory");
        return NULL;
    }

    if (sqlite3_open_v2(path, &db->conn,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "cannot open '%s': %s", path,
                 db->conn ? sqlite3_errmsg(db->conn) : "unknown error");
        sqlite3_close(db->conn);
        free(db);
        return NULL;
    }

    sqlite3_busy_timeout(db->conn, 5000);

    if (sqlite3_exec(db->conn, schema, NULL, NULL, &sql_err) != SQLITE_OK) {
        snprintf(err, errlen, "schema: %s", sql_err ? sql_err : "unknown error");
        sqlite3_free(sql_err);
        sqlite3_close(db->conn);
        free(db);
        return NULL;
    }

    return db;
}

void db_close(db_t *db)
{
    if (!db)
        return;
    sqlite3_close(db->conn);
    free(db);
}

int db_set(db_t *db, long long chat_id, const char *field, const char *value)
{
    static const char *sql =
        "INSERT INTO settings (chat_id, field, value) VALUES (?1, ?2, ?3) "
        "ON CONFLICT(chat_id, field) DO UPDATE SET value = excluded.value";
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (!db || !field || !value)
        return -1;

    rc = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, chat_id);
    sqlite3_bind_text(stmt, 2, field, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, value, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_clear(db_t *db, long long chat_id, const char *field)
{
    static const char *sql =
        "DELETE FROM settings WHERE chat_id = ?1 AND field = ?2";
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (!db || !field)
        return -1;

    rc = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, chat_id);
    sqlite3_bind_text(stmt, 2, field, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_get_value(db_t *db, long long chat_id, const char *field, char **out)
{
    static const char *sql =
        "SELECT value FROM settings WHERE chat_id = ?1 AND field = ?2";
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (out)
        *out = NULL;
    if (!db || !field)
        return -1;

    rc = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, chat_id);
    sqlite3_bind_text(stmt, 2, field, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (out && text) {
            *out = strdup((const char *)text);
            if (!*out) {
                sqlite3_finalize(stmt);
                return -1;
            }
        }
        sqlite3_finalize(stmt);
        return 1;
    }

    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_load_config(db_t *db, long long chat_id, ttn_config_t *cfg)
{
    static const char *sql =
        "SELECT field, value FROM settings WHERE chat_id = ?1";
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (!db || !cfg)
        return -1;

    memset(cfg, 0, sizeof(*cfg));

    rc = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, chat_id);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *field = sqlite3_column_text(stmt, 0);
        const unsigned char *value = sqlite3_column_text(stmt, 1);

        if (!field)
            continue;

        if (cfg->count < MAX_FIELDS) {
            ttn_field_entry_t *e = &cfg->entries[cfg->count++];
            snprintf(e->name, sizeof(e->name), "%s", (const char *)field);
            snprintf(e->value, sizeof(e->value), "%s",
                     value ? (const char *)value : "");
            e->has_value = 1;
        }
    }

    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_set_pending(db_t *db, long long chat_id, long long user_id,
                   const char *field, int prompt_message_id,
                   int menu_message_id)
{
    static const char *sql =
        "INSERT INTO pending (chat_id, user_id, field, prompt_message_id, menu_message_id) "
        "VALUES (?1, ?2, ?3, ?4, ?5) "
        "ON CONFLICT(chat_id, user_id) DO UPDATE SET "
        "  field = excluded.field,"
        "  prompt_message_id = excluded.prompt_message_id,"
        "  menu_message_id = excluded.menu_message_id";
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (!db || !field)
        return -1;

    rc = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, chat_id);
    sqlite3_bind_int64(stmt, 2, user_id);
    sqlite3_bind_text(stmt, 3, field, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, prompt_message_id);
    if (menu_message_id > 0)
        sqlite3_bind_int(stmt, 5, menu_message_id);
    else
        sqlite3_bind_null(stmt, 5);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_take_pending(db_t *db, long long chat_id, long long user_id,
                    pending_t *out)
{
    static const char *sel =
        "SELECT chat_id, user_id, field, prompt_message_id, menu_message_id "
        "FROM pending WHERE chat_id = ?1 AND user_id = ?2";
    static const char *del =
        "DELETE FROM pending WHERE chat_id = ?1 AND user_id = ?2";
    sqlite3_stmt *stmt = NULL;
    int rc;
    int found = 0;

    if (!db)
        return -1;

    rc = sqlite3_prepare_v2(db->conn, sel, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, chat_id);
    sqlite3_bind_int64(stmt, 2, user_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        found = 1;
        if (out) {
            const unsigned char *field = sqlite3_column_text(stmt, 2);
            out->chat_id = sqlite3_column_int64(stmt, 0);
            out->user_id = sqlite3_column_int64(stmt, 1);
            snprintf(out->field, sizeof(out->field), "%s",
                     field ? (const char *)field : "");
            out->prompt_message_id = sqlite3_column_int(stmt, 3);
            out->menu_message_id = sqlite3_column_type(stmt, 4) == SQLITE_NULL
                                       ? 0
                                       : sqlite3_column_int(stmt, 4);
        }
    }
    sqlite3_finalize(stmt);

    if (!found)
        return 0;

    rc = sqlite3_prepare_v2(db->conn, del, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, chat_id);
    sqlite3_bind_int64(stmt, 2, user_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE ? 1 : -1;
}

int db_clear_pending(db_t *db, long long chat_id, long long user_id)
{
    static const char *sql =
        "DELETE FROM pending WHERE chat_id = ?1 AND user_id = ?2";
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (!db)
        return -1;

    rc = sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, chat_id);
    sqlite3_bind_int64(stmt, 2, user_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}
