/* Tests related to the FIFO Policy */

#include "common.h"
#include "libpcache.h"
#include "tst.h"

#include <sqlite3.h>
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

/* Retrieve the page for `index` and verify its bytes match what put_one stored. */
static bool content_ok(pcache_handle handle, uint32_t index) {
    unsigned char id[ID_SIZE], expected[PAGE_SIZE], actual[PAGE_SIZE];
    make_id_with_index(id, ID_SIZE, index);
    make_page_with_index(expected, PAGE_SIZE, index);
    pcache_get_error get_error = (pcache_get_error)-1;
    pcache_get_page(handle, id, actual, &get_error, NULL, NULL);
    return get_error == PCACHE_GET_OK && memcmp(expected, actual, PAGE_SIZE) == 0;
}

static void delete_one(pcache_handle handle, uint32_t index, bool wipe) {
    unsigned char id[ID_SIZE];
    make_id_with_index(id, ID_SIZE, index);
    pcache_delete_error delete_error = (pcache_delete_error)-1;
    pcache_delete_page(handle, id, wipe, true, &delete_error, NULL, NULL);
}

/* Number of empty runs in the pages table: empty slots whose circular
 * predecessor (max_pages wraps to before 1) is occupied. */
static long long count_empty_runs(const char *database_path, long long max_pages) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(database_path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK)
        return -1;
    sqlite3_stmt *stmt = NULL;
    long long     runs = -1;
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(*) FROM pages p WHERE p.id_hash IS NULL AND EXISTS ("
                           "  SELECT 1 FROM pages q WHERE q.rowid = CASE WHEN p.rowid = 1 THEN ?1"
                           "  ELSE p.rowid - 1 END AND q.id_hash IS NOT NULL)",
                           -1,
                           &stmt,
                           NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, max_pages);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            runs = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return runs;
}

/* Null out one row directly, simulating a crash between a delete commit and
 * its compaction pass (or a volume written by a pre-compaction library). */
static bool null_row_directly(const char *database_path, long long rowid) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(database_path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK)
        return false;
    sqlite3_stmt *stmt = NULL;
    bool          ok   = false;
    if (sqlite3_prepare_v2(db, "UPDATE pages SET id_hash=NULL, id=NULL WHERE rowid=?", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, rowid);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return ok;
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

    tstcase("delete in steady state preserves the FIFO eviction order") {
        test_paths paths;
        test_paths_init(&paths, "fifo_delete_steady");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIFO,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        /* Ten puts: ids 0..2 evicted, live set {3..9}, oldest 3, newest 9. */
        for (uint32_t i = 0; i <= 9; i++)
            tstcheck(put_one(handle, i) == PCACHE_PUT_OK, "put OK");

        /* Delete id 8 — its slot sits below the cursor, the hijack scenario. */
        delete_one(handle, 8, false);

        pcache_inspect_page_count_error count_error = (pcache_inspect_page_count_error)-1;
        pcache_page_count               counts      = pcache_inspect_page_count(handle, &count_error, NULL);
        tstcheck(counts.used == MAX_PAGES - 2, "one page deleted");

        /* The next put must not evict anyone — the volume has room. */
        tstcheck(put_one(handle, 10) == PCACHE_PUT_OK, "put after delete OK");
        tstcheck(exists(handle, 9), "newest page survives a put after delete");
        bool all_live = true;
        for (uint32_t i = 3; i <= 7; i++)
            all_live = all_live && exists(handle, i);
        tstcheck(all_live && exists(handle, 10), "full live set intact after refill");
        tstcheck(content_ok(handle, 9) && content_ok(handle, 3), "relocated and stationary bytes intact");

        /* Further puts evict strictly oldest-first: 3, then 4. */
        tstcheck(put_one(handle, 11) == PCACHE_PUT_OK, "put OK");
        tstcheck(!exists(handle, 3) && exists(handle, 4), "oldest (3) evicted first");
        tstcheck(put_one(handle, 12) == PCACHE_PUT_OK, "put OK");
        tstcheck(!exists(handle, 4) && exists(handle, 5), "next oldest (4) evicted second");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("delete during fill-up does not cause premature eviction") {
        test_paths paths;
        test_paths_init(&paths, "fifo_delete_fillup");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIFO,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        /* No preallocation: the volume grows row by row (fill-up phase). */
        pcache_create_error create_error = (pcache_create_error)-1;
        pcache_create(&paths.pair, &config, false, false, &create_error, NULL, NULL);
        tstcheck(create_error == PCACHE_CREATE_OK, "create OK");
        pcache_open_error open_error = (pcache_open_error)-1;
        pcache_handle     handle     = pcache_open(&paths.pair, &open_error, NULL, NULL);
        tstcheck(open_error == PCACHE_OPEN_OK, "open OK");

        for (uint32_t i = 0; i <= 2; i++)
            tstcheck(put_one(handle, i) == PCACHE_PUT_OK, "fill-up put OK");
        delete_one(handle, 1, false);

        /* Refill: with 2 live pages and capacity 8, five more puts fit without
         * evicting anyone. */
        for (uint32_t i = 3; i <= 7; i++)
            tstcheck(put_one(handle, i) == PCACHE_PUT_OK, "refill put OK");
        tstcheck(exists(handle, 0), "id 0 not evicted while capacity remains");

        pcache_inspect_page_count_error count_error = (pcache_inspect_page_count_error)-1;
        pcache_page_count               counts      = pcache_inspect_page_count(handle, &count_error, NULL);
        tstcheck(counts.used == MAX_PAGES - 1, "volume filled to max_pages - 1");

        /* The put that reaches capacity evicts the oldest (0), nothing else. */
        tstcheck(put_one(handle, 8) == PCACHE_PUT_OK, "capacity put OK");
        tstcheck(!exists(handle, 0), "oldest (0) evicted at capacity");
        bool rest = exists(handle, 2);
        for (uint32_t i = 3; i <= 8; i++)
            rest = rest && exists(handle, i);
        tstcheck(rest, "remaining pages all alive");
        tstcheck(content_ok(handle, 2), "page relocated by fill-up compaction has intact bytes");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("range delete with wipe preserves eviction order and relocated bytes") {
        test_paths paths;
        test_paths_init(&paths, "fifo_delete_range");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIFO,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        /* Live set {3..9}, oldest 3, newest 9. */
        for (uint32_t i = 0; i <= 9; i++)
            tstcheck(put_one(handle, i) == PCACHE_PUT_OK, "put OK");

        unsigned char first[ID_SIZE], last[ID_SIZE];
        make_id_with_index(first, ID_SIZE, 4);
        make_id_with_index(last, ID_SIZE, 6);
        pcache_delete_error delete_error = (pcache_delete_error)-1;
        pcache_delete_pages_range(handle, first, last, /*wipe*/ true, true, &delete_error, NULL, NULL);
        tstcheck(delete_error == PCACHE_DELETE_OK, "range delete OK");

        pcache_inspect_page_count_error count_error = (pcache_inspect_page_count_error)-1;
        pcache_page_count               counts      = pcache_inspect_page_count(handle, &count_error, NULL);
        tstcheck(counts.used == 4, "three pages deleted");
        tstcheck(content_ok(handle, 3) && content_ok(handle, 7) && content_ok(handle, 8) && content_ok(handle, 9),
                 "wipe did not touch relocated live pages");

        /* Three refills reuse the freed capacity without evicting anyone. */
        for (uint32_t i = 10; i <= 12; i++)
            tstcheck(put_one(handle, i) == PCACHE_PUT_OK, "refill put OK");
        tstcheck(exists(handle, 3), "oldest (3) not evicted while capacity remains");
        counts = pcache_inspect_page_count(handle, &count_error, NULL);
        tstcheck(counts.used == MAX_PAGES - 1, "volume back at max_pages - 1");

        /* The next put evicts the oldest. */
        tstcheck(put_one(handle, 13) == PCACHE_PUT_OK, "capacity put OK");
        tstcheck(!exists(handle, 3) && exists(handle, 7), "oldest (3) evicted, next oldest (7) alive");

        pcache_close(handle, NULL, NULL, NULL);
        test_paths_cleanup(&paths);
    }

    tstcase("open repairs a FIFO volume left with two empty runs") {
        test_paths paths;
        test_paths_init(&paths, "fifo_open_repair");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIFO,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        /* Live set {3..9}; the cursor slot is the single empty run. */
        for (uint32_t i = 0; i <= 9; i++)
            tstcheck(put_one(handle, i) == PCACHE_PUT_OK, "put OK");
        pcache_close(handle, NULL, NULL, NULL);

        /* Null a live row directly: the volume now has two empty runs, as a
         * crash between a delete commit and its compaction would leave it. */
        tstcheck(null_row_directly(paths.database_path, 6), "inject stray hole");
        tstcheck(count_empty_runs(paths.database_path, MAX_PAGES) == 2, "two empty runs before open");

        pcache_open_error open_error = (pcache_open_error)-1;
        handle                       = pcache_open(&paths.pair, &open_error, NULL, NULL);
        tstcheck(open_error == PCACHE_OPEN_OK, "reopen OK");

        pcache_inspect_page_count_error count_error = (pcache_inspect_page_count_error)-1;
        pcache_page_count               counts      = pcache_inspect_page_count(handle, &count_error, NULL);
        tstcheck(counts.used == 6, "live count preserved by repair");

        bool survivors_ok = true;
        for (uint32_t i = 3; i <= 9; i++) {
            if (i == 5)
                continue; /* rowid 6 held id 5 */
            survivors_ok = survivors_ok && content_ok(handle, i);
        }
        tstcheck(survivors_ok, "all surviving pages readable with intact bytes");

        pcache_close(handle, NULL, NULL, NULL);
        tstcheck(count_empty_runs(paths.database_path, MAX_PAGES) == 1, "single empty run after repair");

        test_paths_cleanup(&paths);
    }
}
