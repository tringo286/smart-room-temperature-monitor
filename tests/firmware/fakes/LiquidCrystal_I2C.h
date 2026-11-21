// Fake 16x2 LCD that keeps what is on screen so tests can read it back.
#pragma once
#include "Arduino.h"

class LiquidCrystal_I2C {
 public:
  LiquidCrystal_I2C(uint8_t, uint8_t cols, uint8_t rows) : cols_(cols), rows_(rows) { clear(); }
  void init() {}
  void backlight() {}
  void clear() {
    for (auto& l : lines_) l = std::string(cols_, ' ');
    col_ = row_ = 0;
  }
  void setCursor(uint8_t col, uint8_t row) { col_ = col; row_ = row; }

  template <typename T> void print(const T& v) { write(printable(v)); }
  void print(float v, int decimals) { write(printable((double)v, decimals)); }

  // Line contents with trailing spaces removed; the degree sign (0xDF on
  // the HD44780) is shown as '°'.
  std::string line(int row) const {
    std::string s = lines_[row];
    s.erase(s.find_last_not_of(' ') + 1);
    std::string out;
    for (char c : s) out += (c == (char)223) ? std::string("°") : std::string(1, c);
    return out;
  }

 private:
  void write(const std::string& s) {
    for (char c : s) {
      if (row_ < rows_ && col_ < cols_) lines_[row_][col_] = c;
      col_++;
    }
  }
  uint8_t cols_, rows_, col_ = 0, row_ = 0;
  std::string lines_[2];
};
