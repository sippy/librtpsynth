#if !defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE < 200809L)
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "rtp_sync.h"

#if defined(CLOCK_MONOTONIC) && !defined(__APPLE__) && !defined(__MACH__) && !defined(_WIN32)
#define RTP_SYNC_HAVE_MONOTONIC_COND_CLOCK 1
#else
#define RTP_SYNC_HAVE_MONOTONIC_COND_CLOCK 0
#endif

#if !RTP_SYNC_HAVE_MONOTONIC_COND_CLOCK
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC warning "rtp_sync: monotonic pthread_cond clock support is disabled at compile time; falling back to CLOCK_REALTIME."
#endif
#endif

int
rtp_sync_waiter_init(rtp_sync_waiter *waiter)
{
    assert(waiter != NULL);
    if (pthread_mutex_init(&waiter->lock, NULL) != 0)
        return -1;
    if (pthread_cond_init(&waiter->cv, NULL) != 0) {
        pthread_mutex_destroy(&waiter->lock);
        return -1;
    }
    waiter->done = 0;
    waiter->status = 0;
    return 0;
}

void
rtp_sync_waiter_destroy(rtp_sync_waiter *waiter)
{
    assert(waiter != NULL);
    pthread_cond_destroy(&waiter->cv);
    pthread_mutex_destroy(&waiter->lock);
}

int
rtp_sync_waiter_reset(rtp_sync_waiter *waiter)
{
    if (pthread_mutex_lock(&waiter->lock) != 0)
        return -1;
    waiter->done = 0;
    waiter->status = 0;
    pthread_mutex_unlock(&waiter->lock);
    return 0;
}

void
rtp_sync_waiter_complete(rtp_sync_waiter *waiter, int status)
{
    int rc;

    assert(waiter != NULL);
    rc = pthread_mutex_lock(&waiter->lock);
    assert(rc == 0);
    if (rc != 0)
        return;
    waiter->status = status;
    waiter->done = 1;
    pthread_cond_signal(&waiter->cv);
    rc = pthread_mutex_unlock(&waiter->lock);
    assert(rc == 0);
}

int
rtp_sync_waiter_wait(rtp_sync_waiter *waiter)
{
    int status = -1;
    assert(waiter != NULL);
    if (pthread_mutex_lock(&waiter->lock) != 0)
        return status;
    while (!waiter->done)
        (void)pthread_cond_wait(&waiter->cv, &waiter->lock);
    status = waiter->status;
    pthread_mutex_unlock(&waiter->lock);
    return status;
}

void
rtp_sync_cmdq_push(rtp_sync_cmdq *cmdq, void *cmd)
{
    void **cmd_next;

    assert(cmdq != NULL);
    assert(cmdq->head != NULL);
    assert(cmdq->tail != NULL);
    assert(cmd != NULL);

    cmd_next = (void **)(((char *)cmd) + cmdq->next_off);
    *cmd_next = NULL;

    if (*cmdq->tail != NULL) {
        void **tail_next = (void **)(((char *)*cmdq->tail) + cmdq->next_off);
        *tail_next = cmd;
        *cmdq->tail = cmd;
    } else {
        *cmdq->head = cmd;
        *cmdq->tail = cmd;
    }
}

void *
rtp_sync_cmdq_detach_all(rtp_sync_cmdq *cmdq)
{
    void *out;

    assert(cmdq != NULL);
    assert(cmdq->head != NULL);
    assert(cmdq->tail != NULL);

    out = *cmdq->head;
    *cmdq->head = NULL;
    *cmdq->tail = NULL;
    return out;
}

int
rtp_sync_cond_timedwait_abs_ns(rtp_sync_cond_ctx *cond_ctx, uint64_t abs_ns)
{
    struct timespec ts;

    assert(cond_ctx != NULL);
    assert(cond_ctx->cv != NULL);
    assert(cond_ctx->lock != NULL);
    assert(cond_ctx->clock_id != NULL);

    ts.tv_sec = (time_t)(abs_ns / 1000000000ULL);
    ts.tv_nsec = (long)(abs_ns % 1000000000ULL);
    return pthread_cond_timedwait(cond_ctx->cv, cond_ctx->lock, &ts);
}

int
rtp_sync_cond_timedwait_ns(rtp_sync_cond_ctx *cond_ctx, uint64_t wait_ns)
{
    struct timespec ts;
    uint64_t abs_ns;
    uint64_t sec;

    assert(cond_ctx != NULL);
    assert(cond_ctx->cv != NULL);
    assert(cond_ctx->lock != NULL);
    assert(cond_ctx->clock_id != NULL);

    if (clock_gettime(*cond_ctx->clock_id, &ts) != 0)
        return -1;

    sec = wait_ns / 1000000000ULL;
    ts.tv_sec += (time_t)sec;
    ts.tv_nsec += (long)(wait_ns % 1000000000ULL);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    abs_ns = ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
    return rtp_sync_cond_timedwait_abs_ns(cond_ctx, abs_ns);
}

int
rtp_sync_cond_init_monotonic(rtp_sync_cond_ctx *cond_ctx)
{
    pthread_condattr_t cond_attr;

    assert(cond_ctx != NULL);
    assert(cond_ctx->cv != NULL);
    assert(cond_ctx->clock_id != NULL);

    *cond_ctx->clock_id = CLOCK_REALTIME;
    if (pthread_condattr_init(&cond_attr) != 0)
        return -1;
#if RTP_SYNC_HAVE_MONOTONIC_COND_CLOCK
    if (pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC) == 0)
        *cond_ctx->clock_id = CLOCK_MONOTONIC;
#endif
    if (pthread_cond_init(cond_ctx->cv, &cond_attr) != 0) {
        pthread_condattr_destroy(&cond_attr);
        return -1;
    }
    pthread_condattr_destroy(&cond_attr);
    return 0;
}
