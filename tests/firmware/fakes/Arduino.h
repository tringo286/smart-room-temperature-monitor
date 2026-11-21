// Minimal fake of the Arduino core so the sketch compiles and runs on a
// computer. Only what smart_room_temperature_monitor.ino uses is provided.
// Tests control time with fake::now_ms and read back the buzzer state.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <math.h>
#include <sstream>
#include <string>

using std::isnan;

typedef uint8_t byte;
#define PROGMEM
typedef const char* PGM_P;

#define OUTPUT 1
#define D1 5
#define D2 4
#define D5 14
#define D6 12

namespace fake {
inline unsigned long now_ms = 0;
inline unsigned int buzzer_hz = 0;  // 0 = silent
inline std::ostringstream serial_log;
}  // namespace fake

inline unsigned long millis() { return fake::now_ms; }
inline void delay(unsigned long ms) { fake::now_ms += ms; }
inline void pinMode(uint8_t, uint8_t) {}
inline void tone(uint8_t, unsigned int hz) { fake::buzzer_hz = hz; }
inline void noTone(uint8_t) { fake::buzzer_hz = 0; }

class String {
 public:
  String() {}
  String(const char* s) : s_(s ? s : "") {}
  String(const std::string& s) : s_(s) {}
  String(char c) : s_(1, c) {}
  String(int v) : s_(std::to_string(v)) {}
  String(unsigned int v) : s_(std::to_string(v)) {}
  String(long v) : s_(std::to_string(v)) {}
  String(unsigned long v) : s_(std::to_string(v)) {}
  String(float v, unsigned char decimals = 2) : s_(fmt(v, decimals)) {}
  String(double v, unsigned char decimals = 2) : s_(fmt(v, decimals)) {}

  const char* c_str() const { return s_.c_str(); }
  unsigned int length() const { return s_.size(); }
  void reserve(unsigned int n) { s_.reserve(n); }
  float toFloat() const { return (float)atof(s_.c_str()); }
  void toLowerCase() { for (auto& c : s_) c = (char)tolower((unsigned char)c); }
  void trim() {
    auto ws = [](unsigned char c) { return isspace(c); };
    s_.erase(s_.begin(), std::find_if_not(s_.begin(), s_.end(), ws));
    s_.erase(std::find_if_not(s_.rbegin(), s_.rend(), ws).base(), s_.end());
  }

  String& operator+=(const String& o) { s_ += o.s_; return *this; }
  String& operator+=(const char* o) { s_ += o; return *this; }
  String& operator+=(char c) { s_ += c; return *this; }
  friend String operator+(const String& a, const String& b) { return String(a.s_ + b.s_); }
  friend String operator+(const String& a, const char* b) { return String(a.s_ + b); }
  bool operator==(const String& o) const { return s_ == o.s_; }
  bool operator==(const char* o) const { return s_ == o; }

  const std::string& str() const { return s_; }

 private:
  static std::string fmt(double v, unsigned char d) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", d, v);
    return buf;
  }
  std::string s_;
};

class IPAddress {
 public:
  IPAddress(uint8_t a = 0, uint8_t b = 0, uint8_t c = 0, uint8_t d = 0) : a_(a), b_(b), c_(c), d_(d) {}
  String toString() const {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a_, b_, c_, d_);
    return String(buf);
  }

 private:
  uint8_t a_, b_, c_, d_;
};

// Print-like helpers shared by Serial and the LCD.
inline std::string printable(const char* s) { return s; }
inline std::string printable(const String& s) { return s.str(); }
inline std::string printable(char c) { return std::string(1, c); }
inline std::string printable(int v) { return std::to_string(v); }
inline std::string printable(unsigned long v) { return std::to_string(v); }
inline std::string printable(const IPAddress& ip) { return ip.toString().str(); }
inline std::string printable(double v, int decimals = 2) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  return buf;
}

class FakeSerial {
 public:
  void begin(unsigned long) {}
  template <typename T> void print(const T& v) { fake::serial_log << printable(v); }
  void print(float v, int d) { fake::serial_log << printable((double)v, d); }
  template <typename T> void println(const T& v) { print(v); println(); }
  void println(float v, int d) { print(v, d); println(); }
  void println() { fake::serial_log << "\n"; }
};
inline FakeSerial Serial;
