/*
 * Copyright (C) 2012      Citrix Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#include <assert.h>
#include <errno.h>
#include <syslog.h>
#include <xenctrl.h>

#include "tapdisk-server.h"
#include "td-ctx.h"

LIST_HEAD(_td_xenio_ctxs);

/**
 * TODO releases a pool?
 */
static void
tapdisk_xenio_ctx_close(struct td_xenio_ctx * const ctx)
{
    assert(ctx);

    if (ctx->ring_event >= 0) {
        tapdisk_server_unregister_event(ctx->ring_event);
        ctx->ring_event = -1;
    }

    if (ctx->xce_handle) {
        xc_evtchn_close(ctx->xce_handle);
        ctx->xce_handle = NULL;
    }

    if (ctx->xcg_handle) {
        xc_evtchn_close(ctx->xcg_handle);
        ctx->xcg_handle = NULL;
    }

    list_del(&ctx->entry);

    /* TODO when do we free it? */
}

/*
 * XXX only called by tapdisk_xenio_ctx_ring_event
 */
static inline struct td_xenblkif *
xenio_pending_blkif(struct td_xenio_ctx * const ctx)
{
    evtchn_port_or_error_t port;
    struct td_xenblkif *blkif;
    int err;

    assert(ctx);

    /*
     * Get the local port for which there is a pending event.
     */
    port = xc_evtchn_pending(ctx->xce_handle);
    if (port == -1) {
        /* TODO log error */
        return NULL;
    }

    /*
     * Find the block interface with that local port.
     */
    tapdisk_xenio_ctx_find_blkif(ctx, blkif,
            blkif->port == port);
    if (blkif) {
        err = xc_evtchn_unmask(ctx->xce_handle, port);
        if (err) {
            /* TODO log error */
            return NULL;
        }
    }
    /*
     * TODO Is it possible to have an pending event channel but no block
     * interface associated with it?
     */

    return blkif;
}

#define blkif_get_req(dst, src)                 \
{                                               \
    int i, n = BLKIF_MAX_SEGMENTS_PER_REQUEST;  \
    dst->operation = src->operation;            \
    dst->nr_segments = src->nr_segments;        \
    dst->handle = src->handle;                  \
    dst->id = src->id;                          \
    dst->sector_number = src->sector_number;    \
    xen_rmb();                                  \
    if (n > dst->nr_segments)                   \
        n = dst->nr_segments;                   \
    for (i = 0; i < n; i++)                     \
        dst->seg[i] = src->seg[i];              \
}

/**
 * Utility function that retrieves a request using @idx as the ring index,
 * copying it to the @dst in a H/W independent way.
 *
 * @param blkif the block interface
 * @param dst address that receives the request
 * @param rc the index of the request in the ring
 */
static inline void
xenio_blkif_get_request(struct td_xenblkif * const blkif,
        blkif_request_t *const dst, const RING_IDX idx)
{
    blkif_back_rings_t * rings;

    assert(blkif);
    assert(dst);

    rings = &blkif->rings;

    switch (blkif->proto) {
        case BLKIF_PROTOCOL_NATIVE:
            {
                blkif_request_t *src;
                src = RING_GET_REQUEST(&rings->native, idx);
                memcpy(dst, src, sizeof(blkif_request_t));
                break;
            }

        case BLKIF_PROTOCOL_X86_32:
            {
                blkif_x86_32_request_t *src;
                src = RING_GET_REQUEST(&rings->x86_32, idx);
                blkif_get_req(dst, src);
                break;
            }

        case BLKIF_PROTOCOL_X86_64:
            {
                blkif_x86_64_request_t *src;
                src = RING_GET_REQUEST(&rings->x86_64, idx);
                blkif_get_req(dst, src);
                break;
            }

        default:
            /*
             * TODO log error
             */
            assert(0);
    }
}

/**
 * Retrieves at most @count request descriptors from the ring, copying them to
 * @reqs.
 *
 * @param blkif the block interface
 * @param reqs array of pointers where each element points to sufficient memory
 * space that receives each request descriptor
 * @param count retrieve at most that many request descriptors
 * @returns the number of retrieved request descriptors
 *
 *  XXX only called by xenio_blkif_get_requests
 */
static inline int
__xenio_blkif_get_requests(struct td_xenblkif * const blkif,
        blkif_request_t *reqs[], const unsigned int count)
{
    blkif_common_back_ring_t * ring;
    RING_IDX rp, rc;
    unsigned int n;

    assert(blkif);
    assert(reqs);

    if (!count)
        return 0;

    ring = &blkif->rings.common;

    rp = ring->sring->req_prod;
    xen_rmb(); /* TODO why? */

    for (rc = ring->req_cons, n = 0; rc != rp; rc++) {
        blkif_request_t *dst = reqs[n];

        if (n++ >= count)
            break;

        xenio_blkif_get_request(blkif, dst, rc);
    }

    ring->req_cons = rc;

    return n;
}

/**
 * Retrieves at most @count request descriptors.
 *
 * @param blkif the block interface
 * @param reqs array of pointers where each pointer points to sufficient
 * memory to hold a request descriptor
 * @count maximum number of request descriptors to retrieve
 * @param final re-enable notifications before it stops reading
 * @returns the number of request descriptors retrieved
 *
 * TODO change name
 */
static inline int
xenio_blkif_get_requests(struct td_xenblkif * const blkif,
        blkif_request_t *reqs[], const int count, const int final)
{
    blkif_common_back_ring_t * ring;
    int n = 0;
    int work = 0;

    assert(blkif);
    assert(reqs);
    assert(count > 0);

    ring = &blkif->rings.common;

    do {
        if (final)
            RING_FINAL_CHECK_FOR_REQUESTS(ring, work);
        else
            work = RING_HAS_UNCONSUMED_REQUESTS(ring);

        if (!work)
            break;

        if (n >= count)
            break;

        n += __xenio_blkif_get_requests(blkif, reqs + n, count - n);
    } while (1);

    return n;
}

/**
 * Callback executed when there is a request descriptor in the ring. Copies as
 * many request descriptors as possible (limited by local buffer space) to the
 * td_blkif's local request buffer and queues them to the tapdisk queue.
 */
static inline void
tapdisk_xenio_ctx_ring_event(event_id_t id __attribute__((unused)),
        char mode __attribute__((unused)), void *private)
{
    struct td_xenio_ctx *ctx = private;
    struct td_xenblkif *blkif = NULL;
    int n_reqs;
    int final = 0;
    int start;
    blkif_request_t **reqs;

    assert(ctx);

    blkif = xenio_pending_blkif(ctx);
    if (!blkif) {
        /* TODO log error */
        return;
    }

    start = blkif->n_reqs_free;
    blkif->stats.kicks.in++;

    /*
     * In each iteration, copy as many request descriptors from the shared ring
     * that can fit in the td_blkif's buffer.
     */
    do {
        reqs = &blkif->reqs_free[blkif->ring_size - blkif->n_reqs_free];

        assert(reqs);

        n_reqs = xenio_blkif_get_requests(blkif, reqs, blkif->n_reqs_free,
                final);
        assert(n_reqs >= 0);
        if (!n_reqs)
            break;

        blkif->n_reqs_free -= n_reqs;
        final = 1;

    } while (1);

    n_reqs = start - blkif->n_reqs_free;
    if (!n_reqs)
        /* TODO If there are no requests to be copied, why was there a
         * notification in the first place?
         */
        return;
    blkif->stats.reqs.in += n_reqs;
    reqs = &blkif->reqs_free[blkif->ring_size - start];
    tapdisk_xenblkif_queue_requests(blkif, reqs, n_reqs);
}

/* NB. may be NULL, but then the image must be bouncing I/O */
#define TD_XENBLKIF_DEFAULT_POOL "td-xenio-default"

/**
 * Opens a context on the specified pool.
 *
 * @param pool the pool, it can either be NULL or a non-zero length string
 * @returns 0 in success
 *
 * TODO The pool is ignored, we always open the default pool.
 */
static inline int
tapdisk_xenio_ctx_open(const char *pool)
{
    struct td_xenio_ctx *ctx;
    int fd, err;

    /* zero-length pool names are not allowed */
    if (pool && !strlen(pool))
        return EINVAL;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        err = errno;
        syslog(LOG_ERR, "cannot allocate memory");
        goto fail;
    }

    ctx->ring_event = -1; /* TODO is there a special value? */
    ctx->pool = TD_XENBLKIF_DEFAULT_POOL;
	INIT_LIST_HEAD(&ctx->blkifs);
    list_add(&ctx->entry, &_td_xenio_ctxs);

    ctx->xce_handle = xc_evtchn_open(NULL, 0);
    if (ctx->xce_handle == NULL) {
        err = errno;
        syslog(LOG_ERR, "failed to open the event channel driver: %s\n",
                strerror(err));
        goto fail;
    }

    ctx->xcg_handle = xc_gnttab_open(NULL, 0);
    if (ctx->xcg_handle == NULL) {
        err = errno;
        syslog(LOG_ERR, "failed to open the grant table driver: %s\n",
                strerror(err));
        goto fail;
    }

    fd = xc_evtchn_fd(ctx->xce_handle);
    if (fd < 0) {
        err = errno;
        syslog(LOG_ERR, "failed to get the event channel file descriptor: %s\n",
                strerror(err));
        goto fail;
    }

    ctx->ring_event = tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
        fd, 0, tapdisk_xenio_ctx_ring_event, ctx);
    if (ctx->ring_event < 0) {
        err = -ctx->ring_event;
        syslog(LOG_ERR, "failed to register event: %s\n", strerror(err));
        goto fail;
    }

    return 0;

fail:
    tapdisk_xenio_ctx_close(ctx);
    return err;
}


/**
 * Tells whether @ctx belongs to @pool.
 *
 * If no @pool is not specified and a default pool is set, @ctx is compared
 * against the default pool. Note that NULL is valid pool name value.
 */
static inline int
__td_xenio_ctx_match(struct td_xenio_ctx * ctx, const char *pool)
{
    if (unlikely(!pool)) {
        if (NULL != TD_XENBLKIF_DEFAULT_POOL)
            return !strcmp(ctx->pool, TD_XENBLKIF_DEFAULT_POOL);
        else
            return !ctx->pool;
    }

    return !strcmp(ctx->pool, pool);
}

#define tapdisk_xenio_find_ctx(_ctx, _cond)	\
	do {									\
		int found = 0;						\
		tapdisk_xenio_for_each_ctx(_ctx) {	\
			if (_cond) {					\
				found = 1;					\
				break;						\
			}								\
		}									\
		if (!found)							\
			_ctx = NULL;					\
	} while (0)

int
tapdisk_xenio_ctx_get(const char *pool, struct td_xenio_ctx ** _ctx)
{
    struct td_xenio_ctx *ctx;
    int err = 0;

    do {
        tapdisk_xenio_find_ctx(ctx, __td_xenio_ctx_match(ctx, pool));
        if (ctx) {
            *_ctx = ctx;
            return 0;
        }

        err = tapdisk_xenio_ctx_open(pool);
    } while (!err);

    return err;
}

void
tapdisk_xenio_ctx_put(struct td_xenio_ctx * ctx)
{
    if (list_empty(&ctx->blkifs))
        tapdisk_xenio_ctx_close(ctx);
}