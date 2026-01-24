## Project overview
- **librtpsynth** is a low-level RTP packet generator + parser/jitter buffer with both C and Python bindings.
- Primary C headers: `src/rtpsynth.h`, `src/rtpjbuf.h`, `src/rtp.h`, `src/rtp_info.h`.

## Layout
- `src/`: C core (`rtpsynth.c`, `rtp.c`, `rtpjbuf.c`) + headers.
- `python/`: Python package (`RtpSynth.py`, `RtpJBuf.py`) and CPython extensions (`RtpSynth_mod.c`, `RtpJBuf_mod.c`).
- `tests/`: Python tests (e.g. `tests/test_jbuf.py`).
- `setup.py`: build script for both extensions.

## Build
- Build extensions: `python setup.py build`
- Extensions built:
  - `_rtpsynth` (C core exposed to Python via `ctypes` in `python/RtpSynth.py`)
  - `rtpsynth.RtpJBuf` (CPython extension in `python/RtpJBuf_mod.c`)
- `setup.py` adds `include_dirs=['src']` for headers and uses `src/Symbol.map` for `_rtpsynth` (non-mac/non-win).

## Jitter buffer (rtpjbuf)
- C API in `src/rtpjbuf.h`:
  - `rtpjbuf_ctor`, `rtpjbuf_dtor`, `rtpjbuf_udp_in`, `rtpjbuf_flush`, `rtpjbuf_frame_dtor`
- `rtpjbuf_udp_in` returns `struct rjb_udp_in_r` with linked lists:
  - `ready`: in-order frames
  - `drop`: duplicates/late frames
- Frames are `struct rtp_frame`:
  - `type == RFT_RTP` has `rtp.info`, `rtp.lseq`, `rtp.data`
  - `type == RFT_ERS` has `ers` info

## Python jitter buffer
- `python/RtpJBuf_mod.c` is the fast CPython module; it manages a **fixed-size linear ref buffer** (capacity-sized) to keep input bytes alive.
  - It matches `rtp.data` pointers from `ready/drop` lists against stored pointers.
- `python/RtpJBuf.py` remains as the ctypes fallback and uses `_ref_cache`.

## Tests
- Run jitter buffer test: `python tests/test_jbuf.py`

## Notes / conventions
- Code style is C11 with `-Wall -pedantic` flags.
- Keep edits ASCII unless file already uses Unicode.
