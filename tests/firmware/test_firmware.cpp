// Runs the real firmware (smart_room_temperature_monitor.ino) on a computer
// against fake hardware libraries (see fakes/). Each test runs in its own
// forked process so it starts from a freshly booted board.
//
// Run with:  python3 tests/firmware/run_tests.py
//
// Lines starting with "JSON\t" carry JSON the firmware produced; the runner
// parses them and checks them against the web page, Grafana, and the tools.

#include <sys/wait.h>
#include <unistd.h>

#include <iostream>
#include <vector>

#include "smart_room_temperature_monitor.ino"

// ---------- tiny test framework ----------

static int failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::cout << "    FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond "\n"; \
      failures++;                                                                \
    }                                                                            \
  } while (0)

#define CHECK_EQ(a, b)                                                                     \
  do {                                                                                     \
    auto _a = (a);                                                                         \
    auto _b = (b);                                                                         \
    if (!(_a == _b)) {                                                                     \
      std::cout << "    FAIL " << __FILE__ << ":" << __LINE__ << "  " #a " == " #b "\n" \
                << "         got: " << _a << "\n         want: " << _b << "\n";           \
      failures++;                                                                          \
    }                                                                                      \
  } while (0)

static bool contains(const std::string& s, const std::string& part) {
  return s.find(part) != std::string::npos;
}

static void emit(const std::string& label, const std::string& json) {
  std::cout << "JSON\t" << label << "\t" << json << "\n" << std::flush;
}

// ---------- helpers ----------

// Boot the board: setup() plus the first pass through loop().
static void boot() {
  setup();
  loop();
}

// Let the firmware run for `ms` of simulated time.
static void run_for(unsigned long ms) {
  unsigned long end = fake::now_ms + ms;
  while (fake::now_ms < end) {
    loop();
    fake::now_ms += 50;
  }
}

static std::vector<FakePublish> telemetry() {
  std::vector<FakePublish> out;
  for (auto& p : mqtt.published)
    if (p.topic == "smartroom/room1/telemetry") out.push_back(p);
  return out;
}

static std::string last_telemetry() {
  auto t = telemetry();
  return t.empty() ? std::string() : t.back().payload;
}

static FakeResponse get(const std::string& path) { return server.request(HTTP_GET, path); }
static FakeResponse post(const std::string& path, std::map<std::string, std::string> args) {
  return server.request(HTTP_POST, path, std::move(args));
}

// ---------- tests ----------

static void test_boot_connects_to_mqtt() {
  boot();
  CHECK(server.started);
  CHECK(mqtt.connected());
  CHECK_EQ(mqtt.client_id, std::string("smartroom-room1"));
  CHECK_EQ(mqtt.will.topic, std::string("smartroom/room1/status"));
  CHECK_EQ(mqtt.will.payload, std::string("offline"));
  CHECK(mqtt.will.retained);
  CHECK(!mqtt.published.empty());
  CHECK_EQ(mqtt.published.front().topic, std::string("smartroom/room1/status"));
  CHECK_EQ(mqtt.published.front().payload, std::string("online"));
  CHECK(mqtt.published.front().retained);
  CHECK_EQ(mqtt.subscriptions.size(), (size_t)3);
  CHECK(std::find(mqtt.subscriptions.begin(), mqtt.subscriptions.end(), "smartroom/room1/cmd/threshold") != mqtt.subscriptions.end());
  CHECK(std::find(mqtt.subscriptions.begin(), mqtt.subscriptions.end(), "smartroom/room1/cmd/mute") != mqtt.subscriptions.end());
  CHECK(std::find(mqtt.subscriptions.begin(), mqtt.subscriptions.end(), "smartroom/room1/forecast") != mqtt.subscriptions.end());
}

static void test_status_before_first_reading() {
  fake::sensor_temp = NAN;  // sensor never answers
  boot();
  run_for(5000);
  auto r = get("/api/status");
  CHECK_EQ(r.code, 200);
  CHECK_EQ(r.type, std::string("application/json"));
  CHECK(contains(r.body, "\"has_reading\":false"));
  CHECK(contains(r.body, "\"temperature\":null"));
  CHECK(contains(r.body, "\"alert\":false"));
  CHECK(telemetry().empty());
  CHECK_EQ(lcd.line(0), std::string("Sensor Error"));
  emit("status_no_reading", r.body);
  emit("history_empty", get("/api/history").body);
}

static void test_normal_reading() {
  boot();
  run_for(2500);
  CHECK_EQ(lcd.line(0), std::string("Temp: 24.0°C"));
  CHECK_EQ(lcd.line(1), std::string("Hum:  45.0%"));
  CHECK_EQ(fake::buzzer_hz, 0u);
  std::string t = last_telemetry();
  CHECK(contains(t, "\"temperature\":24.0"));
  CHECK(contains(t, "\"humidity\":45.0"));
  CHECK(contains(t, "\"alert\":0"));
  CHECK(contains(t, "\"threshold\":30.0"));
  auto r = get("/api/status");
  CHECK(contains(r.body, "\"has_reading\":true"));
  CHECK(contains(r.body, "\"ip\":\"10.0.0.118\""));
  CHECK(contains(r.body, "\"mqtt\":true"));
  emit("telemetry", t);
  emit("status", r.body);
}

static void test_high_temperature_alert() {
  fake::sensor_temp = 31.0f;
  boot();
  run_for(2500);
  CHECK_EQ(lcd.line(1), std::string("HIGH TEMP!"));
  CHECK_EQ(fake::buzzer_hz, 2000u);
  CHECK(contains(last_telemetry(), "\"alert\":1"));
  CHECK(contains(get("/api/status").body, "\"alert\":true"));
  emit("status_alert", get("/api/status").body);
}

static void test_alert_at_exact_threshold() {
  fake::sensor_temp = 30.0f;
  boot();
  run_for(2500);
  CHECK_EQ(fake::buzzer_hz, 2000u);
  fake::sensor_temp = 29.9f;
  run_for(2500);
  CHECK_EQ(fake::buzzer_hz, 0u);
}

static void test_mute_over_mqtt() {
  fake::sensor_temp = 31.0f;
  boot();
  run_for(2500);
  size_t before = telemetry().size();
  mqtt.deliver("smartroom/room1/cmd/mute", "1");
  CHECK_EQ(fake::buzzer_hz, 0u);
  CHECK_EQ(lcd.line(1), std::string("HIGH TEMP (mute)"));
  CHECK_EQ(telemetry().size(), before + 1);  // published right away
  CHECK(contains(last_telemetry(), "\"buzzer_muted\":1"));
  CHECK(contains(last_telemetry(), "\"alert\":1"));
  mqtt.deliver("smartroom/room1/cmd/mute", " OFF ");
  CHECK_EQ(fake::buzzer_hz, 2000u);
  CHECK(contains(last_telemetry(), "\"buzzer_muted\":0"));
}

static void test_threshold_over_mqtt() {
  fake::sensor_temp = 31.0f;
  boot();
  run_for(2500);
  mqtt.deliver("smartroom/room1/cmd/threshold", "32.5");
  CHECK_EQ(fake::buzzer_hz, 0u);
  CHECK(contains(last_telemetry(), "\"threshold\":32.5"));
  CHECK(contains(last_telemetry(), "\"alert\":0"));
  mqtt.deliver("smartroom/room1/cmd/threshold", "-5");
  mqtt.deliver("smartroom/room1/cmd/threshold", "hot");
  mqtt.deliver("smartroom/room1/cmd/threshold", "");
  CHECK(contains(get("/api/status").body, "\"threshold\":32.5"));
}

static void test_threshold_over_http() {
  fake::sensor_temp = 31.0f;
  boot();
  run_for(2500);
  auto r = post("/api/threshold", {{"value", "32"}});
  CHECK_EQ(r.code, 200);
  CHECK(contains(r.body, "\"threshold\":32.0"));
  CHECK(contains(r.body, "\"alert\":false"));
  CHECK_EQ(fake::buzzer_hz, 0u);
  CHECK_EQ(lcd.line(1), std::string("Hum:  45.0%"));
  CHECK(contains(last_telemetry(), "\"threshold\":32.0"));
  emit("threshold_response", r.body);
}

static void test_invalid_threshold_rejected() {
  boot();
  run_for(2500);
  for (const char* bad : {"abc", "99", "0", "-3", ""}) {
    auto r = post("/api/threshold", {{"value", bad}});
    CHECK_EQ(r.code, 400);
    emit("error_response", r.body);
  }
  CHECK_EQ(post("/api/threshold", {}).code, 400);
  CHECK_EQ(post("/api/mute", {}).code, 400);
  CHECK_EQ(get("/api/threshold").code, 404);  // wrong method
  CHECK(contains(get("/api/status").body, "\"threshold\":30.0"));
}

static void test_mute_over_http() {
  fake::sensor_temp = 31.0f;
  boot();
  run_for(2500);
  auto r = post("/api/mute", {{"value", "1"}});
  CHECK_EQ(r.code, 200);
  CHECK(contains(r.body, "\"muted\":true"));
  CHECK_EQ(fake::buzzer_hz, 0u);
  r = post("/api/mute", {{"value", "0"}});
  CHECK(contains(r.body, "\"muted\":false"));
  CHECK_EQ(fake::buzzer_hz, 2000u);
}

static void test_sensor_failure_and_recovery() {
  fake::sensor_temp = 31.0f;
  boot();
  run_for(2500);
  CHECK_EQ(fake::buzzer_hz, 2000u);
  size_t before = telemetry().size();

  fake::sensor_temp = NAN;
  run_for(2100);
  CHECK_EQ(lcd.line(0), std::string("Sensor Error"));
  CHECK_EQ(fake::buzzer_hz, 0u);
  auto r = get("/api/status");
  CHECK(contains(r.body, "\"sensor_error\":true"));
  CHECK(contains(r.body, "\"temperature\":31.0"));  // last good reading kept
  CHECK(contains(r.body, "\"alert\":false"));
  CHECK_EQ(telemetry().size(), before);  // nothing stale published
  emit("status_sensor_error", r.body);

  fake::sensor_temp = 25.0f;
  run_for(2100);
  CHECK_EQ(lcd.line(0), std::string("Temp: 25.0°C"));
  CHECK(contains(last_telemetry(), "\"sensor_errors\":1"));
  CHECK(contains(get("/api/status").body, "\"sensor_error\":false"));
}

static void test_no_stale_telemetry_during_sensor_error() {
  boot();
  run_for(2500);
  fake::sensor_temp = NAN;
  run_for(2100);
  size_t before = telemetry().size();
  mqtt.deliver("smartroom/room1/cmd/mute", "1");
  post("/api/threshold", {{"value", "28"}});
  CHECK_EQ(telemetry().size(), before);
}

static void test_history_ring_buffer() {
  boot();
  // 130 samples, one every 10 s, each 0.1 °C warmer than the last.
  for (int i = 0; i < 130; i++) {
    fake::sensor_temp = 20.0f + i * 0.1f;
    run_for(10000);
  }
  auto r = get("/api/history");
  CHECK_EQ(r.code, 200);
  emit("history_full", r.body);
  // The runner checks the length (120), the order, and the newest value.
  CHECK(contains(r.body, "\"interval_s\":10"));
}

static void test_min_max_since_boot() {
  boot();
  for (float t : {24.0f, 19.5f, 28.0f, 22.0f}) {
    fake::sensor_temp = t;
    run_for(2100);
  }
  auto r = get("/api/status");
  CHECK(contains(r.body, "\"min_temp\":19.5"));
  CHECK(contains(r.body, "\"max_temp\":28.0"));
}

static void test_telemetry_fits_mqtt_buffer() {
  fake::sensor_temp = -40.0f;
  fake::sensor_hum = 100.0f;
  fake::rssi = -100;
  boot();
  fake::now_ms = 4000000000UL;  // ~46 days of uptime
  run_for(2100);
  for (int i = 0; i < 25; i++) {  // rack up sensor errors
    fake::sensor_temp = NAN;
    run_for(2000);
  }
  fake::sensor_temp = 59.9f;
  size_t before = telemetry().size();
  run_for(2100);
  CHECK_EQ(telemetry().size(), before + 1);
  CHECK(last_telemetry().size() < 200);
  emit("telemetry_extreme", last_telemetry());
}

// The board must stay usable when the computer running the broker is
// unreachable. Every failed connect blocks for ~5 s (TCP timeout).
static void test_broker_down_keeps_board_responsive() {
  fake::broker_up = false;
  boot();
  unsigned long start = fake::now_ms, blocked_start = fake::blocked_ms;
  run_for(5 * 60 * 1000);
  double blocked = double(fake::blocked_ms - blocked_start) / double(fake::now_ms - start);
  std::cout << "    blocked " << int(blocked * 100) << "% of 5 minutes, "
            << fake::connect_attempts << " connect attempts\n";
  CHECK(blocked < 0.15);
  // Readings and the web dashboard keep working meanwhile.
  CHECK_EQ(lcd.line(0), std::string("Temp: 24.0°C"));
  CHECK(contains(get("/api/status").body, "\"mqtt\":false"));
}

static void test_reconnects_after_broker_restart() {
  boot();
  run_for(2500);
  fake::broker_up = false;
  run_for(30000);
  CHECK(!mqtt.connected());
  fake::broker_up = true;
  mqtt.published.clear();
  mqtt.subscriptions.clear();
  run_for(65000);  // within the longest retry delay
  CHECK(mqtt.connected());
  CHECK_EQ(mqtt.subscriptions.size(), (size_t)3);
  CHECK(!mqtt.published.empty() && mqtt.published.front().payload == "online");
  CHECK(!telemetry().empty());
}

static void test_no_mqtt_attempts_without_wifi() {
  fake::wifi_connected = true;
  fake::broker_up = false;
  setup();
  fake::wifi_connected = false;
  int before = fake::connect_attempts;
  run_for(60000);
  CHECK_EQ(fake::connect_attempts, before);
}

static void test_web_page_and_404() {
  boot();
  auto r = get("/");
  CHECK_EQ(r.code, 200);
  CHECK_EQ(r.type, std::string("text/html"));
  CHECK(r.body.rfind("<!DOCTYPE html>", 0) == 0);
  CHECK(contains(r.body, "/api/status"));
  CHECK(contains(r.body, "</html>"));
  CHECK_EQ(get("/nope").code, 404);
  std::cout << "    page size " << r.body.size() << " bytes\n";
}

// A full message in the format server/forecaster publishes (compact JSON).
static std::string forecast_msg(const char* temp, const char* minutes) {
  return std::string("{\"temp_in_10m\":") + temp + ",\"temp_in_10m_low\":27.71,\"temp_in_10m_high\":29.53,"
         "\"slope_c_per_min\":0.1234,\"minutes_to_threshold\":" + minutes +
         ",\"samples\":450,\"mae_10m\":0.165,\"mae_baseline_10m\":0.683,\"evaluations\":50}";
}

// Collects the distinct texts LCD line 2 shows over `ms` of running.
static std::vector<std::string> lcd_line2_over(unsigned long ms) {
  std::vector<std::string> seen;
  unsigned long end = fake::now_ms + ms;
  while (fake::now_ms < end) {
    loop();
    fake::now_ms += 50;
    std::string l = lcd.line(1);
    if (std::find(seen.begin(), seen.end(), l) == seen.end()) seen.push_back(l);
  }
  return seen;
}

static bool shown(const std::vector<std::string>& seen, const std::string& text) {
  return std::find(seen.begin(), seen.end(), text) != seen.end();
}

static void test_forecast_status_before_any() {
  boot();
  run_for(2500);
  auto r = get("/api/status");
  CHECK(contains(r.body, "\"forecast_temp\":null"));
  CHECK(contains(r.body, "\"forecast_minutes\":null"));
}

static void test_forecast_shown_on_lcd_and_web() {
  boot();
  run_for(2500);
  mqtt.deliver("smartroom/room1/forecast", forecast_msg("28.62", "6.2"));
  auto r = get("/api/status");
  CHECK(contains(r.body, "\"forecast_temp\":28.6"));
  CHECK(contains(r.body, "\"forecast_minutes\":6.2"));
  emit("status_forecast", r.body);
  // LCD line 2 alternates between humidity and the forecast.
  auto seen = lcd_line2_over(10000);
  CHECK(shown(seen, "Hum:  45.0%"));
  CHECK(shown(seen, "Alert in ~6 min"));
  CHECK_EQ(seen.size(), (size_t)2);
}

static void test_forecast_lcd_wording() {
  boot();
  run_for(2500);
  mqtt.deliver("smartroom/room1/forecast", forecast_msg("29.9", "0.4"));
  CHECK(shown(lcd_line2_over(10000), "Alert in <1 min"));
  mqtt.deliver("smartroom/room1/forecast", forecast_msg("29.9", "59.6"));
  CHECK(shown(lcd_line2_over(10000), "Alert in ~60 min"));
}

static void test_forecast_not_rising() {
  boot();
  run_for(2500);
  mqtt.deliver("smartroom/room1/forecast", forecast_msg("24.1", "-1"));
  auto r = get("/api/status");
  CHECK(contains(r.body, "\"forecast_temp\":24.1"));
  CHECK(contains(r.body, "\"forecast_minutes\":null"));
  auto seen = lcd_line2_over(10000);
  CHECK_EQ(seen.size(), (size_t)1);
  CHECK(shown(seen, "Hum:  45.0%"));
}

static void test_forecast_hidden_while_alerting() {
  fake::sensor_temp = 31.0f;
  boot();
  run_for(2500);
  mqtt.deliver("smartroom/room1/forecast", forecast_msg("32.0", "0"));
  auto seen = lcd_line2_over(10000);
  CHECK_EQ(seen.size(), (size_t)1);
  CHECK(shown(seen, "HIGH TEMP!"));
  CHECK(contains(get("/api/status").body, "\"forecast_minutes\":0.0"));
}

static void test_forecast_expires() {
  boot();
  run_for(2500);
  mqtt.deliver("smartroom/room1/forecast", forecast_msg("28.6", "6.2"));
  run_for(110000);
  CHECK(contains(get("/api/status").body, "\"forecast_temp\":28.6"));
  run_for(15000);  // > 2 minutes since the last forecast: forecaster is gone
  CHECK(contains(get("/api/status").body, "\"forecast_temp\":null"));
  auto seen = lcd_line2_over(10000);
  CHECK_EQ(seen.size(), (size_t)1);
  CHECK(shown(seen, "Hum:  45.0%"));
}

static void test_bad_forecast_ignored() {
  boot();
  run_for(2500);
  mqtt.deliver("smartroom/room1/forecast", "hello");
  mqtt.deliver("smartroom/room1/forecast", "{\"temp_in_10m\":\"warm\",\"minutes_to_threshold\":5}");
  mqtt.deliver("smartroom/room1/forecast", "{\"temp_in_10m\":28.6}");
  mqtt.deliver("smartroom/room1/forecast", "{\"temp_in_10m\":999,\"minutes_to_threshold\":5}");
  mqtt.deliver("smartroom/room1/forecast", "");
  CHECK(contains(get("/api/status").body, "\"forecast_temp\":null"));
  // A message longer than the copy buffer is cut, not overflowed.
  mqtt.deliver("smartroom/room1/forecast", forecast_msg("28.6", "6.2") + std::string(600, ' '));
  CHECK(contains(get("/api/status").body, "\"forecast_temp\":28.6"));
  // A key must not match a longer key that starts the same way.
  mqtt.deliver("smartroom/room1/forecast", "{\"temp_in_10m_low\":1,\"temp_in_10m\":27.5,\"minutes_to_threshold\":-1}");
  CHECK(contains(get("/api/status").body, "\"forecast_temp\":27.5"));
}

// ---------- runner ----------

struct Test {
  const char* name;
  void (*fn)();
};

int main() {
  const Test tests[] = {
      {"boot connects to MQTT with Last Will", test_boot_connects_to_mqtt},
      {"status before the first reading", test_status_before_first_reading},
      {"normal reading on LCD, MQTT, and web", test_normal_reading},
      {"high temperature alert", test_high_temperature_alert},
      {"alert triggers at exactly the threshold", test_alert_at_exact_threshold},
      {"mute over MQTT", test_mute_over_mqtt},
      {"threshold over MQTT (and bad values ignored)", test_threshold_over_mqtt},
      {"threshold over HTTP", test_threshold_over_http},
      {"invalid HTTP requests rejected", test_invalid_threshold_rejected},
      {"mute over HTTP", test_mute_over_http},
      {"sensor failure and recovery", test_sensor_failure_and_recovery},
      {"no stale telemetry during a sensor error", test_no_stale_telemetry_during_sensor_error},
      {"history ring buffer wraps in order", test_history_ring_buffer},
      {"min/max since boot", test_min_max_since_boot},
      {"telemetry fits the MQTT buffer", test_telemetry_fits_mqtt_buffer},
      {"broker down: board stays responsive", test_broker_down_keeps_board_responsive},
      {"reconnects after broker restart", test_reconnects_after_broker_restart},
      {"no MQTT attempts without Wi-Fi", test_no_mqtt_attempts_without_wifi},
      {"web page served, unknown path 404", test_web_page_and_404},
      {"forecast: none yet -> nulls in status", test_forecast_status_before_any},
      {"forecast: shown on LCD and web", test_forecast_shown_on_lcd_and_web},
      {"forecast: LCD wording (<1 min, ~60 min)", test_forecast_lcd_wording},
      {"forecast: not rising", test_forecast_not_rising},
      {"forecast: hidden on LCD while alerting", test_forecast_hidden_while_alerting},
      {"forecast: expires after 2 minutes", test_forecast_expires},
      {"forecast: bad messages ignored", test_bad_forecast_ignored},
  };

  int failed = 0;
  for (const auto& t : tests) {
    std::cout << std::flush;
    pid_t pid = fork();
    if (pid == 0) {
      t.fn();
      std::cout << std::flush;
      _exit(failures ? 1 : 0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok) failed++;
    std::cout << (ok ? "PASS  " : "FAIL  ") << t.name
              << (WIFSIGNALED(status) ? "  (crashed)" : "") << "\n";
  }
  std::cout << "\n" << (sizeof(tests) / sizeof(tests[0]) - failed) << " passed, " << failed << " failed\n";
  return failed ? 1 : 0;
}
