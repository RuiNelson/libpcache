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
#include <stddef.h>
#include <stdint.h>

#include "libpcache_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────── Types ──────────── */

/**
 * @brief Byte order used when embedding the counter in ::pcache_put_pages_with_counter,
 *        ::pcache_get_pages_with_counter, ::pcache_check_pages_with_counter, and
 *        ::pcache_delete_pages_with_counter.
 */
typedef enum pcache_endianness {
    PCACHE_ENDIANNESS_NATIVE = 0,        /**< Host byte order.  Not recommended: volumes become
                                              non-portable across machines with different byte orders. */
    PCACHE_ENDIANNESS_LITTLE_ENDIAN = 1, /**< Least-significant byte at the lowest index. */
    PCACHE_ENDIANNESS_BIG_ENDIAN    = 2, /**< Most-significant byte at the lowest index. */
} pcache_endianness;

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
 * @brief Page occupancy counts for an open volume.
 */
typedef struct pcache_page_count {
    uint32_t used; /**< Number of pages currently stored. */
    uint32_t free; /**< Number of available slots (max_pages - used). */
} pcache_page_count;

/**
 * @brief Progress callback used by long-running operations.
 *
 * The callback is invoked while the volume's internal mutex is held.
 * Calling any @c pcache_ function that operates on the same volume from
 * within the callback will deadlock.
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
 * @param error             Receives the operation outcome; may be @c NULL.
 * @param sqlite_error      Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error       Receives @c errno on I/O failure; may be @c NULL.
 * @return Positive descriptor on success, or zero on failure.
 */
pcache_handle pcache_open(const pcache_file_pair *paths, pcache_open_error *error, int *sqlite_error, int *posix_error);

/**
 * @brief Close an open volume and release all associated resources.
 *
 * On return, the data file and index database are fsync'd before the handles
 * are closed, ensuring all data is persisted to stable storage.
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
 * @param error Receives the operation outcome; may be @c NULL.
 * @return The volume configuration, or an unspecified value on error.
 */
pcache_configuration pcache_inspect_configuration(pcache_handle handle, pcache_inspect_configuration_error *error);

/**
 * @brief Return the number of used and free pages in an open volume.
 *
 * @param handle       Open volume descriptor.
 * @param error        Receives the operation outcome; may be @c NULL.
 * @param sqlite_error Receives the SQLite error code on failure; may be @c NULL.
 * @return Page counts, or an unspecified value on error.
 */
pcache_page_count
pcache_inspect_page_count(pcache_handle handle, pcache_inspect_page_count_error *error, int *sqlite_error);

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
 * @param fail_if_exists If @c true, verify that no page with the same
 *                            identifier already exists before writing.
 * @param durable             If @c true, block until data is durable on disk.
 * @param error               Receives the operation outcome; may be @c NULL.
 * @param sqlite_error        Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error         Receives @c errno on I/O failure; may be @c NULL.
 */
void pcache_put_page(pcache_handle     handle,
                     const void       *id,
                     const void       *page_data,
                     bool              fail_if_exists,
                     bool              durable,
                     pcache_put_error *error,
                     int              *sqlite_error,
                     int              *posix_error);

/**
 * @brief Store multiple pages in a single atomic operation.
 *
 * The operation is atomic: either all pages are written, or none are.
 * On FIFO volumes, pages beyond @c max_pages are evicted automatically.
 * On FIXED volumes, writes beyond capacity fail.
 *
 * @param handle              Open volume descriptor.
 * @param count               Number of pages to store.
 * @param ids                 Page identifiers; must be @c count * id_size bytes.
 * @param pages_data          Page contents; must be @c count * page_size bytes.
 * @param fail_if_exists If @c true, verify that none of the identifiers
 *                            already exist before writing.
 * @param durable             If @c true, block until data is durable on disk.
 * @param error               Receives the operation outcome; may be @c NULL.
 * @param sqlite_error        Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error         Receives @c errno on I/O failure; may be @c NULL.
 */
void pcache_put_pages(pcache_handle     handle,
                      size_t            count,
                      const void       *ids,
                      const void       *pages_data,
                      bool              fail_if_exists,
                      bool              durable,
                      pcache_put_error *error,
                      int              *sqlite_error,
                      int              *posix_error);

/**
 * @brief Store multiple pages, computing each identifier automatically from a template.
 *
 * Starting from @p id_base, computes @p count identifiers by XORing a @c uint32_t
 * counter — initial value @p start, incremented by one per page — into four consecutive
 * bytes of the template.  The counter occupies bytes at indices
 * <tt>[id_size − 4 − position, id_size − 1 − position]</tt>; @p position = 0 places
 * it in the last four bytes.  @p endianness controls the byte order of the counter
 * within those four bytes.
 *
 * The library validates that @p position is in bounds, that the counter does not
 * overflow, and that @p endianness is a recognized enum value, failing with
 * @c PCACHE_PUT_INVALID_ARGUMENT if any check fails.
 * Because counter values are all distinct and XOR with a fixed base preserves distinctness,
 * identifiers within the batch are guaranteed unique by construction; the intra-batch
 * duplicate check is skipped.
 * In all other respects the operation is identical to ::pcache_put_pages.
 * See also ::pcache_get_pages_with_counter, ::pcache_check_pages_with_counter,
 * and ::pcache_delete_pages_with_counter for the read, existence-check and delete
 * counterparts using the same identifier derivation scheme.
 *
 * @param handle              Open volume descriptor.
 * @param count               Number of pages to store.
 * @param id_base             Template identifier; must be exactly @c id_size bytes.
 * @param start               Initial counter value.
 * @param position            Offset from the end of the identifier where the counter ends
 *                            (0 = last four bytes).
 * @param endianness          Byte order for the counter.
 * @param pages_data          Page contents; must be @c count * page_size bytes.
 * @param fail_if_exists      If @c true, verify that none of the computed identifiers
 *                            already exist before writing.
 * @param durable             If @c true, block until data is durable on disk.
 * @param error               Receives the operation outcome; may be @c NULL.
 * @param sqlite_error        Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error         Receives @c errno on I/O failure; may be @c NULL.
 */
void pcache_put_pages_with_counter(pcache_handle     handle,
                                   size_t            count,
                                   const void       *id_base,
                                   uint32_t          start,
                                   uint32_t          position,
                                   pcache_endianness endianness,
                                   const void       *pages_data,
                                   bool              fail_if_exists,
                                   bool              durable,
                                   pcache_put_error *error,
                                   int              *sqlite_error,
                                   int              *posix_error);

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
void pcache_get_page(pcache_handle     handle,
                     const void       *id,
                     void             *page_buffer,
                     pcache_get_error *error,
                     int              *sqlite_error,
                     int              *posix_error);

/**
 * @brief Retrieve multiple pages whose identifiers are computed from a template.
 *
 * Counterpart of ::pcache_put_pages_with_counter for reads.  Derives @p count
 * identifiers using the same counter-XOR scheme and retrieves the corresponding pages
 * into @p pages_buffer, which must have at least @c count * page_size bytes available.
 * The operation is fail-fast: if any computed identifier is not found, the operation
 * fails and the buffer contents are unspecified.
 *
 * Fails with @c PCACHE_GET_INVALID_ARGUMENT if @p position is out of bounds, the counter
 * overflows, or @p endianness is unrecognized.  Fails with @c PCACHE_GET_OUT_OF_MEMORY
 * if the internal identifier buffer cannot be allocated.
 *
 * @param handle       Open volume descriptor.
 * @param count        Number of pages to retrieve.
 * @param id_base      Template identifier; must be exactly @c id_size bytes.
 * @param start        Initial counter value.
 * @param position     Offset from the end of the identifier where the counter ends (0 = last four bytes).
 * @param endianness   Byte order for the counter.
 * @param pages_buffer Destination buffer; must be at least @c count * page_size bytes.
 * @param error        Receives the operation outcome; may be @c NULL.
 * @param sqlite_error Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error  Receives @c errno on I/O failure; may be @c NULL.
 */
void pcache_get_pages_with_counter(pcache_handle     handle,
                                   size_t            count,
                                   const void       *id_base,
                                   uint32_t          start,
                                   uint32_t          position,
                                   pcache_endianness endianness,
                                   void             *pages_buffer,
                                   pcache_get_error *error,
                                   int              *sqlite_error,
                                   int              *posix_error);

/**
 * @brief Retrieve multiple pages in a single atomic operation.
 *
 * The operation is fail-fast: if any identifier is not found, the operation fails and
 * the buffer contents are unspecified.
 *
 * @param handle       Open volume descriptor.
 * @param count        Number of pages to retrieve.
 * @param ids          Page identifiers; must be @c count * id_size bytes.
 * @param pages_buffer Destination buffer; must be @c count * page_size bytes.
 * @param error        Receives the operation outcome; may be @c NULL.
 * @param sqlite_error Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error  Receives @c errno on I/O failure; may be @c NULL.
 */
void pcache_get_pages(pcache_handle     handle,
                      size_t            count,
                      const void       *ids,
                      void             *pages_buffer,
                      pcache_get_error *error,
                      int              *sqlite_error,
                      int              *posix_error);

/**
 * @brief Retrieve all pages whose identifier falls within the closed interval [@p first, @p last].
 *
 * Uses byte-by-byte comparison (SQLite BLOB ordering).  Pages are returned in ascending
 * identifier order.  The caller provides @p ids_buffer (at least @p buffer_capacity * id_size
 * bytes) and @p pages_buffer (at least @p buffer_capacity * page_size bytes).  @p count_out
 * receives the number of pages retrieved.  If the range contains more pages than
 * @p buffer_capacity, fails with @c PCACHE_GET_RANGE_BUFFER_TOO_SMALL without writing to the
 * buffers.  An empty match is not an error; @p count_out is set to zero.
 *
 * @param handle           Open volume descriptor.
 * @param first            Lower bound of the identifier range (inclusive); exactly @c id_size bytes.
 * @param last             Upper bound of the identifier range (inclusive); exactly @c id_size bytes.
 * @param ids_buffer       Destination for retrieved identifiers; at least @p buffer_capacity * id_size bytes.
 * @param pages_buffer     Destination for retrieved page data; at least @p buffer_capacity * page_size bytes.
 * @param buffer_capacity  Maximum number of pages the caller buffers can hold.
 * @param count_out        Receives the number of pages retrieved; may be @c NULL.
 * @param error            Receives the operation outcome; may be @c NULL.
 * @param sqlite_error     Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error      Receives @c errno on I/O failure; may be @c NULL.
 */
void pcache_get_pages_range(pcache_handle     handle,
                            const void       *first,
                            const void       *last,
                            void             *ids_buffer,
                            void             *pages_buffer,
                            uint32_t          buffer_capacity,
                            uint32_t         *count_out,
                            pcache_get_error *error,
                            int              *sqlite_error,
                            int              *posix_error);

/**
 * @brief Test whether a page identified by @p id exists in the volume.
 *
 * Thin wrapper around ::pcache_check_pages with @p count = 1.
 * The operation is serviced entirely against the index database; the data file is not read.
 *
 * @param handle       Open volume descriptor.
 * @param id           Page identifier; must be exactly @c id_size bytes.
 * @param error        Receives the operation outcome; may be @c NULL.
 * @param sqlite_error Receives the SQLite error code on failure; may be @c NULL.
 * @return @c true if the page exists, @c false otherwise (or on error).
 */
bool pcache_check_page(pcache_handle handle, const void *id, pcache_check_error *error, int *sqlite_error);

/**
 * @brief Test whether multiple pages exist in the volume.
 *
 * @param handle       Open volume descriptor.
 * @param count        Number of pages to check.
 * @param ids          Page identifiers; must be @c count * id_size bytes.
 * @param results      Caller-supplied array of at least @p count booleans; @c true for each
 *                     page that exists, @c false otherwise.  On error, entries from the failed
 *                     position onward are unspecified.
 * @param error        Receives the operation outcome; may be @c NULL.
 * @param sqlite_error Receives the SQLite error code on failure; may be @c NULL.
 */
void pcache_check_pages(
    pcache_handle handle, size_t count, const void *ids, bool *results, pcache_check_error *error, int *sqlite_error);

/**
 * @brief Test whether multiple pages exist, with identifiers computed from a template.
 *
 * Counterpart of ::pcache_put_pages_with_counter for existence checks.  Derives @p count
 * identifiers using the same counter-XOR scheme and writes results into @p results.
 *
 * Fails with @c PCACHE_CHECK_INVALID_ARGUMENT if @p position is out of bounds, the counter
 * overflows, or @p endianness is unrecognized.
 *
 * @param handle       Open volume descriptor.
 * @param count        Number of pages to check.
 * @param id_base      Template identifier; must be exactly @c id_size bytes.
 * @param start        Initial counter value.
 * @param position     Offset from the end of the identifier where the counter ends (0 = last four bytes).
 * @param endianness   Byte order for the counter.
 * @param results      Caller-supplied array of at least @p count booleans.
 * @param error        Receives the operation outcome; may be @c NULL.
 * @param sqlite_error Receives the SQLite error code on failure; may be @c NULL.
 */
void pcache_check_pages_with_counter(pcache_handle       handle,
                                     size_t              count,
                                     const void         *id_base,
                                     uint32_t            start,
                                     uint32_t            position,
                                     pcache_endianness   endianness,
                                     bool               *results,
                                     pcache_check_error *error,
                                     int                *sqlite_error);

/**
 * @brief Count pages whose identifier falls within the closed interval [@p first, @p last].
 *
 * Uses byte-by-byte comparison (SQLite BLOB ordering).  An empty match is not an error.
 * If @p first is greater than @p last, fails with @c PCACHE_CHECK_RANGE_INVALID_RANGE.
 *
 * @param handle       Open volume descriptor.
 * @param first        Lower bound of the identifier range (inclusive); exactly @c id_size bytes.
 * @param last         Upper bound of the identifier range (inclusive); exactly @c id_size bytes.
 * @param count_out    Receives the number of matching pages; may be @c NULL.
 * @param error        Receives the operation outcome; may be @c NULL.
 * @param sqlite_error Receives the SQLite error code on failure; may be @c NULL.
 */
void pcache_check_pages_range(pcache_handle       handle,
                              const void         *first,
                              const void         *last,
                              uint32_t           *count_out,
                              pcache_check_error *error,
                              int                *sqlite_error);

/**
 * @brief Delete the page identified by @p id from the volume.
 *
 * Thin wrapper around ::pcache_delete_pages with @p count = 1.
 * If no page with @p id exists, the call is a silent no-op.
 *
 * @param handle         Open volume descriptor.
 * @param id             Page identifier; must be exactly @c id_size bytes.
 * @param wipe_data_file If @c true, overwrite the page data with zeros.
 * @param durable        If @c true, block until data is durable on disk.
 * @param error          Receives the operation outcome; may be @c NULL.
 * @param sqlite_error   Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error    Receives @c errno on I/O failure; may be @c NULL.
 */
void pcache_delete_page(pcache_handle        handle,
                        const void          *id,
                        bool                 wipe_data_file,
                        bool                 durable,
                        pcache_delete_error *error,
                        int                 *sqlite_error,
                        int                 *posix_error);

/**
 * @brief Delete multiple pages from the volume.
 *
 * Identifiers that are not present in the volume are silently skipped — only the matching
 * pages are deleted — and duplicate identifiers within the batch are tolerated (the second
 * occurrence simply finds nothing to delete). The deletions of the matching pages are
 * committed atomically in a single transaction.
 * When @p wipe_data_file is @c true, page regions are zeroed sequentially; the first
 * wipe failure is reported and subsequent pages are not wiped.
 *
 * @param handle         Open volume descriptor.
 * @param count          Number of pages to delete.
 * @param ids            Page identifiers; must be @c count * id_size bytes.
 * @param wipe_data_file If @c true, overwrite each deleted page's data with zeros.
 * @param durable        If @c true, block until data is durable on disk.
 * @param error          Receives the operation outcome; may be @c NULL.
 * @param sqlite_error   Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error    Receives @c errno on I/O failure; may be @c NULL.
 */
void pcache_delete_pages(pcache_handle        handle,
                         size_t               count,
                         const void          *ids,
                         bool                 wipe_data_file,
                         bool                 durable,
                         pcache_delete_error *error,
                         int                 *sqlite_error,
                         int                 *posix_error);

/**
 * @brief Delete multiple pages whose identifiers are computed from a template.
 *
 * Counterpart of ::pcache_put_pages_with_counter for deletions.  Derives @p count
 * identifiers using the same counter-XOR scheme and deletes the corresponding pages.
 * Identifiers that are not present in the volume are silently skipped, exactly as in
 * ::pcache_delete_pages. In all other respects the operation is identical to
 * ::pcache_delete_pages.
 *
 * Fails with @c PCACHE_DELETE_INVALID_ARGUMENT if @p position is out of bounds, the counter
 * overflows, or @p endianness is unrecognized.  Fails with @c PCACHE_DELETE_OUT_OF_MEMORY
 * if the internal identifier buffer cannot be allocated.
 *
 * @param handle         Open volume descriptor.
 * @param count          Number of pages to delete.
 * @param id_base        Template identifier; must be exactly @c id_size bytes.
 * @param start          Initial counter value.
 * @param position       Offset from the end of the identifier where the counter ends (0 = last four bytes).
 * @param endianness     Byte order for the counter.
 * @param wipe_data_file If @c true, overwrite each deleted page's data with zeros.
 * @param durable        If @c true, block until data is durable on disk.
 * @param error          Receives the operation outcome; may be @c NULL.
 * @param sqlite_error   Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error    Receives @c errno on I/O failure; may be @c NULL.
 */
void pcache_delete_pages_with_counter(pcache_handle        handle,
                                      size_t               count,
                                      const void          *id_base,
                                      uint32_t             start,
                                      uint32_t             position,
                                      pcache_endianness    endianness,
                                      bool                 wipe_data_file,
                                      bool                 durable,
                                      pcache_delete_error *error,
                                      int                 *sqlite_error,
                                      int                 *posix_error);

/**
 * @brief Delete all pages whose identifier falls within the closed interval
 *        [@p first, @p last] (byte-by-byte comparison).
 *
 * An empty match — no pages in the range — is not an error.
 * When @p wipe_data_file is @c true, page regions are zeroed sequentially; the first
 * wipe failure is reported and subsequent pages are not wiped.
 *
 * Both @p first and @p last must be exactly @c id_size bytes. If @p first is
 * greater than @p last, the operation fails with @c PCACHE_DELETE_INVALID_RANGE.
 *
 * @param handle         Open volume descriptor.
 * @param first          Lower bound of the identifier range (inclusive).
 * @param last           Upper bound of the identifier range (inclusive).
 * @param wipe_data_file If @c true, overwrite each deleted page's data with zeros.
 * @param durable        If @c true, block until data is durable on disk.
 * @param error          Receives the operation outcome; may be @c NULL.
 * @param sqlite_error   Receives the SQLite error code on failure; may be @c NULL.
 * @param posix_error    Receives @c errno on I/O failure; may be @c NULL.
 */
void pcache_delete_pages_range(pcache_handle        handle,
                               const void          *first,
                               const void          *last,
                               bool                 wipe_data_file,
                               bool                 durable,
                               pcache_delete_error *error,
                               int                 *sqlite_error,
                               int                 *posix_error);

/* ──────────── Maintenance ──────────── */

/**
 * @brief Relocate live pages contiguously toward the start of the data file.
 *
 * The @p progress_callback is invoked after each page and may cancel the
 * operation by returning @c false; the volume remains consistent in all cases —
 * pages processed up to the cancellation point have been relocated and the index
 * updated; unprocessed pages remain in their original positions.
 * See @c pcache_progress_fn for reentrancy constraints.
 *
 * On @c PCACHE_CAPACITY_FIFO volumes the operation is a no-op: the FIFO eviction
 * order is encoded in the relative positions of live and empty slots, so any
 * rearrangement would corrupt that order.  The callback is invoked once with
 * @p progress = 1.0 and the function returns @c PCACHE_DEFRAGMENT_OK without
 * touching the volume.
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
 * Growth is always permitted.  On @c PCACHE_CAPACITY_FIXED volumes, any live pages
 * that reside beyond @p new_max_pages are automatically moved into free slots within
 * @c [1, new_max_pages]; the operation fails with @c PCACHE_SET_MAX_PAGES_WOULD_DISCARD_PAGES
 * only when the total number of live pages exceeds @p new_max_pages.
 * On @c PCACHE_CAPACITY_FIFO volumes, reduction physically drops every row with
 * @c ROWID > @p new_max_pages and truncates the data file accordingly; this is not
 * strictly oldest-first and may discard pages that are not the oldest.
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

#ifdef __cplusplus
}
#endif
