"""Offline exhaustive checks against hashes from compiled ITU-T G.191 STL.
Regenerate independently with scripts/generate_g711_tables.py.
"""
from array import array
import hashlib
import json
from pathlib import Path
import struct
import unittest
from rtpsynth import RtpUtils as module
from rtpsynth.RtpUtils import RtpUtils

REFERENCE = json.loads(Path(__file__).with_name('g711_reference_hashes.json').read_text())

class TestG711(unittest.TestCase):
    def test_exhaustive_encode(self):
        pcm = array('h', range(-32768, 32768))
        for law in ('alaw', 'ulaw'):
            with self.subTest(law=law):
                function = getattr(module, 'linear2' + law)
                encoded = function(pcm)
                self.assertEqual(len(encoded), 65536)
                self.assertEqual(hashlib.sha256(encoded).hexdigest(), REFERENCE['sha256'][law + '_compress'])
                self.assertEqual(encoded, function(pcm.tobytes()))
                self.assertEqual(encoded, getattr(RtpUtils, 'linear2' + law)(pcm))
                self.assertEqual(encoded, bytes(function(sample) for sample in pcm))

    def test_exhaustive_decode(self):
        for law in ('alaw', 'ulaw'):
            with self.subTest(law=law):
                function = getattr(module, law + '2linear')
                decoded = function(bytes(range(256)))
                self.assertEqual(decoded.typecode, 'h')
                self.assertEqual(len(decoded), 256)
                canonical = struct.pack('<256h', *decoded)
                self.assertEqual(hashlib.sha256(canonical).hexdigest(), REFERENCE['sha256'][law + '_expand'])
                self.assertEqual(decoded, array('h', (function(code) for code in range(256))))
                self.assertEqual(decoded, getattr(RtpUtils, law + '2linear')(bytes(range(256))))

    def test_canonical_vectors(self):
        for code, value in ((0xD5, 8), (0x55, -8), (0xAA, 32256), (0x2A, -32256)):
            self.assertEqual(module.alaw2linear(code), value)
        self.assertEqual(module.ulaw2linear(0xFF), 0)
        self.assertEqual(module.ulaw2linear(0x7F), 0)
        self.assertEqual(module.linear2alaw(0), 0xD5)
        self.assertEqual(module.linear2ulaw(0), 0xFF)

    def test_scalar_limits(self):
        for owner in (module, RtpUtils):
            for law in ('alaw', 'ulaw'):
                encoder = getattr(owner, 'linear2' + law)
                decoder = getattr(owner, law + '2linear')
                for value in (-32769, 32768, -(2**63), 2**63 - 1, -(2**100), 2**100):
                    with self.subTest(owner=owner.__name__, law=law, pcm=value):
                        with self.assertRaises(ValueError):
                            encoder(value)
                for value in (-1, 256, -(2**63), 2**63 - 1, -(2**100), 2**100):
                    with self.subTest(owner=owner.__name__, law=law, codeword=value):
                        with self.assertRaises(ValueError):
                            decoder(value)
                endpoints = array('h', [-32768, 32767])
                self.assertEqual(bytes(encoder(value) for value in endpoints), encoder(endpoints))
                self.assertEqual(array('h', [decoder(0), decoder(255)]), decoder(bytes([0, 255])))

    def test_buffers_and_errors(self):
        for law in ('alaw', 'ulaw'):
            encode = getattr(module, 'linear2' + law)
            decode = getattr(module, law + '2linear')
            self.assertEqual(encode(b''), b'')
            self.assertEqual(decode(b''), array('h'))
            for function in (encode, decode):
                with self.assertRaises(TypeError):
                    function('invalid')
                with self.assertRaises(TypeError):
                    function(array('i', [1]))
            with self.assertRaises(ValueError):
                encode(b'x')
            with self.assertRaises((BufferError, TypeError)):
                decode(memoryview(b'1234')[::2])
            pcm = array('h', [-32768, -1, 0, 1, 32767])
            self.assertEqual(encode(memoryview(pcm)), encode(pcm))
            self.assertEqual(encode(bytearray(pcm.tobytes())), encode(pcm))
            self.assertEqual(decode(bytearray(range(256))), decode(bytes(range(256))))

if __name__ == '__main__':
    unittest.main()
