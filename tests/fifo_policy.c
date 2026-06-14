/* Tests related to the FIFO Policy */

#include "common.h"
#include "libpcache.h"
#include "tst.h"

#include <stdint.h>
#include <string.h>

#define ID_SIZE   16
#define PAGE_SIZE 128
#define MAX_PAGES 8

void pcache_test_fail_put_pwrite_after(size_t successful_writes);

static pcache_put_error put_one(pcache_handle handle, uint32_t index) {
    unsigned char id[ID_SIZE], page[PAGE_SIZE];
    make_id_with_index(id, ID_SIZE, index);
    make_page_with_index(page, PAGE_SIZE, index);
    pcache_put_error put_error = (pcache_put_error)-1;
    pcache_put_page(handle, id, page, false, true, &put_error, NULL, NULL);
    return put_error;
}

static bool exists(pcache_handle handle, uint32_t index) {
    unsigned char id[ID_SIZE];
    make_id_with_index(id, ID_SIZE, index);
    pcache_check_error check_error = (pcache_check_error)-1;
    return pcache_check_page(handle, id, &check_error, NULL);
}

tstsuite("FIFO policy") {

    tstcase("during fill-up phase put never fails and never evicts") {
        test_paths paths;
        test_paths_init(&paths, "fifo_fillup");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIFO,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        for (uint32_t i = 0; i < MAX_PAGES - 1; i++) {
            tstcheck(put_one(handle, i) == PCACHE_PUT_OK, "fill-up put OK");
        }

        pcache_inspect_page_count_error count_error = (pcache_inspect_page_count_error)-1;
        pcache_page_count               counts      = pcache_inspect_page_count(handle, &count_error, NULL);
        tstcheck(counts.used == MAX_PAGES - 1, "used == max_pages - 1 mid fill-up");

        bool all_present = true;
        for (uint32_t i = 0; i < MAX_PAGES - 1; i++)
            if (!exists(handle, i))
                all_present = false;
        tstcheck(all_present, "no eviction during fill-up");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("steady state: invariant holds (used == max_pages - 1) and oldest is evicted in order") {
        test_paths paths;
        test_paths_init(&paths, "fifo_steady");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIFO,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        /* Write exactly max_pages pages. The max_pages-th write transitions the volume
         * to steady state and evicts id 0 (the first written). */
        for (uint32_t i = 0; i < MAX_PAGES; i++)
            tstcheck(put_one(handle, i) == PCACHE_PUT_OK, "steady-state put OK");

        pcache_inspect_page_count_error count_error = (pcache_inspect_page_count_error)-1;
        pcache_page_count               counts      = pcache_inspect_page_count(handle, &count_error, NULL);
        tstcheck(counts.used == MAX_PAGES - 1, "after max_pages writes, used == max_pages - 1");

        tstcheck(!exists(handle, 0), "id 0 (oldest) evicted after writing max_pages pages");
        bool rest_present = true;
        for (uint32_t i = 1; i < MAX_PAGES; i++)
            if (!exists(handle, i))
                rest_present = false;
        tstcheck(rest_present, "ids 1..max_pages-1 still present");

        /* One more write evicts id 1, the now-oldest. */
        tstcheck(put_one(handle, MAX_PAGES) == PCACHE_PUT_OK, "extra put OK on FIFO");
        tstcheck(!exists(handle, 1), "id 1 evicted on next write");
        tstcheck(exists(handle, MAX_PAGES), "newest id is present");

        counts = pcache_inspect_page_count(handle, &count_error, NULL);
        tstcheck(counts.used == MAX_PAGES - 1, "invariant preserved after wrap");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("eviction order is stable across many writes") {
        test_paths paths;
        test_paths_init(&paths, "fifo_many");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIFO,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        /* Write 3 * max_pages pages. Only the most recent max_pages - 1 should remain. */
        const uint32_t total = 3 * MAX_PAGES;
        for (uint32_t i = 0; i < total; i++)
            (void)put_one(handle, i);

        uint32_t live_count = 0;
        for (uint32_t i = 0; i < total; i++) {
            if (exists(handle, i))
                live_count++;
        }
        tstcheck(live_count == MAX_PAGES - 1, "exactly max_pages - 1 ids live");

        /* The MAX_PAGES - 1 most recently inserted ids must be the live ones. */
        bool tail_alive = true;
        for (uint32_t i = total - (MAX_PAGES - 1); i < total; i++)
            if (!exists(handle, i))
                tail_alive = false;
        tstcheck(tail_alive, "tail of insertions is the surviving set");

        bool head_evicted = true;
        for (uint32_t i = 0; i < total - (MAX_PAGES - 1); i++)
            if (exists(handle, i))
                head_evicted = false;
        tstcheck(head_evicted, "older insertions all evicted");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("FIFO survives a roundtrip close/reopen") {
        test_paths paths;
        test_paths_init(&paths, "fifo_roundtrip");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIFO,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        for (uint32_t i = 0; i < MAX_PAGES + 3; i++)
            (void)put_one(handle, i);

        pcache_close(handle, NULL, NULL, NULL);

        pcache_open_error open_error = (pcache_open_error)-1;
        handle                       = pcache_open(&paths.pair, &open_error, NULL, NULL);
        tstcheck(open_error == PCACHE_OPEN_OK, "reopen OK");

        pcache_inspect_page_count_error count_error = (pcache_inspect_page_count_error)-1;
        pcache_page_count               counts      = pcache_inspect_page_count(handle, &count_error, NULL);
        tstcheck(counts.used == MAX_PAGES - 1, "invariant survives reopen");

        /* The first 4 ids should be evicted (4 writes past steady state required so far). */
        bool oldest_gone = !exists(handle, 0) && !exists(handle, 1) && !exists(handle, 2) && !exists(handle, 3);
        tstcheck(oldest_gone, "evictions preserved across reopen");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("FIFO put_pages bigger than capacity ends with the last (max_pages-1) ids alive") {
        test_paths paths;
        test_paths_init(&paths, "fifo_batch");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIFO,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        const size_t  count = 2 * MAX_PAGES;
        unsigned char ids[2 * MAX_PAGES * ID_SIZE];
        unsigned char pages[2 * MAX_PAGES * PAGE_SIZE];
        for (size_t i = 0; i < count; i++) {
            make_id_with_index(ids + i * ID_SIZE, ID_SIZE, (uint32_t)i);
            make_page_with_index(pages + i * PAGE_SIZE, PAGE_SIZE, (uint32_t)i);
        }

        pcache_put_error put_error = (pcache_put_error)-1;
        pcache_put_pages(handle, count, ids, pages, false, true, &put_error, NULL, NULL);
        tstcheck(put_error == PCACHE_PUT_OK, "batch larger than capacity OK on FIFO");

        pcache_inspect_page_count_error count_error = (pcache_inspect_page_count_error)-1;
        pcache_page_count               counts      = pcache_inspect_page_count(handle, &count_error, NULL);
        tstcheck(counts.used == MAX_PAGES - 1, "invariant after batch put");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("FIFO failed batch restores page bytes referenced by the rolled-back index") {
        test_paths paths;
        test_paths_init(&paths, "fifo_batch_rollback");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIFO,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        for (uint32_t i = 0; i < MAX_PAGES; i++)
            tstcheck(put_one(handle, i) == PCACHE_PUT_OK, "initial FIFO put OK");

        unsigned char ids[3 * ID_SIZE];
        unsigned char pages[3 * PAGE_SIZE];
        for (uint32_t i = 0; i < 3; i++) {
            make_id_with_index(ids + i * ID_SIZE, ID_SIZE, 100 + i);
            make_page_with_index(pages + i * PAGE_SIZE, PAGE_SIZE, 100 + i);
        }

        pcache_test_fail_put_pwrite_after(3);
        pcache_put_error put_error = (pcache_put_error)-1;
        pcache_put_pages(handle, 3, ids, pages, false, false, &put_error, NULL, NULL);
        pcache_test_fail_put_pwrite_after(0);
        tstcheck(put_error == PCACHE_PUT_IO_ERROR, "injected third write failure is reported");

        bool old_pages_preserved = true;
        for (uint32_t i = 1; i < MAX_PAGES; i++) {
            unsigned char id[ID_SIZE], expected[PAGE_SIZE], actual[PAGE_SIZE];
            make_id_with_index(id, ID_SIZE, i);
            make_page_with_index(expected, PAGE_SIZE, i);

            pcache_get_error get_error = (pcache_get_error)-1;
            pcache_get_page(handle, id, actual, &get_error, NULL, NULL);
            if (get_error != PCACHE_GET_OK || memcmp(expected, actual, PAGE_SIZE) != 0)
                old_pages_preserved = false;
        }
        tstcheck(old_pages_preserved, "all pre-existing FIFO pages retain their original bytes");

        bool               new_results[3] = {true, true, true};
        pcache_check_error check_error    = (pcache_check_error)-1;
        pcache_check_pages(handle, 3, ids, new_results, &check_error, NULL);
        tstcheck(!new_results[0] && !new_results[1] && !new_results[2], "rolled-back ids are absent");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }
}
