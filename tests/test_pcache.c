#include "tst.h"
#include "libpcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ──────────── Utilities ──────────── */

#define TMP_DB   "/tmp/pcache_test.db"
#define TMP_DATA "/tmp/pcache_test.dat"

#define PAGE_SIZE  512u
#define MAX_PAGES  8u
#define ID_SIZE    16u

static void cleanup_files(void)
{
    unlink(TMP_DB);
    unlink(TMP_DATA);
}

static const pcache_file_pair TEST_PATHS = {TMP_DB, TMP_DATA};

static const pcache_configuration FIXED_CFG = {
    .capacity_policy = PCACHE_CAPACITY_FIXED,
    .page_size       = PAGE_SIZE,
    .max_pages       = MAX_PAGES,
    .id_size         = ID_SIZE,
};


/* Fill buf with a repeating pattern derived from seed. */
static void fill_page(uint8_t *buf, uint8_t seed)
{
    for (uint32_t i = 0; i < PAGE_SIZE; i++)
        buf[i] = (uint8_t)(seed + i);
}

/* Fill id with a pattern derived from n. */
static void make_id(uint8_t *id, uint8_t n)
{
    memset(id, n, ID_SIZE);
}

/* ──────────── Test suite ──────────── */

tstsuite("libpcache")
{
    /* ── Create / open / close ── */
    tstcase("create and open FIXED volume")
    {
        cleanup_files();

        pcache_create_error ce = PCACHE_CREATE_OK;
        pcache_create(&TEST_PATHS, &FIXED_CFG, false, false, &ce, NULL, NULL);
        tstcheck(ce == PCACHE_CREATE_OK, "create must succeed");

        pcache_open_error oe = PCACHE_OPEN_OK;
        pcache_handle     h  = pcache_open(&TEST_PATHS, false, &oe, NULL, NULL);
        tstcheck(h > 0, "open must return a valid handle");
        tstcheck(oe == PCACHE_OPEN_OK, "open error must be OK");

        pcache_close_error cc = PCACHE_CLOSE_OK;
        pcache_close(h, &cc, NULL, NULL);
        tstcheck(cc == PCACHE_CLOSE_OK, "close must succeed");

        cleanup_files();
    }

    tstcase("create fails when files already exist")
    {
        cleanup_files();

        pcache_create_error ce = PCACHE_CREATE_OK;
        pcache_create(&TEST_PATHS, &FIXED_CFG, false, false, &ce, NULL, NULL);
        tstcheck(ce == PCACHE_CREATE_OK, "first create must succeed");

        pcache_create(&TEST_PATHS, &FIXED_CFG, false, false, &ce, NULL, NULL);
        tstcheck(ce == PCACHE_CREATE_FILE_EXISTS, "second create must report file-exists");

        cleanup_files();
    }

    tstcase("get_configuration returns stored values")
    {
        cleanup_files();

        pcache_create(&TEST_PATHS, &FIXED_CFG, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&TEST_PATHS, false, NULL, NULL, NULL);

        pcache_get_configuration_error gce = PCACHE_GET_CONFIGURATION_OK;
        pcache_configuration           cfg = pcache_get_configuration(h, &gce, NULL);

        tstcheck(gce == PCACHE_GET_CONFIGURATION_OK, "get_configuration must succeed");
        tstcheck(cfg.page_size == PAGE_SIZE, "page_size must match");
        tstcheck(cfg.max_pages == MAX_PAGES, "max_pages must match");
        tstcheck(cfg.id_size == ID_SIZE, "id_size must match");
        tstcheck(cfg.capacity_policy == PCACHE_CAPACITY_FIXED, "capacity_policy must match");

        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    /* ── put / get / check ── */
    tstcase("put and get a page")
    {
        cleanup_files();

        pcache_create(&TEST_PATHS, &FIXED_CFG, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&TEST_PATHS, false, NULL, NULL, NULL);

        uint8_t id[ID_SIZE], page_out[PAGE_SIZE], page_in[PAGE_SIZE];
        make_id(id, 42);
        fill_page(page_out, 42);

        pcache_put_page_error pe = PCACHE_PUT_PAGE_OK;
        pcache_put_page(h, id, page_out, false, false, &pe, NULL, NULL);
        tstcheck(pe == PCACHE_PUT_PAGE_OK, "put_page must succeed");

        pcache_get_page_error ge = PCACHE_GET_PAGE_OK;
        pcache_get_page(h, id, page_in, &ge, NULL, NULL);
        tstcheck(ge == PCACHE_GET_PAGE_OK, "get_page must succeed");
        tstcheck(memcmp(page_in, page_out, PAGE_SIZE) == 0, "retrieved data must match stored data");

        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    tstcase("check_page returns correct presence")
    {
        cleanup_files();

        pcache_create(&TEST_PATHS, &FIXED_CFG, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&TEST_PATHS, false, NULL, NULL, NULL);

        uint8_t id_a[ID_SIZE], id_b[ID_SIZE], page[PAGE_SIZE];
        make_id(id_a, 1);
        make_id(id_b, 2);
        fill_page(page, 1);

        pcache_put_page(h, id_a, page, false, false, NULL, NULL, NULL);

        pcache_check_page_error ce = PCACHE_CHECK_PAGE_OK;
        tstcheck(pcache_check_page(h, id_a, &ce, NULL) == true, "id_a must be present");
        tstcheck(ce == PCACHE_CHECK_PAGE_OK, "check error must be OK");
        tstcheck(pcache_check_page(h, id_b, &ce, NULL) == false, "id_b must be absent");

        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    tstcase("get_page returns NOT_FOUND for missing id")
    {
        cleanup_files();

        pcache_create(&TEST_PATHS, &FIXED_CFG, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&TEST_PATHS, false, NULL, NULL, NULL);

        uint8_t id[ID_SIZE], buf[PAGE_SIZE];
        make_id(id, 99);

        pcache_get_page_error ge = PCACHE_GET_PAGE_OK;
        pcache_get_page(h, id, buf, &ge, NULL, NULL);
        tstcheck(ge == PCACHE_GET_PAGE_NOT_FOUND, "missing page must give NOT_FOUND");

        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    tstcase("duplicate id check")
    {
        cleanup_files();

        pcache_create(&TEST_PATHS, &FIXED_CFG, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&TEST_PATHS, false, NULL, NULL, NULL);

        uint8_t id[ID_SIZE], page[PAGE_SIZE];
        make_id(id, 7);
        fill_page(page, 7);

        pcache_put_page(h, id, page, false, false, NULL, NULL, NULL);

        pcache_put_page_error pe = PCACHE_PUT_PAGE_OK;
        pcache_put_page(h, id, page, true, false, &pe, NULL, NULL);
        tstcheck(pe == PCACHE_PUT_PAGE_DUPLICATE_ID, "duplicate must be detected");

        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    /* ── delete ── */
    tstcase("delete page removes it from the index")
    {
        cleanup_files();

        pcache_create(&TEST_PATHS, &FIXED_CFG, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&TEST_PATHS, false, NULL, NULL, NULL);

        uint8_t id[ID_SIZE], page[PAGE_SIZE];
        make_id(id, 5);
        fill_page(page, 5);
        pcache_put_page(h, id, page, false, false, NULL, NULL, NULL);

        pcache_delete_page_error de = PCACHE_DELETE_PAGE_OK;
        pcache_delete_page(h, id, false, false, &de, NULL, NULL);
        tstcheck(de == PCACHE_DELETE_PAGE_OK, "delete must succeed");

        tstcheck(pcache_check_page(h, id, NULL, NULL) == false, "page must be absent after delete");

        /* Deleting a non-existent page must report NOT_FOUND. */
        pcache_delete_page(h, id, false, false, &de, NULL, NULL);
        tstcheck(de == PCACHE_DELETE_PAGE_NOT_FOUND, "second delete must report NOT_FOUND");

        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    tstcase("FIXED: deleted slot is reused")
    {
        cleanup_files();

        pcache_create(&TEST_PATHS, &FIXED_CFG, false, false, NULL, NULL, NULL);
        /* Preload so the free list is populated from the start. */
        pcache_handle h = pcache_open(&TEST_PATHS, true, NULL, NULL, NULL);

        uint8_t id_a[ID_SIZE], id_b[ID_SIZE], page[PAGE_SIZE];
        make_id(id_a, 10);
        make_id(id_b, 20);
        fill_page(page, 10);

        pcache_put_page(h, id_a, page, false, false, NULL, NULL, NULL);
        pcache_delete_page(h, id_a, false, false, NULL, NULL, NULL);

        fill_page(page, 20);
        pcache_put_page_error pe = PCACHE_PUT_PAGE_OK;
        pcache_put_page(h, id_b, page, false, false, &pe, NULL, NULL);
        tstcheck(pe == PCACHE_PUT_PAGE_OK, "slot must be reusable after delete");

        uint8_t retrieved[PAGE_SIZE];
        pcache_get_page(h, id_b, retrieved, NULL, NULL, NULL);
        tstcheck(memcmp(retrieved, page, PAGE_SIZE) == 0, "retrieved data must match");

        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    /* ── FIXED capacity ── */
    tstcase("FIXED: capacity exceeded")
    {
        cleanup_files();

        /* Use max_pages = 2 for a quick fill. */
        const pcache_configuration small_cfg = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = 2,
            .id_size         = ID_SIZE,
        };
        pcache_create(&TEST_PATHS, &small_cfg, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&TEST_PATHS, false, NULL, NULL, NULL);

        uint8_t id[ID_SIZE], page[PAGE_SIZE];
        fill_page(page, 0);

        make_id(id, 1);
        pcache_put_page(h, id, page, false, false, NULL, NULL, NULL);
        make_id(id, 2);
        pcache_put_page(h, id, page, false, false, NULL, NULL, NULL);

        make_id(id, 3);
        pcache_put_page_error pe = PCACHE_PUT_PAGE_OK;
        pcache_put_page(h, id, page, false, false, &pe, NULL, NULL);
        tstcheck(pe == PCACHE_PUT_PAGE_CAPACITY_EXCEEDED, "third write must exceed capacity");

        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    /* ── FIFO eviction ── */
    tstcase("FIFO: oldest page is evicted when full")
    {
        cleanup_files();

        const pcache_configuration fifo2 = {
            .capacity_policy = PCACHE_CAPACITY_FIFO,
            .page_size       = PAGE_SIZE,
            .max_pages       = 2,
            .id_size         = ID_SIZE,
        };
        pcache_create(&TEST_PATHS, &fifo2, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&TEST_PATHS, false, NULL, NULL, NULL);

        uint8_t id_a[ID_SIZE], id_b[ID_SIZE], id_c[ID_SIZE], page[PAGE_SIZE];
        make_id(id_a, 1);
        make_id(id_b, 2);
        make_id(id_c, 3);
        fill_page(page, 0);

        pcache_put_page(h, id_a, page, false, false, NULL, NULL, NULL);
        pcache_put_page(h, id_b, page, false, false, NULL, NULL, NULL);

        /* Third write must evict id_a (oldest). */
        pcache_put_page_error pe = PCACHE_PUT_PAGE_OK;
        pcache_put_page(h, id_c, page, false, false, &pe, NULL, NULL);
        tstcheck(pe == PCACHE_PUT_PAGE_OK, "FIFO write beyond capacity must succeed");

        tstcheck(pcache_check_page(h, id_a, NULL, NULL) == false, "evicted page must be gone");
        tstcheck(pcache_check_page(h, id_b, NULL, NULL) == true, "second page must survive");
        tstcheck(pcache_check_page(h, id_c, NULL, NULL) == true, "newest page must be present");

        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    /* ── Preallocate ── */
    tstcase("preallocate database and data file")
    {
        cleanup_files();

        pcache_create(&TEST_PATHS, &FIXED_CFG, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&TEST_PATHS, false, NULL, NULL, NULL);

        pcache_preallocate_error pre = PCACHE_PREALLOCATE_OK;
        pcache_preallocate(h, true, true, false, &pre, NULL, NULL);
        tstcheck(pre == PCACHE_PREALLOCATE_OK, "preallocate must succeed");

        /* After preallocation we should be able to store max_pages pages. */
        uint8_t id[ID_SIZE], page[PAGE_SIZE];
        fill_page(page, 0);
        pcache_put_page_error pe = PCACHE_PUT_PAGE_OK;
        for (uint32_t i = 0; i < MAX_PAGES && pe == PCACHE_PUT_PAGE_OK; i++) {
            make_id(id, (uint8_t)i);
            pcache_put_page(h, id, page, false, false, &pe, NULL, NULL);
        }
        tstcheck(pe == PCACHE_PUT_PAGE_OK, "all slots must be usable after preallocate");

        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    /* ── Defragment ── */
    tstcase("defragment compacts pages")
    {
        cleanup_files();

        pcache_create(&TEST_PATHS, &FIXED_CFG, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&TEST_PATHS, false, NULL, NULL, NULL);

        uint8_t id[ID_SIZE], page[PAGE_SIZE], retrieved[PAGE_SIZE];

        /* Write 4 pages, then delete the even-indexed ones, leaving gaps. */
        for (uint8_t i = 0; i < 4; i++) {
            make_id(id, i);
            fill_page(page, i);
            pcache_put_page(h, id, page, false, false, NULL, NULL, NULL);
        }
        make_id(id, 0);
        pcache_delete_page(h, id, false, false, NULL, NULL, NULL);
        make_id(id, 2);
        pcache_delete_page(h, id, false, false, NULL, NULL, NULL);

        pcache_defragment_error de = PCACHE_DEFRAGMENT_OK;
        pcache_defragment(h, NULL, NULL, true, false, &de, NULL, NULL);
        tstcheck(de == PCACHE_DEFRAGMENT_OK, "defragment must succeed");

        /* Pages 1 and 3 must still be readable with correct data. */
        make_id(id, 1);
        fill_page(page, 1);
        pcache_get_page(h, id, retrieved, NULL, NULL, NULL);
        tstcheck(memcmp(retrieved, page, PAGE_SIZE) == 0, "page 1 data intact after defrag");

        make_id(id, 3);
        fill_page(page, 3);
        pcache_get_page(h, id, retrieved, NULL, NULL, NULL);
        tstcheck(memcmp(retrieved, page, PAGE_SIZE) == 0, "page 3 data intact after defrag");

        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    /* ── Persistence across close/open ── */
    tstcase("data persists across close and reopen")
    {
        cleanup_files();

        pcache_create(&TEST_PATHS, &FIXED_CFG, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&TEST_PATHS, false, NULL, NULL, NULL);

        uint8_t id[ID_SIZE], page_out[PAGE_SIZE];
        make_id(id, 77);
        fill_page(page_out, 77);
        pcache_put_page(h, id, page_out, false, true, NULL, NULL, NULL);
        pcache_close(h, NULL, NULL, NULL);

        /* Reopen and verify. */
        h = pcache_open(&TEST_PATHS, false, NULL, NULL, NULL);
        uint8_t page_in[PAGE_SIZE];
        pcache_get_page_error ge = PCACHE_GET_PAGE_OK;
        pcache_get_page(h, id, page_in, &ge, NULL, NULL);
        tstcheck(ge == PCACHE_GET_PAGE_OK, "page must be found after reopen");
        tstcheck(memcmp(page_in, page_out, PAGE_SIZE) == 0, "data must survive close/reopen");

        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }

    /* ── set_max_pages ── */
    tstcase("set_max_pages grows the volume")
    {
        cleanup_files();

        const pcache_configuration small_cfg = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = 2,
            .id_size         = ID_SIZE,
        };
        pcache_create(&TEST_PATHS, &small_cfg, false, false, NULL, NULL, NULL);
        pcache_handle h = pcache_open(&TEST_PATHS, false, NULL, NULL, NULL);

        /* Fill the volume. */
        uint8_t id[ID_SIZE], page[PAGE_SIZE];
        fill_page(page, 0);
        make_id(id, 1);
        pcache_put_page(h, id, page, false, false, NULL, NULL, NULL);
        make_id(id, 2);
        pcache_put_page(h, id, page, false, false, NULL, NULL, NULL);

        /* Grow and add a third page. */
        pcache_set_max_pages_error se = PCACHE_SET_MAX_PAGES_OK;
        pcache_set_max_pages(h, 4, false, &se, NULL, NULL);
        tstcheck(se == PCACHE_SET_MAX_PAGES_OK, "growing must succeed");

        make_id(id, 3);
        pcache_put_page_error pe = PCACHE_PUT_PAGE_OK;
        pcache_put_page(h, id, page, false, false, &pe, NULL, NULL);
        tstcheck(pe == PCACHE_PUT_PAGE_OK, "write after growth must succeed");

        pcache_close(h, NULL, NULL, NULL);
        cleanup_files();
    }
}
