/* Edge-case testing */

#include "common.h"
#include "libpcache.h"
#include "tst.h"

#include <stdint.h>
#include <string.h>

#define ID_SIZE   16
#define PAGE_SIZE 128
#define MAX_PAGES 8

tstsuite("edge cases") {

    tstcase("opening a non-existent volume returns NOT_FOUND") {
        test_paths paths;
        test_paths_init(&paths, "edge_not_found");
        /* Files do not exist (test_paths_init unlinks them). */

        pcache_open_error open_error = (pcache_open_error)-1;
        pcache_handle     handle     = pcache_open(&paths.pair, &open_error, NULL, NULL);
        tstcheck(handle == 0, "handle is zero on failure");
        tstcheck(open_error == PCACHE_OPEN_NOT_FOUND, "open_error == NOT_FOUND");
    }

    tstcase("creating a volume that already exists returns FILE_EXISTS") {
        test_paths paths;
        test_paths_init(&paths, "edge_exists");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_create_error create_error = (pcache_create_error)-1;
        pcache_create(&paths.pair, &config, true, true, &create_error, NULL, NULL);
        tstcheck(create_error == PCACHE_CREATE_OK, "first create OK");

        pcache_create(&paths.pair, &config, true, true, &create_error, NULL, NULL);
        tstcheck(create_error == PCACHE_CREATE_FILE_EXISTS, "second create returns FILE_EXISTS");

        test_paths_cleanup(&paths);
    }

    tstcase("operations on an invalid handle return INVALID_HANDLE") {
        pcache_close_error close_error = (pcache_close_error)-1;
        pcache_close(0, &close_error, NULL, NULL);
        tstcheck(close_error == PCACHE_CLOSE_INVALID_HANDLE, "close(0) -> INVALID_HANDLE");

        pcache_inspect_configuration_error config_error = (pcache_inspect_configuration_error)-1;
        (void)pcache_inspect_configuration(99999, &config_error);
        tstcheck(config_error == PCACHE_INSPECT_CONFIGURATION_INVALID_HANDLE,
                 "inspect_configuration(invalid) -> INVALID_HANDLE");

        pcache_inspect_page_count_error count_error = (pcache_inspect_page_count_error)-1;
        (void)pcache_inspect_page_count(99999, &count_error, NULL);
        tstcheck(count_error == PCACHE_INSPECT_PAGE_COUNT_INVALID_HANDLE,
                 "inspect_page_count(invalid) -> INVALID_HANDLE");
    }

    tstcase("get_page on missing identifier returns NOT_FOUND") {
        test_paths paths;
        test_paths_init(&paths, "edge_get_missing");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        unsigned char id[ID_SIZE], page_out[PAGE_SIZE];
        make_id_with_index(id, ID_SIZE, 0xDEAD);

        pcache_get_error get_error = (pcache_get_error)-1;
        pcache_get_page(handle, id, page_out, &get_error, NULL, NULL);
        tstcheck(get_error == PCACHE_GET_NOT_FOUND, "get on missing id -> NOT_FOUND");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("delete_page on missing identifier is a silent no-op") {
        test_paths paths;
        test_paths_init(&paths, "edge_delete_missing");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        unsigned char id[ID_SIZE];
        make_id_with_index(id, ID_SIZE, 0xBEEF);
        pcache_delete_error delete_error = (pcache_delete_error)-1;
        pcache_delete_page(handle, id, false, true, &delete_error, NULL, NULL);
        tstcheck(delete_error == PCACHE_DELETE_OK, "delete on missing id returns OK");

        pcache_inspect_page_count_error count_error = (pcache_inspect_page_count_error)-1;
        pcache_page_count               counts      = pcache_inspect_page_count(handle, &count_error, NULL);
        tstcheck(counts.used == 0, "no spurious side effects on the volume");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("delete_pages with duplicate identifiers is silently tolerated") {
        test_paths paths;
        test_paths_init(&paths, "edge_delete_dup");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        unsigned char id[ID_SIZE], page[PAGE_SIZE];
        make_id_with_index(id, ID_SIZE, 1);
        make_page_with_index(page, PAGE_SIZE, 1);
        pcache_put_page(handle, id, page, false, true, NULL, NULL, NULL);

        unsigned char ids[2 * ID_SIZE];
        memcpy(ids + 0 * ID_SIZE, id, ID_SIZE);
        memcpy(ids + 1 * ID_SIZE, id, ID_SIZE);

        pcache_delete_error delete_error = (pcache_delete_error)-1;
        pcache_delete_pages(handle, 2, ids, false, true, &delete_error, NULL, NULL);
        tstcheck(delete_error == PCACHE_DELETE_OK, "duplicate ids tolerated, returns OK");

        /* The page is gone after the first occurrence; the second is a no-op. */
        pcache_check_error check_error = (pcache_check_error)-1;
        bool               still_there = pcache_check_page(handle, id, &check_error, NULL);
        tstcheck(!still_there, "page deleted exactly once");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("get_pages_range invalid range and buffer-too-small are reported") {
        test_paths paths;
        test_paths_init(&paths, "edge_range_errors");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        for (uint32_t i = 1; i <= 5; i++) {
            unsigned char id[ID_SIZE], page[PAGE_SIZE];
            make_id_with_index(id, ID_SIZE, i);
            make_page_with_index(page, PAGE_SIZE, i);
            pcache_put_page(handle, id, page, false, true, NULL, NULL, NULL);
        }

        unsigned char first[ID_SIZE], last[ID_SIZE];
        make_id_with_index(first, ID_SIZE, 5); /* note: first > last */
        make_id_with_index(last, ID_SIZE, 1);

        unsigned char    out_ids[5 * ID_SIZE];
        unsigned char    out_pages[5 * PAGE_SIZE];
        uint32_t         got       = 0;
        pcache_get_error get_error = (pcache_get_error)-1;
        pcache_get_pages_range(handle, first, last, out_ids, out_pages, 5, &got, &get_error, NULL, NULL);
        tstcheck(get_error == PCACHE_GET_RANGE_INVALID_RANGE, "first > last -> INVALID_RANGE");

        /* Now a valid range but a buffer that's too small. */
        make_id_with_index(first, ID_SIZE, 1);
        make_id_with_index(last, ID_SIZE, 5);
        pcache_get_pages_range(handle, first, last, out_ids, out_pages, 2, &got, &get_error, NULL, NULL);
        tstcheck(get_error == PCACHE_GET_RANGE_BUFFER_TOO_SMALL, "buffer too small -> BUFFER_TOO_SMALL");

        /* check_pages_range with first > last. */
        make_id_with_index(first, ID_SIZE, 99);
        make_id_with_index(last, ID_SIZE, 0);
        uint32_t           count       = 0;
        pcache_check_error check_error = (pcache_check_error)-1;
        pcache_check_pages_range(handle, first, last, &count, &check_error, NULL);
        tstcheck(check_error == PCACHE_CHECK_RANGE_INVALID_RANGE, "check range first > last -> INVALID_RANGE");

        /* delete_pages_range with first > last. */
        pcache_delete_error delete_error = (pcache_delete_error)-1;
        pcache_delete_pages_range(handle, first, last, false, true, &delete_error, NULL, NULL);
        tstcheck(delete_error == PCACHE_DELETE_INVALID_RANGE, "delete range first > last -> INVALID_RANGE");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("with_counter validation: position out of bounds and counter overflow") {
        test_paths paths;
        test_paths_init(&paths, "edge_counter_validate");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        unsigned char id_base[ID_SIZE];
        memset(id_base, 0, ID_SIZE);

        /* position + 4 must not exceed id_size — id_size = 16, so position <= 12. */
        unsigned char    pages_buffer[1 * PAGE_SIZE];
        pcache_put_error put_error = (pcache_put_error)-1;
        pcache_put_pages_with_counter(handle,
                                      1,
                                      id_base,
                                      0,
                                      /*position*/ 13,
                                      PCACHE_ENDIANNESS_BIG_ENDIAN,
                                      pages_buffer,
                                      false,
                                      true,
                                      &put_error,
                                      NULL,
                                      NULL);
        tstcheck(put_error == PCACHE_PUT_INVALID_ARGUMENT, "position out of bounds -> INVALID_ARGUMENT (put)");

        pcache_get_error get_error = (pcache_get_error)-1;
        pcache_get_pages_with_counter(
            handle, 1, id_base, 0, 13, PCACHE_ENDIANNESS_BIG_ENDIAN, pages_buffer, &get_error, NULL, NULL);
        tstcheck(get_error == PCACHE_GET_INVALID_ARGUMENT, "position out of bounds -> INVALID_ARGUMENT (get)");

        bool               results[1];
        pcache_check_error check_error = (pcache_check_error)-1;
        pcache_check_pages_with_counter(
            handle, 1, id_base, 0, 13, PCACHE_ENDIANNESS_BIG_ENDIAN, results, &check_error, NULL);
        tstcheck(check_error == PCACHE_CHECK_INVALID_ARGUMENT, "position out of bounds -> INVALID_ARGUMENT (check)");

        pcache_delete_error delete_error = (pcache_delete_error)-1;
        pcache_delete_pages_with_counter(
            handle, 1, id_base, 0, 13, PCACHE_ENDIANNESS_BIG_ENDIAN, false, true, &delete_error, NULL, NULL);
        tstcheck(delete_error == PCACHE_DELETE_INVALID_ARGUMENT, "position out of bounds -> INVALID_ARGUMENT (delete)");

        /* Counter overflow: start = UINT32_MAX, count = 2 -> start + count = UINT32_MAX + 2 > UINT32_MAX + 1. */
        pcache_put_pages_with_counter(handle,
                                      2,
                                      id_base,
                                      UINT32_MAX,
                                      0,
                                      PCACHE_ENDIANNESS_BIG_ENDIAN,
                                      pages_buffer,
                                      false,
                                      true,
                                      &put_error,
                                      NULL,
                                      NULL);
        tstcheck(put_error == PCACHE_PUT_INVALID_ARGUMENT, "counter overflow -> INVALID_ARGUMENT");

        /* Invalid endianness value. */
        pcache_put_pages_with_counter(
            handle, 1, id_base, 0, 0, (pcache_endianness)99, pages_buffer, false, true, &put_error, NULL, NULL);
        tstcheck(put_error == PCACHE_PUT_INVALID_ARGUMENT, "invalid endianness -> INVALID_ARGUMENT");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("delete_pages with a mix of present and missing ids deletes only the present ones") {
        test_paths paths;
        test_paths_init(&paths, "edge_delete_partial");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        for (uint32_t i = 1; i <= 3; i++) {
            unsigned char id[ID_SIZE], page[PAGE_SIZE];
            make_id_with_index(id, ID_SIZE, i);
            make_page_with_index(page, PAGE_SIZE, i);
            pcache_put_page(handle, id, page, false, true, NULL, NULL, NULL);
        }

        unsigned char ids[3 * ID_SIZE];
        make_id_with_index(ids + 0 * ID_SIZE, ID_SIZE, 1);
        make_id_with_index(ids + 1 * ID_SIZE, ID_SIZE, 999); /* missing */
        make_id_with_index(ids + 2 * ID_SIZE, ID_SIZE, 2);

        pcache_delete_error delete_error = (pcache_delete_error)-1;
        pcache_delete_pages(handle, 3, ids, false, true, &delete_error, NULL, NULL);
        tstcheck(delete_error == PCACHE_DELETE_OK, "missing id is silently skipped, returns OK");

        unsigned char id1[ID_SIZE], id2[ID_SIZE], id3[ID_SIZE];
        make_id_with_index(id1, ID_SIZE, 1);
        make_id_with_index(id2, ID_SIZE, 2);
        make_id_with_index(id3, ID_SIZE, 3);
        pcache_check_error check_error = (pcache_check_error)-1;
        bool               gone1       = !pcache_check_page(handle, id1, &check_error, NULL);
        bool               gone2       = !pcache_check_page(handle, id2, &check_error, NULL);
        bool               kept3       = pcache_check_page(handle, id3, &check_error, NULL);
        tstcheck(gone1 && gone2, "the present ids are deleted");
        tstcheck(kept3, "untouched id remains");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("handle table grows beyond initial segment: all volumes remain functional") {
        /* Open 20 volumes — more than the initial segment capacity (16).
         * Verifies that the segmented table grows correctly and that handles
         * computed after growth remain correct. */
        const int         count = 20;
        test_paths        all_paths[20];
        pcache_handle     handles[20];
        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };

        for (int i = 0; i < count; i++) {
            char prefix[32];
            snprintf(prefix, sizeof prefix, "edge_seg_%d", i);
            test_paths_init(&all_paths[i], prefix);
            handles[i] = make_volume_and_open(&all_paths[i], &config);
            tstcheck(handles[i] != 0, "volume opens after segment boundary");
        }

        /* Each volume must accept a put and return the same data on get. */
        bool all_ok = true;
        for (int i = 0; i < count; i++) {
            unsigned char id[ID_SIZE], page_out[PAGE_SIZE];
            make_id_with_index(id, ID_SIZE, (uint32_t)i);
            make_page_with_index(page_out, PAGE_SIZE, (uint32_t)i);
            pcache_put_error put_error = (pcache_put_error)-1;
            pcache_put_page(handles[i], id, page_out, false, true, &put_error, NULL, NULL);
            if (put_error != PCACHE_PUT_OK)
                all_ok = false;
        }
        tstcheck(all_ok, "put succeeds on every volume after table growth");

        bool all_readable = true;
        for (int i = 0; i < count; i++) {
            unsigned char id[ID_SIZE], expected[PAGE_SIZE], actual[PAGE_SIZE];
            make_id_with_index(id, ID_SIZE, (uint32_t)i);
            make_page_with_index(expected, PAGE_SIZE, (uint32_t)i);
            pcache_get_error get_error = (pcache_get_error)-1;
            pcache_get_page(handles[i], id, actual, &get_error, NULL, NULL);
            if (get_error != PCACHE_GET_OK || memcmp(expected, actual, PAGE_SIZE) != 0)
                all_readable = false;
        }
        tstcheck(all_readable, "data round-trips correctly on every volume after table growth");

        for (int i = 0; i < count; i++) {
            pcache_close(handles[i], NULL, NULL, NULL);
            test_paths_cleanup(&all_paths[i]);
        }
    }

    tstcase("handle table: slot recycling works after table growth") {
        /* Open 20 volumes, close the first 4, then reopen 4 new volumes.
         * The recycled slots must get valid handles and function correctly. */
        const int         total = 20;
        test_paths        all_paths[20];
        pcache_handle     handles[20];
        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };

        for (int i = 0; i < total; i++) {
            char prefix[32];
            snprintf(prefix, sizeof prefix, "edge_recycle_%d", i);
            test_paths_init(&all_paths[i], prefix);
            handles[i] = make_volume_and_open(&all_paths[i], &config);
        }

        /* Close and clean up the first 4 volumes to free their slots. */
        for (int i = 0; i < 4; i++) {
            pcache_close(handles[i], NULL, NULL, NULL);
            test_paths_cleanup(&all_paths[i]);
        }

        /* Reopen 4 new volumes — they should recycle the freed slots. */
        test_paths  new_paths[4];
        pcache_handle new_handles[4];
        bool all_recycled = true;
        for (int i = 0; i < 4; i++) {
            char prefix[32];
            snprintf(prefix, sizeof prefix, "edge_recycle_new_%d", i);
            test_paths_init(&new_paths[i], prefix);
            new_handles[i] = make_volume_and_open(&new_paths[i], &config);
            if (new_handles[i] == 0)
                all_recycled = false;
        }
        tstcheck(all_recycled, "recycled slots produce valid handles");

        bool all_functional = true;
        for (int i = 0; i < 4; i++) {
            unsigned char id[ID_SIZE], page[PAGE_SIZE];
            make_id_with_index(id, ID_SIZE, (uint32_t)i);
            make_page_with_index(page, PAGE_SIZE, (uint32_t)i);
            pcache_put_error put_error = (pcache_put_error)-1;
            pcache_put_page(new_handles[i], id, page, false, true, &put_error, NULL, NULL);
            if (put_error != PCACHE_PUT_OK)
                all_functional = false;
        }
        tstcheck(all_functional, "put succeeds on recycled-slot volumes");

        for (int i = 0; i < 4; i++) {
            pcache_close(new_handles[i], NULL, NULL, NULL);
            test_paths_cleanup(&new_paths[i]);
        }
        for (int i = 4; i < total; i++) {
            pcache_close(handles[i], NULL, NULL, NULL);
            test_paths_cleanup(&all_paths[i]);
        }
    }
}
