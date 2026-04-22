#pragma once

/**
 * @file libpcache.h
 * @brief libpcache — persistent, paged, random-access storage indexed by key.
 *
 * A *volume* consists of two files: a binary data file and a SQLite index.
 * All public functions are thread-safe.
 *
 * @par Error reporting
 * Functions do not use their return value to signal errors.  Instead each
 * function accepts a pointer to a function-specific @c pcache_<fn>_error
 * enumeration.  Additional @p sqlite_error and @p posix_error out-parameters
 * carry the raw error codes from the underlying subsystems.  Any of these
 * pointers may be @c NULL if the caller does not need the information.
 */

#include <stdbool.h>
#include <stdint.h>

#include "libpcache_errors.h"

/* ──────────── Types ──────────── */

/**
 * @brief Opaque descriptor for an open volume.
 *
 * Zero denotes failure; any positive value is a valid descriptor that must be
 * supplied to every subsequent operation on the volume.
 */
typedef int pcache_handle;

/**
 * @brief Capacity policy applied when the volume is full.
 */
typedef enum pcache_capacity_policy {
    PCACHE_CAPACITY_FIXED = 0, /**< Writes beyond @c max_pages fail. */
    PCACHE_CAPACITY_FIFO  = 1, /**< Writes beyond @c max_pages evict the oldest page. */
} pcache_capacity_policy;

/**
 * @brief Paths to the two files that compose a volume.
 *
 * Both strings must be null-terminated UTF-8.
 */
typedef struct pcache_file_pair {
    const char *database_path; /**< Path to the SQLite index database. */
    const char *data_path;     /**< Path to the binary data file. */
} pcache_file_pair;

/**
 * @brief Immutable configuration parameters of a volume.
 *
 * These values are fixed at creation time and stored in the @c metadata table.
 */
typedef struct pcache_configuration {
    pcache_capacity_policy capacity_policy; /**< Eviction policy. */
    uint32_t               page_size;       /**< Size of every page, in bytes. */
    uint32_t               max_pages;       /**< Maximum number of pages the volume can hold. */
    uint32_t               id_size;         /**< Length of every page identifier, in bytes. */
} pcache_configuration;

/**
 * @brief Progress callback used by long-running operations.
 *
 * @param progress  Fraction of work completed, in the closed interval [0, 1].
 * @param user_data Opaque pointer supplied by the caller.
 * @return @c true to continue; @c false to request cancellation.
 */
typedef bool (*pcache_progress_fn)(double progress, void *user_data);

/* ──────────── Volume lifecycle ──────────── */

/**
 * @brief Create a new volume on the filesystem.
 *
 * Fails immediately if either file already exists.
 *
 * @param paths                Paths to the database and data files.
 * @param config               Volume parameters (page size, capacity, …).
 * @param preallocate_database If @c true, pre-allocate slots in the index for
 *                              all @c max_pages pages, enabling O(1) allocation
 *                              on subsequent inserts.
 * @param preallocate_datafile If @c true, extend the data file to its maximum
 *                              size immediately.
 * @param error                Receives the operation outcome; may be @c NULL.
 * @param sqlite_error         Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error          Receives @c errno on I/O failure; may be @c NULL.
 */
void pcache_create(const pcache_file_pair     *paths,
                   const pcache_configuration *config,
                   bool                        preallocate_database,
                   bool                        preallocate_datafile,
                   pcache_create_error        *error,
                   int                        *sqlite_error,
                   int                        *posix_error);

/**
 * @brief Open an existing volume.
 *
 * @param paths             Paths to the database and data files.
 * @param preload_free_list If @c true, load all recyclable slot identifiers at
 *                          open time (higher startup cost, faster inserts).
 *                          If @c false, slots are located lazily. Ignored on
 *                          FIFO volumes.
 * @param error             Receives the operation outcome; may be @c NULL.
 * @param sqlite_error      Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error       Receives @c errno on I/O failure; may be @c NULL.
 * @return Positive descriptor on success, or zero on failure.
 */
pcache_handle pcache_open(const pcache_file_pair *paths,
                          bool                    preload_free_list,
                          pcache_open_error      *error,
                          int                    *sqlite_error,
                          int                    *posix_error);

/**
 * @brief Close an open volume and release all associated resources.
 *
 * @param handle       Descriptor returned by ::pcache_open.
 * @param error        Receives the operation outcome; may be @c NULL.
 * @param sqlite_error Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error  Receives @c errno on I/O failure; may be @c NULL.
 */
void pcache_close(pcache_handle handle, pcache_close_error *error, int *sqlite_error, int *posix_error);

/* ──────────── Introspection ──────────── */

/**
 * @brief Return the configuration of an open volume.
 *
 * The configuration is cached in memory after ::pcache_open.
 *
 * @param handle       Open volume descriptor.
 * @param error        Receives the operation outcome; may be @c NULL.
 * @param sqlite_error Unused; reserved for future use. May be @c NULL.
 * @return The volume configuration, or an unspecified value on error.
 */
pcache_configuration
pcache_get_configuration(pcache_handle handle, pcache_get_configuration_error *error, int *sqlite_error);

/* ──────────── Page operations ──────────── */

/**
 * @brief Store a page identified by @p id.
 *
 * On FIFO volumes, pages beyond @c max_pages are evicted automatically.
 * On FIXED volumes, writes beyond capacity fail.
 *
 * @param handle              Open volume descriptor.
 * @param id                  Page identifier; must be exactly @c id_size bytes.
 * @param page_data           Page content; must be at least @c page_size bytes.
 * @param check_id_uniqueness If @c true, verify that no page with the same
 *                            identifier already exists before writing.
 * @param durable             If @c true, block until data is durable on disk.
 * @param error               Receives the operation outcome; may be @c NULL.
 * @param sqlite_error        Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error         Receives @c errno on I/O failure; may be @c NULL.
 */
void pcache_put_page(pcache_handle          handle,
                     const void            *id,
                     const void            *page_data,
                     bool                   check_id_uniqueness,
                     bool                   durable,
                     pcache_put_page_error *error,
                     int                   *sqlite_error,
                     int                   *posix_error);

/**
 * @brief Retrieve the page identified by @p id.
 *
 * @param handle       Open volume descriptor.
 * @param id           Page identifier; must be exactly @c id_size bytes.
 * @param page_buffer  Destination buffer; must be at least @c page_size bytes.
 * @param error        Receives the operation outcome; may be @c NULL.
 * @param sqlite_error Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error  Receives @c errno on I/O failure; may be @c NULL.
 */
void pcache_get_page(pcache_handle          handle,
                     const void            *id,
                     void                  *page_buffer,
                     pcache_get_page_error *error,
                     int                   *sqlite_error,
                     int                   *posix_error);

/**
 * @brief Test whether a page identified by @p id exists in the volume.
 *
 * @param handle       Open volume descriptor.
 * @param id           Page identifier; must be exactly @c id_size bytes.
 * @param error        Receives the operation outcome; may be @c NULL.
 * @param sqlite_error Receives the SQLite error code on failure; may be @c NULL.
 * @return @c true if the page exists, @c false otherwise (or on error).
 */
bool pcache_check_page(pcache_handle handle, const void *id, pcache_check_page_error *error, int *sqlite_error);

/**
 * @brief Delete the page identified by @p id from the volume.
 *
 * @param handle         Open volume descriptor.
 * @param id             Page identifier; must be exactly @c id_size bytes.
 * @param wipe_data_file If @c true, overwrite the page data with zeros.
 * @param durable        If @c true, block until data is durable on disk.
 * @param error          Receives the operation outcome; may be @c NULL.
 * @param sqlite_error   Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error    Receives @c errno on I/O failure; may be @c NULL.
 */
void pcache_delete_page(pcache_handle             handle,
                        const void               *id,
                        bool                      wipe_data_file,
                        bool                      durable,
                        pcache_delete_page_error *error,
                        int                      *sqlite_error,
                        int                      *posix_error);

/* ──────────── Maintenance ──────────── */

/**
 * @brief Relocate live pages contiguously toward the start of the data file.
 *
 * The @p progress_callback is invoked after each page and may cancel the
 * operation by returning @c false; the volume remains consistent in all cases.
 *
 * @param handle              Open volume descriptor.
 * @param progress_callback   Called after each page; @c NULL disables callbacks.
 * @param progress_user_data  Passed verbatim to @p progress_callback.
 * @param shrink_file         If @c true, truncate the data file to the minimum
 *                            size after relocation.
 * @param durable             If @c true, block until data is durable on disk.
 * @param error               Receives the operation outcome; may be @c NULL.
 * @param sqlite_error        Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error         Receives @c errno on I/O failure; may be @c NULL.
 */
void pcache_defragment(pcache_handle            handle,
                       pcache_progress_fn       progress_callback,
                       void                    *progress_user_data,
                       bool                     shrink_file,
                       bool                     durable,
                       pcache_defragment_error *error,
                       int                     *sqlite_error,
                       int                     *posix_error);

/**
 * @brief Adjust the maximum capacity of the volume.
 *
 * Growth is always permitted.  On FIXED volumes, reduction fails if any live
 * page would end up beyond the new limit (defragment first).  On FIFO volumes,
 * the oldest pages are evicted silently to fit.
 *
 * @param handle        Open volume descriptor.
 * @param new_max_pages New maximum page count; must be ≥ 1.
 * @param durable       If @c true, block until data is durable on disk.
 * @param error         Receives the operation outcome; may be @c NULL.
 * @param sqlite_error  Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error   Receives @c errno on I/O failure; may be @c NULL.
 */
void pcache_set_max_pages(pcache_handle               handle,
                          uint32_t                    new_max_pages,
                          bool                        durable,
                          pcache_set_max_pages_error *error,
                          int                        *sqlite_error,
                          int                        *posix_error);

/**
 * @brief Preallocate space in an already-open volume.
 *
 * @p preallocate_database reserves slots in the index; @p preallocate_datafile
 * extends the data file to its maximum size.
 *
 * @param handle               Open volume descriptor.
 * @param preallocate_database Preallocate the index database.
 * @param preallocate_datafile Preallocate the data file.
 * @param durable              If @c true, block until data is durable on disk.
 * @param error                Receives the operation outcome; may be @c NULL.
 * @param sqlite_error         Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error          Receives @c errno on I/O failure; may be @c NULL.
 */
void pcache_preallocate(pcache_handle             handle,
                        bool                      preallocate_database,
                        bool                      preallocate_datafile,
                        bool                      durable,
                        pcache_preallocate_error *error,
                        int                      *sqlite_error,
                        int                      *posix_error);
