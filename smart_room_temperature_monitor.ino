#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>

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
const unsigned long MQTT_RETRY_MS = 5000;

// Both can be changed at runtime over MQTT (see README).
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
bool hasReading = false;
bool sensorError = false;
unsigned long sensorErrorCount = 0;

String topicTelemetry;     // smartroom/<id>/telemetry      (device -> broker)
String topicStatus;        // smartroom/<id>/status         (retained online/offline)
String topicCmdThreshold;  // smartroom/<id>/cmd/threshold  (broker -> device)
String topicCmdMute;       // smartroom/<id>/cmd/mute       (broker -> device)

bool isAlert() {
  return hasReading && !sensorError && temperature >= alertTemp;
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
  if (!mqtt.connected()) {
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

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  char value[32];
  unsigned int n = length < sizeof(value) - 1 ? length : sizeof(value) - 1;
  memcpy(value, payload, n);
  value[n] = '\0';

  String t = topic;
  if (t == topicCmdThreshold) {
    float v = atof(value);
    if (v > 0 && v <= 60) {
      alertTemp = v;
      Serial.print("Alert threshold set to ");
      Serial.println(alertTemp, 1);
    } else {
      Serial.print("Ignored invalid threshold: ");
      Serial.println(value);
    }
  } else if (t == topicCmdMute) {
    String v = value;
    v.toLowerCase();
    buzzerMuted = (v == "1" || v == "true" || v == "on");
    Serial.println(buzzerMuted ? "Buzzer muted" : "Buzzer unmuted");
  } else {
    return;
  }

  // Apply the change right away instead of waiting for the next reading.
  if (hasReading && !sensorError) {
    updateDisplay();
    publishTelemetry();
  }
}

// Non-blocking: tries at most once every MQTT_RETRY_MS so the LCD,
// buzzer, and web page keep working while the broker is unreachable.
void ensureMqttConnected() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) {
    return;
  }

  static unsigned long lastAttempt = 0;
  if (lastAttempt != 0 && millis() - lastAttempt < MQTT_RETRY_MS) {
    return;
  }
  lastAttempt = millis();

  String clientId = String("smartroom-") + DEVICE_ID;
  Serial.print("Connecting to MQTT broker ");
  Serial.print(MQTT_HOST);
  Serial.print("... ");

  // Last Will: the broker publishes "offline" if the device drops off.
  if (mqtt.connect(clientId.c_str(), topicStatus.c_str(), 1, true, "offline")) {
    Serial.println("connected");
    mqtt.publish(topicStatus.c_str(), "online", true);
    mqtt.subscribe(topicCmdThreshold.c_str());
    mqtt.subscribe(topicCmdMute.c_str());
  } else {
    Serial.print("failed, rc=");
    Serial.println(mqtt.state());
  }
}

void handleRoot() {
  String statusText;
  if (!hasReading) {
    statusText = "Waiting for first reading";
  } else if (sensorError) {
    statusText = "SENSOR ERROR (showing last good reading)";
  } else if (isAlert()) {
    statusText = buzzerMuted ? "HIGH TEMPERATURE ALERT (buzzer muted)"
                             : "HIGH TEMPERATURE ALERT";
  } else {
    statusText = "Temperature normal";
  }

  String page = "<!DOCTYPE html><html><head>";
  page += "<meta http-equiv='refresh' content='3'>";
  page += "<title>Smart Room Monitor</title></head><body>";
  page += "<h1>Smart Room Temperature Monitor</h1>";
  page += "<h2>Temperature: ";
  page += hasReading ? String(temperature, 1) : String("--");
  page += " &deg;C</h2>";
  page += "<h2>Humidity: ";
  page += hasReading ? String(humidity, 1) : String("--");
  page += " %</h2>";
  page += "<h2>Status: ";
  page += statusText;
  page += "</h2>";
  page += "<p>Alert threshold: ";
  page += String(alertTemp, 1);
  page += " &deg;C &middot; MQTT: ";
  page += mqtt.connected() ? "connected" : "disconnected";
  page += "</p></body></html>";

  server.send(200, "text/html", page);
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
  delay(1500);
  lcd.clear();

  server.on("/", handleRoot);
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
      hasReading = true;
      temperature = t;
      humidity = h;
      heatIndex = dht.computeHeatIndex(t, h, false);

      updateDisplay();
      publishTelemetry();

      Serial.print("Temperature: ");
      Serial.print(temperature, 1);
      Serial.print(" C, Humidity: ");
      Serial.print(humidity, 1);
      Serial.println(" %");
    }
  }
}
