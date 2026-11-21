#!/usr/bin/env python3
"""Preview the NodeMCU web dashboard on your computer without flashing the board.

Serves the page straight from firmware/.../dashboard_page.h with a mock
/api/status, /api/history, /api/threshold and /api/mute that return the same
JSON as the firmware. Edit the .h file and reload the browser to see changes.

    python3 tools/preview_dashboard.py              # http://localhost:8000
    python3 tools/preview_dashboard.py --hot        # start above the alert threshold
"""

import argparse
import json
import math
import random
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs

PAGE_FILE = (Path(__file__).resolve().parent.parent / "firmware" /
             "smart_room_temperature_monitor" / "dashboard_page.h")
HISTORY_INTERVAL_S = 10
HISTORY_SIZE = 120


def load_page():
    src = PAGE_FILE.read_text(encoding="utf-8")
    start = src.index('R"rawliteral(') + len('R"rawliteral(')
    return src[start:src.index(')rawliteral"', start)].encode()


def heat_index_c(temp_c, rh):
    t = temp_c * 9 / 5 + 32
    hi = 0.5 * (t + 61.0 + (t - 68.0) * 1.2 + rh * 0.094)
    if hi > 79:
        hi = (-42.379 + 2.04901523 * t + 10.14333127 * rh - 0.22475541 * t * rh
              - 0.00683783 * t * t - 0.05481717 * rh * rh + 0.00122874 * t * t * rh
              + 0.00085282 * t * rh * rh - 0.00000199 * t * t * rh * rh)
    return (hi - 32) * 5 / 9


class MockDevice:
    def __init__(self, hot):
        self.start = time.time()
        self.base = 31.0 if hot else 24.5
        self.threshold = 30.0
        self.muted = False
        # Pretend the board has been running for a while so the chart is full.
        self.boot = self.start - HISTORY_SIZE * HISTORY_INTERVAL_S - 3 * 3600
        self.history = [self.sample(self.start - (HISTORY_SIZE - i) * HISTORY_INTERVAL_S)
                        for i in range(HISTORY_SIZE)]
        self.last_history = self.start
        temps = [t for t, _ in self.history]
        self.min_temp, self.max_temp = min(temps), max(temps)

    def sample(self, at):
        k = at / 60
        temp = self.base + 1.6 * math.sin(k / 6) + 0.5 * math.sin(k / 1.7) + random.gauss(0, 0.08)
        hum = 46 - (temp - 24) * 1.5 + 2 * math.sin(k / 4) + random.gauss(0, 0.3)
        return round(temp, 1), round(hum, 1)

    def status(self):
        now = time.time()
        temp, hum = self.sample(now)
        if now - self.last_history >= HISTORY_INTERVAL_S:
            self.history = self.history[1:] + [(temp, hum)]
            self.last_history = now
        self.min_temp, self.max_temp = min(self.min_temp, temp), max(self.max_temp, temp)
        return {
            "device": "room1", "has_reading": True, "sensor_error": False,
            "temperature": temp, "humidity": hum, "heat_index": round(heat_index_c(temp, hum), 1),
            "min_temp": self.min_temp, "max_temp": self.max_temp,
            "threshold": self.threshold, "alert": temp >= self.threshold, "muted": self.muted,
            "mqtt": True, "mqtt_host": "10.0.0.25", "ip": "10.0.0.118",
            "rssi": random.randint(-63, -57), "uptime_s": int(now - self.boot), "sensor_errors": 2,
        }

    def history_json(self):
        return {"interval_s": HISTORY_INTERVAL_S,
                "temperature": [t for t, _ in self.history],
                "humidity": [h for _, h in self.history]}


def make_handler(device):
    class Handler(BaseHTTPRequestHandler):
        def send(self, code, body, ctype="application/json"):
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path == "/":
                self.send(200, load_page(), "text/html; charset=utf-8")
            elif self.path == "/api/status":
                self.send(200, device.status())
            elif self.path == "/api/history":
                self.send(200, device.history_json())
            else:
                self.send(404, b"Not found", "text/plain")

        def do_POST(self):
            length = int(self.headers.get("Content-Length", 0))
            value = parse_qs(self.rfile.read(length).decode()).get("value", [""])[0]
            if self.path == "/api/threshold":
                try:
                    v = float(value)
                except ValueError:
                    v = -1
                if not 0 < v <= 60:
                    self.send(400, {"error": "threshold must be between 0 and 60"})
                    return
                device.threshold = v
            elif self.path == "/api/mute":
                device.muted = value.strip().lower() in ("1", "true", "on")
            else:
                self.send(404, b"Not found", "text/plain")
                return
            self.send(200, device.status())

        def log_message(self, fmt, *args):
            pass

    return Handler


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--port", type=int, default=8000)
    p.add_argument("--hot", action="store_true", help="start above the alert threshold")
    args = p.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(MockDevice(args.hot)))
    print(f"Preview at http://localhost:{args.port}  (Ctrl+C to stop)", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
