#include "RelayController.h"

#include <Arduino.h>

void RelayController::begin(uint8_t pin, bool activeLow) {
  pin_ = pin;
  activeLow_ = activeLow;
  on_ = false;
  // Drive the inactive level *before* switching the pin to an output, so a
  // relay wired active-low doesn't get a momentary "on" pulse from the pin's
  // default LOW state during boot.
  digitalWrite(pin_, activeLow_ ? HIGH : LOW);
  pinMode(pin_, OUTPUT);
  attached_ = true;
  applyToPin();
}

void RelayController::writeRelay(bool on) {
  if (on == on_ && attached_) return;
  on_ = on;
  applyToPin();
}

void RelayController::setActiveLow(bool activeLow) {
  if (activeLow == activeLow_) return;
  activeLow_ = activeLow;
  applyToPin();
}

void RelayController::applyToPin() {
  if (!attached_) return;
  const bool level = activeLow_ ? !on_ : on_;
  digitalWrite(pin_, level ? HIGH : LOW);
  // One line per actual change (writeRelay early-returns when nothing moved),
  // so "did my toggle reach the board at all?" is answerable from the serial
  // monitor without any instrumentation — including during sequence playback,
  // where it shows the recorded light track being replayed.
  Serial.printf("[RELAY] %s (pin %u -> %s)\n", on_ ? "ON" : "off", pin_, level ? "HIGH" : "LOW");
}
