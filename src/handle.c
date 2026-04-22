#include "handle.h"

static pcache_volume  *s_vols;
static size_t          s_vol_count;
static size_t          s_vol_capacity;
static pthread_mutex_t s_table_lock;
static pthread_once_t  s_once = PTHREAD_ONCE_INIT;

#define INITIAL_CAPACITY 16

static void global_init(void) {
    pthread_mutex_init(&s_table_lock, NULL);
}

static bool grow_table(void) {
    size_t         new_cap  = s_vol_capacity ? s_vol_capacity * 2 : INITIAL_CAPACITY;
    pcache_volume *new_vols = realloc(s_vols, new_cap * sizeof *new_vols);
    if (!new_vols)
        return false;

    size_t old_count = s_vol_capacity;
    memset(new_vols + old_count, 0, (new_cap - old_count) * sizeof *new_vols);
    for (size_t i = old_count; i < new_cap; i++)
        pthread_mutex_init(&new_vols[i].mutex, NULL);

    s_vols         = new_vols;
    s_vol_capacity = new_cap;
    return true;
}

pcache_volume *alloc_slot(void) {
    pthread_once(&s_once, global_init);
    pthread_mutex_lock(&s_table_lock);

    pcache_volume *v = NULL;
    for (size_t i = 0; i < s_vol_count && !v; i++) {
        if (!s_vols[i].in_use) {
            s_vols[i].in_use = true;
            v                = &s_vols[i];
        }
    }

    if (!v) {
        if (s_vol_count >= s_vol_capacity && !grow_table()) {
            pthread_mutex_unlock(&s_table_lock);
            return NULL;
        }
        if (s_vol_count < s_vol_capacity) {
            v         = &s_vols[s_vol_count];
            v->in_use = true;
            s_vol_count++;
        }
    }

    pthread_mutex_unlock(&s_table_lock);
    return v;
}

void release_slot(pcache_volume *v) {
    pthread_once(&s_once, global_init);
    pthread_mutex_lock(&s_table_lock);
    v->in_use = false;
    pthread_mutex_unlock(&s_table_lock);
}

pcache_volume *vol_from_handle(pcache_handle h) {
    if (h <= 0)
        return NULL;
    pthread_once(&s_once, global_init);
    pthread_mutex_lock(&s_table_lock);
    size_t idx   = (size_t)(h - 1);
    bool   valid = idx < s_vol_count;
    pthread_mutex_unlock(&s_table_lock);
    if (!valid)
        return NULL;
    pcache_volume *v = &s_vols[idx];
    pthread_mutex_lock(&v->mutex);
    pthread_mutex_lock(&s_table_lock);
    bool in_use = v->in_use;
    pthread_mutex_unlock(&s_table_lock);
    if (!in_use) {
        pthread_mutex_unlock(&v->mutex);
        return NULL;
    }
    return v;
}

pcache_handle handle_of(const pcache_volume *v) {
    return (pcache_handle)(v - s_vols + 1);
}
