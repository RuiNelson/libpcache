#pragma once

#include "internal.h"

/**
 * @file handle.h
 * @brief Global handle table: allocation, release, and lookup of open volumes.
 */

/** Reserve a free slot and return a pointer to it, or NULL if the table is full. */
pcache_volume *allocate_slot(void);

/** Mark the slot occupied by @p v as free. */
void release_slot(pcache_volume *volume);

/**
 * Validate @p h and return the corresponding volume, or NULL if the handle is
 * invalid or refers to a closed volume.
 */
pcache_volume *volume_from_handle(pcache_handle handle);

/**
 * Validate @p h, mark it closed, and return the locked volume.
 *
 * No new operations can acquire the handle after this function returns.
 */
pcache_volume *volume_from_handle_for_close(pcache_handle handle);

/** Compute the public handle value for an allocated slot. */
pcache_handle handle_of(const pcache_volume *volume);
