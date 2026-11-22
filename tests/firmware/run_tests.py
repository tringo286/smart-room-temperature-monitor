#!/usr/bin/env python3
"""Build and run the firmware tests on this computer (no board needed).

1. Compiles firmware/smart_room_temperature_monitor/*.ino together with
   tests/firmware/test_firmware.cpp against the fake libraries in fakes/,
   with all warnings and AddressSanitizer/UndefinedBehaviorSanitizer on.
2. Runs the behaviour tests (LCD, buzzer, MQTT, web API, history, ...).
3. Checks that the JSON the firmware produces is valid and has every field
   the web dashboard, Grafana dashboard, simulator, and preview mock use, and
   that the forecaster's messages fit what the firmware and Grafana expect.
4. Runs the forecast model's unit tests (tests/forecaster).

    python3 tests/firmware/run_tests.py

What this does NOT cover: ESP8266-specific compile errors, timing, memory
limits, and real hardware. Compile in the Arduino IDE for those.
"""

import importlib.util
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SKETCH_DIR = ROOT / "firmware" / "smart_room_temperature_monitor"
BUILD = HERE / "build"

problems = []


def check(cond, msg):
    if not cond:
        problems.append(msg)


def build():
    cxx = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
    if not cxx:
        sys.exit("No C++ compiler found (install Xcode Command Line Tools: xcode-select --install)")
    BUILD.mkdir(exist_ok=True)
    exe = BUILD / "test_firmware"
    cmd = [cxx, "-std=c++17", "-g", "-O0",
           "-Wall", "-Wextra", "-Wformat=2", "-Wno-unused-parameter",
           "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
           "-I", str(HERE / "fakes"), "-I", str(SKETCH_DIR),
           str(HERE / "test_firmware.cpp"), "-o", str(exe)]
    print("Compiling firmware with fake hardware libraries...")
    r = subprocess.run(cmd, capture_output=True, text=True)
    warnings = [l for l in r.stderr.splitlines() if "warning:" in l]
    if r.returncode != 0:
        print(r.stderr)
        sys.exit("Build failed.")
    for w in warnings:
        print("  " + w.replace(str(ROOT) + "/", ""))
    print(f"  ok ({len(warnings)} warnings)\n")
    return exe


def run(exe):
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    samples = {}
    for line in r.stdout.splitlines():
        if line.startswith("JSON\t"):
            _, label, payload = line.split("\t", 2)
            samples.setdefault(label, []).append(payload)
        else:
            print(line)
    if r.stderr.strip():
        print(r.stderr)
    return r.returncode == 0, samples


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def contract_checks(samples):
    parsed = {}
    for label, payloads in samples.items():
        for p in payloads:
            try:
                parsed.setdefault(label, []).append(json.loads(p))
            except json.JSONDecodeError as e:
                problems.append(f"{label}: firmware produced invalid JSON ({e}): {p}")

    status = parsed["status"][0]
    history = parsed["history_full"][0]
    telemetry = parsed["telemetry"][0]

    # Types the web page relies on.
    for key in ("has_reading", "sensor_error", "alert", "muted", "mqtt"):
        check(isinstance(status[key], bool), f"status.{key} should be true/false, got {status[key]!r}")
    for key in ("temperature", "humidity", "heat_index", "threshold", "min_temp", "max_temp", "rssi", "uptime_s"):
        check(isinstance(status[key], (int, float)), f"status.{key} should be a number, got {status[key]!r}")
    no_reading = parsed["status_no_reading"][0]
    check(no_reading["temperature"] is None and no_reading["has_reading"] is False,
          "before the first reading, temperature should be null and has_reading false")
    check(parsed["history_empty"][0]["temperature"] == [], "history should be empty before the first reading")

    # History buffer: full, oldest first, newest last.
    temps = history["temperature"]
    check(len(temps) == 120 and len(history["humidity"]) == 120,
          f"history should hold 120 samples, got {len(temps)}")
    check(all(a < b for a, b in zip(temps, temps[1:])), "history should be ordered oldest to newest")
    check(temps and temps[-1] >= 32.8, f"newest history sample should be the latest reading, got {temps[-1:]}")

    # Telemetry: everything numeric (Telegraf stores JSON numbers only).
    for label in ("telemetry", "telemetry_extreme"):
        for k, v in parsed[label][0].items():
            check(isinstance(v, (int, float)) and not isinstance(v, bool),
                  f"{label}.{k} should be a number for Telegraf, got {v!r}")

    # Every field the web page reads exists in the firmware's JSON.
    page = (SKETCH_DIR / "dashboard_page.h").read_text()
    script = page[page.index("<script>"):]
    used_status = set(re.findall(r"\bs\.([a-z_]+)", script)) | set(re.findall(r"\bstatus\.([a-z_]+)", script))
    used_history = set(re.findall(r"\bhistory\.([a-z_]+)", script)) - {"length"}
    check(not used_status - status.keys(), f"web page reads status fields the firmware doesn't send: {used_status - status.keys()}")
    check(not used_history - history.keys(), f"web page reads history fields the firmware doesn't send: {used_history - history.keys()}")

    # Every field the Grafana dashboard queries exists in the telemetry.
    grafana = (ROOT / "tools" / "build_grafana_dashboard.py").read_text()
    used_fields = set(re.findall(r'q_(?:last|reduce)\("(\w+)"', grafana))
    for args in re.findall(r"q_series\(([^)]*)\)", grafana):
        used_fields |= set(re.findall(r'"(\w+)"', args)) - {"mean", "max", "min"}
    check(not used_fields - telemetry.keys(), f"Grafana queries fields the firmware doesn't send: {used_fields - telemetry.keys()}")

    # Forecast: numbers when one has arrived, null before.
    with_fc = parsed["status_forecast"][0]
    for key in ("forecast_temp", "forecast_minutes"):
        check(isinstance(with_fc[key], (int, float)), f"status.{key} should be a number after a forecast, got {with_fc[key]!r}")
        check(status[key] is None, f"status.{key} should be null before any forecast, got {status[key]!r}")

    # The forecaster's messages: every field Grafana queries exists, and a
    # worst-case message fits the firmware's 384-byte copy buffer.
    fm = load_module(ROOT / "server" / "forecaster" / "forecast_model.py", "forecast_model")
    used_fc = set()
    for args in re.findall(r"q_forecast(?:_last)?\(([^)]*)\)", grafana):
        used_fc |= set(re.findall(r'"(\w+)"', args))
    check(used_fc and not used_fc - fm.OUTPUT_FIELDS,
          f"Grafana queries forecast fields the forecaster doesn't send: {used_fc - fm.OUTPUT_FIELDS}")
    worst = {k: -123.4567 for k in fm.OUTPUT_FIELDS}
    worst_len = len(json.dumps(worst, separators=(",", ":")))
    check(worst_len < 384, f"forecast message can be {worst_len} bytes; the firmware copies at most 383")

    # The simulator and the preview mock stay in step with the firmware.
    sim = (ROOT / "tools" / "simulate_sensor.py").read_text()
    sim_payload = sim[sim.index("payload = {"):]
    sim_keys = set(re.findall(r'"(\w+)":', sim_payload[:sim_payload.index("}")]))
    check(sim_keys == telemetry.keys(),
          f"simulator telemetry differs from firmware: missing {telemetry.keys() - sim_keys}, extra {sim_keys - telemetry.keys()}")

    preview = load_module(ROOT / "tools" / "preview_dashboard.py", "preview_dashboard")
    mock = preview.MockDevice(hot=False)
    check(mock.status().keys() == status.keys(),
          f"preview mock /api/status differs from firmware: missing {status.keys() - mock.status().keys()}, extra {mock.status().keys() - status.keys()}")
    check(mock.history_json().keys() == history.keys(), "preview mock /api/history differs from firmware")

    print("Contract checks (firmware JSON vs. web page, Grafana, simulator, preview mock):")
    print(f"  status fields: {len(status)}, page reads {len(used_status)}; "
          f"telemetry fields: {len(telemetry)}, Grafana queries {len(used_fields)}")
    print(f"  example telemetry: {samples['telemetry'][0]}")
    print(f"  forecast fields Grafana queries: {len(used_fc)}; longest forecast message {worst_len} bytes")


def forecast_model_tests():
    print("\nForecast model unit tests (tests/forecaster):")
    r = subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", str(ROOT / "tests" / "forecaster")],
                       capture_output=True, text=True)
    summary = [l for l in r.stderr.splitlines() if l.startswith(("Ran ", "OK", "FAILED"))]
    print("  " + "  ".join(summary))
    if r.returncode != 0:
        print(r.stderr)
    return r.returncode == 0


def main():
    exe = build()
    ok, samples = run(exe)
    print()
    try:
        contract_checks(samples)
    except (KeyError, IndexError, ValueError) as e:
        problems.append(f"could not run contract checks (a test producing sample JSON probably failed): {e!r}")
    ok = forecast_model_tests() and ok
    if problems:
        print("\nContract problems:")
        for p in problems:
            print("  - " + p)
    print()
    if ok and not problems:
        print("ALL PASSED")
        return 0
    print("FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
