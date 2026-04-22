#include "handle.h"

static pcache_volume   s_vols[PCACHE_MAX_HANDLES];
static pthread_mutex_t s_table_lock;
static pthread_once_t  s_once = PTHREAD_ONCE_INIT;

static void global_init(void) {
    pthread_mutex_init(&s_table_lock, NULL);
    memset(s_vols, 0, sizeof s_vols);
    for (int i = 0; i < PCACHE_MAX_HANDLES; i++)
        pthread_mutex_init(&s_vols[i].mutex, NULL);
}

pcache_volume *alloc_slot(void) {
    pthread_once(&s_once, global_init);
    pthread_mutex_lock(&s_table_lock);
    pcache_volume *v = NULL;
    for (int i = 0; i < PCACHE_MAX_HANDLES && !v; i++) {
        if (!s_vols[i].in_use) {
            s_vols[i].in_use = true;
            v                = &s_vols[i];
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
    if (h <= 0 || h > PCACHE_MAX_HANDLES)
        return NULL;
    pcache_volume *v = &s_vols[h - 1];
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
