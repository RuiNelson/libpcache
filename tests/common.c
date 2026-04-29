#include "common.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int next_sequence(void) {
    static int seq = 0;
    return seq++;
}

void test_paths_init(test_paths *paths, const char *prefix) {
    int sequence = next_sequence();
    snprintf(paths->database_path, sizeof(paths->database_path), "/tmp/pcache_%s_%d_%d.db", prefix, (int)getpid(),
             sequence);
    snprintf(paths->data_path, sizeof(paths->data_path), "/tmp/pcache_%s_%d_%d.dat", prefix, (int)getpid(), sequence);

    /* Make sure no leftover files trip pcache_create. */
    unlink(paths->database_path);
    unlink(paths->data_path);

    paths->pair.database_path = paths->database_path;
    paths->pair.data_path     = paths->data_path;
}

void test_paths_cleanup(test_paths *paths) {
    unlink(paths->database_path);
    unlink(paths->data_path);
}

pcache_handle make_volume_and_open(const test_paths *paths, const pcache_configuration *config) {
    pcache_create_error create_error = (pcache_create_error)-1;
    pcache_create(&paths->pair, config, true, true, &create_error, NULL, NULL);
    if (create_error != PCACHE_CREATE_OK)
        return 0;

    pcache_open_error open_error = (pcache_open_error)-1;
    pcache_handle     handle     = pcache_open(&paths->pair, &open_error, NULL, NULL);
    if (open_error != PCACHE_OPEN_OK)
        return 0;
    return handle;
}

void make_id_with_index(void *id_buffer, size_t id_size, uint32_t index) {
    unsigned char *bytes = (unsigned char *)id_buffer;
    memset(bytes, 0, id_size);
    /* Place the index in the last four bytes in big-endian order so that
     * byte-by-byte comparison of two ids produces the same ordering as the
     * numeric ordering of the indices. */
    if (id_size >= 4) {
        bytes[id_size - 4] = (unsigned char)((index >> 24) & 0xFF);
        bytes[id_size - 3] = (unsigned char)((index >> 16) & 0xFF);
        bytes[id_size - 2] = (unsigned char)((index >> 8) & 0xFF);
        bytes[id_size - 1] = (unsigned char)(index & 0xFF);
    }
}

void make_page_with_index(void *page_buffer, size_t page_size, uint32_t index) {
    unsigned char *bytes = (unsigned char *)page_buffer;
    for (size_t i = 0; i < page_size; i++) {
        bytes[i] = (unsigned char)((index + i) & 0xFF);
    }
}

off_t file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return st.st_size;
}
