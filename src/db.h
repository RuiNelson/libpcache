#pragma once

#include "internal.h"

/**
 * @file db.h
 * @brief SQLite helpers: metadata I/O, page lookup, and durability.
 */

/**
 * @brief Execute a SQL string, discarding any result rows.
 * @return SQLite result code (SQLITE_OK on success).
 */
int db_exec(sqlite3 *db, const char *sql);

/**
 * @brief Enable WAL mode and set a generous busy timeout.
 */
void db_configure(sqlite3 *db);

/**
 * @brief Insert a uint32 value stored as a 4-byte little-endian blob.
 * @return true on success, false on failure.
 */
bool db_meta_write_u32(sqlite3 *db, const char *key, uint32_t value);

/**
 * @brief Insert a null-terminated string stored as a blob (including the NUL).
 * @return true on success, false on failure.
 */
bool db_meta_write_str(sqlite3 *db, const char *key, const char *value);

/**
 * @brief Read a uint32 value previously written by db_meta_write_u32.
 * @return SQLITE_ROW on success, SQLITE_DONE if the key does not exist,
 *         SQLITE_CORRUPT if the stored blob has an unexpected size, or
 *         another SQLite error code on failure.
 */
int db_meta_read_u32(sqlite3 *db, const char *key, uint32_t *value);

/**
 * @brief Read a string value previously written by db_meta_write_str.
 * @return SQLITE_ROW on success, SQLITE_DONE if the key does not exist,
 *         SQLITE_CORRUPT if the stored blob is invalid or too large, or
 *         another SQLite error code on failure.
 */
int db_meta_read_str(sqlite3 *db, const char *key, char *value, size_t max_len);

/**
 * @brief Update an existing uint32 metadata entry in place.
 * @return true on success, false on failure.
 */
bool db_meta_update_u32(sqlite3 *db, const char *key, uint32_t val);

/**
 * @brief Converts a ROWID to the corresponding byte offset in the data file.
 * @return Byte offset (ROWID 1 maps to offset 0).
 */
off_t rowid_to_offset(int64_t rowid, size_t page_size);

/**
 * @brief Find the ROWID of the page with the given identifier.
 * @return > 0 on success, 0 if not found, -1 on SQLite error.
 */
int64_t find_rowid(pcache_volume *volume, const void *id, int *sqlite_error);

/**
 * @brief fsync the data file and checkpoint the SQLite WAL.
 * @param volume The volume to synchronize.
 * @param posix_error Pointer to store errno if fsync fails (can be NULL).
 * @param sqlite_error Pointer to store SQLite error code if checkpoint fails (can be NULL).
 */
void wait_for_synchronization(pcache_volume *volume, int *posix_error, int *sqlite_error);

/**
 * @brief Synchronize data to disk if durability is requested.
 * @return true if sync succeeded or was not needed, false on failure.
 */
bool sync_if_durable(pcache_volume *volume, bool durable, int *posix_error, int *sqlite_error);

/**
 * @brief Prepare a single-parameter statement, bind an int64, step, and finalize it.
 * @return SQLITE_OK on success, or an SQLite error code.
 */
int db_exec_bind_int64(sqlite3 *db, const char *sql, int64_t value);
