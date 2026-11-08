#include "platform.h"

#include <stdlib.h>

#ifdef _WIN32

#include <fcntl.h>
#include <io.h>
#include <wchar.h>

/* All failures are reported as raw Win32 system error codes via SetLastError/GetLastError. */

int platform_last_error(void) {
    return (int)GetLastError();
}

void platform_set_last_error(int code) {
    SetLastError((DWORD)code);
}

/* Convert a UTF-8 path to a newly allocated wide string; NULL with the last error set on failure. */
static wchar_t *wide_path_from_utf8(const char *path) {
    int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (wide_length <= 0)
        return NULL; /* MultiByteToWideChar has set the last error. */
    wchar_t *wide_path = malloc((size_t)wide_length * sizeof *wide_path);
    if (!wide_path) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide_path, wide_length) <= 0) {
        DWORD last_error = GetLastError();
        free(wide_path);
        SetLastError(last_error);
        return NULL;
    }
    return wide_path;
}

/* Fetch the OS handle behind a CRT descriptor; INVALID_HANDLE_VALUE with the last error set on failure. */
static HANDLE handle_from_fd(int fd) {
    if (fd < 0) {
        SetLastError(ERROR_INVALID_HANDLE);
        return INVALID_HANDLE_VALUE;
    }
    intptr_t os_handle = _get_osfhandle(fd);
    if (os_handle == -1) {
        SetLastError(ERROR_INVALID_HANDLE);
        return INVALID_HANDLE_VALUE;
    }
    return (HANDLE)os_handle;
}

static OVERLAPPED overlapped_at(platform_off_t offset) {
    OVERLAPPED overlapped = {0};
    overlapped.Offset     = (DWORD)((uint64_t)offset & 0xFFFFFFFFu);
    overlapped.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    return overlapped;
}

int platform_open_rw(const char *path, bool create_exclusive) {
    wchar_t *wide_path = wide_path_from_utf8(path);
    if (!wide_path)
        return -1;

    HANDLE file       = CreateFileW(wide_path,
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL,
                              create_exclusive ? CREATE_NEW : OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              NULL);
    DWORD  last_error = GetLastError();
    free(wide_path);
    if (file == INVALID_HANDLE_VALUE) {
        SetLastError(last_error);
        return -1;
    }

    int fd = _open_osfhandle((intptr_t)file, _O_BINARY);
    if (fd < 0) {
        CloseHandle(file);
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return -1;
    }
    return fd;
}

int platform_close(int fd) {
    HANDLE file = handle_from_fd(fd);
    if (file == INVALID_HANDLE_VALUE)
        return -1;
    if (_close(fd) != 0) {
        unsigned long os_error = _doserrno;
        SetLastError(os_error ? (DWORD)os_error : ERROR_INVALID_HANDLE);
        return -1;
    }
    return 0;
}

platform_ssize_t platform_pread(int fd, void *buffer, size_t size, platform_off_t offset) {
    if (offset < 0) {
        SetLastError(ERROR_NEGATIVE_SEEK);
        return -1;
    }
    HANDLE file = handle_from_fd(fd);
    if (file == INVALID_HANDLE_VALUE)
        return -1;

    DWORD      request     = size > MAXDWORD ? MAXDWORD : (DWORD)size;
    DWORD      transferred = 0;
    OVERLAPPED overlapped  = overlapped_at(offset);
    if (!ReadFile(file, buffer, request, &transferred, &overlapped)) {
        if (GetLastError() == ERROR_HANDLE_EOF)
            return 0; /* Reading at or past end of file: report a short (empty) read, like POSIX. */
        return -1;
    }
    return (platform_ssize_t)transferred;
}

platform_ssize_t platform_pwrite(int fd, const void *buffer, size_t size, platform_off_t offset) {
    if (offset < 0) {
        SetLastError(ERROR_NEGATIVE_SEEK);
        return -1;
    }
    HANDLE file = handle_from_fd(fd);
    if (file == INVALID_HANDLE_VALUE)
        return -1;

    DWORD      request     = size > MAXDWORD ? MAXDWORD : (DWORD)size;
    DWORD      transferred = 0;
    OVERLAPPED overlapped  = overlapped_at(offset);
    if (!WriteFile(file, buffer, request, &transferred, &overlapped))
        return -1;
    return (platform_ssize_t)transferred;
}

int platform_fsync(int fd) {
    HANDLE file = handle_from_fd(fd);
    if (file == INVALID_HANDLE_VALUE)
        return -1;
    return FlushFileBuffers(file) ? 0 : -1;
}

int platform_ftruncate(int fd, platform_off_t size) {
    if (size < 0) {
        SetLastError(ERROR_NEGATIVE_SEEK);
        return -1;
    }
    HANDLE file = handle_from_fd(fd);
    if (file == INVALID_HANDLE_VALUE)
        return -1;

    FILE_END_OF_FILE_INFO end_of_file;
    end_of_file.EndOfFile.QuadPart = size;
    return SetFileInformationByHandle(file, FileEndOfFileInfo, &end_of_file, sizeof end_of_file) ? 0 : -1;
}

int platform_unlink(const char *path) {
    wchar_t *wide_path = wide_path_from_utf8(path);
    if (!wide_path)
        return -1;
    BOOL  deleted    = DeleteFileW(wide_path);
    DWORD last_error = GetLastError();
    free(wide_path);
    if (!deleted) {
        SetLastError(last_error);
        return -1;
    }
    return 0;
}

bool platform_file_exists(const char *path) {
    wchar_t *wide_path = wide_path_from_utf8(path);
    if (!wide_path)
        return false;
    DWORD attributes = GetFileAttributesW(wide_path);
    DWORD last_error = GetLastError();
    free(wide_path);
    SetLastError(last_error);
    return attributes != INVALID_FILE_ATTRIBUTES;
}

void platform_mutex_init(platform_mutex *mutex) {
    InitializeSRWLock(mutex);
}

void platform_mutex_destroy(platform_mutex *mutex) {
    (void)mutex; /* SRW locks hold no resources. */
}

void platform_mutex_lock(platform_mutex *mutex) {
    AcquireSRWLockExclusive(mutex);
}

void platform_mutex_unlock(platform_mutex *mutex) {
    ReleaseSRWLockExclusive(mutex);
}

static BOOL CALLBACK run_once_routine(PINIT_ONCE flag, PVOID parameter, PVOID *context) {
    (void)flag;
    (void)context;
    void (**routine)(void) = parameter;
    (*routine)();
    return TRUE;
}

void platform_once(platform_once_flag *flag, void (*routine)(void)) {
    InitOnceExecuteOnce(flag, run_once_routine, (PVOID)&routine, NULL);
}

#else /* POSIX */

#include <fcntl.h>
#include <unistd.h>

int platform_last_error(void) {
    return errno;
}

void platform_set_last_error(int code) {
    errno = code;
}

int platform_open_rw(const char *path, bool create_exclusive) {
    if (create_exclusive)
        return open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
    return open(path, O_RDWR);
}

int platform_close(int fd) {
    return close(fd);
}

platform_ssize_t platform_pread(int fd, void *buffer, size_t size, platform_off_t offset) {
    return pread(fd, buffer, size, (off_t)offset);
}

platform_ssize_t platform_pwrite(int fd, const void *buffer, size_t size, platform_off_t offset) {
    return pwrite(fd, buffer, size, (off_t)offset);
}

int platform_fsync(int fd) {
    return fsync(fd);
}

int platform_ftruncate(int fd, platform_off_t size) {
    return ftruncate(fd, (off_t)size);
}

int platform_unlink(const char *path) {
    return unlink(path);
}

bool platform_file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

void platform_mutex_init(platform_mutex *mutex) {
    pthread_mutex_init(mutex, NULL);
}

void platform_mutex_destroy(platform_mutex *mutex) {
    pthread_mutex_destroy(mutex);
}

void platform_mutex_lock(platform_mutex *mutex) {
    pthread_mutex_lock(mutex);
}

void platform_mutex_unlock(platform_mutex *mutex) {
    pthread_mutex_unlock(mutex);
}

void platform_once(platform_once_flag *flag, void (*routine)(void)) {
    pthread_once(flag, routine);
}

#endif
