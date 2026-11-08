#pragma once

/**
 * @file platform.h
 * @brief Portability layer: file I/O and synchronization primitives.
 *
 * Every platform-specific system call used by the library goes through the
 * functions declared here. On failure they return -1 (or false) and leave the
 * native error code retrievable through ::platform_last_error: @c errno on
 * POSIX systems, the Win32 system error code (@c GetLastError) on Windows.
 * No translation between the two is performed.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

typedef SRWLOCK   platform_mutex;
typedef INIT_ONCE platform_once_flag;
#define PLATFORM_ONCE_INIT         INIT_ONCE_STATIC_INIT
#define PLATFORM_ERROR_IO          ERROR_IO_DEVICE      /**< Generic I/O failure (short transfer, injected failure). */
#define PLATFORM_ERROR_FILE_EXISTS ERROR_FILE_EXISTS    /**< Exclusive creation found an existing file. */
#define PLATFORM_ERROR_BAD_HANDLE  ERROR_INVALID_HANDLE /**< Invalid file descriptor. */
#else
#include <errno.h>
#include <pthread.h>

typedef pthread_mutex_t platform_mutex;
typedef pthread_once_t  platform_once_flag;
#define PLATFORM_ONCE_INIT         PTHREAD_ONCE_INIT
#define PLATFORM_ERROR_IO          EIO    /**< Generic I/O failure (short transfer, injected failure). */
#define PLATFORM_ERROR_FILE_EXISTS EEXIST /**< Exclusive creation found an existing file. */
#define PLATFORM_ERROR_BAD_HANDLE  EBADF  /**< Invalid file descriptor. */
#endif

/** @brief Byte offset within a file; always 64 bits wide. */
typedef int64_t platform_off_t;

/** @brief Signed byte count returned by positional reads and writes (-1 on error). */
typedef ptrdiff_t platform_ssize_t;

/** @brief Native error code of the last failed platform call: @c errno, or @c GetLastError() on Windows. */
int platform_last_error(void);

/** @brief Set the native error code reported by ::platform_last_error. */
void platform_set_last_error(int code);

/**
 * @brief Open @p path for reading and writing.
 * @param create_exclusive When true, create the file and fail with ::PLATFORM_ERROR_FILE_EXISTS if it already exists.
 * @return A file descriptor, or -1 on failure.
 */
int platform_open_rw(const char *path, bool create_exclusive);

/** @brief Close @p fd. @return 0 on success, -1 on failure. */
int platform_close(int fd);

/** @brief Read up to @p size bytes at @p offset. @return Bytes read (0 at EOF), or -1 on failure. */
platform_ssize_t platform_pread(int fd, void *buffer, size_t size, platform_off_t offset);

/** @brief Write @p size bytes at @p offset. @return Bytes written, or -1 on failure. */
platform_ssize_t platform_pwrite(int fd, const void *buffer, size_t size, platform_off_t offset);

/** @brief Flush file data to stable storage. @return 0 on success, -1 on failure. */
int platform_fsync(int fd);

/** @brief Truncate or extend the file to exactly @p size bytes. @return 0 on success, -1 on failure. */
int platform_ftruncate(int fd, platform_off_t size);

/** @brief Remove the file at @p path. @return 0 on success, -1 on failure. */
int platform_unlink(const char *path);

/** @brief Whether a file exists at @p path. */
bool platform_file_exists(const char *path);

/** @brief Initialize a mutex. */
void platform_mutex_init(platform_mutex *mutex);

/** @brief Destroy a mutex initialized by ::platform_mutex_init. */
void platform_mutex_destroy(platform_mutex *mutex);

/** @brief Acquire @p mutex. */
void platform_mutex_lock(platform_mutex *mutex);

/** @brief Release @p mutex. */
void platform_mutex_unlock(platform_mutex *mutex);

/** @brief Run @p routine exactly once for the given @p flag, across all threads. */
void platform_once(platform_once_flag *flag, void (*routine)(void));
