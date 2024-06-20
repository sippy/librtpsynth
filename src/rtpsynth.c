/*
 # Copyright (c) 2018 Sippy Software, Inc. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions are met:
 #
 # * Redistributions of source code must retain the above copyright notice, this
 #   list of conditions and the following disclaimer.
 #
 # * Redistributions in binary form must reproduce the above copyright notice,
 #   this list of conditions and the following disclaimer in the documentation
 #   and/or other materials provided with the distribution.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 # AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 # DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 # FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 # DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 # SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 # CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 # OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#if defined(_WIN32) || defined(_WIN64)
#define _CRT_RAND_S
#pragma comment(linker, "/export:rsynth_ctor")
#pragma comment(linker, "/export:rsynth_next_pkt")
#pragma comment(linker, "/export:rsynth_next_pkt_pa")
#pragma comment(linker, "/export:rsynth_skip")
#pragma comment(linker, "/export:rsynth_pkt_free")
#pragma comment(linker, "/export:rsynth_dtor")
#pragma comment(linker, "/export:rsynth_set_mbt")
#pragma comment(linker, "/export:rsynth_resync")
#endif

#define _DEFAULT_SOURCE

#if !defined(_WIN32) && !defined(_WIN64)
#include <arpa/inet.h>
#else
#include "winnet.h"
#endif

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32) || defined(_WIN64)
static long
random(void)
{
    unsigned int r;

    rand_s(&r);
    return r;
}
#endif

#include "rtp.h"
#include "rtpsynth.h"
#include "rsth_timeops.h"

struct rsynth_inst {
    int srate;
    int ptime;
    struct rsynth_seq l;
    int ts_inc;
    struct timespec last_ts;
    struct rtp_hdr model;
};

#if defined(_WIN32) || defined(_WIN64)
#include <profileapi.h>

static int
clock_gettime_monotonic(struct timespec *tv)
{
    static LARGE_INTEGER ticksPerSec;
    LARGE_INTEGER ticks;

    if (!ticksPerSec.QuadPart) {
        QueryPerformanceFrequency(&ticksPerSec);
        if (!ticksPerSec.QuadPart) {
            errno = ENOTSUP;
            return -1;
        }
    }

    QueryPerformanceCounter(&ticks);

    tv->tv_sec = (long)(ticks.QuadPart / ticksPerSec.QuadPart);
    tv->tv_nsec = (long)(((ticks.QuadPart % ticksPerSec.QuadPart) * NSEC_IN_SEC) / ticksPerSec.QuadPart);

    return 0;
}
#define clock_gettime(_, x) clock_gettime_monotonic(x)
#endif

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
    rip->ts_inc = 80 * ptime / 10;
    rip->model.version = 2;
    rip->model.mbt = 1;
    rip->model.ssrc = random();
    rip->l.ts = random() & 0xfffffffe;
    rip->l.seq = random() & 0xffff;
    (void)clock_gettime(CLOCK_MONOTONIC, &rip->last_ts);
    return ((void *)rip);
}

int
rsynth_next_pkt_pa(void *_rip, int plen, int pt, char *buf, unsigned int blen,
  int filled)
{
    struct rsynth_inst *rip;
    struct rtp_hdr *rnp;
    unsigned int rs, hl;

    rip = (struct rsynth_inst *)_rip;
    hl = RTP_HDR_LEN(&rip->model);
    rs = hl + plen;
    if (rs > blen)
        return (-1);
    rnp = (struct rtp_hdr *)buf;
    if (filled == 0) {
        memset(buf + sizeof(struct rtp_hdr), '\0', blen - sizeof(struct rtp_hdr));
    } else {
        memmove(buf + hl, buf, plen);
        memset(buf + hl + plen, '\0', blen - hl - plen);
    }

    memcpy(rnp, &rip->model, sizeof(struct rtp_hdr));
    rnp->pt = pt;
    rnp->seq = htons(rip->l.seq);
    rnp->ts = htonl(rip->l.ts);
    rip->model.mbt = 0;
    rip->l.seq++;
    rip->l.ts += rip->ts_inc;

    (void)clock_gettime(CLOCK_MONOTONIC, &rip->last_ts);

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
    rsynth_next_pkt_pa(_rip, plen, pt, (char *)rnp, rs, 0);

    return (rnp);
}

void
rsynth_skip(void *_rip, int npkts)
{
    struct rsynth_inst *rip;

    rip = (struct rsynth_inst *)_rip;
    rip->l.ts += rip->ts_inc * npkts;
    return;
}

unsigned int
rsynth_set_mbt(void *_rip, unsigned int new_st)
{
    struct rsynth_inst *rip;
    unsigned int old_st;

    rip = (struct rsynth_inst *)_rip;
    old_st = rip->model.mbt;
    rip->model.mbt = new_st;
    return (old_st);
}

void
rsynth_resync(void *_rip, struct rsynth_seq *rsp)
{
    struct timespec curr_ts;
    struct rsynth_inst *rip;

    rip = (struct rsynth_inst *)_rip;
    if (rsp != NULL) {
        *rsp = rip->l;
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &curr_ts);
    timespecsub(&curr_ts, &rip->last_ts);
    rip->l.ts += timespec2un64time(&curr_ts) * rip->srate / NSEC_IN_SEC;
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
