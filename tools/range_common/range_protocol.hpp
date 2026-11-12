#pragma once

#include <cstddef>
#include <cstdint>

// Range test frame format, shared by tools/range_ping (Thingy:53) and tools/range_pong (MFS_1).
//
// Both frames travel as manufacturer-specific data (AD type 0xFF) in a non-connectable,
// non-scannable extended advertisement on the LE Coded PHY (S8), primary and secondary.
//
//   [0..1] company ID 0xFFFF, little-endian (Bluetooth SIG: reserved for internal testing)
//   [2..3] magic "CR" (CLASS range)
//   [4]    frame type: 0x01 ping, 0x02 pong
//   [5..6] sequence number, little-endian - a pong echoes the ping it answers
//   [7]    pong only: RSSI of that ping as heard by the MFS_1, signed dBm
//
// Bench tool only. Nothing here is authenticated and the main firmware never sends it.

namespace alc::range
{

  constexpr uint16_t M_COMPANY_ID { 0xFFFF };
  constexpr uint8_t M_MAGIC_0 { 'C' };
  constexpr uint8_t M_MAGIC_1 { 'R' };

  enum class FrameType : uint8_t {
    Ping = 0x01,
    Pong = 0x02,
  };

  constexpr size_t M_PING_LENGTH { 7 };
  constexpr size_t M_PONG_LENGTH { 8 };

  constexpr size_t M_OFFSET_MAGIC { 2 };
  constexpr size_t M_OFFSET_TYPE { 4 };
  constexpr size_t M_OFFSET_SEQUENCE { 5 };
  constexpr size_t M_OFFSET_RSSI { 7 };

  constexpr uint8_t M_BYTE_MASK { 0xFF };
  constexpr uint8_t M_BITS_PER_BYTE { 8 };

  // 100 ms advertising interval, in 0.625 ms units.
  constexpr uint32_t M_ADV_INTERVAL { 160 };

  // Continuous coded scan: window equals interval (60 ms, in 0.625 ms units).
  constexpr uint16_t M_SCAN_INTERVAL { 96 };
  constexpr uint16_t M_SCAN_WINDOW { 96 };

  struct Frame
  {
      FrameType type;
      uint16_t sequence;
      int8_t rssiDbm;
  };

  // Writes a frame into buffer, which must hold M_PONG_LENGTH bytes. Returns the encoded length.
  inline size_t Encode(const Frame& frame, uint8_t* buffer)
  {
    buffer[0]                     = static_cast<uint8_t>(M_COMPANY_ID & M_BYTE_MASK);
    buffer[1]                     = static_cast<uint8_t>(M_COMPANY_ID >> M_BITS_PER_BYTE);
    buffer[M_OFFSET_MAGIC]        = M_MAGIC_0;
    buffer[M_OFFSET_MAGIC + 1]    = M_MAGIC_1;
    buffer[M_OFFSET_TYPE]         = static_cast<uint8_t>(frame.type);
    buffer[M_OFFSET_SEQUENCE]     = static_cast<uint8_t>(frame.sequence & M_BYTE_MASK);
    buffer[M_OFFSET_SEQUENCE + 1] = static_cast<uint8_t>(frame.sequence >> M_BITS_PER_BYTE);

    if (frame.type == FrameType::Ping) { return M_PING_LENGTH; }
    buffer[M_OFFSET_RSSI] = static_cast<uint8_t>(frame.rssiDbm);
    return M_PONG_LENGTH;
  }

  // Parses manufacturer data. Returns false for anything that is not a well-formed range frame.
  inline bool Decode(const uint8_t* data, size_t length, Frame& frame)
  {
    uint16_t companyId { 0 };

    if (length < M_PING_LENGTH) { return false; }
    companyId = static_cast<uint16_t>(data[0] | (data[1] << M_BITS_PER_BYTE));
    if (companyId != M_COMPANY_ID || data[M_OFFSET_MAGIC] != M_MAGIC_0 || data[M_OFFSET_MAGIC + 1] != M_MAGIC_1) { return false; }

    frame.sequence = static_cast<uint16_t>(data[M_OFFSET_SEQUENCE] | (data[M_OFFSET_SEQUENCE + 1] << M_BITS_PER_BYTE));
    frame.rssiDbm  = 0;

    switch (data[M_OFFSET_TYPE]) {
      case static_cast<uint8_t>(FrameType::Ping):
        frame.type = FrameType::Ping;
        return true;

      case static_cast<uint8_t>(FrameType::Pong):
        if (length < M_PONG_LENGTH) { return false; }
        frame.type    = FrameType::Pong;
        frame.rssiDbm = static_cast<int8_t>(data[M_OFFSET_RSSI]);
        return true;

      default:
        return false;
    }
  }

}
