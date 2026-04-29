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
    BATCH_DUP_NONE,           /**< No duplicate found. */
    BATCH_DUP_FOUND,          /**< At least one duplicate ID is present. */
    BATCH_DUP_OUT_OF_MEMORY,  /**< Detection aborted: scratch allocation failed. */
} batch_dup_result;

/** @brief Check whether the flat ID buffer contains any duplicate entry. */
batch_dup_result batch_has_duplicate_ids(const void *ids, size_t count, size_t id_size);

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
