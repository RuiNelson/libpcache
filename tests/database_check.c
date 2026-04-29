/* Check the database with SQLite to confirm that the library is doing what's supposed to do */

#include "common.h"
#include "libpcache.h"
#include "tst.h"

#include <fcntl.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define ID_SIZE   16
#define PAGE_SIZE 256
#define MAX_PAGES 8

/* Run a query and return the integer in the first column of the first row, or fail_value on error. */
static long long query_int(sqlite3 *db, const char *sql, long long fail_value) {
    sqlite3_stmt *stmt = NULL;
    long long     result = fail_value;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return fail_value;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        result = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return result;
}

static bool fetch_metadata_blob(sqlite3 *db, const char *key, void *buffer, int buffer_size, int *out_size) {
    sqlite3_stmt *stmt = NULL;
    bool          ok   = false;
    if (sqlite3_prepare_v2(db, "SELECT value FROM metadata WHERE key = ?", -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(stmt, 0);
        int         size = sqlite3_column_bytes(stmt, 0);
        if (size <= buffer_size) {
            memcpy(buffer, blob, size);
            *out_size = size;
            ok        = true;
        }
    }
    sqlite3_finalize(stmt);
    return ok;
}

tstsuite("database structure (direct SQLite inspection)") {

    tstcase("metadata table contains all required entries with correct values") {
        test_paths paths;
        test_paths_init(&paths, "db_metadata");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_create_error create_error = (pcache_create_error)-1;
        pcache_create(&paths.pair, &config, true, true, &create_error, NULL, NULL);
        tstcheck(create_error == PCACHE_CREATE_OK, "create OK");

        sqlite3 *db = NULL;
        tstcheck(sqlite3_open(paths.database_path, &db) == SQLITE_OK, "open db OK");

        unsigned char buffer[64];
        int           size = 0;

        tstcheck(fetch_metadata_blob(db, "version", buffer, sizeof(buffer), &size), "version present");
        tstcheck(size == 4, "version is 4 bytes (UInt32)");
        uint32_t version = (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) | ((uint32_t)buffer[2] << 16) |
                           ((uint32_t)buffer[3] << 24);
        tstcheck(version == 1, "schema version is 1");

        tstcheck(fetch_metadata_blob(db, "page_size", buffer, sizeof(buffer), &size), "page_size present");
        uint32_t page_size = (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) | ((uint32_t)buffer[2] << 16) |
                             ((uint32_t)buffer[3] << 24);
        tstcheck(page_size == PAGE_SIZE, "page_size matches config (little-endian)");

        tstcheck(fetch_metadata_blob(db, "max_pages", buffer, sizeof(buffer), &size), "max_pages present");
        uint32_t max_pages = (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) | ((uint32_t)buffer[2] << 16) |
                             ((uint32_t)buffer[3] << 24);
        tstcheck(max_pages == MAX_PAGES, "max_pages matches config");

        tstcheck(fetch_metadata_blob(db, "id_size", buffer, sizeof(buffer), &size), "id_size present");
        uint32_t id_size = (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) | ((uint32_t)buffer[2] << 16) |
                           ((uint32_t)buffer[3] << 24);
        tstcheck(id_size == ID_SIZE, "id_size matches config");

        tstcheck(fetch_metadata_blob(db, "capacity_policy", buffer, sizeof(buffer), &size), "capacity_policy present");
        tstcheck(size == 6 && memcmp(buffer, "FIXED", 5) == 0 && buffer[5] == '\0',
                 "capacity_policy is null-terminated FIXED");

        sqlite3_close(db);
        test_paths_cleanup(&paths);
    }

    tstcase("FIFO volume metadata records the correct policy string") {
        test_paths paths;
        test_paths_init(&paths, "db_metadata_fifo");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIFO,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_create(&paths.pair, &config, true, true, NULL, NULL, NULL);

        sqlite3 *db = NULL;
        sqlite3_open(paths.database_path, &db);

        unsigned char buffer[16];
        int           size = 0;
        fetch_metadata_blob(db, "capacity_policy", buffer, sizeof(buffer), &size);
        tstcheck(size == 5 && memcmp(buffer, "FIFO", 4) == 0 && buffer[4] == '\0',
                 "capacity_policy is null-terminated FIFO");

        sqlite3_close(db);
        test_paths_cleanup(&paths);
    }

    tstcase("pages table has the expected columns and indexes") {
        test_paths paths;
        test_paths_init(&paths, "db_indexes");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_create(&paths.pair, &config, true, true, NULL, NULL, NULL);

        sqlite3 *db = NULL;
        sqlite3_open(paths.database_path, &db);

        long long has_id_hash =
            query_int(db, "SELECT COUNT(*) FROM pragma_table_info('pages') WHERE name='id_hash'", -1);
        long long has_id = query_int(db, "SELECT COUNT(*) FROM pragma_table_info('pages') WHERE name='id'", -1);
        tstcheck(has_id_hash == 1, "pages.id_hash exists");
        tstcheck(has_id == 1, "pages.id exists");

        long long has_lookup_index =
            query_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_lookup'", -1);
        long long has_free_slots_index =
            query_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_free_slots'", -1);
        tstcheck(has_lookup_index == 1, "idx_lookup index exists");
        tstcheck(has_free_slots_index == 1, "idx_free_slots index exists");

        sqlite3_close(db);
        test_paths_cleanup(&paths);
    }

    tstcase("a stored page lives at byte offset (ROWID - 1) * page_size") {
        test_paths paths;
        test_paths_init(&paths, "db_offset");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        /* Insert two pages so we expect rows at ROWID 1 and 2 with distinguishable contents. */
        unsigned char id_a[ID_SIZE], page_a[PAGE_SIZE];
        unsigned char id_b[ID_SIZE], page_b[PAGE_SIZE];
        make_id_with_index(id_a, ID_SIZE, 1);
        make_id_with_index(id_b, ID_SIZE, 2);
        make_page_with_index(page_a, PAGE_SIZE, 1);
        make_page_with_index(page_b, PAGE_SIZE, 2);
        pcache_put_page(handle, id_a, page_a, false, true, NULL, NULL, NULL);
        pcache_put_page(handle, id_b, page_b, false, true, NULL, NULL, NULL);

        pcache_close(handle, NULL, NULL, NULL);

        sqlite3 *db = NULL;
        sqlite3_open(paths.database_path, &db);

        sqlite3_stmt *stmt = NULL;
        sqlite3_prepare_v2(db, "SELECT rowid, id FROM pages WHERE id IS NOT NULL ORDER BY rowid", -1, &stmt, NULL);

        int data_fd = open(paths.data_path, O_RDONLY);
        tstcheck(data_fd >= 0, "data file opened");

        bool offsets_match = true;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            sqlite3_int64 rowid     = sqlite3_column_int64(stmt, 0);
            const void   *id_blob   = sqlite3_column_blob(stmt, 1);
            int           id_blen   = sqlite3_column_bytes(stmt, 1);
            off_t         offset    = (off_t)(rowid - 1) * PAGE_SIZE;
            unsigned char data_buf[PAGE_SIZE];
            ssize_t       read_size = pread(data_fd, data_buf, PAGE_SIZE, offset);
            tstcheck(read_size == PAGE_SIZE, "pread returns full page");

            unsigned char expected_page[PAGE_SIZE];
            if (id_blen == ID_SIZE && memcmp(id_blob, id_a, ID_SIZE) == 0) {
                make_page_with_index(expected_page, PAGE_SIZE, 1);
            } else if (id_blen == ID_SIZE && memcmp(id_blob, id_b, ID_SIZE) == 0) {
                make_page_with_index(expected_page, PAGE_SIZE, 2);
            } else {
                offsets_match = false;
                break;
            }
            if (memcmp(data_buf, expected_page, PAGE_SIZE) != 0) {
                offsets_match = false;
                break;
            }
        }
        tstcheck(offsets_match, "byte_offset = (ROWID - 1) * page_size holds for every live page");

        close(data_fd);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        test_paths_cleanup(&paths);
    }

    tstcase("preallocated database has max_pages rows with NULL id_hash and id") {
        test_paths paths;
        test_paths_init(&paths, "db_prealloc");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_create(&paths.pair, &config, true, true, NULL, NULL, NULL);

        sqlite3 *db = NULL;
        sqlite3_open(paths.database_path, &db);

        long long total = query_int(db, "SELECT COUNT(*) FROM pages", -1);
        tstcheck(total == MAX_PAGES, "preallocated table has max_pages rows");

        long long null_id = query_int(db, "SELECT COUNT(*) FROM pages WHERE id IS NULL AND id_hash IS NULL", -1);
        tstcheck(null_id == MAX_PAGES, "all preallocated rows have NULL id and id_hash");

        sqlite3_close(db);
        test_paths_cleanup(&paths);
    }

    tstcase("after a delete the corresponding row has NULL id and id_hash (FIXED)") {
        test_paths paths;
        test_paths_init(&paths, "db_after_delete");

        pcache_configuration config = {
            .capacity_policy = PCACHE_CAPACITY_FIXED,
            .page_size       = PAGE_SIZE,
            .max_pages       = MAX_PAGES,
            .id_size         = ID_SIZE,
        };
        pcache_handle handle = make_volume_and_open(&paths, &config);

        unsigned char id[ID_SIZE], page[PAGE_SIZE];
        make_id_with_index(id, ID_SIZE, 42);
        make_page_with_index(page, PAGE_SIZE, 42);
        pcache_put_page(handle, id, page, false, true, NULL, NULL, NULL);
        pcache_delete_page(handle, id, false, true, NULL, NULL, NULL);

        pcache_close(handle, NULL, NULL, NULL);

        sqlite3 *db = NULL;
        sqlite3_open(paths.database_path, &db);

        long long live_rows = query_int(db, "SELECT COUNT(*) FROM pages WHERE id IS NOT NULL", -1);
        tstcheck(live_rows == 0, "no live rows remain after deleting the only stored id");

        sqlite3_close(db);
        test_paths_cleanup(&paths);
    }
}
