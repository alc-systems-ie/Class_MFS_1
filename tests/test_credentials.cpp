#include <cassert>
#include <cstdio>

#include "credentials.hpp"

void run_credentials_tests()
{
  using namespace alc::credentials;

  uint8_t bytes[4] {};
  uint32_t deviceId { 0 };

  // Mixed case is accepted.
  assert(ParseHexBytes("DEADbeef", bytes, sizeof(bytes)));
  assert(bytes[0] == 0xDE && bytes[1] == 0xAD && bytes[2] == 0xBE && bytes[3] == 0xEF);

  // STRICT LENGTH. A short secret must fail at boot, not become a key padded
  // with zeros that every such build shares.
  assert(!ParseHexBytes("DEADBE", bytes, sizeof(bytes)));
  assert(!ParseHexBytes("DEADBEEF00", bytes, sizeof(bytes)));
  assert(!ParseHexBytes("", bytes, sizeof(bytes)));

  // Non-hex anywhere fails, including the last digit.
  assert(!ParseHexBytes("DEADBEEG", bytes, sizeof(bytes)));
  assert(!ParseHexBytes("DE ADBEEF", bytes, sizeof(bytes)));
  assert(!ParseHexBytes(nullptr, bytes, sizeof(bytes)));

  // The device ID is big-endian as written: "4D465331" is 0x4D465331, 'MFS1'.
  assert(ParseDeviceId("4D465331", deviceId));
  assert(deviceId == 0x4D465331);
  assert(!ParseDeviceId("4D46533", deviceId));

  const uint8_t zeros[3] {};
  const uint8_t notZero[3] { 0, 0, 1 };
  assert(IsAllZero(zeros, sizeof(zeros)));
  assert(!IsAllZero(notZero, sizeof(notZero)));

  printf("credentials: OK\n");
}
