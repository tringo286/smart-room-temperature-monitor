#!/usr/bin/env python3
"""Simulated NodeMCU for testing the MQTT -> InfluxDB -> Grafana pipeline
without the hardware.

It publishes the same topics and JSON payload as the firmware, sets the same
Last Will, and obeys the same cmd/threshold and cmd/mute commands.

    pip install -r tools/requirements.txt
    python tools/simulate_sensor.py                 # normal room
    python tools/simulate_sensor.py --heatwave 120  # warm past the threshold in ~2 min
    python tools/simulate_sensor.py --heatwave 900 --swing 0.3 --swing-period 1200
                                                    # slow, realistic warm-up (forecast demo)
"""

import argparse
import json
import math
import random
import signal
import time

import paho.mqtt.client as mqtt


def heat_index_c(temp_c, rh):
    """Same NOAA formula the Adafruit DHT library uses in computeHeatIndex()."""
    t = temp_c * 9 / 5 + 32
    hi = 0.5 * (t + 61.0 + (t - 68.0) * 1.2 + rh * 0.094)
    if hi > 79:
        hi = (-42.379 + 2.04901523 * t + 10.14333127 * rh
              - 0.22475541 * t * rh - 0.00683783 * t * t
              - 0.05481717 * rh * rh + 0.00122874 * t * t * rh
              + 0.00085282 * t * rh * rh - 0.00000199 * t * t * rh * rh)
        if rh < 13 and 80 <= t <= 112:
            hi -= ((13 - rh) * 0.25) * math.sqrt((17 - abs(t - 95)) / 17)
        elif rh > 85 and 80 <= t <= 87:
            hi += ((rh - 85) * 0.1) * ((87 - t) * 0.2)
    return (hi - 32) * 5 / 9


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--host", default="localhost", help="MQTT broker host")
    p.add_argument("--port", type=int, default=1883)
    p.add_argument("--device", default="sim1", help="device id used in the topics")
    p.add_argument("--interval", type=float, default=2.0, help="seconds between readings")
    p.add_argument("--base-temp", type=float, default=24.0, help="normal room temperature (C)")
    p.add_argument("--heatwave", type=float, metavar="SECONDS", default=0,
                   help="ramp the room up by 9 C over this many seconds, then hold")
    p.add_argument("--swing", type=float, default=1.5, metavar="C",
                   help="amplitude of the up-and-down temperature wave (C)")
    p.add_argument("--swing-period", type=float, default=300, metavar="SECONDS",
                   help="period of the temperature wave; the default 5 minutes is faster than a real "
                        "room and makes the 10-minute forecast follow the wave")
    args = p.parse_args()

    base = f"smartroom/{args.device}"
    t_telemetry, t_status = f"{base}/telemetry", f"{base}/status"
    t_threshold, t_mute = f"{base}/cmd/threshold", f"{base}/cmd/mute"

    state = {"threshold": 30.0, "muted": False}

    def on_connect(client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            print(f"MQTT connect failed: {reason_code}")
            return
        print(f"Connected to {args.host}:{args.port} as {args.device}", flush=True)
        client.publish(t_status, "online", qos=1, retain=True)
        client.subscribe([(t_threshold, 0), (t_mute, 0)])

    def on_message(client, userdata, msg):
        value = msg.payload.decode(errors="replace").strip().lower()
        if msg.topic == t_threshold:
            try:
                v = float(value)
            except ValueError:
                v = -1
            if 0 < v <= 60:
                state["threshold"] = v
                print(f"Alert threshold set to {v:.1f}")
            else:
                print(f"Ignored invalid threshold: {value!r}")
        elif msg.topic == t_mute:
            state["muted"] = value in ("1", "true", "on")
            print("Buzzer muted" if state["muted"] else "Buzzer unmuted")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"smartroom-{args.device}")
    client.will_set(t_status, "offline", qos=1, retain=True)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.host, args.port, keepalive=15)
    client.loop_start()

    running = True

    def stop(*_):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    start = time.time()
    temp = args.base_temp
    hum = 45.0
    while running:
        elapsed = time.time() - start
        target = args.base_temp + args.swing * math.sin(elapsed / args.swing_period * 2 * math.pi)
        if args.heatwave > 0:
            target += 9.0 * min(1.0, elapsed / args.heatwave)
        # Drift toward the target with some sensor noise.
        temp += (target - temp) * 0.2 + random.gauss(0, 0.08)
        hum += (50 - (temp - args.base_temp) * 2 - hum) * 0.1 + random.gauss(0, 0.3)
        hum = max(5.0, min(95.0, hum))

        alert = temp >= state["threshold"]
        payload = {
            "temperature": round(temp, 1),
            "humidity": round(hum, 1),
            "heat_index": round(heat_index_c(temp, hum), 1),
            "threshold": state["threshold"],
            "alert": int(alert),
            "buzzer_muted": int(state["muted"]),
            "sensor_errors": 0,
            "rssi": random.randint(-62, -55),
            "uptime_s": int(elapsed),
        }
        client.publish(t_telemetry, json.dumps(payload))
        print(f"{payload['temperature']:5.1f} C  {payload['humidity']:5.1f} %"
              + ("  ALERT" if alert else ""), flush=True)
        time.sleep(args.interval)

    # Clean shutdown: say we're offline (a crash would trigger the Last Will instead).
    client.publish(t_status, "offline", qos=1, retain=True).wait_for_publish(2)
    client.loop_stop()
    client.disconnect()


if __name__ == "__main__":
    main()
