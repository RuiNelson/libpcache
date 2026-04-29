/* Tests related to the FIXED Policy */

#include "common.h"
#include "libpcache.h"
#include "tst.h"

#include <stdint.h>
#include <string.h>

#define ID_SIZE   16
#define PAGE_SIZE 128
#define MAX_PAGES 8

static pcache_put_error put_one(pcache_handle handle, uint32_t index) {
    unsigned char id[ID_SIZE], page[PAGE_SIZE];
    make_id_with_index(id, ID_SIZE, index);
    make_page_with_index(page, PAGE_SIZE, index);
    pcache_put_error put_error = (pcache_put_error)-1;
    pcache_put_page(handle, id, page, false, true, &put_error, NULL, NULL);
    return put_error;
}

tstsuite("FIXED policy") {

    tstcase("filling exactly to capacity succeeds; one extra fails with CAPACITY_EXCEEDED") {
        test_paths paths;
        test_paths_init(&paths, "fixed_capacity");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        bool fill_ok = true;
        for (uint32_t i = 0; i < MAX_PAGES; i++) {
            if (put_one(handle, i) != PCACHE_PUT_OK) {
                fill_ok = false;
                break;
            }
        }
        tstcheck(fill_ok, "all max_pages writes succeed");

        pcache_inspect_page_count_error count_error = (pcache_inspect_page_count_error)-1;
        pcache_page_count           counts      = pcache_inspect_page_count(handle, &count_error, NULL);
        tstcheck(counts.used == MAX_PAGES, "used == max_pages after fill");
        tstcheck(counts.free == 0, "free == 0 after fill");

        tstcheck(put_one(handle, 999) == PCACHE_PUT_CAPACITY_EXCEEDED, "extra put fails with CAPACITY_EXCEEDED");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("delete frees a slot that the next put reuses (lowest ROWID first)") {
        test_paths paths;
        test_paths_init(&paths, "fixed_recycle");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        for (uint32_t i = 0; i < MAX_PAGES; i++)
            (void)put_one(handle, i);

        /* Delete two pages, leaving two free slots. */
        unsigned char       id3[ID_SIZE], id5[ID_SIZE];
        make_id_with_index(id3, ID_SIZE, 3);
        make_id_with_index(id5, ID_SIZE, 5);
        pcache_delete_error delete_error = (pcache_delete_error)-1;
        pcache_delete_page(handle, id3, false, true, &delete_error, NULL, NULL);
        tstcheck(delete_error == PCACHE_DELETE_OK, "delete id 3 OK");
        pcache_delete_page(handle, id5, false, true, &delete_error, NULL, NULL);
        tstcheck(delete_error == PCACHE_DELETE_OK, "delete id 5 OK");

        pcache_inspect_page_count_error count_error = (pcache_inspect_page_count_error)-1;
        pcache_page_count           counts      = pcache_inspect_page_count(handle, &count_error, NULL);
        tstcheck(counts.used == MAX_PAGES - 2, "used decremented by 2");
        tstcheck(counts.free == 2, "two free slots after delete");

        /* The next two puts must succeed: they reuse the slots vacated by the deletes. */
        tstcheck(put_one(handle, 1000) == PCACHE_PUT_OK, "first reuse put OK");
        tstcheck(put_one(handle, 1001) == PCACHE_PUT_OK, "second reuse put OK");

        /* The volume is now full again. */
        tstcheck(put_one(handle, 1002) == PCACHE_PUT_CAPACITY_EXCEEDED, "next put fails as full");

        /* Both reused ids must be retrievable with their original content. */
        unsigned char id1000[ID_SIZE], page_out[PAGE_SIZE], expected[PAGE_SIZE];
        make_id_with_index(id1000, ID_SIZE, 1000);
        make_page_with_index(expected, PAGE_SIZE, 1000);
        pcache_get_error get_error = (pcache_get_error)-1;
        pcache_get_page(handle, id1000, page_out, &get_error, NULL, NULL);
        tstcheck(get_error == PCACHE_GET_OK, "reused id 1000 retrievable");
        tstcheck(memcmp(page_out, expected, PAGE_SIZE) == 0, "reused id 1000 content correct");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("put_pages atomicity on FIXED: capacity overflow leaves volume unchanged") {
        test_paths paths;
        test_paths_init(&paths, "fixed_batch_overflow");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        /* Fill 6 of 8 slots. */
        for (uint32_t i = 0; i < 6; i++)
            (void)put_one(handle, i);

        /* Try to insert 4 more in a single batch — would overflow by 2. */
        unsigned char ids[4 * ID_SIZE];
        unsigned char pages[4 * PAGE_SIZE];
        for (size_t i = 0; i < 4; i++) {
            make_id_with_index(ids + i * ID_SIZE, ID_SIZE, 100 + (uint32_t)i);
            make_page_with_index(pages + i * PAGE_SIZE, PAGE_SIZE, 100 + (uint32_t)i);
        }

        pcache_put_error put_error = (pcache_put_error)-1;
        pcache_put_pages(handle, 4, ids, pages, false, true, &put_error, NULL, NULL);
        tstcheck(put_error == PCACHE_PUT_CAPACITY_EXCEEDED, "batch overflow returns CAPACITY_EXCEEDED");

        /* No new id from the failed batch should have been written. */
        bool               results[4];
        pcache_check_error check_error = (pcache_check_error)-1;
        pcache_check_pages(handle, 4, ids, results, &check_error, NULL);
        bool none_present = true;
        for (size_t i = 0; i < 4; i++)
            if (results[i])
                none_present = false;
        tstcheck(none_present, "none of the would-be new ids exist after rollback");

        pcache_inspect_page_count_error count_error = (pcache_inspect_page_count_error)-1;
        pcache_page_count           counts      = pcache_inspect_page_count(handle, &count_error, NULL);
        tstcheck(counts.used == 6, "used count unchanged after failed batch");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("FIXED never evicts on put") {
        test_paths paths;
        test_paths_init(&paths, "fixed_no_eviction");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        for (uint32_t i = 0; i < MAX_PAGES; i++)
            (void)put_one(handle, i);

        (void)put_one(handle, 12345); /* fails with CAPACITY_EXCEEDED */

        /* All MAX_PAGES original ids must still be present. */
        unsigned char ids[MAX_PAGES * ID_SIZE];
        bool          results[MAX_PAGES];
        for (uint32_t i = 0; i < MAX_PAGES; i++)
            make_id_with_index(ids + i * ID_SIZE, ID_SIZE, i);

        pcache_check_error check_error = (pcache_check_error)-1;
        pcache_check_pages(handle, MAX_PAGES, ids, results, &check_error, NULL);
        bool all_present = true;
        for (size_t i = 0; i < MAX_PAGES; i++)
            if (!results[i])
                all_present = false;
        tstcheck(all_present, "all original ids still present after failed put");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }
}
