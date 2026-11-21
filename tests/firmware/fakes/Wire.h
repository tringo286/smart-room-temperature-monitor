#pragma once
#include "Arduino.h"

class TwoWire {
 public:
  void begin(int, int) {}
};
inline TwoWire Wire;
