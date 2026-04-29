#pragma once

#include <errno.h>
#include <stdbool.h>

/**
 * @file libpcache_errors.h
 * @brief Error code enumerations for every public libpcache function.
 *
 * Each enumeration exposes only the failure modes that the corresponding
 * function can produce.  Values are assigned explicitly so that numeric codes
 * remain stable and can be compared or logged without ambiguity.
 *
 * Error codes are shared across enums where the same error can occur in
 * multiple operations.  For example, PCACHE_CODE_INVALID_HANDLE is always 1
 * regardless of which operation returned it.
 *
 * Operation-specific errors use values starting at 1000.
 */

/** Operation succeeded. */
#define PCACHE_CODE_OK                         0

// General Errors - Similar to a POSIX Error

#define PCACHE_CODE_IO_ERROR                   EIO    /** A POSIX I/O call failed; inspect @p posix_error. */
#define PCACHE_CODE_OUT_OF_MEMORY              ENOMEM /** Could not allocate memory */
#define PCACHE_CODE_INVALID_HANDLE             EBADF  /** The handle is zero or does not refer to an open volume. */

// General Errors - No similarity to POSIX Errors

#define PCACHE_CODE_SQLITE_ERROR               0x0153514C /** A SQLite call failed; inspect @p sqlite_error. */

// Operation specific codes

#define PCACHE_CODE_CREATE_INVALID_ARGUMENT    0x02000001
#define PCACHE_CODE_CREATE_FILE_EXISTS         0x02000002
#define PCACHE_CODE_OPEN_NOT_FOUND             0x02000003
#define PCACHE_CODE_OPEN_CORRUPT               0x02000004
#define PCACHE_CODE_PUT_PAGE_CAPACITY_EXCEEDED 0x02000005
#define PCACHE_CODE_PUT_DUPLICATE_ID           0x02000006
#define PCACHE_CODE_GET_PAGE_NOT_FOUND         0x02000007
/* 0x02000008 reserved (was PCACHE_CODE_DELETE_PAGE_NOT_FOUND) */
#define PCACHE_CODE_DEFRAGMENT_CANCELLED       0x02000009
#define PCACHE_CODE_SET_MAX_PAGES_WOULD_DISCARD_PAGES 0x0200000A
#define PCACHE_CODE_GET_PAGES_NOT_FOUND               0x0200000B
#define PCACHE_CODE_OPEN_SCHEMA_VERSION_TOO_HIGH      0x0200000C
#define PCACHE_CODE_DELETE_INVALID_RANGE              0x0200000D
/* 0x0200000E reserved (was PCACHE_CODE_DELETE_DUPLICATE_ID) */
#define PCACHE_CODE_PUT_INVALID_ARGUMENT              0x0200000F
#define PCACHE_CODE_GET_INVALID_ARGUMENT              0x02000010
#define PCACHE_CODE_CHECK_INVALID_ARGUMENT            0x02000011
#define PCACHE_CODE_DELETE_INVALID_ARGUMENT           0x02000012
#define PCACHE_CODE_GET_RANGE_INVALID_RANGE           0x02000013
#define PCACHE_CODE_GET_RANGE_BUFFER_TOO_SMALL        0x02000014
#define PCACHE_CODE_CHECK_RANGE_INVALID_RANGE         0x02000015

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
    PCACHE_OPEN_CORRUPT      = PCACHE_CODE_OPEN_CORRUPT,   /**< The index database is missing required metadata. */
    PCACHE_OPEN_SCHEMA_VERSION_TOO_HIGH =
        PCACHE_CODE_OPEN_SCHEMA_VERSION_TOO_HIGH, /**< Database schema version is newer than library supports. */
    PCACHE_OPEN_OUT_OF_MEMORY =
        PCACHE_CODE_OUT_OF_MEMORY, /**< Memory allocation failed while growing the handle table. */
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

/** Outcome of ::pcache_inspect_configuration. */
typedef enum pcache_inspect_configuration_error {
    PCACHE_INSPECT_CONFIGURATION_OK = PCACHE_CODE_OK, /**< Operation succeeded. */
    PCACHE_INSPECT_CONFIGURATION_INVALID_HANDLE =
        PCACHE_CODE_INVALID_HANDLE, /**< The handle is zero or does not refer to an open volume. */
} pcache_inspect_configuration_error;

/** Outcome of ::pcache_put_page, ::pcache_put_pages, and ::pcache_put_pages_with_counter. */
typedef enum pcache_put_error {
    PCACHE_PUT_OK = PCACHE_CODE_OK, /**< All pages were stored successfully. */
    PCACHE_PUT_INVALID_HANDLE =
        PCACHE_CODE_INVALID_HANDLE, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_PUT_CAPACITY_EXCEEDED =
        PCACHE_CODE_PUT_PAGE_CAPACITY_EXCEEDED, /**< FIXED volume is full and has no free slots. */
    PCACHE_PUT_DUPLICATE_ID =
        PCACHE_CODE_PUT_DUPLICATE_ID, /**< @p check_id_uniqueness was true and an identifier already exists. */
    PCACHE_PUT_IO_ERROR         = PCACHE_CODE_IO_ERROR, /**< Writing to the data file failed; inspect @p posix_error. */
    PCACHE_PUT_SQLITE_ERROR     = PCACHE_CODE_SQLITE_ERROR,  /**< A SQLite call failed; inspect @p sqlite_error. */
    PCACHE_PUT_OUT_OF_MEMORY    = PCACHE_CODE_OUT_OF_MEMORY, /**< Memory allocation failed while preparing the write. */
    PCACHE_PUT_INVALID_ARGUMENT = PCACHE_CODE_PUT_INVALID_ARGUMENT, /**< @p position is out of bounds, the counter would
                                                                       overflow, or @p endianness is invalid. */
} pcache_put_error;

/** Outcome of ::pcache_get_page, ::pcache_get_pages, ::pcache_get_pages_with_counter, and ::pcache_get_pages_range. */
typedef enum pcache_get_error {
    PCACHE_GET_OK = PCACHE_CODE_OK, /**< All pages were retrieved successfully. */
    PCACHE_GET_INVALID_HANDLE =
        PCACHE_CODE_INVALID_HANDLE, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_GET_NOT_FOUND =
        PCACHE_CODE_GET_PAGE_NOT_FOUND,              /**< No page with the given identifier exists in the volume. */
    PCACHE_GET_IO_ERROR      = PCACHE_CODE_IO_ERROR, /**< Reading from the data file failed; inspect @p posix_error. */
    PCACHE_GET_SQLITE_ERROR  = PCACHE_CODE_SQLITE_ERROR,  /**< A SQLite call failed; inspect @p sqlite_error. */
    PCACHE_GET_OUT_OF_MEMORY = PCACHE_CODE_OUT_OF_MEMORY, /**< Memory allocation failed while building IDs. */
    PCACHE_GET_INVALID_ARGUMENT = PCACHE_CODE_GET_INVALID_ARGUMENT,       /**< @p position is out of bounds, the counter
                                                                             would overflow, or @p endianness is invalid. */
    PCACHE_GET_RANGE_INVALID_RANGE = PCACHE_CODE_GET_RANGE_INVALID_RANGE, /**< @p first is greater than @p last. */
    PCACHE_GET_RANGE_BUFFER_TOO_SMALL =
        PCACHE_CODE_GET_RANGE_BUFFER_TOO_SMALL, /**< @p buffer_capacity is smaller than the number of matching pages. */
} pcache_get_error;

/** Outcome of ::pcache_check_page, ::pcache_check_pages, ::pcache_check_pages_with_counter, and
 *  ::pcache_check_pages_range. */
typedef enum pcache_check_error {
    PCACHE_CHECK_OK =
        PCACHE_CODE_OK, /**< Operation completed successfully; consult the return value or results array. */
    PCACHE_CHECK_INVALID_HANDLE =
        PCACHE_CODE_INVALID_HANDLE, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_CHECK_SQLITE_ERROR  = PCACHE_CODE_SQLITE_ERROR,  /**< A SQLite call failed; inspect @p sqlite_error. */
    PCACHE_CHECK_OUT_OF_MEMORY = PCACHE_CODE_OUT_OF_MEMORY, /**< Memory allocation failed. */
    PCACHE_CHECK_INVALID_ARGUMENT =
        PCACHE_CODE_CHECK_INVALID_ARGUMENT, /**< @p position is out of bounds, the counter
                                               would overflow, or @p endianness is invalid. */
    PCACHE_CHECK_RANGE_INVALID_RANGE = PCACHE_CODE_CHECK_RANGE_INVALID_RANGE, /**< @p first is greater than @p last. */
} pcache_check_error;

/** Outcome of ::pcache_delete_page, ::pcache_delete_pages, and ::pcache_delete_pages_with_counter.
 *
 * Identifiers that are not present in the volume are silently skipped, so missing pages do not
 * produce a failure. */
typedef enum pcache_delete_error {
    PCACHE_DELETE_OK = PCACHE_CODE_OK, /**< Operation succeeded; matching pages were deleted. */
    PCACHE_DELETE_INVALID_HANDLE =
        PCACHE_CODE_INVALID_HANDLE, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_DELETE_IO_ERROR =
        PCACHE_CODE_IO_ERROR, /**< Writing zeros to the data file failed; inspect @p posix_error. */
    PCACHE_DELETE_SQLITE_ERROR  = PCACHE_CODE_SQLITE_ERROR,  /**< A SQLite call failed; inspect @p sqlite_error. */
    PCACHE_DELETE_OUT_OF_MEMORY = PCACHE_CODE_OUT_OF_MEMORY, /**< Memory allocation failed. */
    PCACHE_DELETE_INVALID_RANGE = PCACHE_CODE_DELETE_INVALID_RANGE, /**< @p first is greater than @p last. */
    PCACHE_DELETE_INVALID_ARGUMENT =
        PCACHE_CODE_DELETE_INVALID_ARGUMENT, /**< @p position is out of bounds, the counter
                                                would overflow, or @p endianness is invalid. */
} pcache_delete_error;

/** Outcome of ::pcache_defragment. */
typedef enum pcache_defragment_error {
    PCACHE_DEFRAGMENT_OK = PCACHE_CODE_OK, /**< Defragmentation completed. */
    PCACHE_DEFRAGMENT_INVALID_HANDLE =
        PCACHE_CODE_INVALID_HANDLE, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_DEFRAGMENT_CANCELLED =
        PCACHE_CODE_DEFRAGMENT_CANCELLED, /**< The progress callback returned false; the volume remains consistent. */
    PCACHE_DEFRAGMENT_IO_ERROR      = PCACHE_CODE_IO_ERROR, /**< A data-file I/O call failed; inspect @p posix_error. */
    PCACHE_DEFRAGMENT_SQLITE_ERROR  = PCACHE_CODE_SQLITE_ERROR,  /**< A SQLite call failed; inspect @p sqlite_error. */
    PCACHE_DEFRAGMENT_OUT_OF_MEMORY = PCACHE_CODE_OUT_OF_MEMORY, /**< Memory allocation failed. */
} pcache_defragment_error;

/** Outcome of ::pcache_set_max_pages. */
typedef enum pcache_set_max_pages_error {
    PCACHE_SET_MAX_PAGES_OK = PCACHE_CODE_OK, /**< Capacity was updated. */
    PCACHE_SET_MAX_PAGES_INVALID_HANDLE =
        PCACHE_CODE_INVALID_HANDLE, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_SET_MAX_PAGES_WOULD_DISCARD_PAGES =
        PCACHE_CODE_SET_MAX_PAGES_WOULD_DISCARD_PAGES,    /**< FIXED volume: total live pages exceed new_max_pages. */
    PCACHE_SET_MAX_PAGES_IO_ERROR = PCACHE_CODE_IO_ERROR, /**< A data-file I/O call failed; inspect @p posix_error. */
    PCACHE_SET_MAX_PAGES_SQLITE_ERROR = PCACHE_CODE_SQLITE_ERROR, /**< A SQLite call failed; inspect @p sqlite_error. */
    PCACHE_SET_MAX_PAGES_OUT_OF_MEMORY =
        PCACHE_CODE_OUT_OF_MEMORY, /**< Memory allocation failed during auto-relocation. */
} pcache_set_max_pages_error;

/** Outcome of ::pcache_preallocate. */
typedef enum pcache_preallocate_error {
    PCACHE_PREALLOCATE_OK = PCACHE_CODE_OK, /**< Preallocation completed. */
    PCACHE_PREALLOCATE_INVALID_HANDLE =
        PCACHE_CODE_INVALID_HANDLE, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_PREALLOCATE_IO_ERROR     = PCACHE_CODE_IO_ERROR, /**< A data-file I/O call failed; inspect @p posix_error. */
    PCACHE_PREALLOCATE_SQLITE_ERROR = PCACHE_CODE_SQLITE_ERROR, /**< A SQLite call failed; inspect @p sqlite_error. */
} pcache_preallocate_error;

/** Outcome of ::pcache_inspect_page_count. */
typedef enum pcache_inspect_page_count_error {
    PCACHE_INSPECT_PAGE_COUNT_OK = PCACHE_CODE_OK, /**< Operation succeeded. */
    PCACHE_INSPECT_PAGE_COUNT_INVALID_HANDLE =
        PCACHE_CODE_INVALID_HANDLE, /**< The handle is zero or does not refer to an open volume. */
    PCACHE_INSPECT_PAGE_COUNT_SQLITE_ERROR =
        PCACHE_CODE_SQLITE_ERROR, /**< A SQLite call failed; inspect @p sqlite_error. */
} pcache_inspect_page_count_error;
