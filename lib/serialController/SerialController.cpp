#include "SerialController.h"
#include <Arduino.h>

SerialController::SerialController() {}

void SerialController::begin(unsigned long baud) {
  Serial.begin(baud);
  while (!Serial)
    delay(10); // wait for serial (USB)
}

void SerialController::print(const char *s) { Serial.print(s); }
void SerialController::print(const String &s) { Serial.print(s); }
void SerialController::println(const char *s) { Serial.println(s); }
void SerialController::println(const String &s) { Serial.println(s); }

void SerialController::print(int val) { Serial.print(val); }
void SerialController::println(int val) { Serial.println(val); }
void SerialController::print(unsigned long val) { Serial.print(val); }
void SerialController::println(unsigned long val) { Serial.println(val); }
int SerialController::available() { return Serial.available(); }

int SerialController::read() { return Serial.read(); }

String SerialController::readStringUntil(char terminator) {
  return Serial.readStringUntil(terminator);
}
