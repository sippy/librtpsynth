from array import array
import random
import unittest

from rtpsynth.RtpUtils import RtpUtils
from rtpsynth.RtpUtils import linear2ulaw, resample_linear, ulaw2linear

_BIAS = 0x84
_CLIP = 32635


def ref_linear2ulaw(sample: int) -> int:
    sign = 0x80 if sample < 0 else 0
    if sample < 0:
        sample = -sample
    if sample > _CLIP:
        sample = _CLIP
    sample += _BIAS
    exponent = 7
    mask = 0x4000
    while exponent > 0 and (sample & mask) == 0:
        exponent -= 1
        mask >>= 1
    mantissa = (sample >> (exponent + 3)) & 0x0F
    return ~(sign | (exponent << 4) | mantissa) & 0xFF


def ref_ulaw2linear(ulaw: int) -> int:
    ulaw = (~ulaw) & 0xFF
    sign = ulaw & 0x80
    exponent = (ulaw >> 4) & 0x07
    mantissa = ulaw & 0x0F
    sample = ((mantissa << 3) + _BIAS) << exponent
    sample -= _BIAS
    if sign:
        sample = -sample
    return sample


def ref_resample_linear(pcm: array, in_rate: int, out_rate: int) -> array:
    if in_rate == out_rate:
        return array("h", pcm)
    n_in = len(pcm)
    if n_in == 0:
        return array("h")
    n_out = max(1, int(round(n_in * out_rate / in_rate)))
    ratio = in_rate / out_rate
    out = array("h")
    for i in range(n_out):
        pos = i * ratio
        idx = int(pos)
        frac = pos - idx
        if idx >= n_in - 1:
            sample = pcm[-1]
        else:
            s0 = pcm[idx]
            s1 = pcm[idx + 1]
            sample = s0 + (s1 - s0) * frac
        val = int(round(sample))
        if val > 32767:
            val = 32767
        elif val < -32768:
            val = -32768
        out.append(val)
    return out


class TestRtpUtils(unittest.TestCase):
    def test_module_level_exports(self):
        self.assertEqual(linear2ulaw(1234), ref_linear2ulaw(1234))
        self.assertEqual(ulaw2linear(0xFF), ref_ulaw2linear(0xFF))
        src = array("h", [0, 1000, -1000])
        self.assertEqual(
            resample_linear(src, 8000, 16000).tolist(),
            ref_resample_linear(src, 8000, 16000).tolist(),
        )

    def test_linear2ulaw_scalar(self):
        for sample in (-50000, -32768, -12345, -1, 0, 1, 12345, 32767, 50000):
            self.assertEqual(RtpUtils.linear2ulaw(sample), ref_linear2ulaw(sample))

    def test_ulaw2linear_scalar(self):
        for ulaw in range(256):
            self.assertEqual(RtpUtils.ulaw2linear(ulaw), ref_ulaw2linear(ulaw))

    def test_linear2ulaw_bulk(self):
        rng = random.Random(1)
        pcm = array("h", (rng.randint(-32768, 32767) for _ in range(2000)))
        expected = bytes(ref_linear2ulaw(v) for v in pcm)

        self.assertEqual(RtpUtils.linear2ulaw(pcm), expected)
        self.assertEqual(RtpUtils.linear2ulaw(pcm.tobytes()), expected)

    def test_ulaw2linear_bulk(self):
        inp = bytes(range(256)) * 4
        expected = array("h", (ref_ulaw2linear(b) for b in inp))
        out = RtpUtils.ulaw2linear(inp)

        self.assertIsInstance(out, array)
        self.assertEqual(out.typecode, "h")
        self.assertEqual(out.tolist(), expected.tolist())

    def test_resample_linear(self):
        src = array("h", [0, 1000, -1000, 2000, -2000, 32767, -32768, 1234, -1234])
        rates = ((8000, 8000), (8000, 16000), (16000, 8000), (8000, 11025))
        for in_rate, out_rate in rates:
            out = RtpUtils.resample_linear(src, in_rate, out_rate)
            expected = ref_resample_linear(src, in_rate, out_rate)
            self.assertEqual(out.tolist(), expected.tolist())

    def test_resample_linear_from_bytes(self):
        src = array("h", [100, -200, 300, -400, 500, -600, 700])
        out = RtpUtils.resample_linear(src.tobytes(), 8000, 16000)
        expected = ref_resample_linear(src, 8000, 16000)
        self.assertEqual(out.tolist(), expected.tolist())


if __name__ == "__main__":
    unittest.main()
