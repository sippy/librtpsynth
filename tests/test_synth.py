import unittest
from time import monotonic

from rtpsynth.RtpSynth import RtpSynth

class TestSynth(unittest.TestCase):
    def test_generate(self):
        tdur = 10.0
        i = 0
        rs = RtpSynth(8000, 30)
        try:
            st = monotonic()
            et = st
            while True:
                rs.next_pkt(170, 0)
                i += 1
                if i % 100000 == 0:
                    et = monotonic()
                    if et - st >= tdur:
                        break
        finally:
            del rs

        dur = et - st
        mpps = i / max(dur, 1e-9) / 1e6
        print(f'Generated {i} packets in {dur} seconds, mpps={mpps:.2f}')
        self.assertGreater(i, 0)
        self.assertGreater(dur, 0.0)


if __name__ == '__main__':
    unittest.main()
