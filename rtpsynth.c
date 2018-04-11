#include <machine/endian.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rtp.h"
#include "rtpsynth.h"

struct rsynth_inst {
    int srate;
    int ptime;
    int plen;
    struct rtp_hdr model;
};

void *
rsynth_ctor(int srate, int ptime, int plen, int pt)
{
    struct rsynth_inst *rip;

    rip = malloc(sizeof(struct rsynth_inst));
    if (rip == NULL)
        return (NULL);
    memset(&rip, '\0', sizeof(struct rsynth_inst));
    rip->srate = srate;
    rip->ptime = ptime;
    rip->plen = plen;
    rip->model.version = 2;
    rip->model.mbt = 1;
    rip->model.pt = pt;
    rip->model.ts = random() & 0xfffffffe;
    rip->model.seq = random() & 0xffff;
    rip->model.ssrc = random();
    return ((void *)rip);
}

void *
rsynth_next(void *_rip)
{
    struct rsynth_inst *rip;

    rip = (struct rsynth_inst *)_rip;
    return (NULL);
}

void
rsynth_dtor(void *_rip)
{
    struct rsynth_inst *rip;

    rip = (struct rsynth_inst *)_rip;
    free(rip);
}
