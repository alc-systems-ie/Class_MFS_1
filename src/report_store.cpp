#include <cerrno>
#include <cstdint>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "report_store.hpp"

LOG_MODULE_REGISTER(report_store, LOG_LEVEL_INF);

namespace alc::report_store
{

  namespace
  {
    // Record: generation (big-endian 16) then reservedUpTo (big-endian 32). The layout is
    // versioned by the key name, so a changed layout is a new key.
    constexpr const char* M_KEY { "report/v1" };
    constexpr size_t M_GENERATION_BYTES { 2 };
    constexpr size_t M_RESERVED_BYTES { 4 };
    constexpr size_t M_RECORD_BYTES { M_GENERATION_BYTES + M_RESERVED_BYTES };
    constexpr uint8_t M_BYTE_BITS { 8 };
    constexpr uint8_t M_BYTE_MASK { 0xFF };

    uint8_t s_loaded[M_RECORD_BYTES] {};
    bool s_record_loaded { false };
    bool s_record_invalid { false };

    int settingsSet(const char* key, size_t length, settings_read_cb readCallback, void* callbackArgument)
    {
      const char* next { nullptr };
      ssize_t readLength { 0 };

      if (!settings_name_steq(key, "v1", &next) || next != nullptr) { return -ENOENT; }
      if (length != sizeof(s_loaded)) {
        s_record_invalid = true;
        LOG_ERR("Stored report counter is %u bytes, expected %u - ignoring it!", static_cast<unsigned>(length),
                static_cast<unsigned>(sizeof(s_loaded)));
        return -EINVAL;
      }

      readLength = readCallback(callbackArgument, s_loaded, sizeof(s_loaded));
      if (readLength != static_cast<ssize_t>(sizeof(s_loaded))) {
        s_record_invalid = true;
        return -EIO;
      }

      s_record_loaded = true;
      return 0;
    }

    SETTINGS_STATIC_HANDLER_DEFINE(mfs_report, "report", nullptr, settingsSet, nullptr, nullptr);
  }

  int Load(uint16_t generation, uint32_t& floor)
  {
    int result { 0 };
    uint16_t storedGeneration { 0 };
    uint32_t storedFloor { 0 };

    floor            = 0;
    s_record_loaded  = false;
    s_record_invalid = false;
    result           = settings_load_subtree("report");

    if (result < 0) {
      LOG_ERR("Failed to load the report counter: %d!", result);
      return result;
    }
    // A damaged record must not read as "no record": a floor of 0 would reuse nonces.
    if (s_record_invalid) {
      LOG_ERR("Stored report counter is invalid - reports disabled this boot!");
      return -EINVAL;
    }
    if (!s_record_loaded) { return 0; }

    storedGeneration = static_cast<uint16_t>((s_loaded[0] << M_BYTE_BITS) | s_loaded[1]);
    storedFloor      = (static_cast<uint32_t>(s_loaded[2]) << (3 * M_BYTE_BITS)) | (static_cast<uint32_t>(s_loaded[3]) << (2 * M_BYTE_BITS)) |
                       (static_cast<uint32_t>(s_loaded[4]) << M_BYTE_BITS) | static_cast<uint32_t>(s_loaded[5]);

    // A new generation derives new keys, so the counter restarts at zero under them.
    if (storedGeneration == generation) { floor = storedFloor; }
    return 0;
  }

  int Persist(uint32_t reservedUpTo, void* context)
  {
    const uint16_t generation { *static_cast<const uint16_t*>(context) };
    uint8_t record[M_RECORD_BYTES] {};
    int result { 0 };

    record[0] = static_cast<uint8_t>((generation >> M_BYTE_BITS) & M_BYTE_MASK);
    record[1] = static_cast<uint8_t>(generation & M_BYTE_MASK);
    record[2] = static_cast<uint8_t>((reservedUpTo >> (3 * M_BYTE_BITS)) & M_BYTE_MASK);
    record[3] = static_cast<uint8_t>((reservedUpTo >> (2 * M_BYTE_BITS)) & M_BYTE_MASK);
    record[4] = static_cast<uint8_t>((reservedUpTo >> M_BYTE_BITS) & M_BYTE_MASK);
    record[5] = static_cast<uint8_t>(reservedUpTo & M_BYTE_MASK);

    result = settings_save_one(M_KEY, record, sizeof(record));
    if (result < 0) { LOG_ERR("Failed to persist the report counter: %d!", result); }
    return result;
  }

}
