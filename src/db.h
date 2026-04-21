#pragma once

#include "internal.h"

/**
 * @file db.h
 * @brief SQLite helpers: metadata I/O, page lookup, and durability.
 */

/** Execute a SQL string, discarding any result rows. */
int db_exec(sqlite3 *db, const char *sql);

/** Enable WAL mode and set a generous busy timeout on @p db. */
void db_configure(sqlite3 *db);

/** Insert a uint32 value stored as a 4-byte little-endian blob. */
bool db_meta_write_u32(sqlite3 *db, const char *key, uint32_t val);

/** Insert a null-terminated ASCII string stored as a blob (including the NUL). */
bool db_meta_write_str(sqlite3 *db, const char *key, const char *val);

/** Read a uint32 value previously written by db_meta_write_u32. */
bool db_meta_read_u32(sqlite3 *db, const char *key, uint32_t *out);

/** Read a string value previously written by db_meta_write_str. */
bool db_meta_read_str(sqlite3 *db, const char *key, char *out, size_t max_len);

/** Update an existing uint32 metadata entry in place. */
bool db_meta_update_u32(sqlite3 *db, const char *key, uint32_t val);

/**
 * Find the ROWID of the page with the given identifier.
 *
 * @return > 0 on success, 0 if not found, -1 on SQLite error (@p sq_err is set).
 */
int64_t find_rowid(pcache_volume *v, const void *id, int *sq_err);

/**
 * fsync the data file and checkpoint the SQLite WAL.
 *
 * @return true on success, false if fsync failed (errno is set).
 */
bool do_sync(pcache_volume *v);
