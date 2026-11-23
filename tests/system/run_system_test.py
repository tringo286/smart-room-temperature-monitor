#!/usr/bin/env python3
"""End-to-end system test against the running Docker stack (about 17 minutes).

    cd server && docker compose up -d && cd ..
    pip install -r tools/requirements.txt
    python3 tests/system/run_system_test.py

The device is the sensor simulator (same topics, payload, Last Will and
commands as the firmware) with a slow heat event, so the test can check that
the forecast predicts the alert before it happens. Each run uses a new device
id ("systest-<time>") and deletes that device's data afterwards. A new id
matters: the forecaster keeps each device's error history while it runs, so
reusing an id would score this run with the previous run's predictions.

Checks, in order:
  1. Stack: all five containers running, InfluxDB and Grafana healthy.
  2. Device online: retained status "online", telemetry every ~2 s with
     every firmware field, rows stored in InfluxDB.
  3. Commands: threshold and mute over MQTT take effect; invalid values
     are ignored.
  4. Broker restart: device, Telegraf and forecaster all reconnect.
  5. Forecast: published and stored; predicts the alert before it fires.
  6. Alert fires; the forecast then reports "now".
  7. Self-scoring: forecast error vs. the persistence baseline.
  8. Grafana: every query of every dashboard panel runs without error and
     returns data for this device.
  9. Device crash: the broker publishes the Last Will "offline".
 10. Device restart: the forecaster drops the old readings.
"""

import base64
import json
import os
import signal
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

import paho.mqtt.client as mqtt

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "server" / "forecaster"))
import forecast_model as fm  # noqa: E402

DEVICE = f"systest-{int(time.time())}"
BASE = f"smartroom/{DEVICE}"
INFLUX = "http://localhost:8086"
TOKEN = "smartroom-dev-token"
GRAFANA = "http://localhost:3000"
GRAFANA_AUTH = "Basic " + base64.b64encode(b"admin:admin").decode()
SERVICES = {"mosquitto", "influxdb", "telegraf", "forecaster", "grafana"}
FIRMWARE_FIELDS = {"temperature", "humidity", "heat_index", "threshold", "alert",
                   "buzzer_muted", "sensor_errors", "rssi", "uptime_s"}
HEATWAVE_S = 900  # 24 -> 33 C over 15 min: crosses 30 C after ~10 min

results, failures = {}, []


def check(name, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""), flush=True)
    if not ok:
        failures.append(name)
    return ok


def http(url, data=None, headers=None, method=None):
    req = urllib.request.Request(url, data=data, headers=headers or {}, method=method)
    with urllib.request.urlopen(req, timeout=15) as r:
        return r.read().decode()


def flux_rows(flux):
    csv = http(f"{INFLUX}/api/v2/query?org=smartroom", flux.encode(),
               {"Authorization": f"Token {TOKEN}", "Content-Type": "application/vnd.flux",
                "Accept": "application/csv"})
    return [l for l in csv.splitlines() if l.strip(",\r") and not l.startswith(",result")]


def count_stored(measurement, since):
    rows = flux_rows(f'from(bucket:"smartroom") |> range(start: {since}) '
                     f'|> filter(fn:(r)=>r._measurement=="{measurement}" and r.device=="{DEVICE}") '
                     f'|> group() |> count()')
    return int(rows[0].split(",")[-1]) if rows else 0


class Watcher:
    """Records everything the device and the forecaster publish."""

    def __init__(self):
        self.lock = threading.Lock()
        self.telemetry, self.forecasts, self.status = [], [], []
        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="systest-watcher")
        self.c.on_connect = lambda c, *_: c.subscribe(f"{BASE}/#")
        self.c.on_message = self.on_message
        self.c.reconnect_delay_set(1, 2)
        self.c.connect("localhost", 1883, keepalive=15)
        self.c.loop_start()

    def on_message(self, c, u, msg):
        now = time.time()
        with self.lock:
            if msg.topic == f"{BASE}/telemetry":
                self.telemetry.append((now, json.loads(msg.payload)))
            elif msg.topic == f"{BASE}/forecast":
                self.forecasts.append((now, json.loads(msg.payload)))
            elif msg.topic == f"{BASE}/status":
                self.status.append((now, msg.payload.decode(), msg.retain))

    def wait(self, what, pred, timeout):
        """Wait until pred(list of (t, value)) returns something truthy."""
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                hit = pred(list(getattr(self, what)))
            if hit:
                return hit
            time.sleep(0.2)
        return None

    def publish(self, topic, payload):
        self.c.publish(f"{BASE}/{topic}", payload).wait_for_publish(5)


def latest_after(t0, key, value):
    return lambda xs: next((t for t, m in xs if t > t0 and m.get(key) == value), None)


def grafana_queries():
    dash = json.loads(http(f"{GRAFANA}/api/dashboards/uid/smart-room",
                           headers={"Authorization": GRAFANA_AUTH}))["dashboard"]
    for p in dash["panels"]:
        for t in p.get("targets", []):
            yield p["title"], t["refId"], t["query"].replace("${device}", DEVICE)


def run_grafana_query(query):
    body = json.dumps({"from": "now-30m", "to": "now+10m", "queries": [{
        "refId": "A", "datasource": {"type": "influxdb", "uid": "influxdb"},
        "query": query, "intervalMs": 5000, "maxDataPoints": 500}]}).encode()
    out = json.loads(http(f"{GRAFANA}/api/ds/query", body,
                          {"Authorization": GRAFANA_AUTH, "Content-Type": "application/json"}))
    res = out["results"]["A"]
    if "error" in res:
        return res["error"], 0
    # Count non-null values in every non-time column (summary stats have no time column).
    values = sum(1 for f in res.get("frames", [])
                 for field, col in zip(f["schema"]["fields"], f["data"]["values"])
                 if field["type"] != "time" for v in col if v is not None)
    return None, values


def cleanup(sim):
    if sim and sim.poll() is None:
        sim.kill()
    body = json.dumps({"start": "1970-01-01T00:00:00Z", "stop": "2100-01-01T00:00:00Z",
                       "predicate": f'device="{DEVICE}"'}).encode()
    http(f"{INFLUX}/api/v2/delete?org=smartroom&bucket=smartroom", body,
         {"Authorization": f"Token {TOKEN}", "Content-Type": "application/json"})
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="systest-clean")
    c.connect("localhost", 1883)
    c.loop_start()
    c.publish(f"{BASE}/status", b"", retain=True).wait_for_publish(5)
    c.loop_stop()


def main():
    print("1. Stack", flush=True)
    ps = subprocess.run(["docker", "compose", "ps", "--format", "{{.Service}} {{.State}}"],
                        cwd=ROOT / "server", capture_output=True, text=True).stdout.split("\n")
    running = {l.split()[0] for l in ps if l.endswith("running")}
    check("all five containers running", SERVICES <= running, " ".join(sorted(running)))
    check("InfluxDB healthy", json.loads(http(f"{INFLUX}/health"))["status"] == "pass")
    check("Grafana healthy", json.loads(http(f"{GRAFANA}/api/health"))["database"] == "ok")
    cleanup(None)

    w = Watcher()
    t_start = time.time()
    sim = subprocess.Popen([sys.executable, str(ROOT / "tools" / "simulate_sensor.py"),
                            "--device", DEVICE, "--interval", "2", "--heatwave", str(HEATWAVE_S),
                            "--swing", "0.2", "--swing-period", "1200"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        print("2. Device online", flush=True)
        check("status 'online' (retained)",
              w.wait("status", lambda xs: any(s == "online" for _, s, _ in xs), 10))
        got = w.wait("telemetry", lambda xs: xs if len(xs) >= 6 else None, 20)
        if check("telemetry received", got):
            gaps = [b[0] - a[0] for a, b in zip(got, got[1:])]
            check("telemetry has every firmware field", set(got[-1][1]) == FIRMWARE_FIELDS,
                  ", ".join(sorted(set(got[-1][1]) ^ FIRMWARE_FIELDS)))
            check("telemetry about every 2 s", all(1.5 < g < 2.5 for g in gaps),
                  f"gaps {min(gaps):.2f}-{max(gaps):.2f} s")
        time.sleep(7)  # Telegraf flushes every 5 s
        check("telemetry stored in InfluxDB", count_stored("room_climate", "-5m") > 0)

        print("3. Commands over MQTT", flush=True)
        t0 = time.time()
        w.publish("cmd/threshold", "27.5")
        check("threshold 27.5 applied", w.wait("telemetry", latest_after(t0, "threshold", 27.5), 6))
        t1 = time.time()
        w.publish("cmd/threshold", "abc")
        w.publish("cmd/threshold", "75")
        time.sleep(4.5)
        with w.lock:
            after = [m["threshold"] for t, m in w.telemetry if t > t1 + 0.5]
        check("invalid thresholds ignored", after and all(v == 27.5 for v in after), f"{after}")
        t0 = time.time()
        w.publish("cmd/mute", "1")
        check("mute applied", w.wait("telemetry", latest_after(t0, "buzzer_muted", 1), 6))
        t0 = time.time()
        w.publish("cmd/mute", "off")
        check("unmute applied", w.wait("telemetry", latest_after(t0, "buzzer_muted", 0), 6))
        t0 = time.time()
        w.publish("cmd/threshold", "30")
        check("threshold back to 30", w.wait("telemetry", latest_after(t0, "threshold", 30.0), 6))

        print("4. Broker restart", flush=True)
        t0 = time.time()
        subprocess.run(["docker", "compose", "restart", "mosquitto"], cwd=ROOT / "server",
                       capture_output=True, check=True)
        t_up = time.time()
        back = w.wait("telemetry", lambda xs: next((t for t, _ in xs if t > t_up), None), 60)
        if check("device telemetry resumes", back, f"{back - t0:.1f} s after restart" if back else ""):
            results["broker_restart_outage_s"] = round(back - t0, 1)
        time.sleep(12)
        check("Telegraf stores again after the restart",
              count_stored("room_climate", f"{int(t_up) + 2}") > 0)

        print("5. Forecast (first one ~5 min after the device starts)", flush=True)
        first = w.wait("forecasts", lambda xs: xs[0] if xs else None, 420)
        if check("forecast published", first, f"{first[0] - t_start:.0f} s after start" if first else ""):
            t_fc, fc = first
            results["first_forecast_s"] = round(t_fc - t_start)
            check("forecast fields documented", set(fc) <= fm.OUTPUT_FIELDS)
            results["first_forecast_eta_min"] = fc["minutes_to_threshold"]
            check("forecast expects the alert", fc["minutes_to_threshold"] > 0,
                  f"in {fc['minutes_to_threshold']} min")
            time.sleep(7)
            check("forecast stored in InfluxDB (room_forecast)", count_stored("room_forecast", "-10m") > 0)
            check("forecaster reconnected after the broker restart", t_fc > t_up)

        print("6. Alert", flush=True)
        alert_t = w.wait("telemetry", lambda xs: next((t for t, m in xs if m["alert"] == 1), None), 600)
        if check("alert fires", alert_t, f"{alert_t - t_start:.0f} s after start" if alert_t else "") and first:
            predicted = first[0] + first[1]["minutes_to_threshold"] * 60
            lead = (alert_t - first[0]) / 60
            results["alert_s"] = round(alert_t - t_start)
            results["warning_lead_min"] = round(lead, 1)
            results["eta_error_min"] = round((predicted - alert_t) / 60, 1)
            check("first forecast warned before the alert", lead > 0,
                  f"{lead:.1f} min ahead; predicted time off by {(predicted - alert_t) / 60:+.1f} min")
            now_fc = w.wait("forecasts", lambda xs: next(
                (m for t, m in xs if t > alert_t + 3 and m["minutes_to_threshold"] == 0), None), 30)
            check("forecast reports 'alert now' once in alert", now_fc)
            with w.lock:
                etas = [(round((t - t_start) / 60, 1), m["minutes_to_threshold"])
                        for t, m in w.forecasts if t < alert_t]
            results["countdown"] = etas[::6]

        print("7. Self-scoring (first score 10 min after the first forecast)", flush=True)
        scored = w.wait("forecasts", lambda xs: next((m for t, m in xs if "mae_10m" in m), None), 720)
        if check("forecast scored against later readings", scored):
            time.sleep(60)  # a few more scores
            with w.lock:
                last = w.forecasts[-1][1]
            results["mae_10m"] = last.get("mae_10m")
            results["mae_baseline_10m"] = last.get("mae_baseline_10m")
            results["evaluations"] = last.get("evaluations")
            results["interval_coverage"] = last.get("interval_coverage")
            check("forecast beats the persistence baseline",
                  last["mae_10m"] < last["mae_baseline_10m"],
                  f"MAE {last['mae_10m']} vs {last['mae_baseline_10m']} C over {last['evaluations']}")

        print("8. Grafana dashboard queries", flush=True)
        n_ok = n = 0
        for title, ref, query in grafana_queries():
            n += 1
            err, values = run_grafana_query(query)
            ok = check(f"panel '{title}' query {ref}", err is None and values > 0,
                       err or f"{values} values")
            n_ok += ok
        results["grafana_queries_ok"] = f"{n_ok}/{n}"

        print("9. Device crash -> Last Will", flush=True)
        t0 = time.time()
        sim.send_signal(signal.SIGKILL)
        off = w.wait("status", lambda xs: next((t for t, s, r in xs if t > t0 and s == "offline"), None), 40)
        if check("broker publishes 'offline'", off, f"{off - t0:.2f} s after the crash" if off else ""):
            results["lwt_after_crash_s"] = round(off - t0, 2)

        print("10. Device restarts within a minute -> forecaster starts over", flush=True)
        sim = subprocess.Popen([sys.executable, str(ROOT / "tools" / "simulate_sensor.py"),
                                "--device", DEVICE, "--interval", "2"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        t_re = time.time()
        check("restarted device publishes again",
              w.wait("telemetry", lambda xs: next((t for t, _ in xs if t > t_re), None), 15))
        time.sleep(45)
        with w.lock:
            stale = [m for t, m in w.forecasts if t > t_re]
        check("no forecast from the old readings after the restart", not stale,
              f"{len(stale)} forecasts" if stale else "waits for 5 min of new readings")
    finally:
        w.c.loop_stop()
        cleanup(sim)

    results["duration_min"] = round((time.time() - t_start) / 60, 1)
    print(json.dumps(results))
    print("ALL PASSED" if not failures else f"{len(failures)} FAILED: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
