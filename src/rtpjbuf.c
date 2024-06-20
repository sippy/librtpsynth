#if !defined(_WIN32) && !defined(_WIN64)
#include <arpa/inet.h>
#else
#include "winnet.h"
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rtp.h"
#include "rtp_info.h"
#include "rtpjbuf.h"

#define LRS_DEFAULT ((uint64_t)-1)
#define LMS_DEFAULT LRS_DEFAULT

#define BOOLVAL(x)  (x)
#if !defined(_WIN32) && !defined(_WIN64)
#define x_unlikely(x) (__builtin_expect((x), 0), (x))
#else
#define x_unlikely(x) (x) [[unlikely]]
#endif

#if defined(_WIN32) || defined(_WIN64)
#pragma comment(linker, "/export:rtpjbuf_ctor")
#pragma comment(linker, "/export:rtpjbuf_dtor")
#pragma comment(linker, "/export:rtpjbuf_frame_dtor")
#pragma comment(linker, "/export:rtpjbuf_udp_in")
#pragma comment(linker, "/export:rtpjbuf_flush")
#endif

struct jitter_buffer {
    struct rtp_frame *head;
    unsigned int size;
    unsigned int capacity;
};

struct rtpjbuf_stats {
    struct {
        uint64_t dup;
        uint64_t late;
        uint64_t perror;
    } drop;
    uint64_t seq_wup;
};

struct rtpjbuf_inst {
    uint64_t last_lseq;
    uint32_t last_ts;
    uint64_t last_max_lseq;
    uint64_t lseq_mask;
    struct jitter_buffer jb;
    struct rtpjbuf_stats jbs;
    struct rtp_frame ers_frame;
};

static void
d_assert(int invar)
{
    if x_unlikely(!invar)
        abort();
}

void *
rtpjbuf_ctor(unsigned int capacity)
{
    struct rtpjbuf_inst *rjbp;

    rjbp = malloc(sizeof(struct rtpjbuf_inst));
    if (rjbp == NULL)
        return (NULL);
    memset(rjbp, '\0', sizeof(struct rtpjbuf_inst));
    rjbp->jb.capacity = capacity;
    rjbp->last_lseq = LRS_DEFAULT;
    rjbp->last_max_lseq = LMS_DEFAULT;
    rjbp->ers_frame.type = RFT_ERS;
    return ((void *)rjbp);
}

void
rtpjbuf_frame_dtor(void *_rfp)
{

    free(_rfp);
}

void
rtpjbuf_dtor(void *_rjbp)
{
    struct rtpjbuf_inst *rjbp;

    rjbp = (struct rtpjbuf_inst *)_rjbp;
    for (struct rtp_frame *rfp = rjbp->jb.head; rfp != NULL;) {
        struct rtp_frame *rfp_next = rfp->next;
        rtpjbuf_frame_dtor(rfp);
        rfp = rfp_next;
    }
    free(rjbp);
}

static struct rtp_frame *
insert_ers_frame(struct rtpjbuf_inst *rjbp, struct rtp_frame *fp)
{
    uint32_t ts_diff, lseq_diff;

    if (rjbp->last_lseq + 1 == fp->rtp.lseq)
        return (fp);
    rjbp->ers_frame.next = fp;
    rjbp->ers_frame.ers.lseq_start = rjbp->last_lseq + 1;
    rjbp->ers_frame.ers.lseq_end = fp->rtp.lseq - 1;
    if (rjbp->last_ts > fp->rtp.info.ts) {
        uint64_t ts_diff64 = (uint64_t)0x100000000 + fp->rtp.info.ts - rjbp->last_ts;
        d_assert(ts_diff64 < 0x100000000);
        ts_diff = ts_diff64;
    } else {
        ts_diff = fp->rtp.info.ts - rjbp->last_ts;
    }
    lseq_diff = rjbp->ers_frame.ers.lseq_end - rjbp->ers_frame.ers.lseq_start + 1;
    rjbp->ers_frame.ers.ts_diff = ts_diff * lseq_diff / (lseq_diff + 1);
    return (&rjbp->ers_frame);
}

static void
save_last(struct rtpjbuf_inst *rjbp, const struct rtp_packet *rp)
{

    d_assert(rjbp->last_lseq == LRS_DEFAULT || rjbp->last_lseq < rp->lseq);
    rjbp->last_lseq = rp->lseq;
    rjbp->last_ts = rp->info.ts;
}

struct rjb_udp_in_r
rtpjbuf_udp_in(void *_rjbp, const unsigned char *data, size_t size)
{
    struct rtpjbuf_inst *rjbp;
    struct rtp_frame *fp, *ifp, *ifp_pre;
    struct rjb_udp_in_r ruir = { 0 };

    rjbp = (struct rtpjbuf_inst *)_rjbp;
    fp = malloc(sizeof(struct rtp_frame));
    if x_unlikely(fp == NULL) {
        ruir.error = RJB_ENOMEM;
        return (ruir);
    }
    memset(fp, '\0', sizeof(struct rtp_frame));
    int perror = rtp_packet_parse_raw(data, size, &fp->rtp.info);
    if x_unlikely(perror != RTP_PARSER_OK) {
        free(fp);
        rjbp->jbs.drop.perror += 1;
        ruir.error = perror;
        return (ruir);
    }
    fp->type = RFT_RTP;
    fp->rtp.data = data;

    /* Check for SEQ wrap-out and convert SEQ to the logical SEQ */
    fp->rtp.lseq = rjbp->lseq_mask | fp->rtp.info.seq;

    int warm_up = BOOLVAL(rjbp->last_lseq == LRS_DEFAULT);
    if x_unlikely(warm_up) {};
    int lms_warm_up = BOOLVAL(rjbp->last_max_lseq == LMS_DEFAULT);
    if x_unlikely(lms_warm_up) {
        d_assert(rjbp->jb.head == NULL);
        goto lms_init;
    }

    d_assert(rjbp->jb.head == NULL || warm_up || rjbp->jb.head->rtp.lseq - 1 > rjbp->last_lseq);

    if x_unlikely(rjbp->last_max_lseq % 65536 < 536  && fp->rtp.info.seq > 65000) {
        /* Pre-wrap packet received after a wrap */
        fp->rtp.lseq -= 0x10000;
    } else if x_unlikely(rjbp->last_max_lseq > 65000 && fp->rtp.lseq < rjbp->last_max_lseq - 65000) {
        rjbp->lseq_mask += 0x10000;
        fp->rtp.lseq += 0x10000;
        rjbp->jbs.seq_wup += 1;
    }
    if x_unlikely(!warm_up && fp->rtp.lseq <= rjbp->last_lseq) {
        int ldist = rjbp->last_lseq - fp->rtp.lseq;
        if (ldist == 0)
            goto gotdup;
        rjbp->jbs.drop.late += 1;
        ruir.drop = fp;
        return (ruir);
    }
    if (rjbp->jb.head == NULL) {
        d_assert(rjbp->jb.size == 0);
        d_assert(rjbp->last_max_lseq < fp->rtp.lseq);
lms_init:
        rjbp->last_max_lseq = fp->rtp.lseq;
        if (!warm_up && rjbp->last_lseq == fp->rtp.lseq - 1) {
            save_last(rjbp, &fp->rtp);
            ruir.ready = fp;
        } else if x_unlikely(warm_up && fp->rtp.lseq == 0) {
            d_assert(rjbp->last_lseq == LRS_DEFAULT);
            save_last(rjbp, &fp->rtp);
            ruir.ready = fp;
        } else {
            rjbp->jb.head = fp;
            rjbp->jb.size = 1;
        }
        return (ruir);
    }
    ifp_pre = NULL;
    for (ifp = rjbp->jb.head; ifp != NULL; ifp = ifp->next) {
        if (ifp->rtp.lseq < fp->rtp.lseq) {
            ifp_pre = ifp;
            continue;
        }
        if (ifp->rtp.lseq == fp->rtp.lseq)
            goto gotdup;
        break;
    }
    if (ifp != NULL) {
        fp->next = ifp;
        if (ifp_pre == NULL) {
            d_assert(ifp == rjbp->jb.head);
            fp->next = ifp;
            rjbp->jb.head = fp;
        } else {
            ifp_pre->next = fp;
        }
    } else {
        ifp_pre->next = fp;
        d_assert (rjbp->last_max_lseq < fp->rtp.lseq);
        rjbp->last_max_lseq = fp->rtp.lseq;
    }
    rjbp->jb.size += 1;
    int flush = BOOLVAL(!warm_up && rjbp->jb.head->rtp.lseq == rjbp->last_lseq + 1);
    if (rjbp->jb.size == rjbp->jb.capacity || flush) {
        fp = rjbp->jb.head;
        rjbp->jb.size -= 1;
        for (ifp = fp; ifp->next != NULL; ifp = ifp->next) {
            if (ifp->rtp.lseq + 1 != ifp->next->rtp.lseq)
                break;
            rjbp->jb.size -= 1;
        }
        rjbp->jb.head = ifp->next;
        d_assert(warm_up || rjbp->last_lseq < fp->rtp.lseq);
        d_assert(!warm_up || rjbp->last_lseq == LMS_DEFAULT);
        if (!warm_up)
            fp = insert_ers_frame(rjbp, fp);
        save_last(rjbp, &ifp->rtp);
        ifp->next = NULL;
        ruir.ready = fp;
        if (rjbp->jb.head == NULL)
            d_assert(rjbp->jb.size == 0);
        else
            d_assert(rjbp->jb.size > 0);
    }
    return (ruir);
gotdup:
    rjbp->jbs.drop.dup += 1;
    ruir.drop = fp;
    return (ruir);
}

struct rjb_udp_in_r
rtpjbuf_flush(void *_rjbp)
{
    struct rtpjbuf_inst *rjbp;
    struct rtp_frame *fp, *ifp;
    struct rjb_udp_in_r ruir = { 0 };
    rjbp = (struct rtpjbuf_inst *)_rjbp;

    if (rjbp->jb.head == NULL)
        return (ruir);
    d_assert(rjbp->jb.head->rtp.lseq - 1 > rjbp->last_lseq);
    fp = rjbp->jb.head;
    for (ifp = fp; ifp->next != NULL; ifp = ifp->next) {
resume:
        if (ifp->rtp.lseq + 1 != ifp->next->rtp.lseq) {
            struct rtp_frame *tfp = ifp->next;
            ifp->next = ruir.drop;
            ruir.drop = fp;
            ifp = fp = tfp;
            if (fp->next == NULL)
                break;
            goto resume;
        }
    }
    ruir.ready = insert_ers_frame(rjbp, fp);
    save_last(rjbp, &ifp->rtp);
    rjbp->jb.head = NULL;
    rjbp->jb.size = 0;
    return (ruir);
}
