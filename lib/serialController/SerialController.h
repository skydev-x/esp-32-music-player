#pragma once
#include <Arduino.h>

class SerialController {
public:
  SerialController();

  void begin(unsigned long baud = 115200);
  void print(const char *s);
  void println(const char *s);
  void println(const String &s);
  void print(const String &s);
  void print(int val);
  void println(int val);
  void print(unsigned long val);
  void println(unsigned long val);
  int available();
  int read();
  String readStringUntil(char terminator);
};
