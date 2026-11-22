#!/usr/bin/env python3
"""Forecast service: reads telemetry from MQTT, publishes temperature forecasts.

    smartroom/<device>/telemetry  ->  forecast_model.Forecaster  ->  smartroom/<device>/forecast

Telegraf stores the forecasts in InfluxDB (measurement room_forecast) for
Grafana, and the NodeMCU shows "Alert in ~N min" on its LCD and web page.

Runs in Docker as part of server/docker-compose.yml, or by hand:

    pip install paho-mqtt
    python3 server/forecaster/forecaster.py --host localhost
"""

import argparse
import json
import time

import paho.mqtt.client as mqtt

from forecast_model import Forecaster

PUBLISH_EVERY_S = 10


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="localhost", help="MQTT broker host")
    ap.add_argument("--port", type=int, default=1883)
    args = ap.parse_args()

    devices = {}       # device id -> Forecaster
    last_publish = {}  # device id -> time of the last forecast sent

    def on_connect(client, userdata, flags, reason_code, properties):
        print(f"Connected to {args.host}:{args.port} ({reason_code})", flush=True)
        client.subscribe("smartroom/+/telemetry")

    def on_message(client, userdata, msg):
        parts = msg.topic.split("/")
        if len(parts) != 3:
            return
        device = parts[1]
        try:
            data = json.loads(msg.payload)
            temperature = float(data["temperature"])
            threshold = float(data["threshold"])
            uptime = data.get("uptime_s")
            uptime = float(uptime) if uptime is not None else None
        except (ValueError, KeyError, TypeError):
            print(f"Ignored bad telemetry from {device}: {msg.payload[:80]!r}", flush=True)
            return

        now = time.time()
        f = devices.setdefault(device, Forecaster())
        f.add(now, temperature, threshold, uptime)

        if now - last_publish.get(device, 0) < PUBLISH_EVERY_S:
            return
        result = f.forecast()
        if result is None:
            return
        last_publish[device] = now
        client.publish(f"smartroom/{device}/forecast", json.dumps(result, separators=(",", ":")))
        eta = result["minutes_to_threshold"]
        print(f"{device}: now {temperature:.1f} C, in 10 min {result['temp_in_10m']:.1f} C "
              f"[{result['temp_in_10m_low']:.1f}..{result['temp_in_10m_high']:.1f}], "
              f"alert {'in %.0f min' % eta if eta > 0 else 'now' if eta == 0 else 'not expected'}"
              + (f", MAE {result['mae_10m']:.2f} vs baseline {result['mae_baseline_10m']:.2f}"
                 if "mae_10m" in result else ""), flush=True)

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="smartroom-forecaster")
    client.on_connect = on_connect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=30)

    while True:
        try:
            client.connect(args.host, args.port, keepalive=30)
            break
        except OSError as e:
            print(f"Broker not reachable ({e}), retrying in 5 s", flush=True)
            time.sleep(5)
    client.loop_forever()


if __name__ == "__main__":
    main()
