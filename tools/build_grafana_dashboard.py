#!/usr/bin/env python3
"""Generate the Grafana dashboard JSON for the Smart Room Monitor.

Grafana loads server/grafana/dashboards/smart-room.json at startup and
re-reads it every few seconds. Edit this script and re-run it rather than
hand-editing the JSON (the Flux queries are easier to maintain here):

    python3 tools/build_grafana_dashboard.py
"""

import json
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "server" / "grafana" / "dashboards" / "smart-room.json"
DS = {"type": "influxdb", "uid": "influxdb"}

# Comfort ranges shaded on the history charts.
COMFORT_TEMP = (20, 26)
COMFORT_HUM = (30, 60)

BASE = '''from(bucket: "smartroom")
  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)
  |> filter(fn: (r) => r._measurement == "room_climate" and r.device == "${device}")'''


# ---------- Flux queries ----------

def q_last(field):
    """Latest value of one field."""
    return BASE + f'''
  |> filter(fn: (r) => r._field == "{field}")
  |> last()
  |> keep(columns: ["_time", "_value"])'''


def q_series(*fields, fn="mean"):
    """Time series of one or more fields, one column per field.

    Empty windows become nulls so the charts show a gap when the device is offline.
    """
    cond = " or ".join(f'r._field == "{f}"' for f in fields)
    return BASE + f'''
  |> filter(fn: (r) => {cond})
  |> aggregateWindow(every: v.windowPeriod, fn: {fn}, createEmpty: true)
  |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")
  |> drop(columns: ["_start", "_stop", "_measurement", "device"])'''


def q_reduce(field, fn, scale=None):
    """One number summarising a field over the selected time range."""
    q = BASE + f'''
  |> filter(fn: (r) => r._field == "{field}")
  |> {fn}()'''
    if scale:
        q += f'''
  |> map(fn: (r) => ({{r with _value: float(v: r._value) * {scale}}}))'''
    return q + '''
  |> keep(columns: ["_value"])'''


Q_DEVICE_STATUS = '''from(bucket: "smartroom")
  |> range(start: -30d)
  |> filter(fn: (r) => r._measurement == "device_status" and r.device == "${device}" and r._field == "value")
  |> last()
  |> keep(columns: ["_time", "_value"])'''


# ---------- Panel helpers ----------

_next_id = 0


def _id():
    global _next_id
    _next_id += 1
    return _next_id


def steps(*pairs):
    return {"mode": "absolute", "steps": [{"color": c, "value": v} for v, c in pairs]}


def value_map(*items):
    return [{"type": "value", "options": {k: {"text": t, "color": c, "index": i}
                                          for i, (k, t, c) in enumerate(items)}}]


def rename(field, name, color=None, **custom):
    props = [{"id": "displayName", "value": name}]
    if color:
        props.append({"id": "color", "value": {"mode": "fixed", "fixedColor": color}})
    props += [{"id": f"custom.{k}", "value": v} for k, v in custom.items()]
    return {"matcher": {"id": "byName", "options": field}, "properties": props}


def row(title, y):
    return {"id": _id(), "type": "row", "title": title, "collapsed": False,
            "gridPos": {"x": 0, "y": y, "w": 24, "h": 1}, "panels": []}


def panel(kind, title, grid, query, defaults=None, overrides=None, options=None,
          description=None, interval=None):
    p = {
        "id": _id(), "type": kind, "title": title, "datasource": DS,
        "gridPos": dict(zip("xywh", grid)),
        "targets": [{"refId": "A", "datasource": DS, "query": query}],
        "fieldConfig": {"defaults": defaults or {}, "overrides": overrides or []},
        "options": options or {},
    }
    if description:
        p["description"] = description
    if interval:
        p["interval"] = interval
    return p


def stat(title, grid, query, defaults, color_mode="value", graph=False, fields="",
         description=None, text_size=None):
    opts = {"reduceOptions": {"calcs": ["lastNotNull"], "fields": fields, "values": False},
            "colorMode": color_mode, "graphMode": "area" if graph else "none",
            "textMode": "value", "justifyMode": "center", "orientation": "auto",
            "wideLayout": True, "showPercentChange": False}
    if text_size:
        opts["text"] = {"valueSize": text_size}
    return panel("stat", title, grid, query, defaults, options=opts, description=description,
                 interval="10s" if graph else None)


def gauge(title, grid, query, defaults, description=None):
    return panel("gauge", title, grid, query, defaults, options={
        "reduceOptions": {"calcs": ["lastNotNull"], "fields": "", "values": False},
        "showThresholdMarkers": True, "showThresholdLabels": False,
        "minVizHeight": 75, "minVizWidth": 75, "sizing": "auto",
    }, description=description)


def timeseries(title, grid, query, defaults, overrides, description=None, legend_calcs=("min", "max", "mean")):
    custom = {"lineWidth": 2, "fillOpacity": 18, "gradientMode": "opacity", "showPoints": "never",
              "spanNulls": False, "lineInterpolation": "smooth", "axisSoftMin": None,
              "thresholdsStyle": {"mode": "area"}}
    custom.update(defaults.pop("custom", {}))
    defaults = {"color": {"mode": "palette-classic"}, "custom": custom, **defaults}
    return panel("timeseries", title, grid, query, defaults, overrides, {
        "legend": {"displayMode": "list", "placement": "bottom", "calcs": list(legend_calcs)},
        "tooltip": {"mode": "multi", "sort": "none"},
    }, description=description, interval="10s")


def comfort_band(lo, hi):
    """Thresholds that shade only the comfort range on a time series."""
    return steps((None, "transparent"), (lo, "rgba(115, 191, 105, 0.12)"), (hi, "transparent"))


# ---------- Dashboard ----------

TEMP_COLORS = steps((None, "blue"), (18, "green"), (27, "orange"), (30, "red"))
ALERT_MAP = value_map(("0", "Normal", "green"), ("1", "HIGH TEMP", "red"))

panels = [
    row("Now", 0),
    gauge("Temperature", (0, 1, 5, 7), q_last("temperature"), {
        "unit": "celsius", "decimals": 1, "min": 10, "max": 40,
        "thresholds": TEMP_COLORS, "color": {"mode": "thresholds"},
    }, description="Latest DHT11 reading. Colour bands: blue < 18 °C, green 18–27 °C, orange 27–30 °C, red ≥ 30 °C."),
    gauge("Humidity", (5, 1, 5, 7), q_last("humidity"), {
        "unit": "percent", "decimals": 0, "min": 0, "max": 100,
        "thresholds": steps((None, "orange"), (30, "green"), (60, "blue")),
        "color": {"mode": "thresholds"},
    }, description="Relative humidity. 30–60 % is the usual indoor comfort range."),
    stat("Feels like", (10, 1, 4, 7), q_series("heat_index"), {
        "unit": "celsius", "decimals": 1, "thresholds": TEMP_COLORS, "color": {"mode": "thresholds"},
        "displayName": "Heat index",
    }, graph=True, description="Heat index computed on the NodeMCU from temperature and humidity (NOAA formula)."),
    stat("Alert", (14, 1, 5, 4), q_last("alert"), {
        "mappings": ALERT_MAP, "thresholds": steps((None, "green"), (1, "red")), "color": {"mode": "thresholds"},
    }, color_mode="background", text_size=34,
        description="HIGH TEMP while the temperature is at or above the alert threshold."),
    stat("Device", (19, 1, 5, 4), Q_DEVICE_STATUS, {
        "mappings": value_map(("online", "Online", "green"), ("offline", "Offline", "red")),
        "color": {"mode": "fixed", "fixedColor": "text"},
    }, color_mode="background", fields="/^Value$/", text_size=34,
        description="From the MQTT Last Will: the broker marks the device offline when it disconnects."),
    stat("Alert threshold", (14, 5, 5, 3), q_last("threshold"), {
        "unit": "celsius", "decimals": 1, "color": {"mode": "fixed", "fixedColor": "red"},
    }, description="Change it from the NodeMCU web page or with MQTT: smartroom/<device>/cmd/threshold"),
    stat("Buzzer", (19, 5, 5, 3), q_last("buzzer_muted"), {
        "mappings": value_map(("0", "Active", "green"), ("1", "Muted", "orange")),
        "color": {"mode": "fixed", "fixedColor": "text"},
    }, description="Mute from the NodeMCU web page or with MQTT: smartroom/<device>/cmd/mute"),

    row("History", 8),
    timeseries("Temperature", (0, 9, 24, 10), q_series("temperature", "heat_index", "threshold"), {
        "unit": "celsius", "decimals": 1, "thresholds": comfort_band(*COMFORT_TEMP),
    }, [
        rename("temperature", "Temperature", "orange", lineWidth=3),
        rename("heat_index", "Feels like", "purple", fillOpacity=0, lineWidth=1),
        rename("threshold", "Alert threshold", "red", fillOpacity=0, lineWidth=2,
               lineInterpolation="stepAfter", lineStyle={"fill": "dash", "dash": [10, 10]}),
    ], description=f"Green band: comfort range {COMFORT_TEMP[0]}–{COMFORT_TEMP[1]} °C. Gaps mean the device was offline."),
    timeseries("Humidity", (0, 19, 12, 8), q_series("humidity"), {
        "unit": "percent", "decimals": 0, "min": 0, "max": 100, "thresholds": comfort_band(*COMFORT_HUM),
    }, [rename("humidity", "Humidity", "blue")],
        description=f"Green band: comfort range {COMFORT_HUM[0]}–{COMFORT_HUM[1]} %."),
    panel("state-timeline", "Alert history", (12, 19, 12, 8), q_series("alert", "buzzer_muted", fn="max"), {
        "custom": {"fillOpacity": 85, "lineWidth": 0},
    }, [
        {"matcher": {"id": "byName", "options": "alert"},
         "properties": [{"id": "displayName", "value": "Alert"}, {"id": "mappings", "value": ALERT_MAP}]},
        {"matcher": {"id": "byName", "options": "buzzer_muted"},
         "properties": [{"id": "displayName", "value": "Buzzer"},
                        {"id": "mappings", "value": value_map(("0", "Active", "green"), ("1", "Muted", "orange"))}]},
    ], {"showValue": "auto", "mergeValues": True, "rowHeight": 0.75, "alignValue": "center",
        "legend": {"showLegend": False}}, interval="10s",
        description="When the room was too hot, and whether the buzzer was muted at the time."),

    row("Summary for the selected time range", 27),
    stat("Lowest", (0, 28, 4, 4), q_reduce("temperature", "min"),
         {"unit": "celsius", "decimals": 1, "thresholds": TEMP_COLORS, "color": {"mode": "thresholds"}}),
    stat("Highest", (4, 28, 4, 4), q_reduce("temperature", "max"),
         {"unit": "celsius", "decimals": 1, "thresholds": TEMP_COLORS, "color": {"mode": "thresholds"}}),
    stat("Average temperature", (8, 28, 4, 4), q_reduce("temperature", "mean"),
         {"unit": "celsius", "decimals": 1, "thresholds": TEMP_COLORS, "color": {"mode": "thresholds"}}),
    stat("Average humidity", (12, 28, 4, 4), q_reduce("humidity", "mean"),
         {"unit": "percent", "decimals": 0, "color": {"mode": "fixed", "fixedColor": "blue"}}),
    stat("Time in alert", (16, 28, 4, 4), q_reduce("alert", "mean", scale=100.0), {
        "unit": "percent", "decimals": 1,
        "thresholds": steps((None, "green"), (1, "orange"), (10, "red")), "color": {"mode": "thresholds"},
    }, color_mode="background", description="Share of readings at or above the alert threshold."),
    stat("Readings stored", (20, 28, 4, 4), q_reduce("temperature", "count"), {
        "unit": "short", "decimals": 0, "color": {"mode": "fixed", "fixedColor": "text"},
    }, description="Number of MQTT telemetry messages saved in InfluxDB for this device."),
]

dashboard = {
    "uid": "smart-room",
    "title": "Smart Room Monitor",
    "description": "CMPE 286 IoT project: NodeMCU ESP8266 + DHT11 → MQTT → Telegraf → InfluxDB → Grafana",
    "tags": ["iot", "cmpe286", "mqtt"],
    "timezone": "browser",
    "refresh": "5s",
    "time": {"from": "now-30m", "to": "now"},
    "timepicker": {"refresh_intervals": ["5s", "10s", "30s", "1m", "5m"]},
    "graphTooltip": 1,
    "liveNow": False,
    "schemaVersion": 39,
    "version": 1,
    "editable": True,
    "links": [],
    "templating": {"list": [{
        "name": "device", "label": "Device", "type": "query", "datasource": DS,
        "query": 'import "influxdata/influxdb/schema"\n'
                 'schema.tagValues(bucket: "smartroom", tag: "device", '
                 'predicate: (r) => r._measurement == "room_climate", start: -30d)',
        "definition": "device tag values",
        "refresh": 2, "includeAll": False, "multi": False, "sort": 1,
        "current": {}, "options": [],
    }]},
    "panels": panels,
}

if __name__ == "__main__":
    OUT.write_text(json.dumps(dashboard, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Wrote {OUT.relative_to(Path.cwd()) if OUT.is_relative_to(Path.cwd()) else OUT}")
