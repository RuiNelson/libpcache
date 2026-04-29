/* Grow/Shrink volumes */

#include "common.h"
#include "libpcache.h"
#include "tst.h"

#include <stdint.h>
#include <string.h>

#define ID_SIZE   16
#define PAGE_SIZE 256

static void put_indexed(pcache_handle handle, uint32_t index) {
    unsigned char id[ID_SIZE], page[PAGE_SIZE];
    make_id_with_index(id, ID_SIZE, index);
    make_page_with_index(page, PAGE_SIZE, index);
    pcache_put_page(handle, id, page, false, true, NULL, NULL, NULL);
}

static bool exists_indexed(pcache_handle handle, uint32_t index) {
    unsigned char      id[ID_SIZE];
    pcache_check_error check_error = (pcache_check_error)-1;
    make_id_with_index(id, ID_SIZE, index);
    return pcache_check_page(handle, id, &check_error, NULL);
}

tstsuite("elasticity - grow and shrink") {

    tstcase("FIXED grow: more puts succeed after capacity increase") {
        test_paths paths;
        test_paths_init(&paths, "elastic_fixed_grow");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = 4,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        for (uint32_t i = 0; i < 4; i++)
            put_indexed(handle, i);

        pcache_put_error put_error = (pcache_put_error)-1;
        unsigned char    id[ID_SIZE], page[PAGE_SIZE];
        make_id_with_index(id, ID_SIZE, 100);
        make_page_with_index(page, PAGE_SIZE, 100);
        pcache_put_page(handle, id, page, false, true, &put_error, NULL, NULL);
        tstcheck(put_error == PCACHE_PUT_CAPACITY_EXCEEDED, "full FIXED rejects new put");

        pcache_set_max_pages_error grow_error = (pcache_set_max_pages_error)-1;
        pcache_set_max_pages(handle, 8, true, &grow_error, NULL, NULL);
        tstcheck(grow_error == PCACHE_SET_MAX_PAGES_OK, "grow OK");

        pcache_put_page(handle, id, page, false, true, &put_error, NULL, NULL);
        tstcheck(put_error == PCACHE_PUT_OK, "post-grow put OK");

        pcache_inspect_configuration_error config_error = (pcache_inspect_configuration_error)-1;
        pcache_configuration           cfg          = pcache_inspect_configuration(handle, &config_error);
        tstcheck(cfg.max_pages == 8, "max_pages reflects new value");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("FIXED shrink with no live pages beyond cutoff: succeeds") {
        test_paths paths;
        test_paths_init(&paths, "elastic_fixed_shrink_ok");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = 8,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        /* Live data only in slots 1..3 — shrink to 4 should be OK. */
        for (uint32_t i = 0; i < 3; i++)
            put_indexed(handle, i);

        pcache_set_max_pages_error shrink_error = (pcache_set_max_pages_error)-1;
        pcache_set_max_pages(handle, 4, true, &shrink_error, NULL, NULL);
        tstcheck(shrink_error == PCACHE_SET_MAX_PAGES_OK, "FIXED shrink OK when live pages fit");

        for (uint32_t i = 0; i < 3; i++)
            tstcheck(exists_indexed(handle, i), "live page survives FIXED shrink");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("FIXED shrink that would discard live pages fails") {
        test_paths paths;
        test_paths_init(&paths, "elastic_fixed_shrink_discard");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = 8,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        /* Fill all 8 slots so a shrink to 4 would discard live pages at slots 5..8. */
        for (uint32_t i = 0; i < 8; i++)
            put_indexed(handle, i);

        pcache_set_max_pages_error shrink_error = (pcache_set_max_pages_error)-1;
        pcache_set_max_pages(handle, 4, true, &shrink_error, NULL, NULL);
        tstcheck(shrink_error == PCACHE_SET_MAX_PAGES_WOULD_DISCARD_PAGES, "FIXED shrink rejects when discard required");

        /* All 8 ids must still be present. */
        bool all_present = true;
        for (uint32_t i = 0; i < 8; i++)
            if (!exists_indexed(handle, i))
                all_present = false;
        tstcheck(all_present, "no pages lost after rejected shrink");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("FIFO shrink physically drops rows beyond new max") {
        test_paths paths;
        test_paths_init(&paths, "elastic_fifo_shrink");

        const uint32_t       initial_max = 16;
        pcache_configuration config      = {
                 .capacity_policy = PCACHE_CAPACITY_FIFO,
                 .page_size       = PAGE_SIZE,
                 .max_pages       = initial_max,
                 .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        /* Fill-up phase: 15 pages live, slot 16 still empty. */
        for (uint32_t i = 0; i < initial_max - 1; i++)
            put_indexed(handle, i);

        pcache_set_max_pages_error shrink_error = (pcache_set_max_pages_error)-1;
        pcache_set_max_pages(handle, 8, true, &shrink_error, NULL, NULL);
        tstcheck(shrink_error == PCACHE_SET_MAX_PAGES_OK, "FIFO shrink OK");

        /* The data file must shrink accordingly. */
        off_t expected_size = (off_t)8 * PAGE_SIZE;
        tstcheck(file_size(paths.data_path) == expected_size, "FIFO data file truncated to new size");

        pcache_inspect_page_count_error count_error = (pcache_inspect_page_count_error)-1;
        pcache_page_count           counts      = pcache_inspect_page_count(handle, &count_error, NULL);
        tstcheck(counts.used <= 8, "no more than new max_pages live");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("preallocate after grow extends the data file") {
        test_paths paths;
        test_paths_init(&paths, "elastic_prealloc");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = 4,
            .id_size         = ID_SIZE,
        };

        /* Create without preallocation. */
        pcache_create_error create_error = (pcache_create_error)-1;
        pcache_create(&paths.pair, &config, false, false, &create_error, NULL, NULL);
        tstcheck(create_error == PCACHE_CREATE_OK, "create without preallocate OK");

        pcache_open_error open_error = (pcache_open_error)-1;
        pcache_handle     handle     = pcache_open(&paths.pair, &open_error, NULL, NULL);
        tstcheck(open_error == PCACHE_OPEN_OK, "open OK");

        off_t initial_size = file_size(paths.data_path);
        tstcheck(initial_size >= 0, "data file exists");

        pcache_preallocate_error prealloc_error = (pcache_preallocate_error)-1;
        pcache_preallocate(handle, true, true, true, &prealloc_error, NULL, NULL);
        tstcheck(prealloc_error == PCACHE_PREALLOCATE_OK, "preallocate OK");

        off_t after_prealloc = file_size(paths.data_path);
        tstcheck(after_prealloc >= (off_t)4 * PAGE_SIZE, "data file at least max_pages * page_size after preallocate");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }
}
