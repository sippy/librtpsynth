#include <arpa/inet.h>

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rtp.h"
#include "rtp_info.h"
#include "rtpjbuf.h"

#define LRS_DEFAULT ((uint64_t)-1)

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
    uint64_t last_max_lseq;
    uint64_t lseq_mask;
    struct jitter_buffer jb;
    struct rtpjbuf_stats jbs;
    struct rtp_frame ers_frame;
};

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

struct rjb_udp_in_r
rtpjbuf_udp_in(void *_rjbp, const unsigned char *data, size_t size)
{
    struct rtpjbuf_inst *rjbp;
    struct rtp_frame *fp, *ifp, *ifp_pre;
    struct rjb_udp_in_r ruir = { 0 };

    rjbp = (struct rtpjbuf_inst *)_rjbp;
    fp = malloc(sizeof(struct rtp_frame));
    if (fp == NULL) {
        ruir.error = RJB_ENOMEM;
        return (ruir);
    }
    memset(fp, '\0', sizeof(struct rtp_frame));
    int perror = rtp_packet_parse_raw(data, size, &fp->rtp.info);
    if (perror != RTP_PARSER_OK) {
        free(fp);
        ruir.error = perror;
        return (ruir);
    }
    fp->type = RFT_RTP;
    fp->rtp.data = data;

    /* Check for SEQ wrap-out and convert SEQ to the logical SEQ */
    fp->rtp.lseq = rjbp->lseq_mask | fp->rtp.info.seq;

    if (rjbp->last_lseq == LRS_DEFAULT) {
        rjbp->last_max_lseq = rjbp->last_lseq = fp->rtp.lseq;
        ruir.ready = fp;
        return (ruir);
    }

    if (rjbp->last_max_lseq % 65536 < 536  && fp->rtp.info.seq > 65000) {
        /* Pre-wrap packet received after a wrap */
        fp->rtp.lseq -= 0x10000;
    } else if (rjbp->last_max_lseq > 65000 && fp->rtp.lseq < rjbp->last_max_lseq - 65000) {
        rjbp->lseq_mask += 0x10000;
        fp->rtp.lseq += 0x10000;
        rjbp->jbs.seq_wup += 1;
    }
    if (fp->rtp.lseq <= rjbp->last_lseq) {
        int ldist = rjbp->last_lseq - fp->rtp.lseq;
        if (ldist == 0)
            goto gotdup;
        rjbp->jbs.drop.late += 1;
        ruir.drop = fp;
        return (ruir);
    }
    if (rjbp->jb.head == NULL) {
        assert(rjbp->jb.size == 0);
        if (rjbp->last_lseq == fp->rtp.lseq - 1) {
            assert(rjbp->last_max_lseq < fp->rtp.lseq);
            assert(rjbp->last_lseq < fp->rtp.lseq);
            rjbp->last_max_lseq = rjbp->last_lseq = fp->rtp.lseq;
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
            assert(ifp == rjbp->jb.head);
            fp->next = ifp;
            rjbp->jb.head = fp;
        } else {
            ifp_pre->next = fp;
        }
    } else {
        ifp_pre->next = fp;
        assert (rjbp->last_max_lseq < fp->rtp.lseq);
        rjbp->last_max_lseq = fp->rtp.lseq;
    }
    rjbp->jb.size += 1;
    if (rjbp->jb.size == rjbp->jb.capacity) {
        fp = rjbp->jb.head;
        rjbp->jb.size -= 1;
        for (ifp = fp; ifp->next != NULL; ifp = ifp->next) {
            if (ifp->rtp.lseq + 1 != ifp->next->rtp.lseq)
                break;
            rjbp->jb.size -= 1;
        }
        rjbp->jb.head = ifp->next;
        assert(rjbp->last_lseq < fp->rtp.lseq);
        if (rjbp->last_lseq + 1 != fp->rtp.lseq) {
            rjbp->ers_frame.next = fp;
            rjbp->ers_frame.ers.lseq_start = rjbp->last_lseq + 1;
            rjbp->ers_frame.ers.lseq_end = fp->rtp.lseq - 1;
            fp = &rjbp->ers_frame;
        }
        rjbp->last_lseq = ifp->rtp.lseq;
        ifp->next = NULL;
        ruir.ready = fp;
        if (rjbp->jb.head == NULL)
            assert(rjbp->jb.size == 0);
        else
            assert(rjbp->jb.size > 0);
    }
    return (ruir);
gotdup:
    rjbp->jbs.drop.dup += 1;
    ruir.drop = fp;
    return (ruir);
}
