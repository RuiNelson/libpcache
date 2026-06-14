#pragma once

#include "libpcache.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* ── Constants ── */

#define PCACHE_SCHEMA_VERSION 1
#define DB_META_STR_MAX_BYTES 128

/* ── Per-volume runtime state ── */

/** @brief Per-volume runtime state: one entry in the open-handle table. */
typedef struct {
    bool          in_use;                   /**< Whether this slot is occupied by an open volume. */
    pcache_handle self_handle;              /**< The public handle value for this slot; set once on first activation. */
    sqlite3      *db;                       /**< SQLite database connection for the index. */
    sqlite3_stmt *statement_find_rowid;     /**< Prepared statement: look up a page ROWID by ID. */
    sqlite3_stmt *statement_insert;         /**< Prepared statement: insert a new page row. */
    sqlite3_stmt *statement_update_slot;    /**< Prepared statement: overwrite an existing page slot. */
    sqlite3_stmt *statement_delete;         /**< Prepared statement: delete a page row. */
    sqlite3_stmt *statement_find_free_slot; /**< FIXED policy: next NULL slot with rowid > ?. */
    sqlite3_stmt *statement_find_fifo_cursor; /**< FIFO policy: lowest empty rowid whose predecessor is occupied. */
    int           fd;                         /**< File descriptor for the data file. */
    pcache_configuration config;              /**< Volume configuration (page size, policy, capacity, etc.). */
    size_t               row_count;           /**< Total rows in the pages table, including NULL slots. */
    uint8_t             *wipe_buffer;         /**< Cached zero buffer used to wipe deleted page slots. */
    pthread_mutex_t      mutex;               /**< Per-volume mutex for thread safety. */
} pcache_volume;

#ifdef PCACHE_TESTING
void pcache_test_fail_put_pwrite_after(size_t successful_writes);
void pcache_test_fail_defragment_metadata_after(size_t successful_loads);
void pcache_test_fail_sync(bool fail);
#endif
