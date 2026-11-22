"""Short-term room temperature forecast (standard library only).

For each device we keep the last 15 minutes of readings and fit a straight
line (temperature vs. time) by weighted least squares. Recent readings count
more than old ones (exponential weights, 4-minute half-life), so the trend
follows the room quickly when it stops warming up or cooling down.

From the line we predict:
  * the temperature 10 minutes from now, with a 95 % prediction interval;
  * how many minutes until the alert threshold is reached, if the room is
    warming up fast enough to get there within the hour.

The model also scores itself: every prediction is compared with the reading
that actually arrives 10 minutes later, and the mean absolute error (MAE) is
reported next to that of a naive baseline ("in 10 minutes it will be the same
as now"), so you can see whether the trend model is worth having. It also
reports how often the 95 % interval really contained the reading, and once it
has scored enough predictions it widens the interval to match the errors it
has actually made (the formula alone only knows about sensor noise, not about
the trend bending, e.g. when a heater cycles or the room stops warming up).
"""

import math
from collections import deque

WINDOW_S = 15 * 60         # readings used for the fit
HALF_LIFE_S = 4 * 60       # weight halves every 4 minutes of age
HORIZON_S = 10 * 60        # how far ahead we predict
MIN_SPAN_S = 5 * 60        # need at least this much history ...
MIN_SAMPLES = 10           # ... and this many readings before forecasting
MAX_GAP_S = 60             # a longer gap means the device was offline: start over
MIN_RISE_C_PER_MIN = 0.02  # slower than 1.2 °C/hour counts as "not rising"
MAX_ALERT_MIN = 60         # don't announce alerts further away than this
EVAL_TOLERANCE_S = 30      # a reading this close to a target time scores it
EVAL_KEEP = 50             # MAE over the last 50 scored predictions
RESIDUAL_FLOOR_C = 0.1     # sensor resolution: never claim less noise than this
MIN_EVALS_TO_CALIBRATE = 10
MAE_TO_SIGMA = 1.2533      # for normally distributed errors, std = 1.2533 * MAE
Z_95 = 1.96


class Forecaster:
    """Forecast state for one device. Feed readings with add(), ask forecast()."""

    def __init__(self):
        self.samples = deque()   # (t, temperature, threshold)
        self.pending = deque()   # (target_t, predicted, baseline, low, high) awaiting the real reading
        self.errors = deque(maxlen=EVAL_KEEP)           # |actual - predicted|
        self.baseline_errors = deque(maxlen=EVAL_KEEP)  # |actual - temperature when predicted|
        self.covered = deque(maxlen=EVAL_KEEP)          # actual inside [low, high]?
        self.last_uptime = None

    # ---------- input ----------

    def add(self, t, temperature, threshold, uptime_s=None):
        """Add a reading. uptime_s (optional) is the device's uptime: when it goes back
        down, the device has restarted and the old readings may not belong to the new run.
        A short drop-out without a restart (Wi-Fi or broker) keeps the history."""
        restarted = (uptime_s is not None and self.last_uptime is not None
                     and uptime_s < self.last_uptime)
        if restarted:
            self.restart()
        elif self.samples and t - self.samples[-1][0] > MAX_GAP_S:
            self.samples.clear()
        self.last_uptime = uptime_s
        self.samples.append((t, float(temperature), float(threshold)))
        while self.samples and self.samples[0][0] < t - WINDOW_S:
            self.samples.popleft()
        self._score(t, float(temperature))

    def restart(self):
        """The device restarted: drop its readings and the predictions
        still waiting to be scored (they were made from the old readings). The error
        history is kept: it is the model's track record for this device."""
        self.samples.clear()
        self.pending.clear()
        self.last_uptime = None

    def _score(self, t, actual):
        while self.pending and self.pending[0][0] <= t:
            target, predicted, baseline, low, high = self.pending.popleft()
            if t - target <= EVAL_TOLERANCE_S:
                self.errors.append(abs(actual - predicted))
                self.baseline_errors.append(abs(actual - baseline))
                self.covered.append(low <= actual <= high)

    # ---------- output ----------

    def forecast(self):
        """Return the forecast as a dict of numbers, or None while warming up."""
        if len(self.samples) < MIN_SAMPLES:
            return None
        t_now = self.samples[-1][0]
        if t_now - self.samples[0][0] < MIN_SPAN_S:
            return None

        # x in minutes relative to now (x = 0 is the latest reading).
        xs = [(t - t_now) / 60.0 for t, _, _ in self.samples]
        ys = [temp for _, temp, _ in self.samples]
        ws = [0.5 ** ((t_now - t) / HALF_LIFE_S) for t, _, _ in self.samples]

        sw = sum(ws)
        n_eff = sw * sw / sum(w * w for w in ws)  # how many readings the weights are "worth"
        xm = sum(w * x for w, x in zip(ws, xs)) / sw
        ym = sum(w * y for w, y in zip(ws, ys)) / sw
        sxx = sum(w * (x - xm) ** 2 for w, x in zip(ws, xs))
        sxy = sum(w * (x - xm) * (y - ym) for w, x, y in zip(ws, xs, ys))
        slope = sxy / sxx              # °C per minute
        level = ym - slope * xm        # fitted temperature now

        # Residual noise, scaled so the weights behave like n_eff equal readings.
        ss = sum(w * (y - (level + slope * x)) ** 2 for w, x, y in zip(ws, xs, ys)) / sw
        s2 = ss * n_eff / (n_eff - 2) if n_eff > 2 else ss
        s2 = max(s2, RESIDUAL_FLOOR_C ** 2)

        x0 = HORIZON_S / 60.0
        predicted = level + slope * x0
        var_fit = s2 * (1.0 / n_eff + (x0 - xm) ** 2 / (sxx * n_eff / sw))
        # The DHT11 reports whole degrees, so the reading we are predicting is
        # itself rounded: add the rounding error (uniform, variance res^2 / 12).
        # Without this the 95 % interval only covered ~89 % in simulation.
        res = 1.0 if all(abs(y - round(y)) < 1e-6 for y in ys) else 0.1
        half_width = Z_95 * math.sqrt(s2 + var_fit + res * res / 12)
        # Never claim more confidence than the errors actually measured allow.
        if len(self.errors) >= MIN_EVALS_TO_CALIBRATE:
            mae = sum(self.errors) / len(self.errors)
            half_width = max(half_width, Z_95 * MAE_TO_SIGMA * mae)

        latest_temp = ys[-1]
        threshold = self.samples[-1][2]
        if latest_temp >= threshold:
            minutes = 0.0
        elif slope < MIN_RISE_C_PER_MIN:
            minutes = -1.0
        else:
            minutes = max(0.0, (threshold - level) / slope)
            if minutes > MAX_ALERT_MIN:
                minutes = -1.0

        # Remember this prediction so it can be scored when its time comes.
        if not self.pending or self.pending[-1][0] < t_now + HORIZON_S:
            self.pending.append((t_now + HORIZON_S, predicted, latest_temp,
                                 predicted - half_width, predicted + half_width))

        out = {
            "temp_in_10m": round(predicted, 2),
            "temp_in_10m_low": round(predicted - half_width, 2),
            "temp_in_10m_high": round(predicted + half_width, 2),
            "slope_c_per_min": round(slope, 4),
            "minutes_to_threshold": round(minutes, 1),
            "samples": len(ys),
        }
        if self.errors:
            out["mae_10m"] = round(sum(self.errors) / len(self.errors), 3)
            out["mae_baseline_10m"] = round(sum(self.baseline_errors) / len(self.baseline_errors), 3)
            out["evaluations"] = len(self.errors)
            out["interval_coverage"] = round(100.0 * sum(self.covered) / len(self.covered), 1)
        return out


# Every field forecast() can return (used by the tests and the Grafana contract check).
OUTPUT_FIELDS = {"temp_in_10m", "temp_in_10m_low", "temp_in_10m_high", "slope_c_per_min",
                 "minutes_to_threshold", "samples", "mae_10m", "mae_baseline_10m", "evaluations",
                 "interval_coverage"}
