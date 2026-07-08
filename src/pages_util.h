#pragma once

#include "internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/** @brief Saved page state used to roll back a partial write failure. */
typedef struct {
    bool     needs_restore; /**< Whether this entry requires a rollback write. */
    int64_t  rowid;         /**< ROWID of the page slot to restore. */
    uint8_t *page_data;     /**< Saved copy of the original page data. */
} page_restore;

/** @brief Zero out the page slot at the given rowid in the data file. */
bool wipe_page_at_rowid(pcache_volume *volume, int64_t rowid);

/** @brief Release memory for an array of page_restore entries. */
void free_page_restores(page_restore *restores, size_t count);

/** @brief Write back page data from a page_restore array, stopping on the first failure. */
bool restore_pages(pcache_volume *volume, page_restore *restores, size_t count, int *posix_error);

/** @brief Write a single page to the data file at the given byte offset using pwrite. */
ssize_t put_pwrite(pcache_volume *volume, const void *page, size_t page_size, off_t byte_offset);

typedef enum {
    BATCH_DUP_NONE,          /**< No duplicate found. */
    BATCH_DUP_FOUND,         /**< At least one duplicate ID is present. */
    BATCH_DUP_OUT_OF_MEMORY, /**< Detection aborted: scratch allocation failed. */
} batch_dup_result;

/** @brief Check whether the flat ID buffer contains any duplicate entry. */
batch_dup_result batch_has_duplicate_ids(const void *ids, size_t count, size_t id_size);

/**
 * @brief Locate the implicit FIFO cursor: the lowest empty rowid whose predecessor
 *        (with wrap-around 1 -> max_pages) is occupied.
 *
 * Returns 1 when no slot matches (volume completely empty or pages table not yet
 * populated), -1 on SQLite error.
 */
int64_t find_fifo_cursor(pcache_volume *volume, int *sqlite_return_code);

/**
 * @brief Rowid of the last empty slot of the FIFO cursor run.
 *
 * Only meaningful while the volume satisfies the single-empty-run invariant.
 * Returns 0 when the volume holds no live pages, -1 on SQLite error.
 */
int64_t fifo_locate_run_end(pcache_volume *volume, int *sqlite_return_code);

typedef enum {
    FIFO_COMPACT_OK,            /**< All holes merged into the cursor run. */
    FIFO_COMPACT_SQLITE_ERROR,  /**< Index operation failed. */
    FIFO_COMPACT_IO_ERROR,      /**< Page copy in the data file failed. */
    FIFO_COMPACT_OUT_OF_MEMORY, /**< Scratch allocation failed. */
} fifo_compact_result;

/**
 * @brief Merge every empty run of a FIFO volume into the single run ending at
 *        @p run_end, relocating live pages while preserving circular age order.
 *
 * When the pages table holds fewer than max_pages rows (fill-up phase),
 * @p run_end is ignored: pages compact toward rowid 1 and the trailing NULL
 * rows are removed. Each relocation is committed in its own transaction so a
 * crash never leaves a live page unreachable. Caller must hold volume->mutex.
 */
fifo_compact_result
fifo_compact_holes(pcache_volume *volume, int64_t run_end, int *sqlite_return_code, int *posix_return_code);

/** @brief Validate arguments for ::pcache_put_pages_with_counter. */
bool validate_with_counter_args(size_t            id_size,
                                size_t            count,
                                uint32_t          start,
                                uint32_t          position,
                                pcache_endianness endianness,
                                size_t           *counter_offset_out);

/** @brief Build a contiguous ID buffer for ::pcache_put_pages_with_counter. */
uint8_t *build_with_counter_ids(size_t            count,
                                size_t            id_size,
                                const void       *id_base,
                                uint32_t          start,
                                size_t            counter_offset,
                                pcache_endianness endianness);
