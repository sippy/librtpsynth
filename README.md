# librtpsynth

Low-level library optimized to generate and/or consume sequence of the RTP
packets. Based on some code and ideas from the RTPProxy projects.

Originally designed to supplement Python code to do low-level bits shuffling
for the proof of concept IoT implementation.

Reasonably fast, 500-700x real-time (i.e. 0.5-1M packets per second) when
used from the Python code. 20x of that (10-20M PPS) if used from C code
directly.

## RTP Generation

Generate continuous sequence of RTP packets of the same payload type.

### C API Functions

`#include <rtpsynth.h>`

- `void *rsynth_ctor(int srate, int ptime);`
  Initializes the RTP synthesizer with given sample rate and packet time.
  Returns a handle to be used in other calls.

- `void *rsynth_next_pkt(void *ri, int plen, int pt);`
  Generates the next RTP packet. Takes the handle, packet length, and
  payload type as parameters. Returns a pointer to the generated packet.

- `int rsynth_next_pkt_pa(void *ri, int plen, int pt, char *buf, unsigned int blen, int pa);`
  Similar to `rsynth_next_pkt` but allows pre-allocated buffer and packet
  attributes. Returns the length of the packet generated.

- `void rsynth_pkt_free(void *rnp);`
  Frees the allocated packet. Takes a pointer to the packet as parameter.

- `void rsynth_dtor(void *ri);`
  Destroys the RTP synthesizer and frees resources. Takes the handle as
  parameter.

- `unsigned int rsynth_set_mbt(void *ri, unsigned int new_st);`
  Sets a new marker bit toggle state. Takes the handle and the new state
  as parameters. Returns the old state.

- `void rsynth_resync(void *ri, struct rsynth_seq *rsp);`
  Resynchronizes the RTP packet sequence. Takes the handle and optionally
  a sequence structure as parameters. Use this function when a time
  discontinuity is expected in packet generation, such as when VAD (Voice
  Activity Detection) is active. The library will recalculate the timestamp
  for the next packet based on the current system clock and the time the
  last packet was generated.

### RtpGen (Python)

## RTP Parser & Validator / Jitter Buffer

Simple RTP parser and validator to process incoming UDP datagrams,
parse & validate RTP headers. Resulting RTP stream is passed through
fixed-size jitter buffer to de-duplicate and re-order packets if
needed.

### rtpjbuf (c)

`#include <rtpjbuf.h>`

- `void *rtpjbuf_ctor(unsigned int capacity);`
  Creates a jitter buffer with the given capacity (number of packets).
  Returns an opaque handle.

- `void rtpjbuf_dtor(void *rjbp);`
  Destroys the jitter buffer and frees all internal state.

- `struct rjb_udp_in_r rtpjbuf_udp_in(void *rjbp, const unsigned char *data, size_t size);`
  Parse and insert a single UDP datagram. Returns a result structure with:
  `ready` list (in-order frames ready to consume), `drop` list (late/dup frames),
  and `error` for parser/memory failures.

- `struct rjb_udp_in_r rtpjbuf_flush(void *rjbp);`
  Flushes the jitter buffer and returns any queued frames (plus any drops).

- `void rtpjbuf_frame_dtor(void *rfp);`
  Frees a single RTP frame returned via `ready`/`drop`.

Frames in `ready`/`drop` are a linked list of `struct rtp_frame`:
- `type == RFT_RTP` provides `rtp.info`, `rtp.lseq`, and `rtp.data`.
- `type == RFT_ERS` provides erasure info (`lseq_start`, `lseq_end`, `ts_diff`).

### RtpJBuf (Python)
