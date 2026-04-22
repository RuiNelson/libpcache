#include "db.h"

#include <unistd.h>

#include <xxhash.h>

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
    sqlite3_bind_blob(s, 2, val, (int)strlen(val) + 1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

bool db_meta_read_u32(sqlite3 *db, const char *key, uint32_t *out) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "SELECT value FROM metadata WHERE key=?", -1, &s, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    bool ok = false;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const uint8_t *b = sqlite3_column_blob(s, 0);
        if (b && sqlite3_column_bytes(s, 0) >= 4) {
            *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
            ok   = true;
        }
    }
    sqlite3_finalize(s);
    return ok;
}

bool db_meta_read_str(sqlite3 *db, const char *key, char *out, size_t max_len) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "SELECT value FROM metadata WHERE key=?", -1, &s, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    bool ok = false;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const void *b = sqlite3_column_blob(s, 0);
        int         n = sqlite3_column_bytes(s, 0);
        if (b && n > 0 && (size_t)n < max_len) {
            memcpy(out, b, n);
            ok = true;
        }
    }
    sqlite3_finalize(s);
    return ok;
}

bool db_meta_update_u32(sqlite3 *db, const char *key, uint32_t val) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "UPDATE metadata SET value=? WHERE key=?", -1, &s, NULL) != SQLITE_OK)
        return false;
    uint8_t buf[4] = {(uint8_t)(val), (uint8_t)(val >> 8), (uint8_t)(val >> 16), (uint8_t)(val >> 24)};
    sqlite3_bind_blob(s, 1, buf, 4, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

int64_t find_rowid(pcache_volume *v, const void *id, int *sq_err) {
    uint32_t      h  = XXH32(id, v->config.id_size, 0);
    sqlite3_stmt *s  = NULL;
    int           rc = sqlite3_prepare_v2(v->db, "SELECT rowid FROM pages WHERE id_hash=? AND id=?", -1, &s, NULL);
    if (rc != SQLITE_OK) {
        SET_ERR(sq_err, rc);
        return -1;
    }
    sqlite3_bind_int64(s, 1, (int64_t)(uint32_t)h);
    sqlite3_bind_blob(s, 2, id, (int)v->config.id_size, SQLITE_STATIC);
    int64_t rowid = 0;
    rc            = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        rowid = sqlite3_column_int64(s, 0);
    } else if (rc != SQLITE_DONE) {
        SET_ERR(sq_err, rc);
        rowid = -1;
    }
    sqlite3_finalize(s);
    return rowid;
}

bool do_sync(pcache_volume *v) {
    if (fsync(v->data_fd) != 0)
        return false;
    int rc = sqlite3_wal_checkpoint_v2(v->db, NULL, SQLITE_CHECKPOINT_RESTART, NULL, NULL);
    return rc == SQLITE_OK;
}
