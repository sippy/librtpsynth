#!/usr/bin/env python3
"""Generate compact G.711 tables (256-byte segment + 256-entry decodes) from ITU-T G.191 STL.
Maintenance command; downloads the reference and requires a C compiler.
Normal builds and tests are offline. No reference C implementation is vendored.
Outputs ONLY 256-byte segment table + 2 decode tables (1,280 bytes total).
Also outputs encode hashes for verification against actual ITU reference.
"""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import urllib.request

REVISION = 'c7a690846ecb401df5fb518849108303ba1ff173'
BASE = 'https://raw.githubusercontent.com/openitu/STL/' + REVISION + '/src/g711/'
ROOT = Path(__file__).resolve().parents[1]

def generate():
    hashes = {}
    with tempfile.TemporaryDirectory(prefix='g711-reference-') as directory:
        directory = Path(directory)
        source = None
        for name in ('g711.c', 'g711.h'):
            with urllib.request.urlopen(BASE + name, timeout=30) as response:
                data = response.read()
            (directory / name).write_bytes(data)
            if name == 'g711.c':
                source = hashlib.sha256(data).hexdigest()
        subprocess.check_call([os.environ.get('CC', 'cc'), '-shared', '-fPIC', '-O2',
                               str(directory / 'g711.c'), '-o', str(directory / 'reference.so')])
        library = ctypes.CDLL(str(directory / 'reference.so'))

        # Generate encode hashes from actual ITU reference
        for law in ('alaw', 'ulaw'):
            function = getattr(library, law + '_compress')
            function.argtypes = [ctypes.c_long, ctypes.POINTER(ctypes.c_short),
                                 ctypes.POINTER(ctypes.c_short)]
            function.restype = None
            inputs = (ctypes.c_short * 65536)(*range(-32768, 32768))
            outputs = (ctypes.c_short * 65536)()
            function(65536, inputs, outputs)
            raw = bytes(outputs[i] & 0xFF for i in range(65536))
            hashes[law + '_compress'] = hashlib.sha256(raw).hexdigest()

        # Generate A-law decode table
        alaw_function = getattr(library, 'alaw_expand')
        alaw_function.argtypes = [ctypes.c_long, ctypes.POINTER(ctypes.c_short),
                                  ctypes.POINTER(ctypes.c_short)]
        alaw_function.restype = None
        alaw_inputs = (ctypes.c_short * 256)(*range(256))
        alaw_outputs = (ctypes.c_short * 256)()
        alaw_function(256, alaw_inputs, alaw_outputs)
        alaw_values = list(alaw_outputs)
        raw = struct.pack('<256h', *alaw_values)
        hashes['alaw_expand'] = hashlib.sha256(raw).hexdigest()

        # Generate μ-law decode table
        ulaw_function = getattr(library, 'ulaw_expand')
        ulaw_function.argtypes = [ctypes.c_long, ctypes.POINTER(ctypes.c_short),
                                  ctypes.POINTER(ctypes.c_short)]
        ulaw_function.restype = None
        ulaw_inputs = (ctypes.c_short * 256)(*range(256))
        ulaw_outputs = (ctypes.c_short * 256)()
        ulaw_function(256, ulaw_inputs, ulaw_outputs)
        ulaw_values = list(ulaw_outputs)
        raw = struct.pack('<256h', *ulaw_values)
        hashes['ulaw_expand'] = hashlib.sha256(raw).hexdigest()

    # Generate segment table: B[i] = i.bit_length() for i in 0..255
    B = [(i).bit_length() for i in range(256)]

    newline = chr(10)
    lines = ['/* Compact G.711 tables from ITU-T G.191 STL g711.c Version 3.01.',
             ' * 256-byte segment table + 256-entry decode tables = 1,280 bytes total.',
             ' * PCM16 order: -32768..32767; codeword order: 0..255.',
             ' * Read-only tables; no runtime initialization or companding.', ' */']

    # Segment table: packed format
    # High nibble: bit_length / segment (0..8)
    # Low nibble: A-law shift (segment+3 if nonzero, otherwise 4)
    # Layout: (segment<<4)|shift, where shift = (segment+3) if segment else 4
    lines.append('static const uint8_t g_g711_segment[256] = {')
    for offset in range(0, 256, 16):
        vals = []
        for i in range(offset, offset + 16):
            seg = B[i]
            shift = (seg + 3) if seg else 4
            packed = (seg << 4) | shift
            vals.append(str(packed))
        lines.append('    ' + ', '.join(vals) + ',')
    lines.append('};')

    # A-law decode table
    lines.append('')
    lines.append('/* A-law 256-entry decode table (signed int16_t, native-endian).')
    lines.append(' * Index: A-law codeword 0..255. Output: signed PCM16 value.')
    lines.append(' */')
    lines.append('static const int16_t g_alaw_to_pcm[256] = {')
    for offset in range(0, 256, 16):
        vals = [str(alaw_values[i]) for i in range(offset, offset + 16)]
        lines.append('    ' + ', '.join(vals) + ',')
    lines.append('};')

    # μ-law decode table
    lines.append('')
    lines.append('/* μ-law 256-entry decode table (signed int16_t, native-endian).')
    lines.append(' * Index: μ-law codeword 0..255. Output: signed PCM16 value.')
    lines.append(' */')
    lines.append('static const int16_t g_ulaw_to_pcm[256] = {')
    for offset in range(0, 256, 16):
        vals = [str(ulaw_values[i]) for i in range(offset, offset + 16)]
        lines.append('    ' + ', '.join(vals) + ',')
    lines.append('};')

    (ROOT / 'python/g711_tables.h').write_text(newline.join(lines) + newline)

    metadata = {'source': 'https://github.com/openitu/STL/blob/' + REVISION + '/src/g711/g711.c',
                'source_sha256': source, 'sha256': hashes}
    (ROOT / 'tests/g711_reference_hashes.json').write_text(json.dumps(metadata, indent=2) + newline)
    print(json.dumps(metadata, indent=2))

if __name__ == '__main__':
    generate()
