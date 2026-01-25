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

import gc
import unittest
from time import monotonic
from random import Random

import rtpsynth.RtpJBuf as RtpJBuf_mod
from rtpsynth.RtpSynth import RtpSynth
from rtpsynth.RtpJBuf import RtpJBuf, RTPFrameType

RTP_HDR_LEN = 12

def make_payload(tag_seq, plen):
    tag_bytes = tag_seq.to_bytes(4, "big")
    return (tag_bytes * ((plen + 3) // 4))[:plen]

def corrupt_packet(packet, rng):
    buf = bytearray(packet)
    if len(buf) == 0:
        return bytes(buf)
    header_len = min(RTP_HDR_LEN, len(buf))
    nbytes = rng.randrange(1, header_len + 1)
    for _ in range(nbytes):
        idx = rng.randrange(header_len)
        buf[idx] ^= rng.randrange(1, 256)
    return bytes(buf)

class TestCase():
    iterations: int
    jbsize: int
    ers_cnt: int
    rtp_cnt: int
    loss_rate: float
    loss_seed: int
    corrupt_rate: float
    corrupt_seed: int
    check_counts: bool
    parse_errs_expected: int

    def __init__(
        self,
        iterations,
        jbsize,
        ers_cnt,
        rtp_cnt,
        loss_rate=0.0,
        loss_seed=0,
        corrupt_rate=0.0,
        corrupt_seed=0,
        check_counts=True,
        parse_errs_expected=0,
    ):
        self.iterations = iterations
        self.jbsize = jbsize
        self.ers_cnt = ers_cnt
        self.rtp_cnt = rtp_cnt
        self.loss_rate = loss_rate
        self.loss_seed = loss_seed
        self.corrupt_rate = corrupt_rate
        self.corrupt_seed = corrupt_seed
        self.check_counts = check_counts
        self.parse_errs_expected = parse_errs_expected

known_cnts = [
    TestCase(1000000, 20, 32394, 967606),
    TestCase(1000000, 10, 189013, 810987),
    TestCase(1000000, 20, 48363, 951637, 0.02, 2002),
    TestCase(1000000, 20, 72586, 927414, 0.05, 5005),
    TestCase(1000000, 20, 113765, 886235, 0.10, 10010),
]


def run_case(test_case, test_data, ts_step, verbose=False):
    rb = RtpJBuf(test_case.jbsize)
    ers_cnt = rtp_cnt = 0
    presented_cnt = 0
    loss_rng = Random(test_case.loss_seed)
    corrupt_rng = Random(test_case.corrupt_seed)
    corrupt_cnt = 0
    parse_errs = 0
    last_rtp_ts = None
    last_rtp_lseq = None
    last_tag = None
    pending_gap = 0

    def process_res(res):
        nonlocal ers_cnt, rtp_cnt, last_rtp_ts, last_rtp_lseq, ts_step, last_tag, pending_gap
        ers_frames = (x for x in res if x.content.type == RTPFrameType.ERS)
        corruption_off = test_case.corrupt_rate == 0.0
        if corruption_off:
            ers_cnt_add = sum([x.content.lseq_end - x.content.lseq_start + 1
                               for x in ers_frames])
        else:
            ers_cnt_add = len(tuple(ers_frames))
        rtp_cnt += sum([1 for x in res if x.content.type == RTPFrameType.RTP])
        if verbose:
            print(res)
        if ers_cnt_add >= 200 and corruption_off:
            raise AssertionError(f'ers_cnt_add too large: {ers_cnt_add}')
        ers_cnt += ers_cnt_add
        for item in res:
            frame = item.content
            if frame.type == RTPFrameType.RTP:
                union = frame.frame
                pkt = union.rtp
                info = pkt.info
                ts = info.ts
                lseq = pkt.lseq
                data = item.data
                rtp_data = item.rtp_data
                if len(data) < RTP_HDR_LEN:
                    raise AssertionError("packet too short for RTP header")
                if len(data) - RTP_HDR_LEN < len(rtp_data):
                    raise AssertionError("rtp_data extends beyond packet")
                if data[RTP_HDR_LEN:RTP_HDR_LEN + len(rtp_data)] != rtp_data:
                    raise AssertionError("rtp_data mismatch with packet data")
                tag = int.from_bytes(rtp_data[:4], "big")
                if rtp_data != make_payload(tag, len(rtp_data)):
                    raise AssertionError("payload tag mismatch")
                if last_tag is not None and corruption_off:
                    if tag <= last_tag:
                        raise AssertionError("tag not increasing")
                    if tag - last_tag != pending_gap + 1:
                        raise AssertionError("tag gap mismatch")
                last_tag = tag
                pending_gap = 0
                if last_rtp_ts is not None:
                    if lseq <= last_rtp_lseq:
                        raise AssertionError("lseq not increasing")
                    diff_lseq = lseq - last_rtp_lseq
                    diff_ts = ts - last_rtp_ts
                    if diff_ts <= 0 and corruption_off:
                        raise AssertionError("ts not increasing")
                    if diff_ts != ts_step * diff_lseq and corruption_off:
                        raise AssertionError("ts gap mismatch")
                last_rtp_ts = ts
                last_rtp_lseq = lseq
            else:
                ers = frame
                missing_count = ers.lseq_end - ers.lseq_start + 1
                if missing_count <= 0:
                    raise AssertionError("ers lseq range invalid")
                if last_rtp_lseq is not None and ers.lseq_start != last_rtp_lseq + 1:
                    raise AssertionError("ers lseq gap mismatch")
                if ers.ts_diff != ts_step * missing_count and corruption_off:
                    raise AssertionError("ers ts_diff gap mismatch")
                pending_gap += missing_count

    print('Testing...')
    btime = 0
    for pkt in test_data:
        if test_case.loss_rate > 0.0 and loss_rng.random() < test_case.loss_rate:
            continue
        data_pkt = pkt
        if test_case.corrupt_rate > 0.0 and corrupt_rng.random() < test_case.corrupt_rate:
            data_pkt = corrupt_packet(pkt, corrupt_rng)
            corrupt_cnt += 1
        try:
            presented_cnt += 1
            btime -= monotonic()
            res = rb.udp_in(data_pkt)
        except RtpJBuf_mod.RTPParseError:
            parse_errs += 1
            continue
        finally:
            btime += monotonic()
        process_res(res)
    process_res(rb.flush())
    dropped_cnt = rb.dropped
    del rb

    mpps = presented_cnt / max(btime, 1e-9) / 1e6

    print(f'ers_cnt: {ers_cnt}')
    print(f'rtp_cnt: {rtp_cnt}')
    print(f'dropped_cnt: {dropped_cnt}')
    print(f'presented_cnt: {presented_cnt}')
    if rtp_cnt + dropped_cnt + parse_errs != presented_cnt:
        raise AssertionError("presented count mismatch")
    if test_case.corrupt_rate > 0.0:
        if parse_errs == corrupt_cnt:
            print(f'parse_errors: {parse_errs}')
        else:
            print(f'parse_errors: {parse_errs}/{corrupt_cnt}')
            if parse_errs > corrupt_cnt:
                raise AssertionError("parse_errors mismatch")
    print(f'---\nConsumed {presented_cnt}, time={btime:.2f}, mpps={mpps:.2f}')
    return ers_cnt, rtp_cnt, parse_errs


class TestJBuf(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        iterations = max(tc.iterations for tc in known_cnts)
        rng = Random(42)
        i = 0
        srate = 8000
        ptime = 30
        cls.ts_step = 8 * ptime
        rs = RtpSynth(srate, ptime)
        robuf = []
        test_data = []
        try:
            print('Generating test data...')
            while i < iterations:
                rp = rs.next_pkt(170, 0, make_payload(i, 170))
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

    def _run_case(
        self,
        test_case,
        verbose=False,
    ):
        check_counts = test_case.check_counts
        parse_errs_expected = test_case.parse_errs_expected
        RtpJBuf_mod._set_dealloc_counting(True)
        RtpJBuf_mod._reset_dealloc_counts()
        ers_cnt, rtp_cnt, parse_errs = run_case(
            test_case,
            self.test_data,
            self.ts_step,
            verbose=verbose,
        )
        if check_counts:
            self.assertEqual(test_case.ers_cnt, ers_cnt)
            self.assertEqual(test_case.rtp_cnt, rtp_cnt)
        self.assertEqual(parse_errs, parse_errs_expected)
        gc.collect()
        gc.collect()
        counts = RtpJBuf_mod._get_dealloc_counts()
        def format_pair(name):
            created = counts.get(f"{name}_created", 0)
            freed = counts.get(f"{name}_freed", 0)
            if created != freed:
                return f"{name}={created}/{freed}"
            return f"{name}={created}"

        summary = ", ".join(
            format_pair(name)
            for name in (
                "RTPInfo",
                "RTPPacket",
                "ERSFrame",
                "RTPFrameUnion",
                "RTPFrame",
                "FrameWrapper",
                "RtpJBuf",
            )
        )
        print(f"alloc_summary: {summary}")
        for name in (
            "RTPInfo",
            "RTPPacket",
            "ERSFrame",
            "RTPFrameUnion",
            "RTPFrame",
            "FrameWrapper",
            "RtpJBuf",
        ):
            created = counts.get(f"{name}_created", 0)
            freed = counts.get(f"{name}_freed", 0)
            self.assertGreater(created, 0, f"{name} created counter is zero")
            self.assertGreater(freed, 0, f"{name} freed counter is zero")
            self.assertEqual(created, freed, f"{name} leaked: {created} created, {freed} freed")
        RtpJBuf_mod._set_dealloc_counting(False)
        return parse_errs

    def test_jbuf_counts(self):
        for test_case in known_cnts:
            with self.subTest(jbsize=test_case.jbsize, loss_rate=test_case.loss_rate):
                self._run_case(test_case)

    def test_jbuf_parse_error(self):
        rb = RtpJBuf(20)
        with self.assertRaises(RtpJBuf_mod.RTPParseError):
            rb.udp_in(b"\x80")
        del rb

    def test_jbuf_corrupt_packets(self):
        test_case = TestCase(
            1000000,
            20,
            33547,
            955455,
            corrupt_rate=0.01,
            corrupt_seed=4242,
            parse_errs_expected=3849,
        )
        self._run_case(
            test_case,
        )


if __name__ == '__main__':
    unittest.main()
