/*  Copyright 2016 Netflix.
 *
 *  Use and distribution licensed under the BSD license.  See
 *  the LICENSE file for full text.
 */

/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "memcached.h"
#include "storage.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>
#include <poll.h>

#include "base64.h"

#define LARGEST_ID POWER_LARGEST

typedef struct {
    void *c; /* original connection structure. still with source thread attached. */
    int sfd; /* client fd. */
    int buflen;
    int bufused;
    char *buf; /* output buffer */
} crawler_client_t;

typedef struct _crawler_module_t crawler_module_t;

typedef void (*crawler_eval_func)(crawler_module_t *cm, item *it, uint32_t hv, int slab_cls);
typedef int (*crawler_init_func)(crawler_module_t *cm, void *data); // TODO: init args?
typedef void (*crawler_deinit_func)(crawler_module_t *cm); // TODO: extra args?
typedef void (*crawler_doneclass_func)(crawler_module_t *cm, int slab_cls);
typedef void (*crawler_finalize_func)(crawler_module_t *cm);

typedef struct {
    crawler_init_func init; /* run before crawl starts */
    crawler_eval_func eval; /* runs on an item. */
    crawler_doneclass_func doneclass; /* runs once per sub-crawler completion. */
    crawler_finalize_func finalize; /* runs once when all sub-crawlers are done. */
    bool needs_lock; /* whether or not we need the LRU lock held when eval is called */
    bool needs_client; /* whether or not to grab onto the remote client */
} crawler_module_reg_t;

struct _crawler_module_t {
    void *data; /* opaque data pointer */
    crawler_client_t c;
    crawler_module_reg_t *mod;
    int status; /* flags/code/etc for internal module usage */
};

static int crawler_expired_init(crawler_module_t *cm, void *data);
static void crawler_expired_doneclass(crawler_module_t *cm, int slab_cls);
static void crawler_expired_finalize(crawler_module_t *cm);
static void crawler_expired_eval(crawler_module_t *cm, item *search, uint32_t hv, int i);

crawler_module_reg_t crawler_expired_mod = {
    .init = crawler_expired_init,
    .eval = crawler_expired_eval,
    .doneclass = crawler_expired_doneclass,
    .finalize = crawler_expired_finalize,
    .needs_lock = true,
    .needs_client = false,
};

static int crawler_metadump_init(crawler_module_t *cm, void *data);
static void crawler_metadump_eval(crawler_module_t *cm, item *search, uint32_t hv, int i);
static void crawler_metadump_finalize(crawler_module_t *cm);

crawler_module_reg_t crawler_metadump_mod = {
    .init = crawler_metadump_init,
    .eval = crawler_metadump_eval,
    .doneclass = NULL,
    .finalize = crawler_metadump_finalize,
    .needs_lock = false,
    .needs_client = true,
};

static int crawler_mgdump_init(crawler_module_t *cm, void *data);
static void crawler_mgdump_eval(crawler_module_t *cm, item *search, uint32_t hv, int i);
static void crawler_mgdump_finalize(crawler_module_t *cm);

crawler_module_reg_t crawler_mgdump_mod = {
    .init = crawler_mgdump_init,
    .eval = crawler_mgdump_eval,
    .doneclass = NULL,
    .finalize = crawler_mgdump_finalize,
    .needs_lock = false,
    .needs_client = true,
};

crawler_module_reg_t *crawler_mod_regs[4] = {
    &crawler_expired_mod,
    &crawler_expired_mod,
    &crawler_metadump_mod,
    &crawler_mgdump_mod,
};

static int lru_crawler_write(crawler_client_t *c);
crawler_module_t active_crawler_mod;
enum crawler_run_type active_crawler_type;

static crawler crawlers[LARGEST_ID];

static int crawler_count = 0;
static volatile int do_run_lru_crawler_thread = 0;
static int lru_crawler_initialized = 0;
static pthread_mutex_t lru_crawler_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  lru_crawler_cond = PTHREAD_COND_INITIALIZER;
#ifdef EXTSTORE
/* TODO: pass this around */
static void *storage;
#endif

/* Will crawl all slab classes a minimum of once per hour */
#define MAX_MAINTCRAWL_WAIT 60 * 60

/*** LRU CRAWLER THREAD ***/

#define LRU_CRAWLER_MINBUFSPACE 8192

static void lru_crawler_close_client(crawler_client_t *c) {
    //fprintf(stderr, "CRAWLER: Closing client\n");
    sidethread_conn_close(c->c);
    c->c = NULL;
    free(c->buf);
    c->buf = NULL;
}

static void lru_crawler_release_client(crawler_client_t *c) {
    //fprintf(stderr, "CRAWLER: Closing client\n");
    redispatch_conn(c->c);
    c->c = NULL;
    free(c->buf);
    c->buf = NULL;
}

static int lru_crawler_expand_buf(crawler_client_t *c) {
    c->buflen *= 2;
    char *nb = realloc(c->buf, c->buflen);
    if (nb == NULL) {
        return -1;
    }
    c->buf = nb;
    return 0;
}

static int crawler_expired_init(crawler_module_t *cm, void *data) {
    struct crawler_expired_data *d;
    if (data != NULL) {
        d = data;
        d->is_external = true;
        cm->data = data;
    } else {
        // allocate data.
        d = calloc(1, sizeof(struct crawler_expired_data));
        if (d == NULL) {
            return -1;
        }
        // init lock.
        pthread_mutex_init(&d->lock, NULL);
        d->is_external = false;
        d->start_time = current_time;

        cm->data = d;
    }
    pthread_mutex_lock(&d->lock);
    memset(&d->crawlerstats, 0, sizeof(crawlerstats_t) * POWER_LARGEST);
    for (int x = 0; x < POWER_LARGEST; x++) {
        d->crawlerstats[x].start_time = current_time;
        d->crawlerstats[x].run_complete = false;
    }
    pthread_mutex_unlock(&d->lock);
    return 0;
}

static void crawler_expired_doneclass(crawler_module_t *cm, int slab_cls) {
    struct crawler_expired_data *d = (struct crawler_expired_data *) cm->data;
    pthread_mutex_lock(&d->lock);
    d->crawlerstats[slab_cls].end_time = current_time;
    d->crawlerstats[slab_cls].run_complete = true;
    pthread_mutex_unlock(&d->lock);
}

static void crawler_expired_finalize(crawler_module_t *cm) {
    struct crawler_expired_data *d = (struct crawler_expired_data *) cm->data;
    pthread_mutex_lock(&d->lock);
    d->end_time = current_time;
    d->crawl_complete = true;
    pthread_mutex_unlock(&d->lock);

    if (!d->is_external) {
        free(d);
    }
}

/* I pulled this out to make the main thread clearer, but it reaches into the
 * main thread's values too much. Should rethink again.
 */
static void crawler_expired_eval(crawler_module_t *cm, item *search, uint32_t hv, int i) {
    struct crawler_expired_data *d = (struct crawler_expired_data *) cm->data;
    pthread_mutex_lock(&d->lock);
    crawlerstats_t *s = &d->crawlerstats[i];
    int is_flushed = item_is_flushed(search);
#ifdef EXTSTORE
    bool is_valid = true;
    if (search->it_flags & ITEM_HDR) {
        is_valid = storage_validate_item(storage, search);
    }
#endif
    if ((search->exptime != 0 && search->exptime < current_time)
        || is_flushed
#ifdef EXTSTORE
        || !is_valid
#endif
        ) {
        crawlers[i].reclaimed++;
        s->reclaimed++;

        if (settings.verbose > 1) {
            int ii;
            char *key = ITEM_key(search);
            fprintf(stderr, "LRU crawler found an expired item (flags: %d, slab: %d): ",
                search->it_flags, search->slabs_clsid);
            for (ii = 0; ii < search->nkey; ++ii) {
                fprintf(stderr, "%c", key[ii]);
            }
            fprintf(stderr, "\n");
        }
        if ((search->it_flags & ITEM_FETCHED) == 0 && !is_flushed) {
            crawlers[i].unfetched++;
        }
#ifdef EXTSTORE
        STORAGE_delete(storage, search);
#endif
        do_item_unlink_nolock(search, hv);
        do_item_remove(search);
    } else {
        s->seen++;
        refcount_decr(search);
        if (search->exptime == 0) {
            s->noexp++;
        } else if (search->exptime - current_time > 3599) {
            s->ttl_hourplus++;
        } else {
            rel_time_t ttl_remain = search->exptime - current_time;
            int bucket = ttl_remain / 60;
            if (bucket <= 60) {
                s->histo[bucket]++;
            }
        }
    }
    pthread_mutex_unlock(&d->lock);
}

static int crawler_metadump_init(crawler_module_t *cm, void *data) {
    cm->status = 0;
    return 0;
}

#define KADD(p, s) { \
    memcpy(p, s, sizeof(s)-1); \
    p += sizeof(s)-1; \
}
#define KSP(p) { \
    *p = ' '; \
    p++; \
}

static void crawler_metadump_eval(crawler_module_t *cm, item *it, uint32_t hv, int i) {
    int is_flushed = item_is_flushed(it);
#ifdef EXTSTORE
    bool is_valid = true;
    if (it->it_flags & ITEM_HDR) {
        is_valid = storage_validate_item(storage, it);
    }
#endif
    /* Ignore expired content. */
    if ((it->exptime != 0 && it->exptime < current_time)
        || is_flushed
#ifdef EXTSTORE
        || !is_valid
#endif
        ) {
        refcount_decr(it);
        return;
    }
    client_flags_t flags;
    FLAGS_CONV(it, flags);
    assert(it->nkey * 3 < LRU_CRAWLER_MINBUFSPACE/2);
    // unrolled snprintf for ~30% time improvement on fullspeed dump
    // key=%s exp=%ld la=%llu cas=%llu fetch=%s cls=%u size=%lu flags=%llu\n
    // + optional ext_page=%u and ext_offset=%u

    char *p = cm->c.buf + cm->c.bufused;
    char *start = p;
    KADD(p, "key=");
    p = uriencode_p(ITEM_key(it), p, it->nkey);
    KSP(p);

    KADD(p, "exp=");
    if (it->exptime == 0) {
        KADD(p, "-1 ");
    } else {
        p = itoa_64((long)(it->exptime + process_started), p);
        KSP(p);
    }

    KADD(p, "la=");
    p = itoa_u64((unsigned long long)(it->time + process_started), p);
    KSP(p);

    KADD(p, "cas=");
    p = itoa_u64(ITEM_get_cas(it), p);
    KSP(p);

    if (it->it_flags & ITEM_FETCHED) {
        KADD(p, "fetch=yes ");
    } else {
        KADD(p, "fetch=no ");
    }

    KADD(p, "cls=");
    p = itoa_u32(ITEM_clsid(it), p);
    KSP(p);

    KADD(p, "size=");
    p = itoa_u64(ITEM_ntotal(it), p);
    KSP(p);

    KADD(p, "flags=");
    p = itoa_u64(flags, p);
    KSP(p);

#ifdef EXTSTORE
    if (it->it_flags & ITEM_HDR) {
#ifdef NEED_ALIGN
    item_hdr hdr_s;
    memcpy(&hdr_s, ITEM_data(it), sizeof(hdr_s));
    item_hdr *hdr = &hdr_s;
#else
    item_hdr *hdr = (item_hdr *)ITEM_data(it);
#endif

    KADD(p, "ext_page=");
    p = itoa_u32(hdr->page_id, p);
    KSP(p);

    KADD(p, "ext_offset=");
    p = itoa_u32(hdr->offset, p);
    KSP(p);
    }
#endif

    KADD(p, "\n");

    refcount_decr(it);
    assert(p - start < LRU_CRAWLER_MINBUFSPACE-1);
    cm->c.bufused += p - start;
}

#undef KADD
#undef KSP

static void crawler_metadump_finalize(crawler_module_t *cm) {
    if (cm->c.c != NULL) {
        // flush any pending data.
        if (lru_crawler_write(&cm->c) == 0) {
            // Only nonzero status right now means we were locked
            if (cm->status != 0) {
                const char *errstr = "ERROR locked try again later\r\n";
                size_t errlen = strlen(errstr);
                memcpy(cm->c.buf, errstr, errlen);
                cm->c.bufused += errlen;
            } else {
                memcpy(cm->c.buf, "END\r\n", 5);
                cm->c.bufused += 5;
            }
        }
    }
}

static int crawler_mgdump_init(crawler_module_t *cm, void *data) {
    cm->status = 0;
    return 0;
}

static void crawler_mgdump_eval(crawler_module_t *cm, item *it, uint32_t hv, int i) {
    int is_flushed = item_is_flushed(it);
    /* Ignore expired content. */
    if ((it->exptime != 0 && it->exptime < current_time)
        || is_flushed) {
        refcount_decr(it);
        return;
    }

    char *p = cm->c.buf + cm->c.bufused; // buffer offset.
    char *start = p;
    memcpy(p, "mg ", 3);
    p += 3;
    if (it->it_flags & ITEM_KEY_BINARY) {
        p += base64_encode((unsigned char *) ITEM_key(it), it->nkey, (unsigned char*) p, LRU_CRAWLER_MINBUFSPACE/2);
        memcpy(p, " b\r\n", 4);
        p += 4;
    } else {
        memcpy(p, ITEM_key(it), it->nkey);
        p += it->nkey;
        memcpy(p, "\r\n", 2);
        p += 2;
    }
    int total = p - start;

    refcount_decr(it);
    cm->c.bufused += total;
}

static void crawler_mgdump_finalize(crawler_module_t *cm) {
    if (cm->c.c != NULL) {
        // flush any pending data.
        if (lru_crawler_write(&cm->c) == 0) {
            // Only nonzero status right now means we were locked
            if (cm->status != 0) {
                const char *errstr = "ERROR locked try again later\r\n";
                size_t errlen = strlen(errstr);
                memcpy(cm->c.buf, errstr, errlen);
                cm->c.bufused += errlen;
            } else {
                memcpy(cm->c.buf, "EN\r\n", 4);
                cm->c.bufused += 4;
            }
        }
    }
}

// write the whole buffer out to the client socket.
static int lru_crawler_write(crawler_client_t *c) {
    unsigned int data_size = c->bufused;
    unsigned int sent = 0;
    struct pollfd to_poll[1];
    to_poll[0].fd = c->sfd;
    to_poll[0].events = POLLOUT;

    if (c->c == NULL) return -1;
    if (data_size == 0) return 0;

    while (sent < data_size) {
        int ret = poll(to_poll, 1, 1000);

        if (ret < 0) {
            // fatal.
            lru_crawler_close_client(c);
            return -1;
        }

        if (ret == 0) return 0;

        // check if socket was closed on us.
        if (to_poll[0].revents & POLLIN) {
            char buf[1];
            int res = ((conn*)c->c)->read(c->c, buf, 1);
            if (res == 0 || (res == -1 && (errno != EAGAIN && errno != EWOULDBLOCK))) {
                lru_crawler_close_client(c);
                return -1;
            }
        }

        if (to_poll[0].revents & (POLLHUP|POLLERR)) {
            // got socket hangup.
            lru_crawler_close_client(c);
            return -1;
        } else if (to_poll[0].revents & POLLOUT) {
            // socket is writeable.
            int total = ((conn*)c->c)->write(c->c, c->buf + sent, data_size - sent);
            if (total == -1) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    lru_crawler_close_client(c);
                    return -1;
                }
            } else if (total == 0) {
                lru_crawler_close_client(c);
                return -1;
            }
            sent += total;
        }
    } // while

    // write buffer now empty.
    c->bufused = 0;

    return 0;
}

static void lru_crawler_class_done(int i) {
    crawlers[i].it_flags = 0;
    crawler_count--;
    do_item_unlinktail_q((item *)&crawlers[i]);
    do_item_stats_add_crawl(i, crawlers[i].reclaimed,
            crawlers[i].unfetched, crawlers[i].checked);
    pthread_mutex_unlock(&lru_locks[i]);
    if (active_crawler_mod.mod->doneclass != NULL)
        active_crawler_mod.mod->doneclass(&active_crawler_mod, i);
}

// ensure we build the buffer a little bit to cut down on poll/write syscalls.
#define MIN_ITEMS_PER_WRITE 16
static void item_crawl_hash(void) {
    // get iterator from assoc. can hang for a long time.
    // - blocks hash expansion
    void *iter = assoc_get_iterator();
    int crawls_persleep = settings.crawls_persleep;
    item *it = NULL;
    int items = 0;

    // Could not get the iterator: probably locked due to hash expansion.
    if (iter == NULL) {
        active_crawler_mod.status = 1;
        return;
    }

    // loop while iterator returns something
    // - iterator func handles bucket-walking
    // - iterator returns with bucket locked.
    while (assoc_iterate(iter, &it)) {
        // if iterator returns true but no item, we're inbetween buckets and
        // can do cleanup work without holding an item lock.
        if (it == NULL) {
            if (active_crawler_mod.c.c != NULL) {
                if (items > MIN_ITEMS_PER_WRITE) {
                    int ret = lru_crawler_write(&active_crawler_mod.c);
                    items = 0;
                    if (ret != 0) {
                        // fail out and finalize.
                        break;
                    }
                }
            } else if (active_crawler_mod.mod->needs_client) {
                // fail out and finalize.
                break;
            }

            // - sleep bits from orig loop
            if (crawls_persleep <= 0 && settings.lru_crawler_sleep) {
                pthread_mutex_unlock(&lru_crawler_lock);
                usleep(settings.lru_crawler_sleep);
                pthread_mutex_lock(&lru_crawler_lock);
                crawls_persleep = settings.crawls_persleep;
            } else if (!settings.lru_crawler_sleep) {
                // TODO: only cycle lock every N?
                pthread_mutex_unlock(&lru_crawler_lock);
                pthread_mutex_lock(&lru_crawler_lock);
            }
            continue;
        }

        // double check that the item isn't in a transitional state.
        if (refcount_incr(it) < 2) {
            refcount_decr(it);
            continue;
        }

        // We're presently holding an item lock, so we cannot flush the
        // buffer to the network socket as the syscall is both slow and could
        // hang waiting for POLLOUT. Instead we must expand the buffer.
        if (active_crawler_mod.c.c != NULL) {
            crawler_client_t *c = &active_crawler_mod.c;
            if (c->buflen - c->bufused < LRU_CRAWLER_MINBUFSPACE) {
                if (lru_crawler_expand_buf(c) != 0) {
                    // failed to expand buffer, stop.
                    break;
                }
            }
        }
        // FIXME: missing hv and i are fine for metadump eval, but not fine
        // for expire eval.
        active_crawler_mod.mod->eval(&active_crawler_mod, it, 0, 0);
        crawls_persleep--;
        items++;
    }

    // must finalize or we leave the hash table expansion blocked.
    assoc_iterate_final(iter);
    return;
}

static void *item_crawler_thread(void *arg) {
    int i;
    int crawls_persleep = settings.crawls_persleep;

    pthread_mutex_lock(&lru_crawler_lock);
    pthread_cond_signal(&lru_crawler_cond);
    settings.lru_crawler = true;
    if (settings.verbose > 2)
        fprintf(stderr, "Starting LRU crawler background thread\n");
    while (do_run_lru_crawler_thread) {
    pthread_cond_wait(&lru_crawler_cond, &lru_crawler_lock);

    if (crawler_count == -1) {
        item_crawl_hash();
        crawler_count = 0;
    } else {
    while (crawler_count) {
        item *search = NULL;
        void *hold_lock = NULL;

        for (i = POWER_SMALLEST; i < LARGEST_ID; i++) {
            if (crawlers[i].it_flags != 1) {
                continue;
            }

            if (active_crawler_mod.c.c != NULL) {
                crawler_client_t *c = &active_crawler_mod.c;
                if (c->buflen - c->bufused < LRU_CRAWLER_MINBUFSPACE) {
                    int ret = lru_crawler_write(c);
                    if (ret != 0) {
                        lru_crawler_class_done(i);
                        continue;
                    }
                }
            } else if (active_crawler_mod.mod->needs_client) {
                lru_crawler_class_done(i);
                continue;
            }
            pthread_mutex_lock(&lru_locks[i]);
            search = do_item_crawl_q((item *)&crawlers[i]);
            if (search == NULL ||
                (crawlers[i].remaining && --crawlers[i].remaining < 1)) {
                if (settings.verbose > 2)
                    fprintf(stderr, "Nothing left to crawl for %d\n", i);
                lru_crawler_class_done(i);
                continue;
            }
            uint32_t hv = hash(ITEM_key(search), search->nkey);
            /* Attempt to hash item lock the "search" item. If locked, no
             * other callers can incr the refcount
             */
            if ((hold_lock = item_trylock(hv)) == NULL) {
                pthread_mutex_unlock(&lru_locks[i]);
                continue;
            }
            /* Now see if the item is refcount locked */
            if (refcount_incr(search) != 2) {
                refcount_decr(search);
                if (hold_lock)
                    item_trylock_unlock(hold_lock);
                pthread_mutex_unlock(&lru_locks[i]);
                continue;
            }

            crawlers[i].checked++;
            /* Frees the item or decrements the refcount. */
            /* Interface for this could improve: do the free/decr here
             * instead? */
            if (!active_crawler_mod.mod->needs_lock) {
                pthread_mutex_unlock(&lru_locks[i]);
            }

            active_crawler_mod.mod->eval(&active_crawler_mod, search, hv, i);

            if (hold_lock)
                item_trylock_unlock(hold_lock);
            if (active_crawler_mod.mod->needs_lock) {
                pthread_mutex_unlock(&lru_locks[i]);
            }

            if (crawls_persleep-- <= 0 && settings.lru_crawler_sleep) {
                pthread_mutex_unlock(&lru_crawler_lock);
                usleep(settings.lru_crawler_sleep);
                pthread_mutex_lock(&lru_crawler_lock);
                crawls_persleep = settings.crawls_persleep;
            } else if (!settings.lru_crawler_sleep) {
                // TODO: only cycle lock every N?
                pthread_mutex_unlock(&lru_crawler_lock);
                pthread_mutex_lock(&lru_crawler_lock);
            }
        }
    } // while
    } // if crawler_count

    if (active_crawler_mod.mod != NULL) {
        if (active_crawler_mod.mod->finalize != NULL)
            active_crawler_mod.mod->finalize(&active_crawler_mod);
        while (active_crawler_mod.c.c != NULL && active_crawler_mod.c.bufused != 0) {
            lru_crawler_write(&active_crawler_mod.c);
        }
        // Double checking in case the client closed during the poll
        if (active_crawler_mod.c.c != NULL) {
            lru_crawler_release_client(&active_crawler_mod.c);
        }
        active_crawler_mod.mod = NULL;
    }

    if (settings.verbose > 2)
        fprintf(stderr, "LRU crawler thread sleeping\n");

    STATS_LOCK();
    stats_state.lru_crawler_running = false;
    STATS_UNLOCK();
    }
    pthread_mutex_unlock(&lru_crawler_lock);
    if (settings.verbose > 2)
        fprintf(stderr, "LRU crawler thread stopping\n");
    settings.lru_crawler = false;

    return NULL;
}

static pthread_t item_crawler_tid;

int stop_item_crawler_thread(bool wait) {
    int ret;
    pthread_mutex_lock(&lru_crawler_lock);
    if (do_run_lru_crawler_thread == 0) {
        pthread_mutex_unlock(&lru_crawler_lock);
        return 0;
    }
    do_run_lru_crawler_thread = 0;
    pthread_cond_signal(&lru_crawler_cond);
    pthread_mutex_unlock(&lru_crawler_lock);
    if (wait && (ret = pthread_join(item_crawler_tid, NULL)) != 0) {
        fprintf(stderr, "Failed to stop LRU crawler thread: %s\n", strerror(ret));
        return -1;
    }
    return 0;
}

/* Lock dance to "block" until thread is waiting on its condition:
 * caller locks mtx. caller spawns thread.
 * thread blocks on mutex.
 * caller waits on condition, releases lock.
 * thread gets lock, sends signal.
 * caller can't wait, as thread has lock.
 * thread waits on condition, releases lock
 * caller wakes on condition, gets lock.
 * caller immediately releases lock.
 * thread is now safely waiting on condition before the caller returns.
 */
int start_item_crawler_thread(void) {
    int ret;

    if (settings.lru_crawler)
        return -1;
    pthread_mutex_lock(&lru_crawler_lock);
    do_run_lru_crawler_thread = 1;
    if ((ret = pthread_create(&item_crawler_tid, NULL,
        item_crawler_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create LRU crawler thread: %s\n",
            strerror(ret));
        pthread_mutex_unlock(&lru_crawler_lock);
        return -1;
    }
    thread_setname(item_crawler_tid, "mc-itemcrawler");
    /* Avoid returning until the crawler has actually started */
    pthread_cond_wait(&lru_crawler_cond, &lru_crawler_lock);
    pthread_mutex_unlock(&lru_crawler_lock);

    return 0;
}

/* 'remaining' is passed in so the LRU maintainer thread can scrub the whole
 * LRU every time.
 */
static int do_lru_crawler_start(uint32_t id, uint32_t remaining) {
    uint32_t sid = id;
    int starts = 0;

    pthread_mutex_lock(&lru_locks[sid]);
    if (crawlers[sid].it_flags == 0) {
        if (settings.verbose > 2)
            fprintf(stderr, "Kicking LRU crawler off for LRU %u\n", sid);
        crawlers[sid].nbytes = 0;
        crawlers[sid].nkey = 0;
        crawlers[sid].it_flags = 1; /* For a crawler, this means enabled. */
        crawlers[sid].next = 0;
        crawlers[sid].prev = 0;
        crawlers[sid].time = 0;
        if (remaining == LRU_CRAWLER_CAP_REMAINING) {
            remaining = do_get_lru_size(sid);
        }
        /* Values for remaining:
         * remaining = 0
         * - scan all elements, until a NULL is reached
         * - if empty, NULL is reached right away
         * remaining = n + 1
         * - first n elements are parsed (or until a NULL is reached)
         */
        if (remaining) remaining++;
        crawlers[sid].remaining = remaining;
        crawlers[sid].slabs_clsid = sid;
        crawlers[sid].reclaimed = 0;
        crawlers[sid].unfetched = 0;
        crawlers[sid].checked = 0;
        do_item_linktail_q((item *)&crawlers[sid]);
        crawler_count++;
        starts++;
    }
    pthread_mutex_unlock(&lru_locks[sid]);
    return starts;
}

static int lru_crawler_set_client(crawler_module_t *cm, void *c, const int sfd) {
    crawler_client_t *crawlc = &cm->c;
    if (crawlc->c != NULL) {
        return -1;
    }
    crawlc->c = c;
    crawlc->sfd = sfd;

    size_t size = LRU_CRAWLER_MINBUFSPACE * 16;
    crawlc->buf = malloc(size);

    if (crawlc->buf == NULL) {
        return -2;
    }
    crawlc->buflen = size;
    crawlc->bufused = 0;
    return 0;
}

int lru_crawler_start(uint8_t *ids, uint32_t remaining,
                             const enum crawler_run_type type, void *data,
                             void *c, const int sfd) {
    int starts = 0;
    bool is_running;
    static rel_time_t block_ae_until = 0;
    pthread_mutex_lock(&lru_crawler_lock);
    STATS_LOCK();
    is_running = stats_state.lru_crawler_running;
    STATS_UNLOCK();
    if (do_run_lru_crawler_thread == 0) {
        pthread_mutex_unlock(&lru_crawler_lock);
        return -2;
    }

    if (is_running &&
            !(type == CRAWLER_AUTOEXPIRE && active_crawler_type == CRAWLER_AUTOEXPIRE)) {
        pthread_mutex_unlock(&lru_crawler_lock);
        block_ae_until = current_time + 60;
        return -1;
    }

    if (type == CRAWLER_AUTOEXPIRE && block_ae_until > current_time) {
        pthread_mutex_unlock(&lru_crawler_lock);
        return -1;
    }

    /* hash table walk only supported with metadump for now. */
    if (ids == NULL && type != CRAWLER_METADUMP && type != CRAWLER_MGDUMP) {
        pthread_mutex_unlock(&lru_crawler_lock);
        return -2;
    }

    /* Configure the module */
    if (!is_running) {
        assert(crawler_mod_regs[type] != NULL);
        active_crawler_mod.mod = crawler_mod_regs[type];
        active_crawler_type = type;
        if (active_crawler_mod.mod->init != NULL) {
            active_crawler_mod.mod->init(&active_crawler_mod, data);
        }
        if (active_crawler_mod.mod->needs_client) {
            if (c == NULL || sfd == 0) {
                pthread_mutex_unlock(&lru_crawler_lock);
                return -2;
            }
            if (lru_crawler_set_client(&active_crawler_mod, c, sfd) != 0) {
                pthread_mutex_unlock(&lru_crawler_lock);
                return -2;
            }
        }
    }

    if (ids == NULL) {
        /* NULL ids means to walk the hash table instead. */
        starts = 1;
        /* FIXME: hack to signal hash mode to the crawler thread.
         * Something more clear would be nice.
         */
        crawler_count = -1;
    } else {
        /* we allow the autocrawler to restart sub-LRU's before completion */
        for (int sid = POWER_SMALLEST; sid < POWER_LARGEST; sid++) {
            if (ids[sid])
                starts += do_lru_crawler_start(sid, remaining);
        }
    }
    if (starts) {
        STATS_LOCK();
        stats_state.lru_crawler_running = true;
        stats.lru_crawler_starts++;
        STATS_UNLOCK();
        pthread_cond_signal(&lru_crawler_cond);
    }
    pthread_mutex_unlock(&lru_crawler_lock);
    return starts;
}

/*
 * Also only clear the crawlerstats once per sid.
 */
enum crawler_result_type lru_crawler_crawl(char *slabs, const enum crawler_run_type type,
        void *c, const int sfd, unsigned int remaining) {
    char *b = NULL;
    uint32_t sid = 0;
    int starts = 0;
    uint8_t tocrawl[POWER_LARGEST];
    bool hash_crawl = false;

    /* FIXME: I added this while debugging. Don't think it's needed? */
    memset(tocrawl, 0, sizeof(uint8_t) * POWER_LARGEST);
    if (strcmp(slabs, "all") == 0) {
        for (sid = 0; sid < POWER_LARGEST; sid++) {
            tocrawl[sid] = 1;
        }
    } else if (strcmp(slabs, "hash") == 0) {
        hash_crawl = true;
    } else {
        for (char *p = strtok_r(slabs, ",", &b);
             p != NULL;
             p = strtok_r(NULL, ",", &b)) {

            if (!safe_strtoul(p, &sid) || sid < POWER_SMALLEST
                    || sid >= MAX_NUMBER_OF_SLAB_CLASSES) {
                return CRAWLER_BADCLASS;
            }
            tocrawl[sid | TEMP_LRU] = 1;
            tocrawl[sid | HOT_LRU] = 1;
            tocrawl[sid | WARM_LRU] = 1;
            tocrawl[sid | COLD_LRU] = 1;
        }
    }

    starts = lru_crawler_start(hash_crawl ? NULL : tocrawl, remaining, type, NULL, c, sfd);
    if (starts == -1) {
        return CRAWLER_RUNNING;
    } else if (starts == -2) {
        return CRAWLER_ERROR; /* FIXME: not very helpful. */
    } else if (starts) {
        return CRAWLER_OK;
    } else {
        return CRAWLER_NOTSTARTED;
    }
}

/* If we hold this lock, crawler can't wake up or move */
void lru_crawler_pause(void) {
    pthread_mutex_lock(&lru_crawler_lock);
}

void lru_crawler_resume(void) {
    pthread_mutex_unlock(&lru_crawler_lock);
}

int init_lru_crawler(void *arg) {
    if (lru_crawler_initialized == 0) {
#ifdef EXTSTORE
        storage = arg;
#endif
        active_crawler_mod.c.c = NULL;
        active_crawler_mod.mod = NULL;
        active_crawler_mod.data = NULL;
        lru_crawler_initialized = 1;
    }
    return 0;
}
