#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>

const char* ssid = "YOUR_WIFI_NAME";
const char* password = "YOUR_WIFI_PASSWORD";

#define DHTPIN D5
#define DHTTYPE DHT11
#define BUZZER_PIN D6

const float ALERT_TEMP = 30.0;

DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);
ESP8266WebServer server(80);

float temperature = 0;
float humidity = 0;
bool sensorError = false;

void updateDisplay() {
  lcd.setCursor(0, 0);
  lcd.print("Temp: ");
  lcd.print(temperature, 1);
  lcd.print((char)223);
  lcd.print("C   ");

  if (temperature >= ALERT_TEMP) {
    lcd.setCursor(0, 1);
    lcd.print("HIGH TEMP!      ");
    tone(BUZZER_PIN, 2000);
  } else {
    lcd.setCursor(0, 1);
    lcd.print("Hum:  ");
    lcd.print(humidity, 1);
    lcd.print("%       ");
    noTone(BUZZER_PIN);
  }
}

void handleRoot() {
  String alertText = temperature >= ALERT_TEMP
                     ? "HIGH TEMPERATURE ALERT"
                     : "Temperature normal";

  String page = "<!DOCTYPE html><html><head>";
  page += "<meta http-equiv='refresh' content='3'>";
  page += "<title>Smart Room Monitor</title></head><body>";
  page += "<h1>Smart Room Temperature Monitor</h1>";
  page += "<h2>Temperature: ";
  page += String(temperature, 1);
  page += " &deg;C</h2>";
  page += "<h2>Humidity: ";
  page += String(humidity, 1);
  page += " %</h2>";
  page += "<h2>Status: ";
  page += alertText;
  page += "</h2></body></html>";

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

  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi");
  delay(1000);

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
}

void loop() {
  server.handleClient();

  static unsigned long lastReading = 0;

  if (millis() - lastReading >= 2000) {
    lastReading = millis();

    humidity = dht.readHumidity();
    temperature = dht.readTemperature();

    if (isnan(humidity) || isnan(temperature)) {
      sensorError = true;
      noTone(BUZZER_PIN);
      lcd.clear();
      lcd.print("Sensor Error");
      Serial.println("DHT11 reading failed");
    } else {
      sensorError = false;
      updateDisplay();

      Serial.print("Temperature: ");
      Serial.print(temperature, 1);
      Serial.print(" C, Humidity: ");
      Serial.print(humidity, 1);
      Serial.println(" %");
    }
  }
}