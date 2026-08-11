#pragma once

#include <stdint.h>
#include <stddef.h>

#include "NetworkLink.h" // SeqAckStatus

class NetworkLink;

// MASTER-only: reads newline-delimited JSON commands from a PC over USB
// Serial and forwards them to NetworkLink as ESP-NOW CMD broadcasts. See
// docs/serial-protocol.md for the wire format.
class SerialBridge {
public:
  void begin(NetworkLink *network);

  // Call every loop() iteration; non-blocking, buffers partial lines.
  void loopTick();

  // Relays a SEQ_ACK (from NetworkLink::onSeqAck) to the PC as one JSON
  // line, including a human-readable "reason" string for a non-Ok status —
  // the PC side otherwise only sees a bare ok:false with no hint whether to
  // just retry, or that the Node likely reset mid-upload, or its flash is
  // actually full.
  void reportUploadResult(uint8_t nodeId, const char *name, SeqAckStatus status, uint16_t points);

  // Relays a SPACE_REPLY (from NetworkLink::onSpaceReply) to the PC.
  void reportSpaceReply(uint8_t nodeId, uint32_t freeBytes);

private:
  static constexpr size_t kLineBufSize = 256;

  NetworkLink *network_ = nullptr;
  char lineBuf_[kLineBufSize];
  size_t lineLen_ = 0;

  void handleLine(const char *line);
};
