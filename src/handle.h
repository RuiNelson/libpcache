#pragma once

#include "internal.h"

/**
 * @file handle.h
 * @brief Global handle table: allocation, release, and lookup of open volumes.
 */

/** Reserve a free slot and return a pointer to it, or NULL if the table is full. */
pcache_volume *alloc_slot(void);

/** Mark the slot occupied by @p v as free. */
void release_slot(pcache_volume *v);

/**
 * Validate @p h and return the corresponding volume, or NULL if the handle is
 * invalid or refers to a closed volume.
 */
pcache_volume *vol_from_handle(pcache_handle h);

/** Compute the public handle value for an allocated slot. */
pcache_handle handle_of(const pcache_volume *v);
