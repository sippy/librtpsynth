# librtpsynth

Low-level library optimized to generate and/or consume sequence of the RTP
packets. Based on some code and ideas from the RTPProxy projects.

Originally designed to supplement Python code to do low-level bits shuffling
for the proof of concept IoT implementation.

Reasonably fast, 2,000-4,000x real-time (i.e. 2-4M packets per second) when
used from the Python code. 5x of that (10-20M PPS) if used from C code
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

Silence (erasure) frames are emitted to indicate voids.

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

## RTP I/O Thread (Python): RtpServer / RtpChannel

`rtpsynth.RtpServer` provides a single worker thread that multiplexes UDP I/O
for many channels. Each `RtpChannel` is a bidirectional RTP pipe with:
- one UDP socket bound to a local address;
- one fixed callback for incoming packets;
- one fixed-size lossy non-blocking output queue.

### Build

Build and install all Python extensions (including `rtpsynth.RtpServer`):

```sh
python setup.py build install
```

### Public API

- `RtpServer(tick_hz=200)`
  Starts the worker thread. `tick_hz` controls loop frequency (`poll recv`,
  drain output queues, sleep until next tick).

- `server.create_channel(pkt_in, bind_host=None, bind_port=0, queue_size=32, bind_family=0)`
  Creates an `RtpChannel` and hands its socket to the worker.
  `pkt_in` is called as `pkt_in(pkt_bytes, (host, port), rtime_ns)`.
  `rtime_ns` is a `CLOCK_MONOTONIC` timestamp captured once when `poll()`
  returns with ready sockets.
  `bind_family` selects socket family explicitly (`0`/`"auto"`, `4`/`"ipv4"`,
  `6`/`"ipv6"`). When `bind_host` is `None`, default bind host is
  `0.0.0.0` for IPv4/auto and `::` for IPv6.
  `queue_size` must be a power of two and greater than zero.

- `channel.set_target(host, port)`
  Sets UDP destination for outgoing packets.

- `channel.send_pkt(data)`
  Enqueues one packet for send. Non-blocking.
  Raises `RtpQueueFullError` when channel queue is full.

- `channel.close()`
  Requests channel removal from the server.

- `server.shutdown()`
  Stops the worker thread (safe to call more than once).

- `channel.local_addr` (property)
  Returns `(host, port)` the channel socket is bound to.

- `channel.closed` (property)
  Boolean closed state.

### Notes

- `send_pkt()` must only be used after `set_target()`.
- Output queues are lossy by design: if a queue is full, packets are dropped.
- If no channels are active, worker sleeps waiting for commands.
- `python/RtpServer.py` is not a ctypes fallback; the CPython extension module
  is required.

### Minimal example

```python
from rtpsynth.RtpServer import RtpQueueFullError, RtpServer

srv = RtpServer(tick_hz=200)
ch = None
try:
    rx = []
    ch = srv.create_channel(
        pkt_in=lambda pkt, addr, rtime_ns: rx.append((pkt, addr, rtime_ns)),
        bind_host="127.0.0.1",
        bind_port=0,
        queue_size=32,
    )
    addr = ch.local_addr
    ch.set_target(addr[0], addr[1])
    try:
        ch.send_pkt(b"hello")
    except RtpQueueFullError:
        pass
finally:
    if ch is not None:
        ch.close()
    srv.shutdown()
```

### Tests

```sh
python tests/test_rtp_server.py
```

## RTP Processing Scheduler (Python): RtpProc / RtpProcChannel

`rtpsynth.RtpProc` is a singleton worker-thread scheduler for periodic
processing callbacks shared across many channels/sessions.

- `RtpProc()`
  Returns the singleton instance.

- `proc.create_channel(proc_in)`
  Creates `RtpProcChannel`.
  `proc_in(now_ns, deadline_ns)` is called immediately on channel creation on
  the worker thread, then periodically according to its return value.
  For the initial call, `deadline_ns == 0`. For scheduled calls,
  `deadline_ns` is the originally requested run timestamp.
  Return `next_run_ns` (monotonic ns) to reschedule.
  Returning `base_ns + period_ns` is recommended to avoid drift, where
  `base_ns = now_ns if deadline_ns == 0 else deadline_ns`.
  Returning `None` stops further scheduling for that channel.
  If callback raises, the channel is also unscheduled; the exception is
  re-raised on `channel.close()` as `ChannelProcError` chained from the
  original callback exception.

- `channel.close()`
  Removes the channel from scheduler.

- `proc.shutdown()`
  Stops the worker thread.

## Audio Utils (Python): RtpUtils

`rtpsynth.RtpUtils` provides optimized CPython helpers for PCM16 and G.711u:

- `RtpUtils.resample_linear(pcm16, in_rate, out_rate) -> array('h')`
- `RtpUtils.linear2ulaw(sample:int) -> int`
- `RtpUtils.linear2ulaw(pcm16) -> bytes`
- `RtpUtils.ulaw2linear(ulaw:int) -> int`
- `RtpUtils.ulaw2linear(ulaw_bytes) -> array('h')`

`pcm16` accepts `array('h')` and bytes-like PCM16 input.

The same helpers are exported at module level, so you can also do:

- `from rtpsynth.RtpUtils import resample_linear, linear2ulaw, ulaw2linear`
