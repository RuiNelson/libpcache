#include "db.h"
#include "macros.h"

#include <errno.h>
#include <unistd.h>
#include <xxhash.h>

off_t rowid_to_offset(int64_t rowid, size_t page_size) {
    return (off_t)(rowid - 1) * page_size;
}

int db_exec(sqlite3 *db, const char *sql) {
    return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

void db_configure(sqlite3 *db) {
    db_exec(db, "PRAGMA journal_mode=WAL");
    sqlite3_busy_timeout(db, 5000);
}

bool db_meta_write_u32(sqlite3 *db, const char *key, uint32_t val) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "INSERT INTO metadata(key,value) VALUES(?,?)", -1, &s, NULL) != SQLITE_OK)
        return false;
    uint8_t buf[4] = {(uint8_t)(val), (uint8_t)(val >> 8), (uint8_t)(val >> 16), (uint8_t)(val >> 24)};
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, buf, 4, SQLITE_TRANSIENT);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

bool db_meta_write_str(sqlite3 *db, const char *key, const char *val) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "INSERT INTO metadata(key,value) VALUES(?,?)", -1, &s, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, val, (int)strlen(val) + 1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

int db_meta_read_u32(sqlite3 *db, const char *key, uint32_t *out) {
    sqlite3_stmt *s;
    int           prepare_rc = sqlite3_prepare_v2(db, "SELECT value FROM metadata WHERE key=?", -1, &s, NULL);
    if (prepare_rc != SQLITE_OK)
        return prepare_rc;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    int step_rc = sqlite3_step(s);
    int result  = step_rc;
    if (step_rc == SQLITE_ROW) {
        const uint8_t *b = sqlite3_column_blob(s, 0);
        if (b && sqlite3_column_bytes(s, 0) == 4)
            *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        else
            result = SQLITE_CORRUPT;
    }
    sqlite3_finalize(s);
    return result;
}

int db_meta_read_str(sqlite3 *db, const char *key, char *out, size_t max_len) {
    sqlite3_stmt *statement;
    int           prepare_rc = sqlite3_prepare_v2(db, "SELECT value FROM metadata WHERE key=?", -1, &statement, NULL);
    if (prepare_rc != SQLITE_OK)
        return prepare_rc;
    sqlite3_bind_text(statement, 1, key, -1, SQLITE_STATIC);
    int step_rc = sqlite3_step(statement);
    int result  = step_rc;
    if (step_rc == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(statement, 0);
        int         size = sqlite3_column_bytes(statement, 0);
        if (blob && size > 0 && size <= DB_META_STR_MAX_BYTES && (size_t)size <= max_len) {
            memcpy(out, blob, size);
            /* Intentional truncation: if no null terminator exists within the blob,
             * overwrite the last byte so the output is always null-terminated.
             * This matches the on-disk format written by db_meta_write_str, which
             * stores the full string including NUL as a blob (no separate length). */
            if (memchr(out, '\0', size) == NULL)
                out[size - 1] = '\0';
        } else {
            result = SQLITE_CORRUPT;
        }
    }
    sqlite3_finalize(statement);
    return result;
}

bool db_meta_update_u32(sqlite3 *db, const char *key, uint32_t val) {
    sqlite3_stmt *statement;
    if (sqlite3_prepare_v2(db, "UPDATE metadata SET value=? WHERE key=?", -1, &statement, NULL) != SQLITE_OK)
        return false;
    uint8_t buf[4] = {(uint8_t)(val), (uint8_t)(val >> 8), (uint8_t)(val >> 16), (uint8_t)(val >> 24)};
    sqlite3_bind_blob(statement, 1, buf, 4, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, key, -1, SQLITE_STATIC);
    int sqlite_rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    return sqlite_rc == SQLITE_DONE;
}

int64_t find_rowid(pcache_volume *volume, const void *id, int *sqlite_error) {
    unsigned int hash = XXH32(id, volume->config.id_size, 0);
    if (!volume->statement_find_rowid) {
        SET_ERR(sqlite_error, SQLITE_MISUSE);
        return -1;
    }
    sqlite3_reset(volume->statement_find_rowid);
    sqlite3_clear_bindings(volume->statement_find_rowid);
    sqlite3_bind_int64(volume->statement_find_rowid, 1, (int64_t)(unsigned int)hash);
    sqlite3_bind_blob(volume->statement_find_rowid, 2, id, (int)volume->config.id_size, SQLITE_STATIC);
    int64_t rowid     = 0;
    int     sqlite_rc = sqlite3_step(volume->statement_find_rowid);
    if (sqlite_rc == SQLITE_ROW) {
        rowid = sqlite3_column_int64(volume->statement_find_rowid, 0);
    } else if (sqlite_rc != SQLITE_DONE) {
        SET_ERR(sqlite_error, sqlite_rc);
        rowid = -1;
    }
    return rowid;
}

void wait_for_synchronization(pcache_volume *volume, int *posix_error, int *sqlite_error) {
    if (fsync(volume->fd) != 0) {
        SET_ERR(posix_error, errno);
        return;
    }
    int sqlite_rc = sqlite3_wal_checkpoint_v2(volume->db, NULL, SQLITE_CHECKPOINT_RESTART, NULL, NULL);
    if (sqlite_rc != SQLITE_OK) {
        SET_ERR(sqlite_error, sqlite_rc);
    }
}

bool sync_if_durable(pcache_volume *volume, bool durable, int *posix_error, int *sqlite_error) {
    if (durable) {
        wait_for_synchronization(volume, posix_error, sqlite_error);
        return NO_ERRORS(posix_error, sqlite_error);
    }
    return true;
}

int db_exec_bind_int64(sqlite3 *db, const char *sql, int64_t value) {
    sqlite3_stmt *statement;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK)
        return sqlite3_errcode(db);
    sqlite3_bind_int64(statement, 1, value);
    int sqlite_return_code = sqlite3_step(statement);
    sqlite3_finalize(statement);
    return (sqlite_return_code == SQLITE_DONE) ? SQLITE_OK : sqlite_return_code;
}
