#pragma once

#include <stdint.h>
#include <stddef.h>

class NetworkLink;

// MASTER-only: reads newline-delimited JSON commands from a PC over USB
// Serial and forwards them to NetworkLink as ESP-NOW CMD broadcasts. See
// docs/serial-protocol.md for the wire format.
class SerialBridge {
public:
  void begin(NetworkLink *network);

  // Call every loop() iteration; non-blocking, buffers partial lines.
  void loopTick();

private:
  static constexpr size_t kLineBufSize = 256;

  NetworkLink *network_ = nullptr;
  char lineBuf_[kLineBufSize];
  size_t lineLen_ = 0;

  void handleLine(const char *line);
};
