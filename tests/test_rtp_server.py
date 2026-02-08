import gc
import errno
import socket
import sys
import time
import unittest

try:
    from rtpsynth.RtpServer import RtpQueueFullError, RtpServer
except (ImportError, ModuleNotFoundError):
    if not sys.platform.startswith("win"):
        raise
    RtpQueueFullError = None
    RtpServer = None


def wait_for(predicate, timeout=2.0, interval=0.01):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return predicate()


def mono_clock_ns():
    if hasattr(time, "clock_gettime_ns") and hasattr(time, "CLOCK_MONOTONIC"):
        return time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    return time.monotonic_ns()


class TestRtpServer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if sys.platform.startswith("win") and RtpServer is None:
            raise unittest.SkipTest("rtpsynth.RtpServer extension module is unavailable")

    SCALING_MAX_CHANNELS = 4096

    def test_send_recv_bidirectional(self):
        recv_a = []
        recv_b = []
        srv = RtpServer(tick_hz=200)
        ch_a = None
        ch_b = None
        try:
            ch_a = srv.create_channel(
                pkt_in=lambda pkt, addr, rtime: recv_a.append((pkt, addr, rtime)),
                bind_host="127.0.0.1",
                bind_port=0,
            )
            ch_b = srv.create_channel(
                pkt_in=lambda pkt, addr, rtime: recv_b.append((pkt, addr, rtime)),
                bind_host="127.0.0.1",
                bind_port=0,
            )

            addr_a = ch_a.local_addr
            addr_b = ch_b.local_addr
            ch_a.set_target(addr_b[0], addr_b[1])
            ch_b.set_target(addr_a[0], addr_a[1])

            sent_a = [f"a-{i}".encode("ascii") for i in range(4)]
            sent_b = [f"b-{i}".encode("ascii") for i in range(4)]
            for pkt in sent_a:
                ch_a.send_pkt(pkt)
            for pkt in sent_b:
                ch_b.send_pkt(pkt)

            ok = wait_for(lambda: len(recv_a) >= len(sent_b) and len(recv_b) >= len(sent_a))
            self.assertTrue(ok, "timeout waiting for packets")

            got_a = [pkt for pkt, _addr, _rtime in recv_a]
            got_b = [pkt for pkt, _addr, _rtime in recv_b]
            self.assertEqual(got_a[:len(sent_b)], sent_b)
            self.assertEqual(got_b[:len(sent_a)], sent_a)
        finally:
            if ch_a is not None:
                ch_a.close()
            if ch_b is not None:
                ch_b.close()
            srv.shutdown()

    def test_channel_destruction_and_shutdown(self):
        received = []
        srv = RtpServer()
        ch = srv.create_channel(
            pkt_in=lambda pkt, addr, rtime: received.append((pkt, addr, rtime)),
            bind_host="127.0.0.1",
            bind_port=0,
        )
        addr = ch.local_addr
        ch = None
        gc.collect()
        gc.collect()
        time.sleep(0.1)

        tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            tx.sendto(b"after-close", addr)
        finally:
            tx.close()

        time.sleep(0.2)
        self.assertEqual(received, [])

        srv.shutdown()
        srv.shutdown()

    def test_create_channel_ipv6_bind_family(self):
        received = []
        srv = RtpServer()
        ch = None
        probe = None
        try:
            if not socket.has_ipv6:
                self.skipTest("IPv6 is not supported by Python runtime")
            try:
                probe = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
                probe.bind(("::1", 0))
            except OSError:
                self.skipTest("IPv6 loopback is unavailable")
            finally:
                if probe is not None:
                    probe.close()

            ch = srv.create_channel(
                pkt_in=lambda pkt, _addr, _rtime: received.append(pkt),
                bind_host="::1",
                bind_port=0,
                bind_family=6,
            )
            addr = ch.local_addr
            ch.set_target(addr[0], addr[1])
            ch.send_pkt(b"ipv6")

            ok = wait_for(lambda: len(received) >= 1)
            self.assertTrue(ok, "timeout waiting for IPv6 loopback packet")
            self.assertEqual(received[0], b"ipv6")
        finally:
            if ch is not None:
                ch.close()
            srv.shutdown()

    def test_send_queue_full_exception(self):
        srv = RtpServer(tick_hz=1)
        ch = None
        try:
            ch = srv.create_channel(
                pkt_in=lambda _pkt, _addr, _rtime: None,
                bind_host="127.0.0.1",
                bind_port=0,
            )
            addr = ch.local_addr
            ch.set_target(addr[0], addr[1])
            time.sleep(0.05)

            payload = b"x" * 60000
            for _ in range(200000):
                ch.send_pkt(payload)
            self.fail("expected RtpQueueFullError")
        except RtpQueueFullError:
            pass
        finally:
            if ch is not None:
                ch.close()
            srv.shutdown()

    def test_channel_double_close_raises(self):
        srv = RtpServer()
        ch = None
        try:
            ch = srv.create_channel(
                pkt_in=lambda _pkt, _addr, _rtime: None,
                bind_host="127.0.0.1",
                bind_port=0,
            )
            self.assertFalse(ch.closed)
            ch.close()
            self.assertTrue(ch.closed)
            with self.assertRaises(RuntimeError):
                ch.close()
        finally:
            srv.shutdown()

    def test_create_channel_huge_queue_size(self):
        srv = RtpServer()
        try:
            with self.assertRaises((MemoryError, OverflowError)):
                srv.create_channel(
                    pkt_in=lambda _pkt, _addr, _rtime: None,
                    bind_host="127.0.0.1",
                    bind_port=0,
                    queue_size=(1 << 62),
                )
        finally:
            srv.shutdown()

    def _run_sender_scaling(self, label, run_seconds, max_channels, pps, require_loss_threshold):
        srv = RtpServer(tick_hz=500)
        payload = b"x" * 160
        target_loss_pct = 5.0
        nch = 1
        send_interval = (1.0 / float(pps)) if pps is not None else None
        backoff_seconds = 0.0005
        results = []
        hit_target = False
        stop_exc = None

        try:
            while nch <= max_channels:
                recv_counts = [0] * nch
                channels = []
                sent_attempted = 0
                sent_queued = 0
                queue_full = 0

                try:
                    for i in range(nch):
                        def make_cb(idx):
                            return lambda _pkt, _addr, _rtime: recv_counts.__setitem__(idx, recv_counts[idx] + 1)

                        ch = srv.create_channel(
                            pkt_in=make_cb(i),
                            bind_host="127.0.0.1",
                            bind_port=0,
                        )
                        channels.append(ch)
                except OSError as ex:
                    if ex.errno == errno.EMFILE:
                        stop_exc = ex
                        for ch in channels:
                            ch.close()
                        break
                    raise

                try:
                    for ch in channels:
                        addr = ch.local_addr
                        ch.set_target(addr[0], addr[1])

                    time.sleep(0.05)
                    started = time.monotonic()
                    deadline = started + run_seconds
                    if send_interval is None:
                        while time.monotonic() < deadline:
                            hit_full = False
                            for ch in channels:
                                sent_attempted += 1
                                try:
                                    ch.send_pkt(payload)
                                    sent_queued += 1
                                except RtpQueueFullError:
                                    queue_full += 1
                                    hit_full = True
                            if hit_full:
                                time.sleep(backoff_seconds)
                    else:
                        next_tick = started
                        while time.monotonic() < deadline:
                            for ch in channels:
                                sent_attempted += 1
                                try:
                                    ch.send_pkt(payload)
                                    sent_queued += 1
                                except RtpQueueFullError:
                                    queue_full += 1
                            next_tick += send_interval
                            now = time.monotonic()
                            if now >= next_tick:
                                missed = int((now - next_tick) / send_interval) + 1
                                next_tick += missed * send_interval
                                continue
                            time.sleep(next_tick - now)

                    elapsed = time.monotonic() - started

                    wait_deadline = time.monotonic() + 2.0
                    while time.monotonic() < wait_deadline and sum(recv_counts) < sent_queued:
                        time.sleep(0.01)

                    recv_total = sum(recv_counts)
                    loss = sent_attempted - recv_total
                    if loss < 0:
                        loss = 0
                    tx_pps = (float(sent_attempted) / elapsed) if elapsed > 0.0 else 0.0
                    rx_pps = (float(recv_total) / elapsed) if elapsed > 0.0 else 0.0
                    loss_pct = (
                        (100.0 * float(loss) / float(sent_attempted))
                        if sent_attempted > 0 else 0.0
                    )
                    results.append(
                        (
                            nch,
                            sent_attempted,
                            sent_queued,
                            recv_total,
                            queue_full,
                            tx_pps,
                            rx_pps,
                            loss_pct,
                        )
                    )
                    if loss_pct >= target_loss_pct:
                        hit_target = True
                finally:
                    for ch in channels:
                        ch.close()

                if hit_target:
                    break
                nch *= 2

            print(label)
            print(
                f"{'channels':>8} {'attempted':>9} {'queued':>6} {'recv':>8} {'queue_full':>10} "
                f"{'tx_pps':>8} {'rx_pps':>8} {'loss_pct':>8}"
            )
            for row in results:
                print(
                    f"{row[0]:>8d} {row[1]:>9d} {row[2]:>6d} {row[3]:>8d} {row[4]:>10d} "
                    f"{row[5]:>8.1f} {row[6]:>8.1f} {row[7]:>8.3f}"
                )
            if hit_target:
                row = results[-1]
                print(
                    f"reached_loss_threshold channels={row[0]} "
                    f"loss_pct={row[7]:.3f} target={target_loss_pct:.3f}"
                )
            else:
                max_tested = results[-1][0] if results else 0
                if stop_exc is not None:
                    stop_reason = "exception"
                    exc_type = type(stop_exc).__name__
                    exc_msg = str(stop_exc)
                elif nch > max_channels:
                    stop_reason = "max_channels_limit"
                    exc_type = "MaxChannelsLimit"
                    exc_msg = f"reached max_channels={max_channels}"
                else:
                    stop_reason = "unknown"
                    exc_type = "UnknownStopCondition"
                    exc_msg = "loop exited without exception and below limit"
                print(
                    f"loss_threshold_not_reached max_tested_channels={max_tested} "
                    f"max_channels={max_channels} target={target_loss_pct:.3f} "
                    f"stop_reason={stop_reason} exception_type={exc_type} "
                    f"exception={exc_msg}"
                )

            self.assertGreaterEqual(len(results), 1)
            self.assertTrue(all(row[1] > 0 for row in results))
            self.assertTrue(all(row[3] <= row[1] for row in results))
            if require_loss_threshold:
                self.assertTrue(hit_target)
        finally:
            srv.shutdown()

    def test_sender_scaling_pps_and_loss(self):
        self._run_sender_scaling(
            label="sender_scaling_pps_and_loss",
            run_seconds=0.40,
            max_channels=self.SCALING_MAX_CHANNELS,
            pps=None,
            require_loss_threshold=True,
        )

    def test_sender_scaling_pps_and_loss_100pps(self):
        self._run_sender_scaling(
            label="sender_scaling_pps_and_loss_100pps",
            run_seconds=1.00,
            max_channels=self.SCALING_MAX_CHANNELS,
            pps=100.0,
            require_loss_threshold=False,
        )

    def test_pkt_in_includes_rtime(self):
        received = []
        max_rtime_lag_ns = 100_000_000
        srv = RtpServer()
        ch = None
        tx = None
        try:
            ch = srv.create_channel(
                pkt_in=lambda pkt, addr, rtime: received.append(
                    (pkt, addr, rtime, mono_clock_ns())
                ),
                bind_host="127.0.0.1",
                bind_port=0,
            )
            addr = ch.local_addr
            tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            tx.sendto(b"rtime-0", addr)
            tx.sendto(b"rtime-1", addr)

            ok = wait_for(lambda: len(received) >= 2)
            self.assertTrue(ok, "timeout waiting for packets with rtime")

            rtimes = [item[2] for item in received]
            self.assertTrue(all(isinstance(v, int) for v in rtimes))
            self.assertTrue(all(v > 0 for v in rtimes))
            self.assertTrue(all(a <= b for a, b in zip(rtimes, rtimes[1:])))
            self.assertTrue(all(item[2] <= item[3] for item in received))
            self.assertTrue(
                all(0 < (item[3] - item[2]) < max_rtime_lag_ns for item in received)
            )
        finally:
            if tx is not None:
                tx.close()
            if ch is not None:
                ch.close()
            srv.shutdown()


if __name__ == "__main__":
    unittest.main()
