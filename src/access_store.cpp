#include <cerrno>
#include <cstring>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "access_store.hpp"

LOG_MODULE_REGISTER(access_store, LOG_LEVEL_INF);

namespace alc::access_store
{

  namespace
  {
    // Fixed-size record, little-endian as the nRF54L05 stores it. The layout is
    // versioned by the key name, so a changed layout is a new key, never a
    // reinterpretation of old bytes.
    constexpr const char* M_KEY { "access/v1" };

    AccessState s_loaded {};
    bool s_record_loaded { false };

    // A record that exists but failed to load (wrong length, short read) must
    // NOT look like "no record" - App would then treat this boot as a first
    // boot, zero next[] and re-open today's already-spent sequence numbers.
    bool s_record_invalid { false };

    int settingsSet(const char* key, size_t length, settings_read_cb readCallback, void* callbackArgument)
    {
      const char* next { nullptr };
      ssize_t readLength { 0 };

      if (!settings_name_steq(key, "v1", &next) || next != nullptr) { return -ENOENT; }
      if (length != sizeof(s_loaded)) {
        s_record_invalid = true;
        LOG_ERR("Stored access state is %u bytes, expected %u - ignoring it!", static_cast<unsigned>(length),
                static_cast<unsigned>(sizeof(s_loaded)));
        return -EINVAL;
      }

      readLength = readCallback(callbackArgument, &s_loaded, sizeof(s_loaded));
      if (readLength != static_cast<ssize_t>(sizeof(s_loaded))) {
        s_record_invalid = true;
        return -EIO;
      }

      s_record_loaded = true;
      return 0;
    }

    SETTINGS_STATIC_HANDLER_DEFINE(mfs_access, "access", nullptr, settingsSet, nullptr, nullptr);
  }

  int Load(AccessState& state)
  {
    s_record_loaded  = false;
    s_record_invalid = false;

    int result { settings_load_subtree("access") };

    if (result < 0) {
      LOG_ERR("Failed to load the access state: %d!", result);
      return result;
    }
    if (s_record_invalid) {
      LOG_ERR("Stored access state is invalid - refusing commands this boot!");
      return -EINVAL;
    }
    if (!s_record_loaded) { return 0; }

    state = s_loaded;
    return 1;
  }

  int Persist(const AccessState& state, void* context)
  {
    int result { settings_save_one(M_KEY, &state, sizeof(state)) };

    ARG_UNUSED(context);

    if (result < 0) { LOG_ERR("Failed to persist the access state: %d!", result); }
    return result;
  }

}
