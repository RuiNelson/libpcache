#include "libpcache.h"
#include "tst.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <AvailabilityMacros.h>

/* ──────────── Configuration ──────────── */

#define TMP_DB    "/tmp/pcache_bench.db"
#define TMP_DATA  "/tmp/pcache_bench.dat"

#define BENCH_PAGE_SIZE (32 * 1024u)   /* 32 KiB */
#define BENCH_ID_SIZE   64u

static const pcache_configuration BENCH_FIXED_CFG = {
    .capacity_policy = PCACHE_CAPACITY_FIXED,
    .page_size       = BENCH_PAGE_SIZE,
    .max_pages       = 0,  /* overridden per benchmark */
    .id_size         = BENCH_ID_SIZE,
};

static void cleanup_files(void) {
    unlink(TMP_DB);
    unlink(TMP_DATA);
}

static const pcache_file_pair BENCH_PATHS = {TMP_DB, TMP_DATA};

/* Return elapsed time in seconds as a double. */
static double elapsed_since(struct timespec start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
}

/* Fill page with a deterministic pattern. */
static void fill_page(uint8_t *buf, uint32_t page_idx) {
    for (uint32_t i = 0; i < BENCH_PAGE_SIZE; i++)
        buf[i] = (uint8_t)(page_idx ^ i);
}

/* Verify page content matches expected pattern. Returns 0 on match, -1 on mismatch. */
static int verify_page(const uint8_t *buf, uint32_t page_idx) {
    for (uint32_t i = 0; i < BENCH_PAGE_SIZE; i++)
        if (buf[i] != (uint8_t)(page_idx ^ i))
            return -1;
    return 0;
}

/* ──────────── Benchmark helpers ──────────── */

typedef struct {
    double   mb_written;
    double   ops_written;
    double   write_time_sec;
    double   mb_read;
    double   ops_read;
    double   read_time_sec;
} benchmark_result;

/* Benchmark sequential write of count pages. */
static void bench_seq_write(pcache_handle h, uint32_t count, benchmark_result *r) {
    uint8_t id[BENCH_ID_SIZE];
    uint8_t *page = malloc(BENCH_PAGE_SIZE);
    struct timespec t0;

    memset(id, 0, BENCH_ID_SIZE);
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (uint32_t i = 0; i < count; i++) {
        fill_page(page, i);
        pcache_put_page(h, id, page, false, false, NULL, NULL, NULL);
        /* advance id by 1 in first byte (16-byte id, easiest way to get unique ids) */
        id[0] = (uint8_t)(i + 1);
    }

    r->write_time_sec = elapsed_since(t0);
    r->mb_written = (double)count * BENCH_PAGE_SIZE / (1024.0 * 1024.0);
    r->ops_written = (double)count;
    free(page);
}

/* Benchmark sequential read of count pages. */
static void bench_seq_read(pcache_handle h, uint32_t count, benchmark_result *r) {
    uint8_t id[BENCH_ID_SIZE];
    uint8_t *page = malloc(BENCH_PAGE_SIZE);
    struct timespec t0;

    memset(id, 0, BENCH_ID_SIZE);
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (uint32_t i = 0; i < count; i++) {
        id[0] = (uint8_t)(i + 1);
        pcache_get_page(h, id, page, NULL, NULL, NULL);
    }

    r->read_time_sec = elapsed_since(t0);
    r->mb_read = (double)count * BENCH_PAGE_SIZE / (1024.0 * 1024.0);
    r->ops_read = (double)count;
    free(page);
}

/* ──────────── Volume space overhead test ──────────── */

static void print_result(const char *label, uint32_t count, benchmark_result *r) {
    double write_mbps = r->mb_written / r->write_time_sec;
    double read_mbps  = r->mb_read   / r->read_time_sec;
    double write_ops  = r->ops_written / r->write_time_sec;
    double read_ops   = r->ops_read   / r->read_time_sec;
    printf("%-40s %8.2f MB/s write  %8.2f MB/s read  %10.0f writes/s  %10.0f reads/s\n",
           label, write_mbps, read_mbps, write_ops, read_ops);
}

/* ──────────── Test suite ──────────── */

tstsuite("pcache benchmarks") {

    tstcase("volume footprint: 1 GB, 32 KiB pages, FIFO policy") {
        cleanup_files();

        /* 1 GB volume with 32 KiB pages → 32768 pages */
        const uint32_t target_pages = (1u * 1024u * 1024u * 1024u) / BENCH_PAGE_SIZE;
        const pcache_configuration cfg = {
            .capacity_policy = PCACHE_CAPACITY_FIFO,
            .page_size       = BENCH_PAGE_SIZE,
            .max_pages       = target_pages,
            .id_size         = BENCH_ID_SIZE,
        };

        printf("\n=== Volume footprint: 1 GB, 32 KiB pages, FIFO ===\n");
        printf("Pages:        %u\n", target_pages);
        printf("Page size:    %u bytes\n", BENCH_PAGE_SIZE);
        printf("Volume size:  %.0f MiB (%.2f GiB)\n",
               (double)target_pages * BENCH_PAGE_SIZE / (1024.0 * 1024.0),
               (double)target_pages * BENCH_PAGE_SIZE / (1024.0 * 1024.0 * 1024.0));

        /* Create volume without preallocation */
        pcache_create_error ce = PCACHE_CREATE_OK;
        pcache_create(&BENCH_PATHS, &cfg, false, false, &ce, NULL, NULL);
        tstcheck(ce == PCACHE_CREATE_OK, "create must succeed");
        pcache_close(0, NULL, NULL, NULL);

        struct stat st_db, st_dat;
        tstcheck(stat(TMP_DB, &st_db) == 0, "stat on database must succeed");
        tstcheck(stat(TMP_DATA, &st_dat) == 0, "stat on data file must succeed");
        printf("Database file (empty):           %10lu bytes\n", (unsigned long)st_db.st_size);
        printf("Data file (empty):               %10lu bytes\n", (unsigned long)st_dat.st_size);

        cleanup_files();

        /* Fill the volume with pages that have zero data and random IDs */
        pcache_create(&BENCH_PATHS, &cfg, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&BENCH_PATHS, false, NULL, NULL, NULL);

        uint8_t *id = malloc(BENCH_ID_SIZE);
        uint8_t *page = malloc(BENCH_PAGE_SIZE);
        memset(page, 0, BENCH_PAGE_SIZE);  /* zeros for data */

        printf("Filling volume with %u pages (zeros + random IDs)...\n", target_pages);
        for (uint32_t i = 0; i < target_pages; i++) {
            for (uint32_t j = 0; j < BENCH_ID_SIZE; j++)
                id[j] = (uint8_t)(arc4random() & 0xff);
            pcache_put_page(h, id, page, false, false, NULL, NULL, NULL);
        }

        stat(TMP_DB, &st_db);
        stat(TMP_DATA, &st_dat);
        printf("Database file (filled):         %10lu bytes\n", (unsigned long)st_db.st_size);
        printf("Data file (filled):             %10lu bytes  (%.2f GiB)\n",
               (unsigned long)st_dat.st_size,
               (double)st_dat.st_size / (1024.0 * 1024.0 * 1024.0));

        /* Write one more page to trigger eviction and measure growth */
        for (uint32_t j = 0; j < BENCH_ID_SIZE; j++)
            id[j] = (uint8_t)(arc4random() & 0xff);
        pcache_put_page(h, id, page, false, false, NULL, NULL, NULL);

        stat(TMP_DB, &st_db);
        stat(TMP_DATA, &st_dat);
        printf("Database file (+1 evicted):     %10lu bytes\n", (unsigned long)st_db.st_size);
        printf("Data file (+1 evicted):         %10lu bytes\n", (unsigned long)st_dat.st_size);

        free(id);
        free(page);
        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    tstcase("sequential write throughput: 1000 pages, 32 KiB") {
        cleanup_files();

        const uint32_t n = 1000;
        const pcache_configuration cfg = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = BENCH_PAGE_SIZE,
            .max_pages       = n,
            .id_size         = BENCH_ID_SIZE,
        };

        pcache_create(&BENCH_PATHS, &cfg, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&BENCH_PATHS, false, NULL, NULL, NULL);

        benchmark_result r = {0};
        bench_seq_write(h, n, &r);

        printf("\n=== Sequential write: %u pages × 32 KiB ===\n", n);
        printf("Time:    %.3f sec\n", r.write_time_sec);
        printf("Written: %.2f MiB\n", r.mb_written);
        printf("Write throughput: %.2f MB/s  (%.0f ops/s)\n",
               r.mb_written / r.write_time_sec,
               r.ops_written / r.write_time_sec);

        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    tstcase("sequential read throughput: 1000 pages, 32 KiB") {
        cleanup_files();

        const uint32_t n = 1000;
        const pcache_configuration cfg = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = BENCH_PAGE_SIZE,
            .max_pages       = n,
            .id_size         = BENCH_ID_SIZE,
        };

        pcache_create(&BENCH_PATHS, &cfg, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&BENCH_PATHS, false, NULL, NULL, NULL);

        /* Pre-write pages */
        uint8_t id[BENCH_ID_SIZE];
        uint8_t *page = malloc(BENCH_PAGE_SIZE);
        for (uint32_t i = 0; i < n; i++) {
            id[0] = (uint8_t)(i + 1);
            fill_page(page, i);
            pcache_put_page(h, id, page, false, false, NULL, NULL, NULL);
        }
        free(page);

        benchmark_result r = {0};
        bench_seq_read(h, n, &r);

        printf("\n=== Sequential read: %u pages × 32 KiB ===\n", n);
        printf("Time:    %.3f sec\n", r.read_time_sec);
        printf("Read:    %.2f MiB\n", r.mb_read);
        printf("Read throughput:  %.2f MB/s  (%.0f ops/s)\n",
               r.mb_read / r.read_time_sec,
               r.ops_read / r.read_time_sec);

        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    tstcase("write + read round-trip throughput") {
        cleanup_files();

        const uint32_t n = 500;
        const pcache_configuration cfg = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = BENCH_PAGE_SIZE,
            .max_pages       = n,
            .id_size         = BENCH_ID_SIZE,
        };

        pcache_create(&BENCH_PATHS, &cfg, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&BENCH_PATHS, false, NULL, NULL, NULL);

        uint8_t id[BENCH_ID_SIZE];
        uint8_t *page = malloc(BENCH_PAGE_SIZE);
        struct timespec t0;

        /* Write */
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (uint32_t i = 0; i < n; i++) {
            id[0] = (uint8_t)(i + 1);
            fill_page(page, i);
            pcache_put_page(h, id, page, false, false, NULL, NULL, NULL);
        }
        double write_time = elapsed_since(t0);

        /* Read */
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (uint32_t i = 0; i < n; i++) {
            id[0] = (uint8_t)(i + 1);
            pcache_get_page(h, id, page, NULL, NULL, NULL);
        }
        double read_time = elapsed_since(t0);

        double total_mb = (double)n * BENCH_PAGE_SIZE / (1024.0 * 1024.0);
        printf("\n=== Write+Read round-trip: %u pages × 32 KiB ===\n", n);
        printf("Write: %.3f sec  (%.2f MB/s, %.0f ops/s)\n",
               write_time, total_mb / write_time, (double)n / write_time);
        printf("Read:  %.3f sec  (%.2f MB/s, %.0f ops/s)\n",
               read_time, total_mb / read_time, (double)n / read_time);

        free(page);
        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    tstcase("random access latency: 1000 point lookups") {
        cleanup_files();

        const uint32_t n = 1000;
        const pcache_configuration cfg = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = BENCH_PAGE_SIZE,
            .max_pages       = n,
            .id_size         = BENCH_ID_SIZE,
        };

        pcache_create(&BENCH_PATHS, &cfg, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&BENCH_PATHS, false, NULL, NULL, NULL);

        uint8_t id[BENCH_ID_SIZE];
        uint8_t *page = malloc(BENCH_PAGE_SIZE);

        /* Write all pages */
        for (uint32_t i = 0; i < n; i++) {
            id[0] = (uint8_t)(i + 1);
            fill_page(page, i);
            pcache_put_page(h, id, page, false, false, NULL, NULL, NULL);
        }

        /* Lookup at fixed intervals: 0, 100, 200, ... (sequential pattern —
         * real random would need a PRNG, but this is reproducible) */
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (uint32_t i = 0; i < n; i += 100) {
            id[0] = (uint8_t)(i + 1);
            pcache_get_page(h, id, page, NULL, NULL, NULL);
        }
        double elapsed = elapsed_since(t0);
        printf("\n=== Point lookups (every 100th page): 10 lookups ===\n");
        printf("Time:   %.3f sec\n", elapsed);
        printf("Avg per lookup: %.3f ms\n", elapsed / 10.0 * 1000.0);

        free(page);
        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    tstcase("FIFO eviction: full-cycle throughput") {
        cleanup_files();

        /* 100-slot FIFO, write 1000 pages (10× capacity) */
        const uint32_t capacity = 100;
        const uint32_t total     = 1000;
        const pcache_configuration cfg = {
            .capacity_policy = PCACHE_CAPACITY_FIFO,
            .page_size       = BENCH_PAGE_SIZE,
            .max_pages       = capacity,
            .id_size         = BENCH_ID_SIZE,
        };

        pcache_create(&BENCH_PATHS, &cfg, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&BENCH_PATHS, false, NULL, NULL, NULL);

        uint8_t id[BENCH_ID_SIZE];
        uint8_t *page = malloc(BENCH_PAGE_SIZE);
        struct timespec t0;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (uint32_t i = 0; i < total; i++) {
            id[0] = (uint8_t)(i + 1);
            fill_page(page, i);
            pcache_put_page(h, id, page, false, false, NULL, NULL, NULL);
        }
        double elapsed = elapsed_since(t0);

        double mb = (double)total * BENCH_PAGE_SIZE / (1024.0 * 1024.0);
        printf("\n=== FIFO eviction: %u writes (10× capacity of %u) ===\n", total, capacity);
        printf("Time:   %.3f sec\n", elapsed);
        printf("Throughput: %.2f MB/s  (%.0f ops/s)\n", mb / elapsed, (double)total / elapsed);

        /* Verify that only the last 'capacity' pages survived */
        uint32_t present = 0;
        for (uint32_t i = total - capacity; i < total; i++) {
            id[0] = (uint8_t)(i + 1);
            if (pcache_check_page(h, id, NULL, NULL))
                present++;
        }
        printf("Live pages in last %u ids: %u / %u\n", capacity, present, capacity);
        tstcheck((int)present == (int)capacity,
                 "exactly %u pages must survive after %u writes (got %u)",
                 capacity, total, present);

        free(page);
        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    tstcase("delete throughput") {
        cleanup_files();

        const uint32_t n = 500;
        const pcache_configuration cfg = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = BENCH_PAGE_SIZE,
            .max_pages       = n,
            .id_size         = BENCH_ID_SIZE,
        };

        pcache_create(&BENCH_PATHS, &cfg, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&BENCH_PATHS, true, NULL, NULL, NULL);  /* preload free list */

        uint8_t id[BENCH_ID_SIZE];
        uint8_t *page = malloc(BENCH_PAGE_SIZE);

        /* Write pages */
        for (uint32_t i = 0; i < n; i++) {
            id[0] = (uint8_t)(i + 1);
            fill_page(page, i);
            pcache_put_page(h, id, page, false, false, NULL, NULL, NULL);
        }

        /* Delete every other page */
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (uint32_t i = 0; i < n; i += 2) {
            id[0] = (uint8_t)(i + 1);
            pcache_delete_page(h, id, false, false, NULL, NULL, NULL);
        }
        double elapsed = elapsed_since(t0);

        printf("\n=== Delete: %u pages (every other) ===\n", n / 2);
        printf("Time:   %.3f sec\n", elapsed);
        printf("Throughput: %.0f deletes/s\n", (double)(n / 2) / elapsed);

        free(page);
        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    tstcase("defragment throughput") {
        cleanup_files();

        const uint32_t n = 200;
        const pcache_configuration cfg = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = BENCH_PAGE_SIZE,
            .max_pages       = n,
            .id_size         = BENCH_ID_SIZE,
        };

        pcache_create(&BENCH_PATHS, &cfg, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&BENCH_PATHS, false, NULL, NULL, NULL);

        uint8_t id[BENCH_ID_SIZE];
        uint8_t *page = malloc(BENCH_PAGE_SIZE);

        /* Write n pages, delete every other to leave ~100 live pages */
        for (uint32_t i = 0; i < n; i++) {
            id[0] = (uint8_t)(i + 1);
            fill_page(page, i);
            pcache_put_page(h, id, page, false, false, NULL, NULL, NULL);
        }
        for (uint32_t i = 0; i < n; i += 2) {
            id[0] = (uint8_t)(i + 1);
            pcache_delete_page(h, id, false, false, NULL, NULL, NULL);
        }

        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        pcache_defragment(h, NULL, NULL, true, false, NULL, NULL, NULL);
        double elapsed = elapsed_since(t0);

        uint32_t live = 0;
        for (uint32_t i = 0; i < n; i++) {
            id[0] = (uint8_t)(i + 1);
            if (pcache_check_page(h, id, NULL, NULL))
                live++;
        }

        printf("\n=== Defragment: %u original pages, %u live after delete ===\n", n, live);
        printf("Time:   %.3f sec\n", elapsed);
        printf("Throughput: %.0f pages/s\n", (double)n / elapsed);

        free(page);
        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }
}