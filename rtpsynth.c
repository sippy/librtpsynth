#include <machine/endian.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rtp.h"
#include "rtpsynth.h"

struct rsynth_inst {
    int srate;
    int ptime;
    struct rtp_hdr model;
};

void *
rsynth_ctor(int srate, int ptime)
{
    struct rsynth_inst *rip;

    rip = malloc(sizeof(struct rsynth_inst));
    if (rip == NULL)
        return (NULL);
    memset(&rip, '\0', sizeof(struct rsynth_inst));
    rip->srate = srate;
    rip->ptime = ptime;
    rip->model.version = 2;
    rip->model.mbt = 1;
    rip->model.ts = random() & 0xfffffffe;
    rip->model.seq = random() & 0xffff;
    rip->model.ssrc = random();
    return ((void *)rip);
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
    memset(rnp, '\0', rs);
    memcpy(rnp, &rip->model, sizeof(struct rtp_hdr));
    rnp->pt = pt;
    rip->model.mbt = 0;

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
