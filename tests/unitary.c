/* Unit tests for pure helper functions in src/pages_util.h */

#include "libpcache.h"
#include "macros.h"
#include "pages_util.h"
#include "tst.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ID_SIZE 16

static bool host_is_little_endian(void) {
    const uint32_t probe = 0x01020304;
    uint8_t        bytes[4];
    memcpy(bytes, &probe, sizeof(bytes));
    return bytes[0] == 0x04;
}

tstsuite("unit tests for pages_util helpers") {

    /* ───────────────── batch_has_duplicate_ids ───────────────── */

    tstcase("batch_has_duplicate_ids: count < 2 always returns NONE") {
        uint8_t ids[ID_SIZE] = {0};
        tstcheck(batch_has_duplicate_ids(NULL, 0, ID_SIZE) == BATCH_DUP_NONE, "count == 0 -> NONE");
        tstcheck(batch_has_duplicate_ids(ids, 1, ID_SIZE) == BATCH_DUP_NONE, "count == 1 -> NONE");
    }

    tstcase("batch_has_duplicate_ids: distinct ids return NONE") {
        uint8_t ids[5 * ID_SIZE];
        memset(ids, 0, sizeof(ids));
        for (size_t idx = 0; idx < 5; idx++)
            ids[idx * ID_SIZE + ID_SIZE - 1] = (uint8_t)(idx + 1);
        tstcheck(batch_has_duplicate_ids(ids, 5, ID_SIZE) == BATCH_DUP_NONE, "all distinct -> NONE");
    }

    tstcase("batch_has_duplicate_ids: adjacent duplicate detected") {
        uint8_t ids[3 * ID_SIZE];
        memset(ids, 0, sizeof(ids));
        ids[0 * ID_SIZE + ID_SIZE - 1] = 0x01;
        ids[1 * ID_SIZE + ID_SIZE - 1] = 0x01; /* duplicate of [0] */
        ids[2 * ID_SIZE + ID_SIZE - 1] = 0x02;
        tstcheck(batch_has_duplicate_ids(ids, 3, ID_SIZE) == BATCH_DUP_FOUND, "adjacent duplicate -> FOUND");
    }

    tstcase("batch_has_duplicate_ids: distant duplicate detected") {
        uint8_t ids[10 * ID_SIZE];
        memset(ids, 0, sizeof(ids));
        for (size_t idx = 0; idx < 10; idx++)
            ids[idx * ID_SIZE + ID_SIZE - 1] = (uint8_t)idx;
        ids[9 * ID_SIZE + ID_SIZE - 1] = 0; /* duplicate of [0] */
        tstcheck(batch_has_duplicate_ids(ids, 10, ID_SIZE) == BATCH_DUP_FOUND, "first/last duplicate -> FOUND");
    }

    tstcase("batch_has_duplicate_ids: large batch (uses sorted path) - no duplicates") {
        const size_t count = 1500;
        uint8_t     *ids   = calloc(count, ID_SIZE);
        tstcheck(ids != NULL, "alloc OK");
        for (size_t idx = 0; idx < count; idx++) {
            uint8_t *entry     = ids + idx * ID_SIZE;
            uint32_t value     = (uint32_t)idx;
            entry[ID_SIZE - 4] = (uint8_t)(value >> 24);
            entry[ID_SIZE - 3] = (uint8_t)(value >> 16);
            entry[ID_SIZE - 2] = (uint8_t)(value >> 8);
            entry[ID_SIZE - 1] = (uint8_t)value;
        }
        tstcheck(batch_has_duplicate_ids(ids, count, ID_SIZE) == BATCH_DUP_NONE,
                 "1500 unique ids via sorted path -> NONE");
        free(ids);
    }

    tstcase("batch_has_duplicate_ids: large batch (uses sorted path) - one duplicate") {
        const size_t count = 1500;
        uint8_t     *ids   = calloc(count, ID_SIZE);
        for (size_t idx = 0; idx < count; idx++) {
            uint8_t *entry     = ids + idx * ID_SIZE;
            uint32_t value     = (uint32_t)idx;
            entry[ID_SIZE - 4] = (uint8_t)(value >> 24);
            entry[ID_SIZE - 3] = (uint8_t)(value >> 16);
            entry[ID_SIZE - 2] = (uint8_t)(value >> 8);
            entry[ID_SIZE - 1] = (uint8_t)value;
        }
        /* Duplicate the first id in the middle of the array. */
        memcpy(ids + 750 * ID_SIZE, ids, ID_SIZE);
        tstcheck(batch_has_duplicate_ids(ids, count, ID_SIZE) == BATCH_DUP_FOUND,
                 "1500 ids with one duplicate -> FOUND");
        free(ids);
    }

    /* ───────────────── validate_with_counter_args ───────────────── */

    tstcase("validate_with_counter_args: valid arguments produce the documented counter offset") {
        size_t counter_offset = 999;
        tstcheck(validate_with_counter_args(
                     ID_SIZE, 1, 0, /*position*/ 0, PCACHE_ENDIANNESS_BIG_ENDIAN, &counter_offset) == true,
                 "position 0 valid");
        tstcheck(counter_offset == ID_SIZE - 4, "counter_offset == id_size - 4 when position == 0");

        tstcheck(validate_with_counter_args(
                     ID_SIZE, 1, 0, /*position*/ ID_SIZE - 4, PCACHE_ENDIANNESS_BIG_ENDIAN, &counter_offset) == true,
                 "position id_size-4 valid (counter at start)");
        tstcheck(counter_offset == 0, "counter_offset == 0 at boundary");
    }

    tstcase("validate_with_counter_args: position out of bounds rejected") {
        size_t counter_offset = 999;
        tstcheck(validate_with_counter_args(
                     ID_SIZE, 1, 0, /*position*/ ID_SIZE - 3, PCACHE_ENDIANNESS_BIG_ENDIAN, &counter_offset) == false,
                 "position id_size-3 rejected (would overflow)");
        tstcheck(validate_with_counter_args(
                     ID_SIZE, 1, 0, /*position*/ ID_SIZE, PCACHE_ENDIANNESS_BIG_ENDIAN, &counter_offset) == false,
                 "position == id_size rejected");
        tstcheck(validate_with_counter_args(
                     ID_SIZE, 1, 0, /*position*/ UINT32_MAX, PCACHE_ENDIANNESS_BIG_ENDIAN, &counter_offset) == false,
                 "position == UINT32_MAX rejected");
    }

    tstcase("validate_with_counter_args: counter overflow boundary") {
        size_t counter_offset = 0;
        tstcheck(validate_with_counter_args(
                     ID_SIZE, /*count*/ 1, /*start*/ UINT32_MAX, 0, PCACHE_ENDIANNESS_BIG_ENDIAN, &counter_offset) ==
                     true,
                 "start = UINT32_MAX, count = 1 -> exactly UINT32_MAX+1, valid");
        tstcheck(validate_with_counter_args(
                     ID_SIZE, /*count*/ 2, /*start*/ UINT32_MAX, 0, PCACHE_ENDIANNESS_BIG_ENDIAN, &counter_offset) ==
                     false,
                 "start = UINT32_MAX, count = 2 -> exceeds UINT32_MAX+1, invalid");
        tstcheck(validate_with_counter_args(
                     ID_SIZE, /*count*/ UINT32_MAX, /*start*/ 0, 0, PCACHE_ENDIANNESS_BIG_ENDIAN, &counter_offset) ==
                     true,
                 "start = 0, count = UINT32_MAX -> equals UINT32_MAX, valid");
        tstcheck(validate_with_counter_args(ID_SIZE,
                                            /*count*/ (size_t)UINT32_MAX + 2u,
                                            /*start*/ 0,
                                            0,
                                            PCACHE_ENDIANNESS_BIG_ENDIAN,
                                            &counter_offset) == false,
                 "count > UINT32_MAX+1 invalid");
    }

    tstcase("validate_with_counter_args: invalid endianness rejected") {
        size_t counter_offset = 0;
        tstcheck(validate_with_counter_args(ID_SIZE, 1, 0, 0, (pcache_endianness)42, &counter_offset) == false,
                 "endianness 42 rejected");
        tstcheck(validate_with_counter_args(ID_SIZE, 1, 0, 0, (pcache_endianness)-1, &counter_offset) == false,
                 "endianness -1 rejected");
    }

    tstcase("validate_with_counter_args: NULL out parameter is allowed") {
        tstcheck(validate_with_counter_args(ID_SIZE, 1, 0, 0, PCACHE_ENDIANNESS_BIG_ENDIAN, NULL) == true,
                 "NULL counter_offset_out tolerated");
    }

    /* ───────────────── build_with_counter_ids ───────────────── */

    tstcase("build_with_counter_ids: BIG_ENDIAN places MSB first") {
        uint8_t  base[8] = {0};
        uint8_t *result  = build_with_counter_ids(/*count*/ 1,
                                                  /*id_size*/ 8,
                                                  base,
                                                  /*start*/ 0x12345678,
                                                  /*counter_offset*/ 4,
                                                  PCACHE_ENDIANNESS_BIG_ENDIAN);
        tstcheck(result != NULL, "alloc OK");
        tstcheck(result[0] == 0 && result[1] == 0 && result[2] == 0 && result[3] == 0,
                 "bytes outside counter region untouched");
        tstcheck(result[4] == 0x12 && result[5] == 0x34 && result[6] == 0x56 && result[7] == 0x78,
                 "BIG_ENDIAN counter encoded MSB->LSB");
        free(result);
    }

    tstcase("build_with_counter_ids: LITTLE_ENDIAN places LSB first") {
        uint8_t  base[8] = {0};
        uint8_t *result  = build_with_counter_ids(1, 8, base, 0x12345678, 4, PCACHE_ENDIANNESS_LITTLE_ENDIAN);
        tstcheck(result != NULL, "alloc OK");
        tstcheck(result[4] == 0x78 && result[5] == 0x56 && result[6] == 0x34 && result[7] == 0x12,
                 "LITTLE_ENDIAN counter encoded LSB->MSB");
        free(result);
    }

    tstcase("build_with_counter_ids: NATIVE matches host byte order") {
        uint8_t  base[8] = {0};
        uint8_t *result  = build_with_counter_ids(1, 8, base, 0x12345678, 4, PCACHE_ENDIANNESS_NATIVE);
        tstcheck(result != NULL, "alloc OK");
        if (host_is_little_endian()) {
            tstcheck(result[4] == 0x78 && result[7] == 0x12, "NATIVE on little-endian host == LITTLE_ENDIAN");
        } else {
            tstcheck(result[4] == 0x12 && result[7] == 0x78, "NATIVE on big-endian host == BIG_ENDIAN");
        }
        free(result);
    }

    tstcase("build_with_counter_ids: counter is XORed (not overwritten) onto the base") {
        uint8_t  base[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        uint8_t *result  = build_with_counter_ids(1, 8, base, 0x12345678, 4, PCACHE_ENDIANNESS_BIG_ENDIAN);
        tstcheck(result != NULL, "alloc OK");
        tstcheck(result[0] == 0xFF && result[1] == 0xFF && result[2] == 0xFF && result[3] == 0xFF,
                 "non-counter bytes preserved");
        tstcheck(result[4] == (0xFF ^ 0x12) && result[5] == (0xFF ^ 0x34) && result[6] == (0xFF ^ 0x56) &&
                     result[7] == (0xFF ^ 0x78),
                 "counter bytes XORed against base");
        free(result);
    }

    tstcase("build_with_counter_ids: counter advances by one per item") {
        uint8_t  base[8] = {0};
        uint8_t *result  = build_with_counter_ids(/*count*/ 4,
                                                  /*id_size*/ 8,
                                                  base,
                                                  /*start*/ 100,
                                                  /*counter_offset*/ 4,
                                                  PCACHE_ENDIANNESS_BIG_ENDIAN);
        tstcheck(result != NULL, "alloc OK");

        for (uint32_t idx = 0; idx < 4; idx++) {
            uint32_t expected = 100 + idx;
            uint8_t *id       = result + idx * 8;
            uint32_t actual =
                ((uint32_t)id[4] << 24) | ((uint32_t)id[5] << 16) | ((uint32_t)id[6] << 8) | (uint32_t)id[7];
            tstcheck(actual == expected, "counter at index advances by 1 per item");
        }
        free(result);
    }

    tstcase("build_with_counter_ids: counter offset can be 0 (counter at the start)") {
        uint8_t  base[8] = {0};
        uint8_t *result =
            build_with_counter_ids(1, 8, base, 0xAABBCCDD, /*counter_offset*/ 0, PCACHE_ENDIANNESS_BIG_ENDIAN);
        tstcheck(result != NULL, "alloc OK");
        tstcheck(result[0] == 0xAA && result[1] == 0xBB && result[2] == 0xCC && result[3] == 0xDD,
                 "counter placed at offset 0");
        tstcheck(result[4] == 0 && result[5] == 0 && result[6] == 0 && result[7] == 0, "trailing bytes untouched");
        free(result);
    }

    tstcase("build_with_counter_ids: count == 0 returns a valid (empty) allocation") {
        uint8_t  base[8] = {0};
        uint8_t *result  = build_with_counter_ids(0, 8, base, 0, 4, PCACHE_ENDIANNESS_BIG_ENDIAN);
        /* malloc(0) is implementation-defined; either NULL or a freeable pointer is fine. */
        free(result);
        tstcheck(true, "count == 0 does not crash");
    }

    /* ───────────────── SET_ERR / SET_2ERR / SET_3ERR macros ───────────────── */

    tstcase("SET_2ERR in else-without-braces: success path must not write any error value") {
        int err = 0, sqlite_err = 0;
        int rc  = 100; /* simulates SQLITE_ROW */

        if (rc == 100)
            (void)rc;
        else
            SET_2ERR(&err, &sqlite_err, 999, rc);

        tstcheck(err == 0, "err must stay 0 when if-branch is taken");
        tstcheck(sqlite_err == 0, "sqlite_err must stay 0 when if-branch is taken");
    }

    tstcase("SET_3ERR in else-without-braces: success path must not write any error value") {
        int err = 0, sqlite_err = 0, posix_err = 0;
        int rc  = 100;

        if (rc == 100)
            (void)rc;
        else
            SET_3ERR(&err, &sqlite_err, &posix_err, 999, rc, -1);

        tstcheck(err == 0, "err must stay 0 when if-branch is taken");
        tstcheck(sqlite_err == 0, "sqlite_err must stay 0 when if-branch is taken");
        tstcheck(posix_err == 0, "posix_err must stay 0 when if-branch is taken");
    }

    tstcase("SET_2ERR in else-without-braces: error path must set both values") {
        int err = 0, sqlite_err = 0;
        int rc  = 1; /* not SQLITE_ROW */

        if (rc == 100)
            (void)rc;
        else
            SET_2ERR(&err, &sqlite_err, 999, rc);

        tstcheck(err == 999, "err must be set in error path");
        tstcheck(sqlite_err == 1, "sqlite_err must be set in error path");
    }
}
