#include <arpa/inet.h>
#include <machine/endian.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rtp.h"
#include "rtpsynth.h"

struct rsynth_inst {
    int srate;
    int ptime;
    long long ts_l;
    long long seq_l;
    struct rtp_hdr model;
};

void *
rsynth_ctor(int srate, int ptime)
{
    struct rsynth_inst *rip;

    rip = malloc(sizeof(struct rsynth_inst));
    if (rip == NULL)
        return (NULL);
    memset(rip, '\0', sizeof(struct rsynth_inst));
    rip->srate = srate;
    rip->ptime = ptime;
    rip->model.version = 2;
    rip->model.mbt = 1;
    rip->model.ssrc = random();
    rip->ts_l = random() & 0xfffffffe;
    rip->seq_l = random() & 0xffff;
    return ((void *)rip);
}

int
rsynth_next_pkt_pa(void *_rip, int plen, int pt, void *buf, unsigned int blen)
{
    struct rsynth_inst *rip;
    struct rtp_hdr *rnp;
    int rs;

    rip = (struct rsynth_inst *)_rip;
    rs = RTP_HDR_LEN(&rip->model) + plen;
    if (rs > blen)
        return (-1);
    rnp = (struct rtp_hdr *)buf;
    memset(rnp, '\0', rs);
    memcpy(rnp, &rip->model, sizeof(struct rtp_hdr));
    rnp->pt = pt;
    rnp->seq = htons(rip->seq_l);
    rnp->ts = htonl(rip->ts_l);
    rip->model.mbt = 0;
    rip->seq_l++;
    rip->ts_l++;

    return (rs);
}

void *
rsynth_next_pkt(void *_rip, int plen, int pt)
{
    struct rsynth_inst *rip;
    struct rtp_hdr *rnp;
    size_t rs;

    rip = (struct rsynth_inst *)_rip;
    rs = RTP_HDR_LEN(&rip->model) + plen;
    rnp = malloc(rs);
    if (rnp == NULL)
        return (NULL);
    rsynth_next_pkt_pa(_rip, plen, pt, rnp, rs);

    return (rnp);
}

void
rsynth_pkt_free(void *rnp)
{

    free(rnp);
}

void
rsynth_dtor(void *_rip)
{
    struct rsynth_inst *rip;

    rip = (struct rsynth_inst *)_rip;
    free(rip);
}
