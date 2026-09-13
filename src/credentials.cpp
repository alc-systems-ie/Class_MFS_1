#include "credentials.hpp"

namespace alc::credentials
{

  namespace
  {
    constexpr uint8_t M_DEVICE_ID_BYTES { 4 };

    int hexValue(char digit)
    {
      if (digit >= '0' && digit <= '9') { return digit - '0'; }
      if (digit >= 'a' && digit <= 'f') { return digit - 'a' + 10; }
      if (digit >= 'A' && digit <= 'F') { return digit - 'A' + 10; }
      return -1;
    }
  }

  bool ParseHexBytes(const char* hex, uint8_t* out, size_t length)
  {
    if (hex == nullptr || out == nullptr) { return false; }

    for (size_t index = 0; index < length; index++) {
      int high { hexValue(hex[2 * index]) };
      int low { high < 0 ? -1 : hexValue(hex[2 * index + 1]) };

      if (high < 0 || low < 0) { return false; }
      out[index] = static_cast<uint8_t>((high << 4) | low);
    }

    // Exactly the right length: a longer string is a mistake, not a bonus.
    return hex[2 * length] == '\0';
  }

  bool ParseDeviceId(const char* hex, uint32_t& deviceId)
  {
    uint8_t bytes[M_DEVICE_ID_BYTES] {};

    if (!ParseHexBytes(hex, bytes, sizeof(bytes))) { return false; }
    deviceId = (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) | (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
    return true;
  }

  bool IsAllZero(const uint8_t* bytes, size_t length)
  {
    uint8_t accumulated { 0 };

    for (size_t index = 0; index < length; index++) {
      accumulated = static_cast<uint8_t>(accumulated | bytes[index]);
    }
    return accumulated == 0;
  }

}
