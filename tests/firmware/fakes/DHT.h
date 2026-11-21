// Fake DHT sensor. Tests set fake::sensor_temp / fake::sensor_hum
// (NAN simulates a failed read). computeHeatIndex uses the same NOAA
// formula as the Adafruit DHT library.
#pragma once
#include "Arduino.h"

#define DHT11 11

namespace fake {
inline float sensor_temp = 24.0f;
inline float sensor_hum = 45.0f;
}  // namespace fake

class DHT {
 public:
  DHT(uint8_t, uint8_t) {}
  void begin() {}
  float readTemperature() { return fake::sensor_temp; }
  float readHumidity() { return fake::sensor_hum; }

  float computeHeatIndex(float temperature, float humidity, bool isFahrenheit = true) {
    float t = isFahrenheit ? temperature : temperature * 1.8f + 32;
    float hi = 0.5f * (t + 61.0f + ((t - 68.0f) * 1.2f) + (humidity * 0.094f));
    if (hi > 79) {
      hi = -42.379f + 2.04901523f * t + 10.14333127f * humidity +
           -0.22475541f * t * humidity + -0.00683783f * t * t +
           -0.05481717f * humidity * humidity + 0.00122874f * t * t * humidity +
           0.00085282f * t * humidity * humidity + -0.00000199f * t * t * humidity * humidity;
      if ((humidity < 13) && (t >= 80.0f) && (t <= 112.0f))
        hi -= ((13.0f - humidity) * 0.25f) * sqrtf((17.0f - fabsf(t - 95.0f)) * 0.05882f);
      else if ((humidity > 85.0f) && (t >= 80.0f) && (t <= 87.0f))
        hi += ((humidity - 85.0f) * 0.1f) * ((87.0f - t) * 0.2f);
    }
    return isFahrenheit ? hi : (hi - 32) * 0.55555f;
  }
};
