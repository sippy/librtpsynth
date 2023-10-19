# librtpsynth

Low-level library optimized to generate sequence of the RTP packets. Based on
some code and ideas from the RTPProxy projects.

Originally designed to supplement Python code to do low-level bits shuffling
for the proof of concept IoT implementation.

Reasonably fast, 100-200x real-time (i.e. 100-200K packets per second) when
used from the Python code. 100x of that (10-20M PPS) if used from C code
directly.

## RTP Generation

Generate continuous sequence of RTP packets of the same payload type.

### rtpgen (C)

### RtpGen (Python)

## RTP Parser & Validator / Jitter Buffer

Simple RTP parser and validator to process incoming UDP datagrams,
parse & validate RTP headers. Resulting RTP stream is passed through
fixed-size jitter buffer to de-duplicate and re-order packets if
needed.

### rtpjbuf (c)

### RtpJBuf (Python)
