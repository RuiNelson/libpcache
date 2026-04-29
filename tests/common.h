#pragma once

#include "libpcache.h"

#include <stddef.h>
#include <sys/types.h>

/* Helper that owns a pair of unique temporary paths for a test volume. */
typedef struct test_paths {
    char             database_path[256];
    char             data_path[256];
    pcache_file_pair pair;
} test_paths;

/**
 * Initialise a test_paths with two unique paths under /tmp, derived from
 * the calling process PID and an internal sequence counter, and unlink any
 * pre-existing files at those paths.
 *
 * @param prefix Short string used to disambiguate the path from other tests.
 */
void test_paths_init(test_paths *paths, const char *prefix);

/** Remove the two files associated with the given paths. */
void test_paths_cleanup(test_paths *paths);

/**
 * Create a volume at the given paths with the supplied configuration and
 * eager preallocation, then open and return a handle. Returns 0 on failure.
 */
pcache_handle make_volume_and_open(const test_paths *paths, const pcache_configuration *config);

/** Fill an identifier buffer of @p id_size bytes with a deterministic pattern derived from @p index. */
void make_id_with_index(void *id_buffer, size_t id_size, uint32_t index);

/** Fill a page buffer of @p page_size bytes with a deterministic pattern derived from @p index. */
void make_page_with_index(void *page_buffer, size_t page_size, uint32_t index);

/** Return the size in bytes of the file at @p path, or -1 on error. */
off_t file_size(const char *path);
