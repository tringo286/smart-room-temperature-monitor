#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>

#include "dashboard_page.h"

// ---------- Wi-Fi ----------
const char* ssid = "YOUR_WIFI_NAME";
const char* password = "YOUR_WIFI_PASSWORD";

// ---------- MQTT ----------
// IP address of the computer running the Docker stack in server/ (see README).
const char* MQTT_HOST = "YOUR_COMPUTER_IP";
const uint16_t MQTT_PORT = 1883;
// Used in the MQTT topics and shown as the "device" tag in Grafana.
const char* DEVICE_ID = "room1";

// ---------- Hardware ----------
#define DHTPIN D5
#define DHTTYPE DHT11
#define BUZZER_PIN D6

const unsigned long READ_INTERVAL_MS = 2000;
// A failed MQTT connect can block for several seconds (TCP timeout), so
// retries back off from 5 s to 60 s while the broker is unreachable.
const unsigned long MQTT_RETRY_MIN_MS = 5000;
const unsigned long MQTT_RETRY_MAX_MS = 60000;

// Short history kept on the board for the web dashboard's trend chart:
// one sample every 10 s, 120 samples = the last 20 minutes.
const unsigned long HISTORY_INTERVAL_MS = 10000;
const int HISTORY_SIZE = 120;

// Forecasts from server/forecaster arrive every ~10 s; ignore them once they
// are this old (forecaster or broker stopped).
const unsigned long FORECAST_MAX_AGE_MS = 120000;
// LCD line 2 alternates between humidity and the forecast every 4 s.
const unsigned long LCD_ALTERNATE_MS = 4000;

// Both can be changed at runtime from the web dashboard or over MQTT.
float alertTemp = 30.0;
bool buzzerMuted = false;

DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);
ESP8266WebServer server(80);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

float temperature = NAN;
float humidity = NAN;
float heatIndex = NAN;
float minTemp = NAN;  // lowest and highest temperature since boot
float maxTemp = NAN;
bool hasReading = false;
bool sensorError = false;
unsigned long sensorErrorCount = 0;

float historyTemp[HISTORY_SIZE];
float historyHum[HISTORY_SIZE];
int historyCount = 0;  // number of valid samples
int historyNext = 0;   // index the next sample is written to

// Latest forecast received over MQTT (see forecastValid()).
bool hasForecast = false;
float forecastTemp = NAN;     // predicted temperature in 10 minutes
float forecastMinutes = NAN;  // minutes until the threshold; 0 = now, -1 = not expected
unsigned long forecastAt = 0;

String topicTelemetry;     // smartroom/<id>/telemetry      (device -> broker)
String topicStatus;        // smartroom/<id>/status         (retained online/offline)
String topicCmdThreshold;  // smartroom/<id>/cmd/threshold  (broker -> device)
String topicCmdMute;       // smartroom/<id>/cmd/mute       (broker -> device)
String topicForecast;      // smartroom/<id>/forecast       (forecaster -> device)

bool isAlert() {
  return hasReading && !sensorError && temperature >= alertTemp;
}

bool forecastValid() {
  return hasForecast && millis() - forecastAt < FORECAST_MAX_AGE_MS;
}

// True when the forecaster expects the alert within the next hour.
bool alertExpectedSoon() {
  return forecastValid() && forecastMinutes > 0 && forecastMinutes <= 60;
}

void updateDisplay() {
  lcd.setCursor(0, 0);
  lcd.print("Temp: ");
  lcd.print(temperature, 1);
  lcd.print((char)223);
  lcd.print("C   ");

  lcd.setCursor(0, 1);
  if (isAlert()) {
    lcd.print(buzzerMuted ? "HIGH TEMP (mute)" : "HIGH TEMP!      ");
  } else if (alertExpectedSoon() && (millis() / LCD_ALTERNATE_MS) % 2 == 1) {
    char line[17];
    int minutes = (int)(forecastMinutes + 0.5);
    if (minutes < 1) {
      snprintf(line, sizeof(line), "%-16s", "Alert in <1 min");
    } else {
      snprintf(line, sizeof(line), "Alert in ~%d min", minutes);
      for (size_t i = strlen(line); i < 16; i++) line[i] = ' ';
      line[16] = '\0';
    }
    lcd.print(line);
  } else {
    lcd.print("Hum:  ");
    lcd.print(humidity, 1);
    lcd.print("%       ");
  }

  if (isAlert() && !buzzerMuted) {
    tone(BUZZER_PIN, 2000);
  } else {
    noTone(BUZZER_PIN);
  }
}

void publishTelemetry() {
  // Never publish the last good reading again while the sensor is failing:
  // Grafana would store it as if it were a new measurement.
  if (!mqtt.connected() || !hasReading || sensorError) {
    return;
  }

  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"temperature\":%.1f,\"humidity\":%.1f,\"heat_index\":%.1f,"
           "\"threshold\":%.1f,\"alert\":%d,\"buzzer_muted\":%d,"
           "\"sensor_errors\":%lu,\"rssi\":%d,\"uptime_s\":%lu}",
           temperature, humidity, heatIndex,
           alertTemp, isAlert() ? 1 : 0, buzzerMuted ? 1 : 0,
           sensorErrorCount, (int)WiFi.RSSI(), millis() / 1000);

  mqtt.publish(topicTelemetry.c_str(), payload);
}

// ---------- Settings shared by the web dashboard and MQTT ----------

// Apply a settings change right away instead of waiting for the next reading.
void onSettingsChanged() {
  if (hasReading && !sensorError) {
    updateDisplay();
  }
  publishTelemetry();
}

bool setAlertThreshold(float value) {
  if (isnan(value) || value <= 0 || value > 60) {
    return false;
  }
  alertTemp = value;
  Serial.print("Alert threshold set to ");
  Serial.println(alertTemp, 1);
  onSettingsChanged();
  return true;
}

void setBuzzerMuted(bool muted) {
  buzzerMuted = muted;
  Serial.println(buzzerMuted ? "Buzzer muted" : "Buzzer unmuted");
  onSettingsChanged();
}

bool parseOnOff(String value) {
  value.trim();
  value.toLowerCase();
  return value == "1" || value == "true" || value == "on";
}

// ---------- MQTT ----------

// Reads the number stored under "key" in a flat JSON object such as
// {"temp_in_10m":28.9,"minutes_to_threshold":6.2}. Returns NAN if missing.
float jsonNumber(const char* json, const char* key) {
  char pattern[40];
  snprintf(pattern, sizeof(pattern), "\"%s\":", key);
  const char* p = strstr(json, pattern);
  if (p == nullptr) {
    return NAN;
  }
  p += strlen(pattern);
  char* end;
  float v = strtod(p, &end);
  return end == p ? NAN : v;
}

void onForecast(const char* json) {
  float temp = jsonNumber(json, "temp_in_10m");
  float minutes = jsonNumber(json, "minutes_to_threshold");
  if (isnan(temp) || isnan(minutes) || temp < -40 || temp > 100) {
    Serial.println("Ignored invalid forecast");
    return;
  }
  hasForecast = true;
  forecastTemp = temp;
  forecastMinutes = minutes;
  forecastAt = millis();
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  char value[384];  // forecast messages are ~250 bytes; PubSubClient buffer is 512
  unsigned int n = length < sizeof(value) - 1 ? length : sizeof(value) - 1;
  memcpy(value, payload, n);
  value[n] = '\0';

  String t = topic;
  if (t == topicForecast) {
    onForecast(value);
  } else if (t == topicCmdThreshold) {
    if (!setAlertThreshold(atof(value))) {
      Serial.print("Ignored invalid threshold: ");
      Serial.println(value);
    }
  } else if (t == topicCmdMute) {
    setBuzzerMuted(parseOnOff(value));
  }
}

// Each connect attempt can block for seconds when the broker's computer is
// unreachable, so attempts are spaced out (5 s, 10 s, 20 s, ... up to 60 s)
// to keep the LCD, buzzer, and web dashboard responsive in between.
void ensureMqttConnected() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) {
    return;
  }

  static unsigned long lastAttempt = 0;
  static unsigned long retryDelay = 0;  // 0 = try right away
  if (millis() - lastAttempt < retryDelay) {
    return;
  }

  String clientId = String("smartroom-") + DEVICE_ID;
  Serial.print("Connecting to MQTT broker ");
  Serial.print(MQTT_HOST);
  Serial.print("... ");

  // Last Will: the broker publishes "offline" if the device drops off.
  bool ok = mqtt.connect(clientId.c_str(), topicStatus.c_str(), 1, true, "offline");

  // Measured after the attempt, which may itself have taken seconds.
  lastAttempt = millis();

  if (ok) {
    Serial.println("connected");
    mqtt.publish(topicStatus.c_str(), "online", true);
    mqtt.subscribe(topicCmdThreshold.c_str());
    mqtt.subscribe(topicCmdMute.c_str());
    mqtt.subscribe(topicForecast.c_str());
    retryDelay = 0;  // after a future disconnect, try again right away once
  } else {
    if (retryDelay == 0) {
      retryDelay = MQTT_RETRY_MIN_MS;
    } else if (retryDelay * 2 > MQTT_RETRY_MAX_MS) {
      retryDelay = MQTT_RETRY_MAX_MS;
    } else {
      retryDelay *= 2;
    }
    Serial.print("failed, rc=");
    Serial.print(mqtt.state());
    Serial.print(", next try in ");
    Serial.print(retryDelay / 1000);
    Serial.println(" s");
  }
}

// ---------- Web dashboard ----------

// Writes a number with one decimal, or null if there is no valid value.
void appendNumber(String& out, float value) {
  if (isnan(value)) {
    out += "null";
  } else {
    out += String(value, 1);
  }
}

void handleRoot() {
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

void handleStatus() {
  String json;
  json.reserve(448);
  json += "{\"device\":\"";
  json += DEVICE_ID;
  json += "\",\"has_reading\":";
  json += hasReading ? "true" : "false";
  json += ",\"sensor_error\":";
  json += sensorError ? "true" : "false";
  json += ",\"temperature\":";
  appendNumber(json, temperature);
  json += ",\"humidity\":";
  appendNumber(json, humidity);
  json += ",\"heat_index\":";
  appendNumber(json, heatIndex);
  json += ",\"min_temp\":";
  appendNumber(json, minTemp);
  json += ",\"max_temp\":";
  appendNumber(json, maxTemp);
  json += ",\"threshold\":";
  appendNumber(json, alertTemp);
  json += ",\"alert\":";
  json += isAlert() ? "true" : "false";
  json += ",\"muted\":";
  json += buzzerMuted ? "true" : "false";
  json += ",\"mqtt\":";
  json += mqtt.connected() ? "true" : "false";
  json += ",\"mqtt_host\":\"";
  json += MQTT_HOST;
  json += "\",\"ip\":\"";
  json += WiFi.localIP().toString();
  json += "\",\"rssi\":";
  json += String((int)WiFi.RSSI());
  json += ",\"uptime_s\":";
  json += String(millis() / 1000);
  json += ",\"sensor_errors\":";
  json += String(sensorErrorCount);
  // Forecast: null when none has arrived recently; forecast_minutes is also
  // null when the room is not heading for an alert.
  bool fc = forecastValid();
  json += ",\"forecast_temp\":";
  appendNumber(json, fc ? forecastTemp : NAN);
  json += ",\"forecast_minutes\":";
  appendNumber(json, fc && forecastMinutes >= 0 ? forecastMinutes : NAN);
  json += "}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleHistory() {
  String json;
  json.reserve(HISTORY_SIZE * 12 + 64);
  json += "{\"interval_s\":";
  json += String(HISTORY_INTERVAL_MS / 1000);

  // Oldest sample first.
  int start = (historyNext - historyCount + HISTORY_SIZE) % HISTORY_SIZE;

  json += ",\"temperature\":[";
  for (int i = 0; i < historyCount; i++) {
    if (i > 0) json += ',';
    appendNumber(json, historyTemp[(start + i) % HISTORY_SIZE]);
  }
  json += "],\"humidity\":[";
  for (int i = 0; i < historyCount; i++) {
    if (i > 0) json += ',';
    appendNumber(json, historyHum[(start + i) % HISTORY_SIZE]);
  }
  json += "]}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleSetThreshold() {
  if (!server.hasArg("value") || !setAlertThreshold(server.arg("value").toFloat())) {
    server.send(400, "application/json", "{\"error\":\"threshold must be between 0 and 60\"}");
    return;
  }
  handleStatus();
}

void handleSetMute() {
  if (!server.hasArg("value")) {
    server.send(400, "application/json", "{\"error\":\"missing value\"}");
    return;
  }
  setBuzzerMuted(parseOnOff(server.arg("value")));
  handleStatus();
}

void recordHistory() {
  historyTemp[historyNext] = temperature;
  historyHum[historyNext] = humidity;
  historyNext = (historyNext + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE) {
    historyCount++;
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  noTone(BUZZER_PIN);

  Wire.begin(D2, D1);
  lcd.init();
  lcd.backlight();

  dht.begin();

  String base = String("smartroom/") + DEVICE_ID;
  topicTelemetry = base + "/telemetry";
  topicStatus = base + "/status";
  topicCmdThreshold = base + "/cmd/threshold";
  topicCmdMute = base + "/cmd/mute";
  topicForecast = base + "/forecast";

  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi");
  delay(1000);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("Wi-Fi connected");
  Serial.print("NodeMCU IP address: ");
  Serial.println(WiFi.localIP());

  lcd.clear();
  lcd.print("WiFi connected");
  lcd.setCursor(0, 1);
  lcd.print(WiFi.localIP());
  delay(3000);
  lcd.clear();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/threshold", HTTP_POST, handleSetThreshold);
  server.on("/api/mute", HTTP_POST, handleSetMute);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
  Serial.println("Web server started");

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(5);
}

void loop() {
  server.handleClient();
  ensureMqttConnected();
  mqtt.loop();

  static unsigned long lastReading = 0;
  static unsigned long lastHistory = 0;

  if (millis() - lastReading >= READ_INTERVAL_MS) {
    lastReading = millis();

    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (isnan(h) || isnan(t)) {
      sensorError = true;
      sensorErrorCount++;
      noTone(BUZZER_PIN);
      lcd.clear();
      lcd.print("Sensor Error");
      Serial.println("DHT11 reading failed");
    } else {
      sensorError = false;
      bool firstReading = !hasReading;
      hasReading = true;
      temperature = t;
      humidity = h;
      heatIndex = dht.computeHeatIndex(t, h, false);
      if (firstReading || t < minTemp) minTemp = t;
      if (firstReading || t > maxTemp) maxTemp = t;

      updateDisplay();
      publishTelemetry();

      if (firstReading || millis() - lastHistory >= HISTORY_INTERVAL_MS) {
        lastHistory = millis();
        recordHistory();
      }

      Serial.print("Temperature: ");
      Serial.print(temperature, 1);
      Serial.print(" C, Humidity: ");
      Serial.print(humidity, 1);
      Serial.println(" %");
    }
  }
}
