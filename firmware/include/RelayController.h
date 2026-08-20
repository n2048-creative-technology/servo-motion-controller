#pragma once

#include <stdint.h>
#include "IRelaySink.h"

// A single on/off relay output (D7 by default — see RELAY_PIN in Config.h),
// used to switch a light alongside the servo's motion. State is tracked
// logically ("on"/"off"), with the active-low inversion applied only at the
// pin, so recordings and the API mean the same thing regardless of which kind
// of relay board is wired up.
class RelayController : public IRelaySink {
public:
  void begin(uint8_t pin, bool activeLow);

  void writeRelay(bool on) override;
  bool relayState() const override { return on_; }

  // Re-applies the current logical state through the new polarity, so
  // changing this in Settings doesn't leave the relay physically inverted
  // until the next toggle.
  void setActiveLow(bool activeLow);

  bool activeLow() const { return activeLow_; }
  uint8_t pin() const { return pin_; }

private:
  uint8_t pin_ = 0;
  bool activeLow_ = false;
  bool on_ = false;
  bool attached_ = false;

  void applyToPin();
};
