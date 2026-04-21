#pragma once

/**
 * @file libpcache_errors.h
 * @brief Error code enumerations for every public libpcache function.
 *
 * Each enumeration exposes only the failure modes that the corresponding
 * function can produce.  Values are assigned explicitly to preserve binary
 * compatibility across library versions.
 */

/** Outcome of ::pcache_create. */
typedef enum pcache_create_error {
    PCACHE_CREATE_OK               = 0, /**< Operation succeeded. */
    PCACHE_CREATE_INVALID_ARGUMENT = 1, /**< A required pointer was NULL or a numeric parameter was zero. */
    PCACHE_CREATE_FILE_EXISTS      = 2, /**< At least one of the two volume files already exists. */
    PCACHE_CREATE_IO_ERROR         = 3, /**< A POSIX I/O call failed; inspect @p posix_error. */
    PCACHE_CREATE_SQLITE_ERROR     = 4, /**< A SQLite call failed; inspect @p sqlite_error. */
} pcache_create_error;

/** Outcome of ::pcache_open. */
typedef enum pcache_open_error {
    PCACHE_OPEN_OK               = 0, /**< Operation succeeded. */
    PCACHE_OPEN_NOT_FOUND        = 1, /**< At least one of the two volume files does not exist. */
    PCACHE_OPEN_IO_ERROR         = 2, /**< A POSIX I/O call failed; inspect @p posix_error. */
    PCACHE_OPEN_SQLITE_ERROR     = 3, /**< A SQLite call failed; inspect @p sqlite_error. */
    PCACHE_OPEN_CORRUPT          = 4, /**< The index database is missing required metadata or has an unsupported schema version. */
    PCACHE_OPEN_TOO_MANY_HANDLES = 5, /**< The per-process handle table is full. */
} pcache_open_error;

/** Outcome of ::pcache_close. */
typedef enum pcache_close_error {
    PCACHE_CLOSE_OK             = 0, /**< Operation succeeded. */
    PCACHE_CLOSE_INVALID_HANDLE = 1, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_CLOSE_SQLITE_ERROR   = 2, /**< sqlite3_close reported an error; inspect @p sqlite_error. */
    PCACHE_CLOSE_IO_ERROR       = 3, /**< Closing the data file descriptor failed; inspect @p posix_error. */
} pcache_close_error;

/** Outcome of ::pcache_get_configuration. */
typedef enum pcache_get_configuration_error {
    PCACHE_GET_CONFIGURATION_OK             = 0, /**< Operation succeeded. */
    PCACHE_GET_CONFIGURATION_INVALID_HANDLE = 1, /**< The handle is zero or does not refer to an open volume. */
} pcache_get_configuration_error;

/** Outcome of ::pcache_put_page. */
typedef enum pcache_put_page_error {
    PCACHE_PUT_PAGE_OK               = 0, /**< Page was stored successfully. */
    PCACHE_PUT_PAGE_INVALID_HANDLE   = 1, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_PUT_PAGE_CAPACITY_EXCEEDED = 2, /**< FIXED volume is full and has no free slots. */
    PCACHE_PUT_PAGE_DUPLICATE_ID     = 3, /**< @p check_id_uniqueness was true and the identifier already exists. */
    PCACHE_PUT_PAGE_IO_ERROR         = 4, /**< Writing to the data file failed; inspect @p posix_error. */
    PCACHE_PUT_PAGE_SQLITE_ERROR     = 5, /**< A SQLite call failed; inspect @p sqlite_error. */
} pcache_put_page_error;

/** Outcome of ::pcache_get_page. */
typedef enum pcache_get_page_error {
    PCACHE_GET_PAGE_OK             = 0, /**< Page was retrieved successfully. */
    PCACHE_GET_PAGE_INVALID_HANDLE = 1, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_GET_PAGE_NOT_FOUND      = 2, /**< No page with the given identifier exists in the volume. */
    PCACHE_GET_PAGE_IO_ERROR       = 3, /**< Reading from the data file failed; inspect @p posix_error. */
    PCACHE_GET_PAGE_SQLITE_ERROR   = 4, /**< A SQLite call failed; inspect @p sqlite_error. */
} pcache_get_page_error;

/** Outcome of ::pcache_check_page. */
typedef enum pcache_check_page_error {
    PCACHE_CHECK_PAGE_OK             = 0, /**< Lookup completed successfully; consult the return value. */
    PCACHE_CHECK_PAGE_INVALID_HANDLE = 1, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_CHECK_PAGE_SQLITE_ERROR   = 2, /**< A SQLite call failed; inspect @p sqlite_error. */
} pcache_check_page_error;

/** Outcome of ::pcache_delete_page. */
typedef enum pcache_delete_page_error {
    PCACHE_DELETE_PAGE_OK             = 0, /**< Page was deleted successfully. */
    PCACHE_DELETE_PAGE_INVALID_HANDLE = 1, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_DELETE_PAGE_NOT_FOUND      = 2, /**< No page with the given identifier exists in the volume. */
    PCACHE_DELETE_PAGE_IO_ERROR       = 3, /**< Writing zeros to the data file failed; inspect @p posix_error. */
    PCACHE_DELETE_PAGE_SQLITE_ERROR   = 4, /**< A SQLite call failed; inspect @p sqlite_error. */
} pcache_delete_page_error;

/** Outcome of ::pcache_defragment. */
typedef enum pcache_defragment_error {
    PCACHE_DEFRAGMENT_OK             = 0, /**< Defragmentation completed. */
    PCACHE_DEFRAGMENT_INVALID_HANDLE = 1, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_DEFRAGMENT_CANCELLED      = 2, /**< The progress callback returned false; the volume remains consistent. */
    PCACHE_DEFRAGMENT_IO_ERROR       = 3, /**< A data-file I/O call failed; inspect @p posix_error. */
    PCACHE_DEFRAGMENT_SQLITE_ERROR   = 4, /**< A SQLite call failed; inspect @p sqlite_error. */
} pcache_defragment_error;

/** Outcome of ::pcache_set_max_pages. */
typedef enum pcache_set_max_pages_error {
    PCACHE_SET_MAX_PAGES_OK                  = 0, /**< Capacity was updated. */
    PCACHE_SET_MAX_PAGES_INVALID_HANDLE      = 1, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_SET_MAX_PAGES_WOULD_DISCARD_PAGES = 2, /**< FIXED volume: reduction would discard live pages. */
    PCACHE_SET_MAX_PAGES_IO_ERROR            = 3, /**< A data-file I/O call failed; inspect @p posix_error. */
    PCACHE_SET_MAX_PAGES_SQLITE_ERROR        = 4, /**< A SQLite call failed; inspect @p sqlite_error. */
} pcache_set_max_pages_error;

/** Outcome of ::pcache_preallocate. */
typedef enum pcache_preallocate_error {
    PCACHE_PREALLOCATE_OK             = 0, /**< Preallocation completed. */
    PCACHE_PREALLOCATE_INVALID_HANDLE = 1, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_PREALLOCATE_IO_ERROR       = 2, /**< A data-file I/O call failed; inspect @p posix_error. */
    PCACHE_PREALLOCATE_SQLITE_ERROR   = 3, /**< A SQLite call failed; inspect @p sqlite_error. */
} pcache_preallocate_error;
