from rtpsynth.RtpSynth import RtpSynth
from time import monotonic

if __name__ == '__main__':
    tdur = 10.0
    i = 0
    rs = RtpSynth(8000, 30)
    st = monotonic()
    while True:
        rp = rs.next_pkt(170, 0)
        i += 1
        if i % 100000 == 0:
            et = monotonic()
            if et - st >= tdur:
                break

    del rs

    dur = et - st
    pps = int(i / dur)
    print(f'Generated {i} packets in {dur} seconds, {pps} packets per second')
