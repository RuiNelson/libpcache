#include "handle.h"

/* Each segment is a fixed-size array of volumes allocated with malloc and never
 * moved.  Growing the table adds a new segment; existing pointers and mutexes
 * remain at their original addresses, which prevents the use-after-free and
 * moved-mutex UB that a flat realloc() would cause. */
#define SEGMENT_SIZE 16

static pcache_volume    **s_segments;
static size_t             s_segment_count;
static size_t             s_vol_count;
static platform_mutex     s_table_lock;
static platform_once_flag s_once = PLATFORM_ONCE_INIT;

static void global_init(void) {
    platform_mutex_init(&s_table_lock);
}

static pcache_volume *slot_at(size_t idx) {
    return &s_segments[idx / SEGMENT_SIZE][idx % SEGMENT_SIZE];
}

static bool grow_table(void) {
    pcache_volume *seg = calloc(SEGMENT_SIZE, sizeof *seg);
    if (!seg)
        return false;

    for (size_t i = 0; i < SEGMENT_SIZE; i++)
        platform_mutex_init(&seg[i].mutex);

    pcache_volume **new_segs = realloc(s_segments, (s_segment_count + 1) * sizeof *new_segs);
    if (!new_segs) {
        for (size_t i = 0; i < SEGMENT_SIZE; i++)
            platform_mutex_destroy(&seg[i].mutex);
        free(seg);
        return false;
    }

    new_segs[s_segment_count] = seg;
    s_segments                = new_segs;
    s_segment_count++;
    return true;
}

pcache_volume *allocate_slot(void) {
    platform_once(&s_once, global_init);
    platform_mutex_lock(&s_table_lock);

    pcache_volume *v = NULL;
    for (size_t i = 0; i < s_vol_count && !v; i++) {
        pcache_volume *candidate = slot_at(i);
        if (!candidate->in_use) {
            candidate->in_use = true;
            v                 = candidate;
        }
    }

    if (!v) {
        if (s_vol_count >= s_segment_count * SEGMENT_SIZE && !grow_table()) {
            platform_mutex_unlock(&s_table_lock);
            return NULL;
        }
        v              = slot_at(s_vol_count);
        v->in_use      = true;
        v->self_handle = (pcache_handle)(s_vol_count + 1);
        s_vol_count++;
    }

    platform_mutex_unlock(&s_table_lock);
    return v;
}

void release_slot(pcache_volume *volume) {
    if (!volume)
        return;

    platform_once(&s_once, global_init);
    platform_mutex_lock(&s_table_lock);

    bool valid = false;
    for (size_t seg = 0; seg < s_segment_count && !valid; seg++) {
        if (volume >= s_segments[seg] && volume < s_segments[seg] + SEGMENT_SIZE)
            valid = true;
    }
    if (valid)
        volume->in_use = false;

    platform_mutex_unlock(&s_table_lock);
}

pcache_volume *volume_from_handle(pcache_handle handle) {
    if (handle <= 0)
        return NULL;

    platform_once(&s_once, global_init);
    platform_mutex_lock(&s_table_lock);

    size_t idx = (size_t)(handle - 1);
    if (idx >= s_vol_count) {
        platform_mutex_unlock(&s_table_lock);
        return NULL;
    }

    pcache_volume *v = slot_at(idx);
    if (!v->in_use) {
        platform_mutex_unlock(&s_table_lock);
        return NULL;
    }

    platform_mutex_lock(&v->mutex);
    platform_mutex_unlock(&s_table_lock);
    return v;
}

pcache_volume *volume_from_handle_for_close(pcache_handle handle) {
    if (handle <= 0)
        return NULL;

    platform_once(&s_once, global_init);
    platform_mutex_lock(&s_table_lock);

    size_t idx = (size_t)(handle - 1);
    if (idx >= s_vol_count) {
        platform_mutex_unlock(&s_table_lock);
        return NULL;
    }

    pcache_volume *volume = slot_at(idx);
    if (!volume->in_use) {
        platform_mutex_unlock(&s_table_lock);
        return NULL;
    }

    volume->in_use = false;
    platform_mutex_lock(&volume->mutex);
    platform_mutex_unlock(&s_table_lock);
    return volume;
}

pcache_handle handle_of(const pcache_volume *volume) {
    return volume->self_handle;
}
