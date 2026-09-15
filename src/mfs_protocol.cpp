#include "mfs_protocol.hpp"

namespace alc::protocol
{

  bool DecodeCommand(const uint8_t* plaintext, Command& out)
  {
    Command decoded;
    uint16_t minuteField { 0 };

    if (plaintext == nullptr) { return false; }

    minuteField = static_cast<uint16_t>(plaintext[M_PT_MINUTE] | (plaintext[M_PT_MINUTE + 1] << 8));

    decoded.type            = static_cast<CommandType>((plaintext[M_PT_ACTIVATIONS_MODE] >> M_TYPE_SHIFT) & M_TYPE_MASK);
    decoded.delayCode       = (plaintext[M_PT_ARM_DELAY] >> M_DELAY_SHIFT) & M_DELAY_MASK;
    decoded.activations     = static_cast<uint8_t>((plaintext[M_PT_ACTIVATIONS_MODE] & M_ACTIVATIONS_MASK) + 1);
    decoded.mode            = static_cast<Mode>((plaintext[M_PT_ACTIVATIONS_MODE] >> M_MODE_SHIFT) & M_MODE_MASK);
    decoded.cooldownByte    = plaintext[M_PT_COOLDOWN];
    decoded.sensitivityByte = plaintext[M_PT_SENSITIVITY];
    decoded.minuteOfDay     = minuteField & M_MINUTE_MASK;

    // Type 00 is reserved. It rejects UNLESS it carries the FIRE "OMG" magic in
    // bytes 0, 2, 3 with byte 1 held at 0x00 - see the FIRE command amendment
    // section 2. A type-00 payload without the magic still rejects exactly as
    // before, so an all-zero plaintext still does nothing.
    if (decoded.type == CommandType::Reserved) {
      const bool fireMagic = plaintext[0] == M_FIRE_MAGIC_0 && plaintext[M_PT_ACTIVATIONS_MODE] == 0x00 &&
                             plaintext[M_PT_COOLDOWN] == M_FIRE_MAGIC_2 && plaintext[M_PT_SENSITIVITY] == M_FIRE_MAGIC_3;
      if (!fireMagic) { return false; }

      // Bytes 6-7 are per-variant extension space and are never validated,
      // even for FIRE.
      if (decoded.minuteOfDay >= M_MINUTES_PER_DAY) { return false; }

      out             = Command {};
      out.isFire      = true;
      out.type        = CommandType::Reserved;
      out.minuteOfDay = decoded.minuteOfDay;
      return true;
    }

    // Reserved mode is refused rather than quietly treated as one of the others.
    // Silently downgrading an unknown mode to TriggerOnly would be worse: the
    // operator would believe a report had been configured.
    if (decoded.mode == Mode::Reserved) { return false; }

    // Eleven bits hold up to 2047, but a day has 1440 minutes. An out-of-range
    // minute is a bug in the sender, and the freshness check cannot judge it.
    if (decoded.minuteOfDay >= M_MINUTES_PER_DAY) { return false; }

    // Plaintext bytes 6-7, byte 0 bit 0 and minute bits 11-15 are deliberately
    // not examined. They belong to other MFS variants and MFS_1 must tolerate
    // whatever they hold.

    out = decoded;
    return true;
  }

  void EncodeCommand(const Command& command, uint8_t* plaintext)
  {
    uint16_t minuteField { static_cast<uint16_t>(command.minuteOfDay & M_MINUTE_MASK) };

    // FIRE has its own layout: magic in bytes 0, 2, 3, byte 1 held at 0x00, and
    // the minute - nothing else is meaningful.
    if (command.isFire) {
      plaintext[0]                     = M_FIRE_MAGIC_0;
      plaintext[M_PT_ACTIVATIONS_MODE] = 0x00;
      plaintext[M_PT_COOLDOWN]         = M_FIRE_MAGIC_2;
      plaintext[M_PT_SENSITIVITY]      = M_FIRE_MAGIC_3;
      plaintext[M_PT_MINUTE]           = static_cast<uint8_t>(minuteField & 0xFF);
      plaintext[M_PT_MINUTE + 1]       = static_cast<uint8_t>(minuteField >> 8);
      plaintext[6]                     = 0;
      plaintext[7]                     = 0;
      return;
    }

    plaintext[M_PT_ARM_DELAY] = static_cast<uint8_t>((command.delayCode & M_DELAY_MASK) << M_DELAY_SHIFT);
    plaintext[M_PT_ACTIVATIONS_MODE] =
        static_cast<uint8_t>(((command.activations - 1) & M_ACTIVATIONS_MASK) | ((static_cast<uint8_t>(command.mode) & M_MODE_MASK) << M_MODE_SHIFT) |
                             ((static_cast<uint8_t>(command.type) & M_TYPE_MASK) << M_TYPE_SHIFT));
    plaintext[M_PT_COOLDOWN]    = command.cooldownByte;
    plaintext[M_PT_SENSITIVITY] = command.sensitivityByte;
    plaintext[M_PT_MINUTE]      = static_cast<uint8_t>(minuteField & 0xFF);
    plaintext[M_PT_MINUTE + 1]  = static_cast<uint8_t>(minuteField >> 8);
    plaintext[6]                = 0;
    plaintext[7]                = 0;
  }

  const char* CommandTypeName(CommandType type)
  {
    switch (type) {
      case CommandType::Settings:
        return "Settings";
      case CommandType::Arm:
        return "Arm";
      case CommandType::Disarm:
        return "Disarm";
      default:
        return "Reserved";
    }
  }

}
