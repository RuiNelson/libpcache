#pragma once

/**
 * @file libpcache_errors.h
 * @brief Error code enumerations for every public libpcache function.
 *
 * Each enumeration exposes only the failure modes that the corresponding
 * function can produce.  Values are assigned explicitly to preserve binary
 * compatibility across library versions.
 *
 * Error codes are shared across enums where the same error can occur in
 * multiple operations.  For example, PCACHE_CODE_INVALID_HANDLE is always 1
 * regardless of which operation returned it.
 *
 * Operation-specific errors use values starting at 1000.
 */

/** Operation succeeded. */
#define PCACHE_CODE_OK                                0

/** The handle is zero or does not refer to an open volume. */
#define PCACHE_CODE_INVALID_HANDLE                    1

/** A POSIX I/O call failed; inspect @p posix_error. */
#define PCACHE_CODE_IO_ERROR                          2

/** A SQLite call failed; inspect @p sqlite_error. */
#define PCACHE_CODE_SQLITE_ERROR                      3

// Operation specific codes

#define PCACHE_CODE_CREATE_INVALID_ARGUMENT           1000
#define PCACHE_CODE_CREATE_FILE_EXISTS                1001
#define PCACHE_CODE_OPEN_NOT_FOUND                    1002
#define PCACHE_CODE_OPEN_TOO_MANY_HANDLES             1003
#define PCACHE_CODE_OPEN_CORRUPT                      1004
#define PCACHE_CODE_PUT_PAGE_CAPACITY_EXCEEDED        1005
#define PCACHE_CODE_PUT_PAGE_DUPLICATE_ID             1006
#define PCACHE_CODE_GET_PAGE_NOT_FOUND                1007
#define PCACHE_CODE_DELETE_PAGE_NOT_FOUND             1008
#define PCACHE_CODE_DEFRAGMENT_CANCELLED              1009
#define PCACHE_CODE_SET_MAX_PAGES_WOULD_DISCARD_PAGES 1010

/** Outcome of ::pcache_create. */

typedef enum pcache_create_error {
    PCACHE_CREATE_OK = PCACHE_CODE_OK, /**< Operation succeeded. */
    PCACHE_CREATE_INVALID_ARGUMENT =
        PCACHE_CODE_CREATE_INVALID_ARGUMENT, /**< A required pointer was NULL or a numeric parameter was zero. */
    PCACHE_CREATE_FILE_EXISTS =
        PCACHE_CODE_CREATE_FILE_EXISTS,                    /**< At least one of the two volume files already exists. */
    PCACHE_CREATE_IO_ERROR     = PCACHE_CODE_IO_ERROR,     /**< A POSIX I/O call failed; inspect @p posix_error. */
    PCACHE_CREATE_SQLITE_ERROR = PCACHE_CODE_SQLITE_ERROR, /**< A SQLite call failed; inspect @p sqlite_error. */
} pcache_create_error;

/** Outcome of ::pcache_open. */
typedef enum pcache_open_error {
    PCACHE_OPEN_OK           = PCACHE_CODE_OK,             /**< Operation succeeded. */
    PCACHE_OPEN_NOT_FOUND    = PCACHE_CODE_OPEN_NOT_FOUND, /**< At least one of the two volume files does not exist. */
    PCACHE_OPEN_IO_ERROR     = PCACHE_CODE_IO_ERROR,       /**< A POSIX I/O call failed; inspect @p posix_error. */
    PCACHE_OPEN_SQLITE_ERROR = PCACHE_CODE_SQLITE_ERROR,   /**< A SQLite call failed; inspect @p sqlite_error. */
    PCACHE_OPEN_CORRUPT      = PCACHE_CODE_OPEN_CORRUPT, /**< The index database is missing required metadata or has an
                                                            unsupported schema version. */
    PCACHE_OPEN_TOO_MANY_HANDLES = PCACHE_CODE_OPEN_TOO_MANY_HANDLES, /**< The per-process handle table is full. */
} pcache_open_error;

/** Outcome of ::pcache_close. */
typedef enum pcache_close_error {
    PCACHE_CLOSE_OK = PCACHE_CODE_OK, /**< Operation succeeded. */
    PCACHE_CLOSE_INVALID_HANDLE =
        PCACHE_CODE_INVALID_HANDLE, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_CLOSE_SQLITE_ERROR =
        PCACHE_CODE_SQLITE_ERROR, /**< sqlite3_close reported an error; inspect @p sqlite_error. */
    PCACHE_CLOSE_IO_ERROR =
        PCACHE_CODE_IO_ERROR, /**< Closing the data file descriptor failed; inspect @p posix_error. */
} pcache_close_error;

/** Outcome of ::pcache_get_configuration. */
typedef enum pcache_get_configuration_error {
    PCACHE_GET_CONFIGURATION_OK = PCACHE_CODE_OK, /**< Operation succeeded. */
    PCACHE_GET_CONFIGURATION_INVALID_HANDLE =
        PCACHE_CODE_INVALID_HANDLE, /**< The handle is zero or does not refer to an open volume. */
} pcache_get_configuration_error;

/** Outcome of ::pcache_put_page. */
typedef enum pcache_put_page_error {
    PCACHE_PUT_PAGE_OK = PCACHE_CODE_OK, /**< Page was stored successfully. */
    PCACHE_PUT_PAGE_INVALID_HANDLE =
        PCACHE_CODE_INVALID_HANDLE, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_PUT_PAGE_CAPACITY_EXCEEDED =
        PCACHE_CODE_PUT_PAGE_CAPACITY_EXCEEDED, /**< FIXED volume is full and has no free slots. */
    PCACHE_PUT_PAGE_DUPLICATE_ID =
        PCACHE_CODE_PUT_PAGE_DUPLICATE_ID, /**< @p check_id_uniqueness was true and the identifier already exists. */
    PCACHE_PUT_PAGE_IO_ERROR = PCACHE_CODE_IO_ERROR, /**< Writing to the data file failed; inspect @p posix_error. */
    PCACHE_PUT_PAGE_SQLITE_ERROR = PCACHE_CODE_SQLITE_ERROR, /**< A SQLite call failed; inspect @p sqlite_error. */
} pcache_put_page_error;

/** Outcome of ::pcache_get_page. */
typedef enum pcache_get_page_error {
    PCACHE_GET_PAGE_OK = PCACHE_CODE_OK, /**< Page was retrieved successfully. */
    PCACHE_GET_PAGE_INVALID_HANDLE =
        PCACHE_CODE_INVALID_HANDLE, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_GET_PAGE_NOT_FOUND =
        PCACHE_CODE_GET_PAGE_NOT_FOUND,              /**< No page with the given identifier exists in the volume. */
    PCACHE_GET_PAGE_IO_ERROR = PCACHE_CODE_IO_ERROR, /**< Reading from the data file failed; inspect @p posix_error. */
    PCACHE_GET_PAGE_SQLITE_ERROR = PCACHE_CODE_SQLITE_ERROR, /**< A SQLite call failed; inspect @p sqlite_error. */
} pcache_get_page_error;

/** Outcome of ::pcache_check_page. */
typedef enum pcache_check_page_error {
    PCACHE_CHECK_PAGE_OK = PCACHE_CODE_OK, /**< Lookup completed successfully; consult the return value. */
    PCACHE_CHECK_PAGE_INVALID_HANDLE =
        PCACHE_CODE_INVALID_HANDLE, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_CHECK_PAGE_SQLITE_ERROR = PCACHE_CODE_SQLITE_ERROR, /**< A SQLite call failed; inspect @p sqlite_error. */
} pcache_check_page_error;

/** Outcome of ::pcache_delete_page. */
typedef enum pcache_delete_page_error {
    PCACHE_DELETE_PAGE_OK = PCACHE_CODE_OK, /**< Page was deleted successfully. */
    PCACHE_DELETE_PAGE_INVALID_HANDLE =
        PCACHE_CODE_INVALID_HANDLE, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_DELETE_PAGE_NOT_FOUND =
        PCACHE_CODE_DELETE_PAGE_NOT_FOUND, /**< No page with the given identifier exists in the volume. */
    PCACHE_DELETE_PAGE_IO_ERROR =
        PCACHE_CODE_IO_ERROR, /**< Writing zeros to the data file failed; inspect @p posix_error. */
    PCACHE_DELETE_PAGE_SQLITE_ERROR = PCACHE_CODE_SQLITE_ERROR, /**< A SQLite call failed; inspect @p sqlite_error. */
} pcache_delete_page_error;

/** Outcome of ::pcache_defragment. */
typedef enum pcache_defragment_error {
    PCACHE_DEFRAGMENT_OK = PCACHE_CODE_OK, /**< Defragmentation completed. */
    PCACHE_DEFRAGMENT_INVALID_HANDLE =
        PCACHE_CODE_INVALID_HANDLE, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_DEFRAGMENT_CANCELLED =
        PCACHE_CODE_DEFRAGMENT_CANCELLED, /**< The progress callback returned false; the volume remains consistent. */
    PCACHE_DEFRAGMENT_IO_ERROR     = PCACHE_CODE_IO_ERROR, /**< A data-file I/O call failed; inspect @p posix_error. */
    PCACHE_DEFRAGMENT_SQLITE_ERROR = PCACHE_CODE_SQLITE_ERROR, /**< A SQLite call failed; inspect @p sqlite_error. */
} pcache_defragment_error;

/** Outcome of ::pcache_set_max_pages. */
typedef enum pcache_set_max_pages_error {
    PCACHE_SET_MAX_PAGES_OK = PCACHE_CODE_OK, /**< Capacity was updated. */
    PCACHE_SET_MAX_PAGES_INVALID_HANDLE =
        PCACHE_CODE_INVALID_HANDLE, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_SET_MAX_PAGES_WOULD_DISCARD_PAGES =
        PCACHE_CODE_SET_MAX_PAGES_WOULD_DISCARD_PAGES,    /**< FIXED volume: reduction would discard live pages. */
    PCACHE_SET_MAX_PAGES_IO_ERROR = PCACHE_CODE_IO_ERROR, /**< A data-file I/O call failed; inspect @p posix_error. */
    PCACHE_SET_MAX_PAGES_SQLITE_ERROR = PCACHE_CODE_SQLITE_ERROR, /**< A SQLite call failed; inspect @p sqlite_error. */
} pcache_set_max_pages_error;

/** Outcome of ::pcache_preallocate. */
typedef enum pcache_preallocate_error {
    PCACHE_PREALLOCATE_OK = PCACHE_CODE_OK, /**< Preallocation completed. */
    PCACHE_PREALLOCATE_INVALID_HANDLE =
        PCACHE_CODE_INVALID_HANDLE, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_PREALLOCATE_IO_ERROR     = PCACHE_CODE_IO_ERROR, /**< A data-file I/O call failed; inspect @p posix_error. */
    PCACHE_PREALLOCATE_SQLITE_ERROR = PCACHE_CODE_SQLITE_ERROR, /**< A SQLite call failed; inspect @p sqlite_error. */
} pcache_preallocate_error;
