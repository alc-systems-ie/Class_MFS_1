#include <cassert>
#include <cstdio>
#include <initializer_list>

#include "mfs_protocol.hpp"

void run_protocol_tests()
{
  using namespace alc::protocol;

  // Table anchors, straight from the design spec section 5.
  assert(M_THRESHOLD_TABLE[0] == 4000);  // 1000 mg, least sensitive
  assert(M_THRESHOLD_TABLE[143] == 302); // ~75 mg, the present default
  assert(M_THRESHOLD_TABLE[255] == 40);  // 10 mg, most sensitive

  assert(M_COOLDOWN_TABLE[0] == 0); // reserved: no cooldown
  assert(M_COOLDOWN_TABLE[1] == 1);
  assert(M_COOLDOWN_TABLE[128] == 60); // the clean midpoint
  assert(M_COOLDOWN_TABLE[255] == 3600);

  // Sensitivity must be MONOTONICALLY DECREASING in threshold: a higher byte
  // means more sensitive, which means a lower number. Getting this backwards
  // would make the app's slider work in reverse, which is easy to miss on the
  // bench because the device still triggers - just at the wrong setting.
  for (int i = 1; i < 256; i++) {
    assert(M_THRESHOLD_TABLE[i] <= M_THRESHOLD_TABLE[i - 1]);
  }

  // Cooldown must be monotonically increasing from index 1 upward.
  for (int i = 2; i < 256; i++) {
    assert(M_COOLDOWN_TABLE[i] >= M_COOLDOWN_TABLE[i - 1]);
  }

  // Delay: the two seams are where a piecewise encoding goes wrong, so they are
  // asserted explicitly rather than left to the monotonicity check.
  assert(M_DELAY_TABLE[0] == 0);       // default - no delay
  assert(M_DELAY_TABLE[59] == 59);     // 59 s
  assert(M_DELAY_TABLE[60] == 60);     // 1 min - contiguous with 59 s
  assert(M_DELAY_TABLE[118] == 3540);  // 59 min
  assert(M_DELAY_TABLE[119] == 3600);  // 1 h - contiguous with 59 min
  assert(M_DELAY_TABLE[127] == 32400); // 9 h, the maximum

  // Strictly increasing: no delay code may mean the same as another.
  for (int i = 1; i < 128; i++) {
    assert(M_DELAY_TABLE[i] > M_DELAY_TABLE[i - 1]);
  }

  // The ADXL367 threshold register is 13-bit. A table entry that overflowed it
  // would be silently truncated by writeThreshold() into a DIFFERENT threshold.
  for (int i = 0; i < 256; i++) {
    assert(M_THRESHOLD_TABLE[i] <= 0x1FFF);
    assert(M_THRESHOLD_TABLE[i] >= 1);
  }

  printf("protocol tables: OK\n");
}

void run_command_codec_tests()
{
  using namespace alc::protocol;
  uint8_t plaintext[M_PLAINTEXT_BYTES] {};
  Command command;
  Command decoded;

  // The on-air layout must fill exactly one 128-bit UUID.
  assert(M_ROTATING_ID_BYTES + M_PLAINTEXT_BYTES + M_TAG_BYTES == M_UUID_BYTES);
  assert(M_OFFSET_CIPHERTEXT == M_ROTATING_ID_BYTES);
  assert(M_OFFSET_TAG == M_OFFSET_CIPHERTEXT + M_PLAINTEXT_BYTES);

  // Round trip, every field away from its default.
  command.type            = CommandType::Settings;
  command.delayCode       = 119; // 1 h
  command.activations     = 16;  // the 4-bit maximum, stored as 15
  command.mode            = Mode::ReportAndTrigger;
  command.cooldownByte    = 128;
  command.sensitivityByte = 143;
  command.minuteOfDay     = 1439; // 23:59, the largest legal minute
  EncodeCommand(command, plaintext);
  assert(DecodeCommand(plaintext, decoded));
  assert(decoded.type == CommandType::Settings);
  assert(decoded.delayCode == 119);
  assert(decoded.activations == 16);
  assert(decoded.mode == Mode::ReportAndTrigger);
  assert(decoded.cooldownByte == 128);
  assert(decoded.sensitivityByte == 143);
  assert(decoded.minuteOfDay == 1439);

  // Byte 0: bit 0 is reserved and encodes as zero; delay code in bits 1-7.
  assert(plaintext[M_PT_ARM_DELAY] == (119 << 1));

  // Byte 1: activations - 1 low nibble, mode bits 4-5, type bits 6-7.
  assert(plaintext[M_PT_ACTIVATIONS_MODE] == (0x0F | (1 << 4) | (1 << 6)));

  // Every type round-trips.
  for (CommandType type : { CommandType::Settings, CommandType::Arm, CommandType::Disarm }) {
    command.type = type;
    EncodeCommand(command, plaintext);
    assert(DecodeCommand(plaintext, decoded) && decoded.type == type);
  }
  command.type = CommandType::Settings;

  // Mode 3 is reserved and refused rather than treated as one of the others.
  EncodeCommand(command, plaintext);
  plaintext[M_PT_ACTIVATIONS_MODE] = static_cast<uint8_t>(plaintext[M_PT_ACTIVATIONS_MODE] | (3 << M_MODE_SHIFT));
  assert(!DecodeCommand(plaintext, decoded));

  // Mode 3 rejects for an Arm command too - the reserved-mode check is not
  // bypassed just because Arm carries no settings.
  command.type = CommandType::Arm;
  EncodeCommand(command, plaintext);
  plaintext[M_PT_ACTIVATIONS_MODE] = static_cast<uint8_t>(plaintext[M_PT_ACTIVATIONS_MODE] | (3 << M_MODE_SHIFT));
  assert(!DecodeCommand(plaintext, decoded));
  command.type = CommandType::Settings;

  // Minute 1440 fits in eleven bits but is not a minute of any day.
  command.mode        = Mode::TriggerOnly;
  command.minuteOfDay = 1439;
  EncodeCommand(command, plaintext);
  plaintext[M_PT_MINUTE]     = static_cast<uint8_t>(1440 & 0xFF);
  plaintext[M_PT_MINUTE + 1] = static_cast<uint8_t>(1440 >> 8);
  assert(!DecodeCommand(plaintext, decoded));

  // PLAINTEXT BYTES 6-7 AND THE RESERVED BITS BELONG TO OTHER VARIANTS. Anything
  // there must still decode - validating them would make MFS_1 reject a future
  // app build the moment another variant starts using that space.
  EncodeCommand(command, plaintext);
  plaintext[6]               = 0xAA;
  plaintext[7]               = 0xBB;
  plaintext[M_PT_ARM_DELAY]  = static_cast<uint8_t>(plaintext[M_PT_ARM_DELAY] | 0x01);
  plaintext[M_PT_MINUTE + 1] = static_cast<uint8_t>(plaintext[M_PT_MINUTE + 1] | 0xF8);
  assert(DecodeCommand(plaintext, decoded));
  assert(decoded.minuteOfDay == 1439);
  assert(decoded.mode == Mode::TriggerOnly);
  assert(decoded.type == CommandType::Settings);

  // AN ALL-ZERO PLAINTEXT IS NOT A COMMAND. Type 00 is reserved and rejects, so
  // zeros can never mean "apply all-zero settings" (amendment section 2.1).
  for (uint8_t& byte : plaintext) {
    byte = 0;
  }
  assert(!DecodeCommand(plaintext, decoded));

  printf("command codec: OK\n");
}
