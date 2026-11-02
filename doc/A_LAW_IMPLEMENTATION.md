# Table-driven G.711 conversions

## API

The module rtpsynth.RtpUtils and its RtpUtils class expose linear2alaw,
alaw2linear, linear2ulaw, and ulaw2linear. Encoders accept an integer or
native-endian PCM16 buffers. Scalar encoders require [-32768, 32767];
scalar decoders require [0, 255]. Any out-of-range integer, including arbitrary-
precision integers, raises ValueError at the Python boundary before narrowing.
No clipping or wrapping occurs. Bulk encoding returns bytes; bulk decoding
returns array('h'). Odd-sized PCM byte buffers are rejected. Typed PCM16 and
byte buffers inherently provide in-range samples, so no per-sample validation
is needed. Core helpers trust these types and use debug-only assertions for
table indices and shift bounds; assertions disappear when NDEBUG is defined.

All four conversions, including scalar conversion, use compact table-driven
implementations. Encoders compute on-the-fly using the ITU-T G.191 algorithm
with bitshifts/index arithmetic and a shared 256-byte packed lookup table.
Each entry stores the segment number in its high nibble and the precomputed
A-law shift (B + 3 when B is nonzero, otherwise 4) in its low nibble. A-law
uses entry&0xF0 directly for the packed segment and entry&0x0F for its shift;
mu-law extracts the segment via entry>>4.
No A-law shift-selection conditional is needed at runtime.
Decoders use 256-entry lookup tables. No 65,536-entry encode arrays or
per-sample loops/floats/logs. Table storage is 1,280 bytes (256-byte segment
table + 512-byte A-law decode + 512-byte μ-law decode), a 99.0% reduction from
the previous 132,096 bytes.

## Authoritative reference

The source of truth is the ITU-T G.191 Software Tool Library G.711 module,
g711.c Version 3.01 (31 January 2000), pinned to OpenITU STL commit
c7a690846ecb401df5fb518849108303ba1ff173:

https://github.com/openitu/STL/blob/c7a690846ecb401df5fb518849108303ba1ff173/src/g711/g711.c

Source SHA-256: bd95a6589fd4318abb156c747d2082b27529a0d43dd0eea2726114ce40f537a9

The generator downloads and compiles the actual reference C functions
alaw_compress, alaw_expand, ulaw_compress, and ulaw_expand. Their outputs,
not another language library, generate the tables and independent regression
hashes. The reference source carries the ITU-T General Public License;
it is downloaded to a temporary directory, not copied into this repository.

The reference accepts signed PCM16 with significant bits left-justified:
A-law uses the 13-bit convention and mu-law the 14-bit convention. No additional
scaling is applied at the API boundary. Negative encoding uses the reference's
one's-complement magnitude before quantization, not absolute value.
Codewords are right-justified 8-bit values. Decoder outputs retain the reference's
left-justified PCM16 scale. A-law toggles even bits with XOR 0x55.

Canonical A-law decoder values: D5 -> +8, 55 -> -8, AA -> +32256,
2A -> -32256. Both mu-law zero codewords decode to zero. No optional zero trap
is enabled. These codecs are lossy; arbitrary PCM encode/decode is not identity.

## Compatibility correction

Compared with the previous mu-law algorithm, the ITU reference changes exactly
127 of 65,536 PCM16 encoder values, at negative quantization boundaries.
For example, -31612 changes from 0x00 to 0x01. Mu-law decoding is unchanged.
This intentionally prioritizes ITU conformance over legacy rounding.

## Reproduction and tests

Run python3 scripts/generate_g711_tables.py to download the pinned reference,
compile it with CC (default cc), and regenerate python/g711_tables.h and
tests/g711_reference_hashes.json. This explicit maintenance step requires
network access and a C compiler. Normal builds and tests require no network.

Build with python3 setup.py build --force. Set PYTHONPATH to the build/lib.*
directory for that interpreter, then run:

    python3 -m unittest discover -s tests -p 'test_g711_comprehensive.py' -v
    python3 -m unittest discover -s tests -p 'test_utils.py' -v

The offline tests hash complete ordered encoder and decoder output domains
against independently compiled reference outputs. They also cover scalar/bulk
and class/module parity, range exceptions, empty buffers, invalid types and canonical
vectors. Missing APIs fail rather than skip. Decoder hashes use little-endian
serialization independent of host byte order.

Verification additionally compared all four outputs directly against the
compiled reference: zero mismatches over 131,584 distinct input cases.
No throughput claim is made. No installed packages or SIP negotiation code
are changed by this library-only patch.
