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

from rtpsynth.RtpSynth import RtpSynth
from rtpsynth.RtpJBuf import RtpJBuf, RTPFrameType

from random import shuffle, seed, random
from time import monotonic

seed(42)

class TestCase():
    iterations: int
    jbsize: int
    ers_cnt: int
    rtp_cnt: int

    def __init__(self, iterations, jbsize, ers_cnt, rtp_cnt):
        self.iterations = iterations
        self.jbsize = jbsize
        self.ers_cnt = ers_cnt
        self.rtp_cnt = rtp_cnt

known_cnts = [
    TestCase(1000000, 20, 32392, 967608),
    TestCase(1000000, 10, 185812, 814188),
]

def run_test(test_case, verbose = False):
    iterations = test_case.iterations
    i = 0
    rs = RtpSynth(8000, 30)
    rb = RtpJBuf(test_case.jbsize)
    robuf = []
    iterations = 1000000
    ers_cnt = rtp_cnt = 0
    test_data = []
    print('Generating test data...')
    while i < iterations:
        rp = rs.next_pkt(170, 0)
        robuf.append(rp)
        if len(robuf) > int(200 * random()) or i == iterations - 1:
            shuffle(robuf)
            #print(len(robuf))
            robuf.extend(robuf[:int(len(robuf)/10)])
            shuffle(robuf)
            test_data.extend(robuf)
            robuf = []
        i += 1
    print('Testing...')
    btime = monotonic()
    for i, pkt in enumerate(test_data):
        res = rb.udp_in(pkt)
        if i == len(test_data) - 1:
            fres = rb.flush()
            #print(f'len(fres) = {len(fres)}')
            res.extend(fres)
        if len(res) == 0:
            continue
        ers_cnt_add = sum([x.content.lseq_end - x.content.lseq_start + 1 for x in res if x.content.type == RTPFrameType.ERS])
        rtp_cnt += sum([1 for x in res if x.content.type == RTPFrameType.RTP])
        if verbose:
            print(res)
        assert ers_cnt_add < 200
        ers_cnt += ers_cnt_add
    etime = monotonic()

    del rs
    print(f'ers_cnt: {ers_cnt}')
    print(f'rtp_cnt: {rtp_cnt}')
    print(f'---\nTotal: {rtp_cnt + ers_cnt}, time={etime - btime}')
    assert test_case.ers_cnt == ers_cnt and test_case.rtp_cnt == rtp_cnt

if __name__ == '__main__':
    for test_case in known_cnts:
        run_test(test_case)
