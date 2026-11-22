"""Unit tests for server/forecaster/forecast_model.py (standard library only).

    python3 -m unittest discover tests/forecaster
"""

import math
import random
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "server" / "forecaster"))

import forecast_model as fm  # noqa: E402


def feed(f, temps, start=0.0, step=2.0, threshold=30.0):
    """Feed readings every `step` seconds; returns the time of the last one."""
    t = start
    for temp in temps:
        f.add(t, temp, threshold)
        t += step
    return t - step


class ForecastModelTest(unittest.TestCase):
    def test_warms_up_before_forecasting(self):
        f = fm.Forecaster()
        feed(f, [24.0] * 100)  # 200 s: not enough span yet
        self.assertIsNone(f.forecast())
        feed(f, [24.0] * 60, start=200)  # now 5+ minutes
        self.assertIsNotNone(f.forecast())

    def test_ramp_recovers_slope_and_eta(self):
        f = fm.Forecaster()
        # 0.1 °C per minute from 24 °C, 10 minutes of readings every 2 s.
        temps = [24.0 + 0.1 * (i * 2 / 60) for i in range(300)]
        feed(f, temps)
        r = f.forecast()
        self.assertAlmostEqual(r["slope_c_per_min"], 0.1, places=3)
        now = temps[-1]
        self.assertAlmostEqual(r["temp_in_10m"], now + 1.0, places=2)
        self.assertAlmostEqual(r["minutes_to_threshold"], (30.0 - now) / 0.1, delta=0.2)
        self.assertLess(r["temp_in_10m_low"], r["temp_in_10m"])
        self.assertGreater(r["temp_in_10m_high"], r["temp_in_10m"])

    def test_flat_room_is_not_rising(self):
        f = fm.Forecaster()
        feed(f, [24.0] * 300)
        r = f.forecast()
        self.assertEqual(r["minutes_to_threshold"], -1)
        self.assertAlmostEqual(r["temp_in_10m"], 24.0, places=2)

    def test_slow_rise_beyond_an_hour_is_not_reported(self):
        f = fm.Forecaster()
        # 0.05 °C/min from 24 °C: 30 °C is ~2 hours away.
        feed(f, [24.0 + 0.05 * (i * 2 / 60) for i in range(300)])
        self.assertEqual(f.forecast()["minutes_to_threshold"], -1)

    def test_already_at_threshold(self):
        f = fm.Forecaster()
        feed(f, [30.5] * 300)
        self.assertEqual(f.forecast()["minutes_to_threshold"], 0)

    def test_interval_covers_truth_with_noise(self):
        rng = random.Random(286)
        covered, trials = 0, 200
        for _ in range(trials):
            f = fm.Forecaster()
            temps = [24.0 + 0.08 * (i * 2 / 60) + rng.gauss(0, 0.3) for i in range(300)]
            last = feed(f, temps)
            r = f.forecast()
            truth = 24.0 + 0.08 * ((last + fm.HORIZON_S) / 60) + rng.gauss(0, 0.3)
            covered += r["temp_in_10m_low"] <= truth <= r["temp_in_10m_high"]
        # Nominal 95 %; allow for sampling error and the weighted fit.
        self.assertGreater(covered / trials, 0.85)

    def test_interval_covers_truth_at_dht11_resolution(self):
        # The DHT11 reports whole degrees.
        rng = random.Random(7)
        covered, trials = 0, 300
        for _ in range(trials):
            f = fm.Forecaster()
            slope = rng.uniform(-0.05, 0.15)
            last = feed(f, [round(24 + slope * (i * 2 / 60) + rng.gauss(0, 0.2)) for i in range(450)])
            r = f.forecast()
            truth = round(24 + slope * ((last + fm.HORIZON_S) / 60) + rng.gauss(0, 0.2))
            covered += r["temp_in_10m_low"] <= truth <= r["temp_in_10m_high"]
        self.assertGreater(covered / trials, 0.9)

    def test_offline_gap_resets_history(self):
        f = fm.Forecaster()
        last = feed(f, [24.0] * 300)
        self.assertIsNotNone(f.forecast())
        f.add(last + 300, 24.0, 30.0)  # device was gone for 5 minutes
        self.assertEqual(len(f.samples), 1)
        self.assertIsNone(f.forecast())

    def test_device_restart_resets_history(self):
        # Device runs warm, restarts within a minute in a cooler room (uptime goes back to 0).
        f = fm.Forecaster()
        t = 0.0
        for i in range(450):
            f.add(t, 33.0, 30.0, uptime_s=600 + i * 2)
            if i % 5 == 0:
                f.forecast()
            t += 2
        self.assertTrue(f.pending)
        f.add(t + 20, 24.0, 30.0, uptime_s=1)
        self.assertEqual(len(f.samples), 1)
        self.assertFalse(f.pending)  # predictions from the old run are not scored
        self.assertIsNone(f.forecast())
        # The new run then forecasts from its own readings only.
        last = feed(f, [24.0] * 300, start=t + 22)
        self.assertAlmostEqual(f.forecast()["temp_in_10m"], 24.0, places=2)
        self.assertGreater(last - f.samples[0][0], fm.MIN_SPAN_S)

    def test_short_dropout_without_restart_keeps_history(self):
        # Wi-Fi or broker drop-out of 30 s: uptime keeps counting, so it is the same run.
        f = fm.Forecaster()
        t = 0.0
        for i in range(300):
            f.add(t, 24.0, 30.0, uptime_s=t)
            t += 2
        t += 30
        f.add(t, 24.0, 30.0, uptime_s=t)
        self.assertEqual(len(f.samples), 301)
        self.assertIsNotNone(f.forecast())

    def test_window_keeps_only_last_15_minutes(self):
        f = fm.Forecaster()
        feed(f, [24.0] * 900)  # 30 minutes
        span = f.samples[-1][0] - f.samples[0][0]
        self.assertLessEqual(span, fm.WINDOW_S)

    def test_scores_itself_and_beats_baseline_on_a_ramp(self):
        f = fm.Forecaster()
        rng = random.Random(1)
        t, temp = 0.0, 24.0
        r = None
        for _ in range(30 * 30):  # 30 minutes, reading every 2 s, forecast every 10 s
            temp += 0.1 * 2 / 60
            f.add(t, round(temp + rng.gauss(0, 0.1), 1), 40.0)
            if int(t) % 10 == 0:
                r = f.forecast() or r
            t += 2
        self.assertIn("mae_10m", r)
        self.assertGreater(r["evaluations"], 20)
        # Baseline is off by ~1 °C (the 10-minute rise); the trend should be much closer.
        self.assertGreater(r["mae_baseline_10m"], 0.8)
        self.assertLess(r["mae_10m"], 0.3)

    def test_interval_widens_to_match_measured_errors(self):
        # A room warming up while a heater cycles (slow wave): the trend bends,
        # which the noise-only interval formula cannot know about.
        def room(t):
            return 24 + 0.45 * t / 60 + 0.3 * math.sin(t / 1200 * 2 * math.pi)

        f = fm.Forecaster()
        rng = random.Random(3)
        t, r = 0.0, None
        for i in range(60 * 30):  # 60 minutes
            f.add(t, round(room(t) + rng.gauss(0, 0.08), 1), 99.0)
            if i % 5 == 0:
                r = f.forecast() or r
            t += 2
        self.assertGreaterEqual(r["evaluations"], fm.MIN_EVALS_TO_CALIBRATE)
        half = (r["temp_in_10m_high"] - r["temp_in_10m_low"]) / 2
        self.assertGreaterEqual(half, fm.Z_95 * fm.MAE_TO_SIGMA * r["mae_10m"] - 0.01)
        self.assertGreater(r["interval_coverage"], 80)

    def test_output_fields_documented(self):
        f = fm.Forecaster()
        rng = random.Random(2)
        t = 0.0
        r = None
        for i in range(900):
            f.add(t, 24 + 0.05 * t / 60 + rng.gauss(0, 0.1), 30.0)
            if i % 5 == 0:
                r = f.forecast() or r
            t += 2
        self.assertEqual(set(r), fm.OUTPUT_FIELDS)
        for k, v in r.items():
            self.assertIsInstance(v, (int, float), k)


if __name__ == "__main__":
    unittest.main()
