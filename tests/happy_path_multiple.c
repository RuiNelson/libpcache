/* Happy Path with multiple pages being added at the same call */

#include "common.h"
#include "libpcache.h"
#include "tst.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ID_SIZE   16
#define PAGE_SIZE 128
#define BATCH     8
#define MAX_PAGES 64

static void fill_id_and_page_batches(unsigned char *ids, unsigned char *pages, uint32_t first_index, size_t count) {
    for (size_t i = 0; i < count; i++) {
        make_id_with_index(ids + i * ID_SIZE, ID_SIZE, first_index + (uint32_t)i);
        make_page_with_index(pages + i * PAGE_SIZE, PAGE_SIZE, first_index + (uint32_t)i);
    }
}

tstsuite("happy path - multiple page operations") {

    tstcase("put_pages / get_pages / check_pages / delete_pages roundtrip") {
        test_paths paths;
        test_paths_init(&paths, "multi_roundtrip");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        unsigned char ids[BATCH * ID_SIZE];
        unsigned char pages_in[BATCH * PAGE_SIZE];
        unsigned char pages_out[BATCH * PAGE_SIZE];
        fill_id_and_page_batches(ids, pages_in, 100, BATCH);

        pcache_put_error put_error = (pcache_put_error)-1;
        pcache_put_pages(handle, BATCH, ids, pages_in, false, true, &put_error, NULL, NULL);
        tstcheck(put_error == PCACHE_PUT_OK, "put_pages OK");

        bool               results[BATCH];
        pcache_check_error check_error = (pcache_check_error)-1;
        pcache_check_pages(handle, BATCH, ids, results, &check_error, NULL);
        tstcheck(check_error == PCACHE_CHECK_OK, "check_pages OK");
        bool all_present = true;
        for (size_t i = 0; i < BATCH; i++)
            if (!results[i])
                all_present = false;
        tstcheck(all_present, "every stored id reports as present");

        pcache_get_error get_error = (pcache_get_error)-1;
        pcache_get_pages(handle, BATCH, ids, pages_out, &get_error, NULL, NULL);
        tstcheck(get_error == PCACHE_GET_OK, "get_pages OK");
        tstcheck(memcmp(pages_in, pages_out, sizeof(pages_in)) == 0, "all page contents roundtrip");

        pcache_delete_error delete_error = (pcache_delete_error)-1;
        pcache_delete_pages(handle, BATCH, ids, false, true, &delete_error, NULL, NULL);
        tstcheck(delete_error == PCACHE_DELETE_OK, "delete_pages OK");

        pcache_check_pages(handle, BATCH, ids, results, &check_error, NULL);
        bool none_present = true;
        for (size_t i = 0; i < BATCH; i++)
            if (results[i])
                none_present = false;
        tstcheck(none_present, "after delete_pages no ids remain");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("put_pages atomicity: duplicate detection rolls back the whole batch") {
        test_paths paths;
        test_paths_init(&paths, "multi_atomicity");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        /* First batch occupies indices 0..3. */
        unsigned char ids_first[4 * ID_SIZE];
        unsigned char pages_first[4 * PAGE_SIZE];
        fill_id_and_page_batches(ids_first, pages_first, 0, 4);

        pcache_put_error put_error = (pcache_put_error)-1;
        pcache_put_pages(handle, 4, ids_first, pages_first, false, true, &put_error, NULL, NULL);
        tstcheck(put_error == PCACHE_PUT_OK, "first batch OK");

        /* Second batch overlaps id 3, with three new ids 10..12. */
        unsigned char ids_second[4 * ID_SIZE];
        unsigned char pages_second[4 * PAGE_SIZE];
        make_id_with_index(ids_second + 0 * ID_SIZE, ID_SIZE, 10);
        make_id_with_index(ids_second + 1 * ID_SIZE, ID_SIZE, 11);
        make_id_with_index(ids_second + 2 * ID_SIZE, ID_SIZE, 3); /* duplicate of first batch */
        make_id_with_index(ids_second + 3 * ID_SIZE, ID_SIZE, 12);
        for (int i = 0; i < 4; i++)
            make_page_with_index(pages_second + i * PAGE_SIZE, PAGE_SIZE, 1000 + i);

        pcache_put_pages(handle, 4, ids_second, pages_second, true, true, &put_error, NULL, NULL);
        tstcheck(put_error == PCACHE_PUT_DUPLICATE_ID, "second batch fails on duplicate");

        /* None of the new ids 10, 11, 12 should be present after the rollback. */
        bool               results[3];
        unsigned char      probe_ids[3 * ID_SIZE];
        make_id_with_index(probe_ids + 0 * ID_SIZE, ID_SIZE, 10);
        make_id_with_index(probe_ids + 1 * ID_SIZE, ID_SIZE, 11);
        make_id_with_index(probe_ids + 2 * ID_SIZE, ID_SIZE, 12);

        pcache_check_error check_error = (pcache_check_error)-1;
        pcache_check_pages(handle, 3, probe_ids, results, &check_error, NULL);
        tstcheck(!results[0] && !results[1] && !results[2], "atomic rollback: no new ids stored");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("put_pages_with_counter / get_pages_with_counter roundtrip") {
        test_paths paths;
        test_paths_init(&paths, "multi_counter");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        unsigned char id_base[ID_SIZE];
        memset(id_base, 0, ID_SIZE);
        id_base[0] = 0xAB; /* template prefix */

        unsigned char pages_in[BATCH * PAGE_SIZE];
        unsigned char pages_out[BATCH * PAGE_SIZE];
        for (size_t i = 0; i < BATCH; i++)
            make_page_with_index(pages_in + i * PAGE_SIZE, PAGE_SIZE, 7000 + (uint32_t)i);

        pcache_put_error put_error = (pcache_put_error)-1;
        pcache_put_pages_with_counter(handle, BATCH, id_base, /*start*/ 1000, /*position*/ 0,
                                      PCACHE_ENDIANNESS_BIG_ENDIAN, pages_in, false, true, &put_error, NULL, NULL);
        tstcheck(put_error == PCACHE_PUT_OK, "put_pages_with_counter OK");

        pcache_get_error get_error = (pcache_get_error)-1;
        pcache_get_pages_with_counter(handle, BATCH, id_base, 1000, 0, PCACHE_ENDIANNESS_BIG_ENDIAN, pages_out,
                                      &get_error, NULL, NULL);
        tstcheck(get_error == PCACHE_GET_OK, "get_pages_with_counter OK");
        tstcheck(memcmp(pages_in, pages_out, sizeof(pages_in)) == 0, "counter-derived contents match");

        bool               results[BATCH];
        pcache_check_error check_error = (pcache_check_error)-1;
        pcache_check_pages_with_counter(handle, BATCH, id_base, 1000, 0, PCACHE_ENDIANNESS_BIG_ENDIAN, results,
                                        &check_error, NULL);
        tstcheck(check_error == PCACHE_CHECK_OK, "check_pages_with_counter OK");
        bool all_present = true;
        for (size_t i = 0; i < BATCH; i++)
            if (!results[i])
                all_present = false;
        tstcheck(all_present, "all counter-derived ids present");

        /* Out-of-range counter values should not be present. */
        pcache_check_pages_with_counter(handle, 1, id_base, 999, 0, PCACHE_ENDIANNESS_BIG_ENDIAN, results, &check_error,
                                        NULL);
        tstcheck(!results[0], "counter value before range absent");

        pcache_delete_error delete_error = (pcache_delete_error)-1;
        pcache_delete_pages_with_counter(handle, BATCH, id_base, 1000, 0, PCACHE_ENDIANNESS_BIG_ENDIAN, false, true,
                                         &delete_error, NULL, NULL);
        tstcheck(delete_error == PCACHE_DELETE_OK, "delete_pages_with_counter OK");

        pcache_check_pages_with_counter(handle, BATCH, id_base, 1000, 0, PCACHE_ENDIANNESS_BIG_ENDIAN, results,
                                        &check_error, NULL);
        bool none_present = true;
        for (size_t i = 0; i < BATCH; i++)
            if (results[i])
                none_present = false;
        tstcheck(none_present, "after counter delete no ids remain");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("get_pages_range returns matches in ascending id order") {
        test_paths paths;
        test_paths_init(&paths, "multi_range");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        /* Indices 100..120 produce ids ordered numerically because of make_id_with_index. */
        unsigned char ids[21 * ID_SIZE];
        unsigned char pages[21 * PAGE_SIZE];
        fill_id_and_page_batches(ids, pages, 100, 21);

        pcache_put_error put_error = (pcache_put_error)-1;
        pcache_put_pages(handle, 21, ids, pages, false, true, &put_error, NULL, NULL);
        tstcheck(put_error == PCACHE_PUT_OK, "21 pages stored");

        unsigned char first[ID_SIZE], last[ID_SIZE];
        make_id_with_index(first, ID_SIZE, 105);
        make_id_with_index(last, ID_SIZE, 110);

        unsigned char    range_ids[21 * ID_SIZE];
        unsigned char    range_pages[21 * PAGE_SIZE];
        uint32_t         count_out  = 0;
        pcache_get_error get_error  = (pcache_get_error)-1;
        pcache_get_pages_range(handle, first, last, range_ids, range_pages, 21, &count_out, &get_error, NULL, NULL);
        tstcheck(get_error == PCACHE_GET_OK, "range get OK");
        tstcheck(count_out == 6, "range [105, 110] inclusive yields 6 results");

        bool ascending = true;
        for (uint32_t i = 1; i < count_out; i++) {
            if (memcmp(range_ids + (i - 1) * ID_SIZE, range_ids + i * ID_SIZE, ID_SIZE) >= 0) {
                ascending = false;
                break;
            }
        }
        tstcheck(ascending, "range results sorted ascending by id");

        /* Verify the page contents correspond to indices 105..110. */
        bool contents_match = true;
        for (uint32_t i = 0; i < count_out; i++) {
            unsigned char expected[PAGE_SIZE];
            make_page_with_index(expected, PAGE_SIZE, 105 + i);
            if (memcmp(range_pages + i * PAGE_SIZE, expected, PAGE_SIZE) != 0) {
                contents_match = false;
                break;
            }
        }
        tstcheck(contents_match, "range page contents match expected");

        /* check_pages_range should return the same count without touching the data file. */
        uint32_t           check_count = 0;
        pcache_check_error check_error = (pcache_check_error)-1;
        pcache_check_pages_range(handle, first, last, &check_count, &check_error, NULL);
        tstcheck(check_error == PCACHE_CHECK_OK, "check_pages_range OK");
        tstcheck(check_count == 6, "check_pages_range counts the same 6 pages");

        /* delete_pages_range removes the same matches. */
        pcache_delete_error delete_error = (pcache_delete_error)-1;
        pcache_delete_pages_range(handle, first, last, false, true, &delete_error, NULL, NULL);
        tstcheck(delete_error == PCACHE_DELETE_OK, "delete_pages_range OK");

        pcache_check_pages_range(handle, first, last, &check_count, &check_error, NULL);
        tstcheck(check_count == 0, "after delete the range is empty");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("get_pages_range with an empty match is not an error") {
        test_paths paths;
        test_paths_init(&paths, "multi_range_empty");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        unsigned char first[ID_SIZE], last[ID_SIZE];
        make_id_with_index(first, ID_SIZE, 50);
        make_id_with_index(last, ID_SIZE, 60);

        unsigned char    range_ids[1 * ID_SIZE];
        unsigned char    range_pages[1 * PAGE_SIZE];
        uint32_t         count_out = 999;
        pcache_get_error get_error = (pcache_get_error)-1;
        pcache_get_pages_range(handle, first, last, range_ids, range_pages, 1, &count_out, &get_error, NULL, NULL);
        tstcheck(get_error == PCACHE_GET_OK, "empty range get OK");
        tstcheck(count_out == 0, "empty range yields zero matches");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }
}
