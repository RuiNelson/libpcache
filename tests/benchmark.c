/* Benchmark: measure database size and write time for multiple libpcache configurations. */

#include "common.h"
#include "libpcache.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PAGES_PER_BATCH  1000

typedef struct {
    const char      *label;
    uint64_t        volume_bytes;
    uint32_t        page_size;
    uint32_t        id_size;
    uint32_t        max_pages;
} benchmark_config;

int main(void) {
    printf("| Volume | Page | ID | Pages | DB size | Data file | Total | Time | Pages/s |\n");
    printf("|--------|------|----|----:|--------:|----------:|------:|-----:|--------:|\n");

    benchmark_config configs[] = {
        {"1 GB / 64 KB", 1ULL * 1024 * 1024 * 1024, 64 * 1024, 36, 0},
        {"1 GB / 64 KB / 72B ID", 1ULL * 1024 * 1024 * 1024, 64 * 1024, 72, 0},
        {"1 GB / 32 KB", 1ULL * 1024 * 1024 * 1024, 32 * 1024, 36, 0},
        {"2 GB / 128 KB", 2ULL * 1024 * 1024 * 1024, 128 * 1024, 36, 0},
        {"100 MB / 1 KB", 100ULL * 1024 * 1024, 1 * 1024, 36, 0},
    };

    const size_t num_configs = sizeof(configs) / sizeof(configs[0]);

    for (size_t cfg = 0; cfg < num_configs; cfg++) {
        benchmark_config *c = &configs[cfg];
        c->max_pages = (uint32_t)(c->volume_bytes / c->page_size);

        /* Limit batch size to avoid insane memory usage for 100MB/1KB (100K pages = 100MB buffer). */
        uint32_t batch_size = PAGES_PER_BATCH;
        if (c->page_size < 8 * 1024) {
            batch_size = 100; /* 100 pages per batch to keep buffer reasonable */
        }

        const size_t ids_buffer_size  = batch_size * c->id_size;
        const size_t data_buffer_size = batch_size * c->page_size;
        unsigned char *ids_buffer     = malloc(ids_buffer_size);
        unsigned char *data_buffer   = malloc(data_buffer_size);
        if (!ids_buffer || !data_buffer) {
            fprintf(stderr, "%s: failed to allocate buffers\n", c->label);
            free(ids_buffer);
            free(data_buffer);
            continue;
        }
        memset(data_buffer, 0, data_buffer_size);

        /* Build unique paths for this config. */
        char db_path[256], dat_path[256];
        snprintf(db_path, sizeof(db_path), "/tmp/pcache_bench_%zu.db", cfg);
        snprintf(dat_path, sizeof(dat_path), "/tmp/pcache_bench_%zu.dat", cfg);
        unlink(db_path);
        unlink(dat_path);

        pcache_file_pair paths = { db_path, dat_path };

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = c->page_size,
            .max_pages       = c->max_pages,
            .id_size         = c->id_size,
        };

        pcache_create_error create_error = (pcache_create_error)-1;
        pcache_create(&paths, &config, false, false, &create_error, NULL, NULL);
        if (create_error != PCACHE_CREATE_OK) {
            fprintf(stderr, "%s: pcache_create failed: %d\n", c->label, create_error);
            free(ids_buffer);
            free(data_buffer);
            continue;
        }

        pcache_open_error open_error = (pcache_open_error)-1;
        pcache_handle     handle     = pcache_open(&paths, &open_error, NULL, NULL);
        if (open_error != PCACHE_OPEN_OK) {
            fprintf(stderr, "%s: pcache_open failed: %d\n", c->label, open_error);
            free(ids_buffer);
            free(data_buffer);
            continue;
        }

        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        uint32_t pages_written = 0;
        while (pages_written < c->max_pages) {
            uint32_t remaining = c->max_pages - pages_written;
            uint32_t this_batch = (remaining < batch_size) ? remaining : batch_size;

            for (uint32_t i = 0; i < this_batch; i++) {
                uint32_t page_idx = pages_written + i;
                uint32_t h = page_idx * 2654435771ul;
                unsigned char *id = ids_buffer + i * c->id_size;
                memset(id, 0, c->id_size);
                if (c->id_size >= 4) {
                    id[c->id_size - 4] = (unsigned char)((h >> 24) & 0xFF);
                    id[c->id_size - 3] = (unsigned char)((h >> 16) & 0xFF);
                    id[c->id_size - 2] = (unsigned char)((h >> 8) & 0xFF);
                    id[c->id_size - 1] = (unsigned char)(h & 0xFF);
                }
            }

            pcache_put_error put_error = (pcache_put_error)-1;
            pcache_put_pages(handle, this_batch, ids_buffer, data_buffer,
                            false, false, &put_error, NULL, NULL);
            if (put_error != PCACHE_PUT_OK) {
                fprintf(stderr, "%s: pcache_put_pages failed at page %u: %d\n",
                        c->label, pages_written, put_error);
                pcache_close(handle, NULL, NULL, NULL);
                free(ids_buffer);
                free(data_buffer);
                goto next_config;
            }

            pages_written += this_batch;
        }

        clock_gettime(CLOCK_MONOTONIC, &end);
        pcache_close(handle, NULL, NULL, NULL);

        double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
        off_t db_size  = file_size(db_path);
        off_t dat_size = file_size(dat_path);

        double db_mb   = db_size / (1024.0 * 1024.0);
        double dat_gb  = dat_size / (1024.0 * 1024.0 * 1024.0);
        double total_gb = (db_size + dat_size) / (1024.0 * 1024.0 * 1024.0);
        double pages_per_sec = pages_written / elapsed;

        printf("| %s | %u KB | %u B | %u | %.2f MB |\n",
               c->label,
               c->page_size / 1024,
               c->id_size,
               pages_written,
               db_mb);

        /* Clean up temp files. */
        unlink(db_path);
        unlink(dat_path);
        /* SQLite shared-memory and WAL auxiliary files. */
        {
            char shm_path[256], wal_path[256];
            snprintf(shm_path, sizeof(shm_path), "%s-shm", db_path);
            snprintf(wal_path, sizeof(wal_path), "%s-wal", db_path);
            unlink(shm_path);
            unlink(wal_path);
        }

next_config:
        free(ids_buffer);
        free(data_buffer);
    }

    return 0;
}
