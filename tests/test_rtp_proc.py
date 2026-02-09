import os
import random
import sys
import time
import unittest

try:
    from rtpsynth.RtpProc import ChannelProcError, RtpProc
except (ImportError, ModuleNotFoundError):
    if not sys.platform.startswith("win"):
        raise
    ChannelProcError = None
    RtpProc = None


def wait_for(predicate, timeout=1.0, interval=0.005):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return predicate()


class TestRtpProc(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if sys.platform.startswith("win") and RtpProc is None:
            raise unittest.SkipTest("rtpsynth.RtpProc extension module is unavailable")
        cls.proc = RtpProc()

    @classmethod
    def tearDownClass(cls):
        if getattr(cls, "proc", None) is not None:
            cls.proc.shutdown()

    def _run_bulk_profile_case(
        self,
        m_channels,
        rng_seed,
        budget_s,
        init_delay_ns_range,
        period_ns_range,
        total_calls_range,
        interval_abs_tol_ns,
        interval_rel_tol,
    ):
        rng = random.Random(rng_seed)
        states = []

        def make_proc_in(state):
            def proc_in(now_ns, deadline_ns):
                state["calls"].append((now_ns, deadline_ns))
                cnum = len(state["calls"])
                if cnum >= state["total_calls"]:
                    return None
                if cnum == 1:
                    return now_ns + state["initial_delay_ns"]
                return deadline_ns + state["period_ns"]

            return proc_in

        for _ in range(m_channels):
            state = {
                "initial_delay_ns": rng.randint(
                    init_delay_ns_range[0], init_delay_ns_range[1]
                ),
                "period_ns": rng.randint(period_ns_range[0], period_ns_range[1]),
                "total_calls": rng.randint(total_calls_range[0], total_calls_range[1]),
                "calls": [],
                "channel": None,
            }
            state["channel"] = self.proc.create_channel(proc_in=make_proc_in(state))
            states.append(state)

        try:
            done = wait_for(
                lambda: all(len(s["calls"]) >= s["total_calls"] for s in states),
                timeout=budget_s,
                interval=0.002,
            )
            self.assertTrue(
                done, f"timeout waiting for all channels to finish (m={m_channels})"
            )
            time.sleep(0.03)

            expected_total_ns = 0
            actual_total_ns = 0
            interval_bad = 0
            cumulative_rel_overrun = 0.0

            for idx, st in enumerate(states):
                calls = st["calls"]
                self.assertEqual(
                    len(calls),
                    st["total_calls"],
                    f"m={m_channels} channel={idx} unexpected fire count",
                )
                self.assertTrue(
                    all(now_ns >= deadline_ns for now_ns, deadline_ns in calls),
                    f"m={m_channels} channel={idx} observed now<deadline",
                )

                expected_span_ns = st["initial_delay_ns"] + (
                    st["period_ns"] * max(0, st["total_calls"] - 2)
                )
                actual_span_ns = calls[-1][0] - calls[0][0]
                expected_total_ns += expected_span_ns
                actual_total_ns += actual_span_ns

                for i in range(1, len(calls)):
                    expected_interval_ns = (
                        st["initial_delay_ns"] if i == 1 else st["period_ns"]
                    )
                    actual_interval_ns = calls[i][0] - calls[i - 1][0]
                    rel_skew = (
                        (actual_interval_ns - expected_interval_ns)
                        / float(expected_interval_ns)
                    )
                    if rel_skew > interval_rel_tol:
                        cumulative_rel_overrun += (rel_skew - interval_rel_tol)
                    allowed_err_ns = interval_abs_tol_ns + int(
                        expected_interval_ns * interval_rel_tol
                    )
                    if abs(actual_interval_ns - expected_interval_ns) > allowed_err_ns:
                        interval_bad += 1

            drift_pct = (
                0.0
                if expected_total_ns <= 0
                else (
                    abs(actual_total_ns - expected_total_ns)
                    * 100.0
                    / float(expected_total_ns)
                )
            )
            interval_total = max(0, sum(s["total_calls"] - 1 for s in states))
            global_skew_pct = (
                0.0
                if interval_total <= 0
                else (cumulative_rel_overrun * 100.0 / float(interval_total))
            )
            return {
                "m_channels": m_channels,
                "seed": rng_seed,
                "expected_total_ns": expected_total_ns,
                "actual_total_ns": actual_total_ns,
                "drift_pct": drift_pct,
                "global_skew_pct": global_skew_pct,
                "interval_bad": interval_bad,
                "interval_total": interval_total,
            }
        finally:
            for st in states:
                ch = st.get("channel")
                if ch is not None and not ch.closed:
                    ch.close()

    def test_singleton(self):
        proc2 = RtpProc()
        self.assertIs(self.proc, proc2)

    def test_callback_invoked_immediately_then_scheduled(self):
        runs = []

        def proc_in(now_ns, deadline_ns):
            runs.append((now_ns, deadline_ns))
            base_ns = now_ns if deadline_ns == 0 else deadline_ns
            return base_ns + 10_000_000

        ch = self.proc.create_channel(proc_in=proc_in)
        try:
            self.assertGreaterEqual(len(runs), 1)
            ok = wait_for(lambda: len(runs) >= 3, timeout=1.5)
            self.assertTrue(ok, "timeout waiting for scheduled callback runs")
            now_values = [item[0] for item in runs]
            deadline_values = [item[1] for item in runs]
            self.assertTrue(all(a <= b for a, b in zip(now_values, now_values[1:])))
            self.assertTrue(all(a <= b for a, b in zip(deadline_values, deadline_values[1:])))
            self.assertEqual(deadline_values[0], 0)
            self.assertTrue(all(now >= deadline for now, deadline in runs))
        finally:
            ch.close()

    def test_callback_none_unschedules_channel(self):
        runs = []

        def proc_in(now_ns, deadline_ns):
            runs.append((now_ns, deadline_ns))
            return None

        ch = self.proc.create_channel(proc_in=proc_in)
        try:
            self.assertGreaterEqual(len(runs), 1)
            time.sleep(0.05)
            self.assertEqual(len(runs), 1)
        finally:
            ch.close()

    def test_callback_none_after_few_calls_unschedules_cleanly(self):
        period_ns = 5_000_000
        target_calls = 4
        max_lateness_ns = 100_000_000
        runs = []

        def proc_in(now_ns, deadline_ns):
            runs.append((now_ns, deadline_ns))
            if len(runs) >= target_calls:
                return None
            base_ns = now_ns if deadline_ns == 0 else deadline_ns
            return base_ns + period_ns

        ch = self.proc.create_channel(proc_in=proc_in)
        try:
            ok = wait_for(lambda: len(runs) >= target_calls, timeout=1.5)
            self.assertTrue(ok, "timeout waiting for callbacks")
            time.sleep(0.05)
            self.assertEqual(len(runs), target_calls)

            lateness_values = [
                now_ns - deadline_ns
                for now_ns, deadline_ns in runs
                if deadline_ns != 0
            ]
            self.assertGreaterEqual(len(lateness_values), target_calls - 1)
            self.assertTrue(all(lat >= 0 for lat in lateness_values))
            self.assertTrue(all(lat <= max_lateness_ns for lat in lateness_values))
        finally:
            ch.close()

    def test_callback_exception_raised_on_close(self):
        def proc_in(_now_ns, _deadline_ns):
            raise ValueError("boom")

        ch = self.proc.create_channel(proc_in=proc_in)
        with self.assertRaises(ChannelProcError) as cm:
            ch.close()
        self.assertIsInstance(cm.exception.__cause__, ValueError)
        self.assertEqual(str(cm.exception.__cause__), "boom")
        self.assertTrue(ch.closed)

    @unittest.skipUnless(
        os.environ.get("RTPSYNTH_RUN_BULK", "") == "1",
        "bulk RtpProc scaling test disabled; set RTPSYNTH_RUN_BULK=1",
    )
    def test_bulk_randomized_scaling_until_skew_limit(self):
        skew_limit_pct = 5.0
        budget_s = 4.0
        base_seed = 0xC0FFEE
        init_delay_ns_range = (100_000, 2_000_000)
        period_ns_range = (200_000, 1_000_000)
        total_calls_range = (8, 20)
        interval_abs_tol_ns = 20_000_000
        interval_rel_tol = 0.50
        interval_bad_ratio_limit = 0.01
        m = 16
        max_m = 65536
        summaries = []

        while m <= max_m:
            summary = self._run_bulk_profile_case(
                m_channels=m,
                rng_seed=base_seed + m,
                budget_s=budget_s,
                init_delay_ns_range=init_delay_ns_range,
                period_ns_range=period_ns_range,
                total_calls_range=total_calls_range,
                interval_abs_tol_ns=interval_abs_tol_ns,
                interval_rel_tol=interval_rel_tol,
            )
            summaries.append(summary)

            print(
                "bulk_profile "
                f"m={summary['m_channels']} "
                f"global_skew_pct={summary['global_skew_pct']:.3f} "
                f"drift_pct={summary['drift_pct']:.3f} "
                f"interval_bad={summary['interval_bad']}/{summary['interval_total']} "
                f"interval_bad_pct={(100.0 * summary['interval_bad'] / max(1, summary['interval_total'])):.3f} "
                f"expected_s={summary['expected_total_ns'] / 1e9:.3f} "
                f"actual_s={summary['actual_total_ns'] / 1e9:.3f}"
            )

            if summary["global_skew_pct"] > skew_limit_pct:
                break

            self.assertLessEqual(
                summary["interval_bad"] / float(max(1, summary["interval_total"])),
                interval_bad_ratio_limit,
                f"m={m} has too many interval deviations above tolerance",
            )
            m *= 2

        self.assertGreaterEqual(len(summaries), 1)

    def test_create_channel_requires_callable(self):
        with self.assertRaises(TypeError):
            self.proc.create_channel(proc_in=123)

    def test_channel_close_property(self):
        def proc_in(now_ns, _deadline_ns):
            return now_ns + 1_000_000

        ch = self.proc.create_channel(proc_in=proc_in)
        self.assertFalse(ch.closed)
        ch.close()
        self.assertTrue(ch.closed)
        with self.assertRaises(RuntimeError):
            ch.close()
        self.assertTrue(ch.closed)

    def test_z_create_channel_after_shutdown_raises(self):
        self.proc.shutdown()
        with self.assertRaises(RuntimeError):
            self.proc.create_channel(
                proc_in=lambda now_ns, _deadline_ns: now_ns + 1_000_000
            )


if __name__ == "__main__":
    unittest.main()
