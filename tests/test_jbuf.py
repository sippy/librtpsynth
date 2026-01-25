# Copyright (c) 2018 Sippy Software, Inc. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# * Redistributions of source code must retain the above copyright notice, this
#   list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright notice,
#   this list of conditions and the following disclaimer in the documentation
#   and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import unittest
from time import monotonic
from random import Random

from rtpsynth.RtpSynth import RtpSynth
from rtpsynth.RtpJBuf import RtpJBuf, RTPFrameType

class TestCase():
    iterations: int
    jbsize: int
    ers_cnt: int
    rtp_cnt: int
    loss_rate: float
    loss_seed: int

    def __init__(self, iterations, jbsize, ers_cnt, rtp_cnt, loss_rate=0.0, loss_seed=0):
        self.iterations = iterations
        self.jbsize = jbsize
        self.ers_cnt = ers_cnt
        self.rtp_cnt = rtp_cnt
        self.loss_rate = loss_rate
        self.loss_seed = loss_seed

known_cnts = [
    TestCase(1000000, 20, 32392, 967608),
    TestCase(1000000, 10, 187612, 812388),
    TestCase(1000000, 20, 48360, 951640, 0.02, 2002),
    TestCase(1000000, 20, 72582, 927418, 0.05, 5005),
    TestCase(1000000, 20, 113761, 886239, 0.10, 10010),
]


def run_case(test_case, test_data, verbose=False):
    rb = RtpJBuf(test_case.jbsize)
    ers_cnt = rtp_cnt = 0
    loss_rng = Random(test_case.loss_seed)

    def process_res(res):
        nonlocal ers_cnt, rtp_cnt
        if not res:
            return
        ers_cnt_add = sum([x.content.lseq_end - x.content.lseq_start + 1
                           for x in res if x.content.type == RTPFrameType.ERS])
        rtp_cnt += sum([1 for x in res if x.content.type == RTPFrameType.RTP])
        if verbose:
            print(res)
        if ers_cnt_add >= 200:
            raise AssertionError(f'ers_cnt_add too large: {ers_cnt_add}')
        ers_cnt += ers_cnt_add

    print('Testing...')
    btime = monotonic()
    for pkt in test_data:
        if test_case.loss_rate > 0.0 and loss_rng.random() < test_case.loss_rate:
            continue
        res = rb.udp_in(pkt)
        process_res(res)
    process_res(rb.flush())
    etime = monotonic()

    mpps = (rtp_cnt + ers_cnt) / max(etime - btime, 1e-9) / 1e6

    print(f'ers_cnt: {ers_cnt}')
    print(f'rtp_cnt: {rtp_cnt}')
    print(f'---\nConsumed {rtp_cnt + ers_cnt}, time={etime - btime}, mpps={mpps:.2f}')
    return ers_cnt, rtp_cnt


class TestJBuf(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        iterations = max(tc.iterations for tc in known_cnts)
        rng = Random(42)
        i = 0
        rs = RtpSynth(8000, 30)
        robuf = []
        test_data = []
        try:
            print('Generating test data...')
            while i < iterations:
                rp = rs.next_pkt(170, 0)
                robuf.append(rp)
                if len(robuf) > int(200 * rng.random()) or i == iterations - 1:
                    rng.shuffle(robuf)
                    #print(len(robuf))
                    robuf.extend(robuf[:int(len(robuf)/10)])
                    rng.shuffle(robuf)
                    test_data.extend(robuf)
                    robuf = []
                i += 1
        finally:
            del rs

        cls.test_data = test_data

    def _run_case(self, test_case, verbose=False):
        ers_cnt, rtp_cnt = run_case(test_case, self.test_data, verbose=verbose)
        self.assertEqual(test_case.ers_cnt, ers_cnt)
        self.assertEqual(test_case.rtp_cnt, rtp_cnt)

    def test_jbuf_counts(self):
        for test_case in known_cnts:
            with self.subTest(jbsize=test_case.jbsize, loss_rate=test_case.loss_rate):
                self._run_case(test_case)


if __name__ == '__main__':
    unittest.main()
