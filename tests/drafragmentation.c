/* Defragmentation tests */

#include "common.h"
#include "libpcache.h"
#include "tst.h"

#include <stdint.h>
#include <string.h>

#define ID_SIZE   16
#define PAGE_SIZE 256
#define MAX_PAGES 16

typedef struct progress_recorder {
    int    invocations;
    double last_progress;
    bool   reached_one;
} progress_recorder;

static bool record_progress(double progress, void *user_data) {
    progress_recorder *recorder = (progress_recorder *)user_data;
    recorder->invocations++;
    recorder->last_progress = progress;
    if (progress >= 1.0)
        recorder->reached_one = true;
    return true; /* never cancel */
}

static bool cancel_after_first(double progress, void *user_data) {
    int *count = (int *)user_data;
    (*count)++;
    (void)progress;
    return false; /* always cancel */
}

static void put_indexed(pcache_handle handle, uint32_t index) {
    unsigned char id[ID_SIZE], page[PAGE_SIZE];
    make_id_with_index(id, ID_SIZE, index);
    make_page_with_index(page, PAGE_SIZE, index);
    pcache_put_page(handle, id, page, false, true, NULL, NULL, NULL);
}

static void delete_indexed(pcache_handle handle, uint32_t index) {
    unsigned char id[ID_SIZE];
    make_id_with_index(id, ID_SIZE, index);
    pcache_delete_page(handle, id, false, true, NULL, NULL, NULL);
}

tstsuite("defragmentation") {

    tstcase("FIXED defragment compacts live pages and preserves contents") {
        test_paths paths;
        test_paths_init(&paths, "defrag_fixed");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        /* Fill all 16 slots, then delete a scattered subset to create gaps. */
        for (uint32_t i = 0; i < MAX_PAGES; i++)
            put_indexed(handle, i);

        const uint32_t to_delete[] = {1, 4, 7, 9, 12, 14};
        const size_t   ndelete     = sizeof(to_delete) / sizeof(to_delete[0]);
        for (size_t i = 0; i < ndelete; i++)
            delete_indexed(handle, to_delete[i]);

        progress_recorder       recorder   = {0};
        pcache_defragment_error defrag_err = (pcache_defragment_error)-1;
        pcache_defragment(handle, record_progress, &recorder, true, true, &defrag_err, NULL, NULL);
        tstcheck(defrag_err == PCACHE_DEFRAGMENT_OK, "defragment OK");
        tstcheck(recorder.reached_one, "progress reaches 1.0");

        /* All non-deleted ids must still be readable with their original content. */
        const uint32_t survivors[] = {0, 2, 3, 5, 6, 8, 10, 11, 13, 15};
        const size_t   nsurv       = sizeof(survivors) / sizeof(survivors[0]);
        bool           ok          = true;
        for (size_t i = 0; i < nsurv; i++) {
            unsigned char id[ID_SIZE], page_out[PAGE_SIZE], expected[PAGE_SIZE];
            make_id_with_index(id, ID_SIZE, survivors[i]);
            make_page_with_index(expected, PAGE_SIZE, survivors[i]);
            pcache_get_error get_error = (pcache_get_error)-1;
            pcache_get_page(handle, id, page_out, &get_error, NULL, NULL);
            if (get_error != PCACHE_GET_OK || memcmp(page_out, expected, PAGE_SIZE) != 0) {
                ok = false;
                break;
            }
        }
        tstcheck(ok, "all surviving ids readable with original content");

        /* shrink_file=true means data file is truncated to live pages * page_size. */
        off_t expected_size = (off_t)nsurv * PAGE_SIZE;
        tstcheck(file_size(paths.data_path) == expected_size, "data file truncated to live pages");

        pcache_inspect_page_count_error count_error = (pcache_inspect_page_count_error)-1;
        pcache_page_count               counts      = pcache_inspect_page_count(handle, &count_error, NULL);
        tstcheck(counts.used == nsurv, "used count matches number of survivors");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("defragment with cancellation leaves volume consistent") {
        test_paths paths;
        test_paths_init(&paths, "defrag_cancel");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        for (uint32_t i = 0; i < MAX_PAGES; i++)
            put_indexed(handle, i);
        delete_indexed(handle, 0); /* create at least one gap so defrag has work to do */

        int                     callback_count = 0;
        pcache_defragment_error defrag_err     = (pcache_defragment_error)-1;
        pcache_defragment(handle, cancel_after_first, &callback_count, false, true, &defrag_err, NULL, NULL);
        tstcheck(defrag_err == PCACHE_DEFRAGMENT_CANCELLED, "defragment cancelled");

        /* All non-deleted ids must still be retrievable with the correct content. */
        bool ok = true;
        for (uint32_t i = 1; i < MAX_PAGES; i++) {
            unsigned char id[ID_SIZE], page_out[PAGE_SIZE], expected[PAGE_SIZE];
            make_id_with_index(id, ID_SIZE, i);
            make_page_with_index(expected, PAGE_SIZE, i);
            pcache_get_error get_error = (pcache_get_error)-1;
            pcache_get_page(handle, id, page_out, &get_error, NULL, NULL);
            if (get_error != PCACHE_GET_OK || memcmp(page_out, expected, PAGE_SIZE) != 0) {
                ok = false;
                break;
            }
        }
        tstcheck(ok, "post-cancel volume is still readable");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("FIFO defragment is a no-op and reports progress = 1.0") {
        test_paths paths;
        test_paths_init(&paths, "defrag_fifo_noop");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIFO,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        /* Push past steady state so the slot layout is non-trivial. */
        for (uint32_t i = 0; i < MAX_PAGES + 3; i++)
            put_indexed(handle, i);

        /* Snapshot which ids are live. */
        bool live_before[MAX_PAGES + 3];
        for (uint32_t i = 0; i < MAX_PAGES + 3; i++) {
            unsigned char id[ID_SIZE];
            make_id_with_index(id, ID_SIZE, i);
            pcache_check_error check_error = (pcache_check_error)-1;
            live_before[i]                 = pcache_check_page(handle, id, &check_error, NULL);
        }

        progress_recorder       recorder   = {0};
        pcache_defragment_error defrag_err = (pcache_defragment_error)-1;
        pcache_defragment(handle, record_progress, &recorder, false, true, &defrag_err, NULL, NULL);
        tstcheck(defrag_err == PCACHE_DEFRAGMENT_OK, "FIFO defragment returns OK");
        tstcheck(recorder.invocations == 1, "callback invoked exactly once");
        tstcheck(recorder.last_progress >= 1.0, "callback invoked with progress >= 1.0");

        /* The same set of ids must still be live with identical contents. */
        bool same = true;
        for (uint32_t i = 0; i < MAX_PAGES + 3; i++) {
            unsigned char id[ID_SIZE];
            make_id_with_index(id, ID_SIZE, i);
            pcache_check_error check_error = (pcache_check_error)-1;
            bool               now         = pcache_check_page(handle, id, &check_error, NULL);
            if (now != live_before[i]) {
                same = false;
                break;
            }
        }
        tstcheck(same, "live set unchanged by FIFO defrag");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }
}
