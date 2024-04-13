#pragma once

enum rtp_frame_type { RFT_RTP = 0, RFT_ERS = 1};

struct rtp_packet {
    struct rtp_info info;
    uint64_t lseq;
    const unsigned char *data;
};

struct ers_frame {
    uint64_t lseq_start;
    uint64_t lseq_end;
    uint32_t ts_diff;
};

struct rtp_frame {
    enum rtp_frame_type type;
    union {
        struct rtp_packet rtp;
        struct ers_frame ers;
    };
    struct rtp_frame *next;
};

struct rjb_udp_in_r {
    int error;
    struct rtp_frame *ready;
    struct rtp_frame *drop;
};

#define RJB_ENOMEM (RTP_PARSER_IPS-1000)

void *rtpjbuf_ctor(unsigned int capacity);
void rtpjbuf_dtor(void *_rjbp);
void rtpjbuf_frame_dtor(void *_rfp);
struct rjb_udp_in_r rtpjbuf_udp_in(void *_rjbp, const unsigned char *data, size_t size);
struct rjb_udp_in_r rtpjbuf_flush(void *_rjbp);
