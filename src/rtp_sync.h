#pragma once

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    int done;
    int status;
} rtp_sync_waiter;

typedef struct {
    void **head;
    void **tail;
    size_t next_off;
} rtp_sync_cmdq;

typedef struct {
    pthread_cond_t *cv;
    pthread_mutex_t *lock;
    clockid_t *clock_id;
} rtp_sync_cond_ctx;

int rtp_sync_waiter_init(rtp_sync_waiter *waiter);
void rtp_sync_waiter_destroy(rtp_sync_waiter *waiter);
int rtp_sync_waiter_reset(rtp_sync_waiter *waiter);
void rtp_sync_waiter_complete(rtp_sync_waiter *waiter, int status);
int rtp_sync_waiter_wait(rtp_sync_waiter *waiter);

void rtp_sync_cmdq_push(rtp_sync_cmdq *cmdq, void *cmd);
void *rtp_sync_cmdq_detach_all(rtp_sync_cmdq *cmdq);

int rtp_sync_cond_timedwait_abs_ns(rtp_sync_cond_ctx *cond_ctx, uint64_t abs_ns);
int rtp_sync_cond_timedwait_ns(rtp_sync_cond_ctx *cond_ctx, uint64_t wait_ns);
int rtp_sync_cond_init_monotonic(rtp_sync_cond_ctx *cond_ctx);
