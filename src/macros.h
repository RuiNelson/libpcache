#pragma once

#define SET_ERR(p, v)                                                                                                  \
    do {                                                                                                               \
        if (p)                                                                                                         \
            *(p) = (v);                                                                                                \
    } while (0)

#define SET_2ERR(p0, p1, v0, v1)                                                                                       \
    do {                                                                                                               \
        SET_ERR(p0, v0);                                                                                               \
        SET_ERR(p1, v1);                                                                                               \
    } while (0)

#define SET_3ERR(p0, p1, p2, v0, v1, v2)                                                                               \
    do {                                                                                                               \
        SET_ERR(p0, v0);                                                                                               \
        SET_ERR(p1, v1);                                                                                               \
        SET_ERR(p2, v2);                                                                                               \
    } while (0)

#define ZERO_ERR(p0)          SET_ERR(p0, 0)
#define ZERO_2ERR(p0, p1)     SET_2ERR(p0, p1, 0, 0)
#define ZERO_3ERR(p0, p1, p2) SET_3ERR(p0, p1, p2, 0, 0, 0)

#define NO_ERROR(e)           ((e) == NULL || *(e) == 0)
#define HAS_ERROR(e)          ((e) != NULL && *(e) != 0)

#define ANY_ERROR(x, y)       (HAS_ERROR(x) || HAS_ERROR(y))
#define NO_ERRORS(x, y)       (NO_ERROR(x) && NO_ERROR(y))
