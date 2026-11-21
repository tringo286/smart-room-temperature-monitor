# Smart Room Temperature Monitor

An IoT system that measures room temperature and humidity using a DHT11 sensor. The NodeMCU ESP8266 displays the readings on an LCD and provides a web dashboard over Wi-Fi. A buzzer alerts the user when the temperature reaches the alert threshold.

## Components

- NodeMCU ESP8266
- DHT11 temperature and humidity sensor
- 16x2 LCD with I2C backpack
- Buzzer
- Breadboard and jumper wires
- USB data cable
- Wi-Fi network

## Wiring

### DHT11 Sensor

| DHT11 Pin | NodeMCU Pin |
|---|---|
| Signal | D5 |
| VCC | 3V3 |
| GND | GND |

### LCD I2C Backpack

| LCD Pin | NodeMCU Pin |
|---|---|
| GND | GND |
| VCC | 3V3 |
| SDA | D2 |
| SCL | D1 |

### Buzzer

| Buzzer Pin | NodeMCU Pin |
|---|---|
| Positive (+) | D6 |
| Negative (-) | GND |

## Software

- Arduino IDE
- ESP8266 board package
- DHT sensor library by Adafruit
- Adafruit Unified Sensor library
- LiquidCrystal I2C library

## Wi-Fi Configuration

Before uploading the sketch, replace the placeholders with the Wi-Fi network information:

```cpp
const char* ssid = "YOUR_WIFI_NAME";
const char* password = "YOUR_WIFI_PASSWORD";
```

Do not upload real Wi-Fi credentials to a public GitHub repository.

## Temperature Alert

The current alert threshold is 30°C:

```cpp
const float ALERT_TEMP = 30.0;
```

When the temperature reaches or exceeds this value, the LCD displays a high-temperature warning and the buzzer sounds. The web dashboard also reports the alert.

## Running the Project

1. Connect the NodeMCU to the computer with a USB data cable.
2. Open the sketch in Arduino IDE.
3. Select **NodeMCU 1.0 (ESP-12E Module)** as the board.
4. Select the NodeMCU serial port.
5. Upload the sketch.
6. Open Serial Monitor at **115200 baud**.
7. Wait for the NodeMCU to connect to Wi-Fi.
8. Copy the IP address shown in Serial Monitor.
9. Open that IP address in a web browser.

Example:

```text
http://10.0.0.118
```

The webpage refreshes automatically every three seconds.

## System Behavior

- The LCD displays the current temperature and humidity.
- The NodeMCU sends the readings through a web server over Wi-Fi.
- The web dashboard displays the current readings and system status.
- The buzzer sounds during a high-temperature alert.
- The buzzer stops when the temperature returns below the threshold.

## Testing

To test the alert without waiting for a hot room, temporarily lower `ALERT_TEMP` to a value below the current room temperature. After testing, restore it to 30.0 and upload the sketch again.

## Project File

- `smart_room_temperature_monitor.ino` - Arduino source code for the NodeMCU, sensor, LCD, buzzer, Wi-Fi connection, and web dashboard.
