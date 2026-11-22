#!/usr/bin/env python3
"""Measure the server-side data pipeline (numbers used in the report).

Needs the Docker stack in server/ running (docker compose up -d) and
paho-mqtt installed. It uses its own device ids (bench-*) and deletes their
data from InfluxDB afterwards, so the dashboards are not affected.

    python3 tools/measure_pipeline.py

Measures:
  1. MQTT broker hop: publish -> subscriber receives (same computer).
  2. End to end: MQTT publish -> row queryable in InfluxDB (through Telegraf).
  3. Offline detection with the MQTT Last Will:
       a. the client's TCP connection closes (e.g. the board resets)
       b. the client goes silent without closing (e.g. power or Wi-Fi loss),
          detected by the broker's keep-alive timeout.
"""

import json
import socket
import statistics
import subprocess
import sys
import threading
import time
import urllib.request

import paho.mqtt.client as mqtt

HOST, PORT = "localhost", 1883
INFLUX = "http://localhost:8086"
TOKEN = "smartroom-dev-token"
KEEPALIVE_S = 15  # PubSubClient's default keep-alive, same as the firmware


def client(cid, **kw):
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=cid, **kw)
    return c


def summary(name, values, unit):
    values = sorted(values)
    p95 = values[min(len(values) - 1, int(round(0.95 * (len(values) - 1))))]
    out = {"name": name, "n": len(values), "unit": unit,
           "median": round(statistics.median(values), 3), "p95": round(p95, 3),
           "min": round(values[0], 3), "max": round(values[-1], 3)}
    print(f"  {name}: median {out['median']} {unit}, p95 {out['p95']} {unit}, "
          f"min {out['min']}, max {out['max']} (n={len(values)})", flush=True)
    return out


# ---------- 1. broker hop ----------

def broker_hop(n=200):
    got = {}
    ev = threading.Event()

    def on_message(c, u, msg):
        got[msg.payload.decode()] = time.perf_counter()
        ev.set()

    sub = client("bench-sub")
    sub.on_message = on_message
    sub.connect(HOST, PORT)
    sub.subscribe("smartroom/bench-hop/telemetry")
    sub.loop_start()
    pub = client("bench-pub")
    pub.connect(HOST, PORT)
    pub.loop_start()
    time.sleep(0.5)
    lat = []
    for i in range(n):
        ev.clear()
        key = f"hop-{i}"
        t0 = time.perf_counter()
        pub.publish("smartroom/bench-hop/telemetry", key)
        if ev.wait(2) and key in got:
            lat.append((got[key] - t0) * 1000)
        time.sleep(0.01)
    sub.loop_stop(); pub.loop_stop()
    return summary("MQTT publish -> subscriber", lat, "ms")


# ---------- 2. end to end into InfluxDB ----------

def influx_count(device, value):
    flux = (f'from(bucket:"smartroom") |> range(start:-10m) '
            f'|> filter(fn:(r)=>r._measurement=="room_climate" and r.device=="{device}" '
            f'and r._field=="uptime_s" and r._value=={value})')
    req = urllib.request.Request(
        f"{INFLUX}/api/v2/query?org=smartroom", data=flux.encode(),
        headers={"Authorization": f"Token {TOKEN}", "Content-Type": "application/vnd.flux",
                 "Accept": "application/csv"})
    with urllib.request.urlopen(req, timeout=5) as r:
        rows = [l for l in r.read().decode().splitlines() if l and not l.startswith(",result") and l.strip(",\r")]
    return len(rows)


def end_to_end(n=30):
    pub = client("bench-e2e")
    pub.connect(HOST, PORT)
    pub.loop_start()
    time.sleep(0.5)
    lat = []
    for i in range(n):
        marker = 900000 + i  # unique uptime_s value to find the row
        payload = {"temperature": 24.0, "humidity": 45.0, "heat_index": 23.8, "threshold": 30.0,
                   "alert": 0, "buzzer_muted": 0, "sensor_errors": 0, "rssi": -60, "uptime_s": marker}
        t0 = time.perf_counter()
        pub.publish("smartroom/bench-e2e/telemetry", json.dumps(payload))
        while time.perf_counter() - t0 < 30:
            if influx_count("bench-e2e", marker):
                lat.append(time.perf_counter() - t0)
                break
            time.sleep(0.05)
        # spread the publishes over Telegraf's flush cycle
        time.sleep(0.37)
    pub.loop_stop()
    return summary("MQTT publish -> stored in InfluxDB", lat, "s")


# ---------- 3. Last Will ----------

def wait_for_will(device, trigger):
    """Subscribe to the device's status topic, run trigger(), time until 'offline'."""
    ev = threading.Event()
    t = {}

    def on_message(c, u, msg):
        if msg.payload == b"offline" and not msg.retain:
            t["end"] = time.perf_counter()
            ev.set()

    watcher = client(f"bench-watch-{device}")
    watcher.on_message = on_message
    watcher.connect(HOST, PORT)
    watcher.subscribe(f"smartroom/{device}/status")
    watcher.loop_start()
    time.sleep(0.5)
    start = trigger()
    ok = ev.wait(KEEPALIVE_S * 3)
    watcher.loop_stop()
    return (t["end"] - start) if ok else None


DEVICE_SCRIPT = f"""
import sys, time, paho.mqtt.client as mqtt
c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=sys.argv[1])
c.will_set("smartroom/" + sys.argv[1] + "/status", "offline", qos=1, retain=True)
c.connect(sys.argv[2], int(sys.argv[3]), keepalive={KEEPALIVE_S})
c.loop_start()
c.publish("smartroom/" + sys.argv[1] + "/status", "online", qos=1, retain=True)
print("ready", flush=True)
time.sleep(600)
"""


def will_on_reset():
    """Device process killed: the OS closes the TCP connection."""
    dev = "bench-reset"
    p = subprocess.Popen([sys.executable, "-c", DEVICE_SCRIPT, dev, HOST, str(PORT)], stdout=subprocess.PIPE)
    p.stdout.readline()
    time.sleep(1)

    def trigger():
        t0 = time.perf_counter()
        p.kill()
        return t0
    return wait_for_will(dev, trigger)


class StallProxy:
    """TCP proxy that can stop forwarding without closing (like power loss)."""

    def __init__(self):
        self.stalled = False
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        a, _ = self.srv.accept()
        b = socket.create_connection((HOST, PORT))
        self.socks = (a, b)
        for src, dst in ((a, b), (b, a)):
            threading.Thread(target=self._pipe, args=(src, dst), daemon=True).start()

    def _pipe(self, src, dst):
        while True:
            try:
                data = src.recv(4096)
            except OSError:
                return
            if not data:
                return
            if not self.stalled:
                dst.sendall(data)


def will_on_power_loss():
    dev = "bench-powerloss"
    proxy = StallProxy()
    p = subprocess.Popen([sys.executable, "-c", DEVICE_SCRIPT, dev, "127.0.0.1", str(proxy.port)],
                         stdout=subprocess.PIPE)
    p.stdout.readline()
    time.sleep(1)

    def trigger():
        t0 = time.perf_counter()
        proxy.stalled = True
        return t0
    try:
        return wait_for_will(dev, trigger)
    finally:
        p.kill()


def cleanup():
    for device in ("bench-e2e", "bench-reset", "bench-powerloss"):
        body = json.dumps({"start": "1970-01-01T00:00:00Z", "stop": "2100-01-01T00:00:00Z",
                           "predicate": f'device="{device}"'}).encode()
        req = urllib.request.Request(f"{INFLUX}/api/v2/delete?org=smartroom&bucket=smartroom", data=body,
                                     headers={"Authorization": f"Token {TOKEN}",
                                              "Content-Type": "application/json"})
        urllib.request.urlopen(req, timeout=5).read()
    # clear retained status messages left by the test devices
    c = client("bench-clean")
    c.connect(HOST, PORT)
    c.loop_start()
    for device in ("bench-reset", "bench-powerloss"):
        c.publish(f"smartroom/{device}/status", b"", retain=True).wait_for_publish()
    c.loop_stop()


def main():
    results = {}
    print("1. MQTT broker hop", flush=True)
    results["broker_hop"] = broker_hop()
    print("2. End to end into InfluxDB (Telegraf flush interval 5 s)", flush=True)
    results["end_to_end"] = end_to_end()
    print("3a. Offline detection when the device's connection closes", flush=True)
    results["will_reset_s"] = will_on_reset()
    print(f"  offline published after {results['will_reset_s']:.3f} s", flush=True)
    print(f"3b. Offline detection when the device goes silent (keep-alive {KEEPALIVE_S} s)", flush=True)
    results["will_powerloss_s"] = will_on_power_loss()
    print(f"  offline published after {results['will_powerloss_s']:.1f} s", flush=True)
    cleanup()
    print(json.dumps(results))


if __name__ == "__main__":
    main()
