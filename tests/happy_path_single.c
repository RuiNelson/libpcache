/* Happy Path with a single page being added per call */

#include "common.h"
#include "libpcache.h"
#include "tst.h"

#include <string.h>

#define ID_SIZE   16
#define PAGE_SIZE 256
#define MAX_PAGES 16

tstsuite("happy path - single page operations") {

    tstcase("FIXED: put / get / check / delete a single page") {
        test_paths paths;
        test_paths_init(&paths, "single_fixed");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);
        tstcheck(handle > 0, "volume opened");

        unsigned char id[ID_SIZE];
        unsigned char page_in[PAGE_SIZE];
        unsigned char page_out[PAGE_SIZE];
        make_id_with_index(id, ID_SIZE, 1);
        make_page_with_index(page_in, PAGE_SIZE, 1);
        memset(page_out, 0, PAGE_SIZE);

        pcache_put_error put_error = (pcache_put_error)-1;
        pcache_put_page(handle, id, page_in, false, true, &put_error, NULL, NULL);
        tstcheck(put_error == PCACHE_PUT_OK, "put returns OK");

        pcache_check_error check_error = (pcache_check_error)-1;
        bool               exists      = pcache_check_page(handle, id, &check_error, NULL);
        tstcheck(check_error == PCACHE_CHECK_OK, "check returns OK");
        tstcheck(exists, "stored page exists");

        pcache_get_error get_error = (pcache_get_error)-1;
        pcache_get_page(handle, id, page_out, &get_error, NULL, NULL);
        tstcheck(get_error == PCACHE_GET_OK, "get returns OK");
        tstcheck(memcmp(page_in, page_out, PAGE_SIZE) == 0, "page content roundtrip matches");

        pcache_inspect_page_count_error count_error = (pcache_inspect_page_count_error)-1;
        pcache_page_count               counts      = pcache_inspect_page_count(handle, &count_error, NULL);
        tstcheck(count_error == PCACHE_INSPECT_PAGE_COUNT_OK, "inspect_page_count OK");
        tstcheck(counts.used == 1, "used == 1");
        tstcheck(counts.free == MAX_PAGES - 1, "free == max_pages - 1");

        pcache_delete_error delete_error = (pcache_delete_error)-1;
        pcache_delete_page(handle, id, false, true, &delete_error, NULL, NULL);
        tstcheck(delete_error == PCACHE_DELETE_OK, "delete returns OK");

        exists = pcache_check_page(handle, id, &check_error, NULL);
        tstcheck(!exists, "deleted page no longer exists");

        pcache_get_page(handle, id, page_out, &get_error, NULL, NULL);
        tstcheck(get_error == PCACHE_GET_NOT_FOUND, "get after delete returns NOT_FOUND");

        counts = pcache_inspect_page_count(handle, &count_error, NULL);
        tstcheck(counts.used == 0, "after delete used == 0");

        pcache_close_error close_error = (pcache_close_error)-1;
        pcache_close(handle, &close_error, NULL, NULL);
        tstcheck(close_error == PCACHE_CLOSE_OK, "close OK");

        test_paths_cleanup(&paths);
    }

    tstcase("FIFO: put / get / check / delete a single page") {
        test_paths paths;
        test_paths_init(&paths, "single_fifo");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIFO,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);
        tstcheck(handle > 0, "FIFO volume opened");

        unsigned char id[ID_SIZE];
        unsigned char page_in[PAGE_SIZE];
        unsigned char page_out[PAGE_SIZE];
        make_id_with_index(id, ID_SIZE, 7);
        make_page_with_index(page_in, PAGE_SIZE, 7);

        pcache_put_error put_error = (pcache_put_error)-1;
        pcache_put_page(handle, id, page_in, false, true, &put_error, NULL, NULL);
        tstcheck(put_error == PCACHE_PUT_OK, "FIFO put OK");

        pcache_get_error get_error = (pcache_get_error)-1;
        pcache_get_page(handle, id, page_out, &get_error, NULL, NULL);
        tstcheck(get_error == PCACHE_GET_OK, "FIFO get OK");
        tstcheck(memcmp(page_in, page_out, PAGE_SIZE) == 0, "FIFO content matches");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("fail_if_exists detects duplicate identifier") {
        test_paths paths;
        test_paths_init(&paths, "fail_if_exists");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = 4,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        unsigned char id[ID_SIZE], page[PAGE_SIZE];
        make_id_with_index(id, ID_SIZE, 42);
        make_page_with_index(page, PAGE_SIZE, 42);

        pcache_put_error put_error = (pcache_put_error)-1;
        pcache_put_page(handle, id, page, false, true, &put_error, NULL, NULL);
        tstcheck(put_error == PCACHE_PUT_OK, "first put OK");

        pcache_put_page(handle, id, page, true, true, &put_error, NULL, NULL);
        tstcheck(put_error == PCACHE_PUT_DUPLICATE_ID, "duplicate detected with fail_if_exists=true");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("durable=false still makes data observable") {
        test_paths paths;
        test_paths_init(&paths, "non_durable");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = 4,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        unsigned char id[ID_SIZE], page_in[PAGE_SIZE], page_out[PAGE_SIZE];
        make_id_with_index(id, ID_SIZE, 9);
        make_page_with_index(page_in, PAGE_SIZE, 9);

        pcache_put_error put_error = (pcache_put_error)-1;
        pcache_put_page(handle, id, page_in, false, false, &put_error, NULL, NULL);
        tstcheck(put_error == PCACHE_PUT_OK, "non-durable put OK");

        pcache_get_error get_error = (pcache_get_error)-1;
        pcache_get_page(handle, id, page_out, &get_error, NULL, NULL);
        tstcheck(get_error == PCACHE_GET_OK, "non-durable get OK");
        tstcheck(memcmp(page_in, page_out, PAGE_SIZE) == 0, "non-durable content matches");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("configuration is recoverable from a reopened volume") {
        test_paths paths;
        test_paths_init(&paths, "reopen_config");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIFO,
            .page_size       = 512,
            .max_pages       = 11,
            .id_size         = 24,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);
        pcache_close(handle, NULL, NULL, NULL);

        pcache_open_error open_error = (pcache_open_error)-1;
        handle                       = pcache_open(&paths.pair, &open_error, NULL, NULL);
        tstcheck(open_error == PCACHE_OPEN_OK, "reopen OK");

        pcache_inspect_configuration_error config_error = (pcache_inspect_configuration_error)-1;
        pcache_configuration               recovered    = pcache_inspect_configuration(handle, &config_error);
        tstcheck(config_error == PCACHE_INSPECT_CONFIGURATION_OK, "inspect_configuration OK");
        tstcheck(recovered.capacity_policy == PCACHE_CAPACITY_FIFO, "policy preserved");
        tstcheck(recovered.page_size == 512, "page_size preserved");
        tstcheck(recovered.max_pages == 11, "max_pages preserved");
        tstcheck(recovered.id_size == 24, "id_size preserved");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }
}
