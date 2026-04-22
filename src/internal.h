#pragma once

#include "libpcache.h"

#include <sqlite3.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants ── */

#define PCACHE_SCHEMA_VERSION 1
#define PCACHE_FL_CHUNK       64 /* initial free-list capacity */

/* ── Helpers ── */

#define SET_ERR(p, v) \
    do {              \
        if (p)        \
            *(p) = (v); \
    } while (0)

/* ── rowid_vec: growable array of SQLite ROWIDs used as a free list ── */

typedef struct {
    int64_t *data;
    size_t   count;
    size_t   capacity;
} rowid_vec;

static inline bool rv_push(rowid_vec *rv, int64_t id)
{
    if (rv->count >= rv->capacity) {
        size_t   cap = rv->capacity ? rv->capacity * 2 : PCACHE_FL_CHUNK;
        int64_t *p   = realloc(rv->data, cap * sizeof *p);
        if (!p)
            return false;
        rv->data     = p;
        rv->capacity = cap;
    }
    rv->data[rv->count++] = id;
    return true;
}

static inline int64_t rv_pop(rowid_vec *rv)
{
    return rv->count ? rv->data[--rv->count] : 0;
}

static inline void rv_free(rowid_vec *rv)
{
    free(rv->data);
    rv->data     = NULL;
    rv->count    = 0;
    rv->capacity = 0;
}

/* ── Per-volume runtime state ── */

typedef struct {
    bool                 in_use;
    sqlite3             *db;
    int                  data_fd;
    pcache_configuration config;
    uint32_t             row_count; /* total rows in pages table (incl. NULL) */
    int64_t              fifo_next; /* FIFO: ROWID of next slot to overwrite  */
    rowid_vec            free_list; /* FIXED: recyclable ROWIDs               */
    pthread_mutex_t           mutex;
} pcache_volume;
