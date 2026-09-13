#include "settings.hpp"

#if defined(__ZEPHYR__)
#include <cstring>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(settings_store, LOG_LEVEL_INF);
#endif

namespace alc
{

  namespace
  {
    // Byte 143 is ~75 mg, the threshold the firmware used before this feature.
    // A device that has never been configured therefore behaves as it always did.
    constexpr uint8_t M_DEFAULT_ACTIVATIONS { 1 };
    constexpr uint8_t M_DEFAULT_COOLDOWN_BYTE { 0 };
    constexpr uint8_t M_DEFAULT_SENSITIVITY_BYTE { 143 };

    // No delay and Trigger-only: the behaviour the device had before either
    // setting existed. A device that has never been configured must not
    // suddenly acquire a delay or start advertising.
    constexpr uint8_t M_DEFAULT_DELAY_CODE { 0 };
    constexpr protocol::Mode M_DEFAULT_MODE { protocol::Mode::TriggerOnly };
  }

  Settings::Settings()
      : m_activations(M_DEFAULT_ACTIVATIONS)
      , m_cooldown_byte(M_DEFAULT_COOLDOWN_BYTE)
      , m_sensitivity_byte(M_DEFAULT_SENSITIVITY_BYTE)
      , m_delay_code(M_DEFAULT_DELAY_CODE)
      , m_mode(M_DEFAULT_MODE)
  {}

  bool Settings::ApplyFrom(const protocol::Command& command, bool allowModeChange)
  {
    protocol::Mode mode { allowModeChange ? command.mode : m_mode };

    if (command.activations == m_activations && command.cooldownByte == m_cooldown_byte && command.sensitivityByte == m_sensitivity_byte &&
        command.delayCode == m_delay_code && mode == m_mode) {
      return false;
    }

    m_activations      = command.activations;
    m_cooldown_byte    = command.cooldownByte;
    m_sensitivity_byte = command.sensitivityByte;
    m_delay_code       = command.delayCode;
    m_mode             = mode;
    save();
    return true;
  }

#if defined(__ZEPHYR__)

  namespace
  {
    // activations, cooldown, sensitivity, delay, mode.
    constexpr uint8_t M_RECORD_BYTES { 5 };

    // The static handler has no instance, so loaded bytes land here and Load()
    // copies them into the object.
    uint8_t s_loaded_record[M_RECORD_BYTES] {};
    bool s_record_loaded { false };

    int settingsSet(const char* key, size_t length, settings_read_cb readCallback, void* callbackArgument)
    {
      const char* next { nullptr };
      ssize_t readLength { 0 };

      if (!settings_name_steq(key, "v1", &next) || next != nullptr) { return -ENOENT; }
      if (length != sizeof(s_loaded_record)) { return -EINVAL; }

      readLength = readCallback(callbackArgument, s_loaded_record, sizeof(s_loaded_record));
      if (readLength != static_cast<ssize_t>(sizeof(s_loaded_record))) { return -EIO; }

      s_record_loaded = true;
      return 0;
    }

    SETTINGS_STATIC_HANDLER_DEFINE(mfs_params, "params", nullptr, settingsSet, nullptr, nullptr);
  }

  int Settings::Load()
  {
    int result { settings_load_subtree("params") };

    if (result < 0) {
      LOG_ERR("Failed to load settings: %d!", result);
      return result;
    }
    if (!s_record_loaded) { return 0; }

    // A stored record is validated like a payload. A corrupt activation count
    // would otherwise become a device that silently never triggers.
    if (s_loaded_record[0] < protocol::M_ACTIVATIONS_MIN || s_loaded_record[0] > protocol::M_ACTIVATIONS_MAX ||
        s_loaded_record[4] >= static_cast<uint8_t>(protocol::Mode::Reserved)) {
      LOG_ERR("Stored settings are invalid - keeping defaults!");
      return -EINVAL;
    }

    m_activations      = s_loaded_record[0];
    m_cooldown_byte    = s_loaded_record[1];
    m_sensitivity_byte = s_loaded_record[2];
    m_delay_code       = s_loaded_record[3] & protocol::M_DELAY_MASK;
    m_mode             = static_cast<protocol::Mode>(s_loaded_record[4]);
    return 0;
  }

  int Settings::save()
  {
    const uint8_t record[M_RECORD_BYTES] { m_activations, m_cooldown_byte, m_sensitivity_byte, m_delay_code, static_cast<uint8_t>(m_mode) };
    int result { settings_save_one("params/v1", record, sizeof(record)) };

    if (result < 0) { LOG_ERR("Failed to persist settings: %d!", result); }
    return result;
  }

#else

  // Host build: the conversion and change-detection logic is what the tests
  // exercise; persistence is a Zephyr concern.
  int Settings::Load()
  {
    return 0;
  }
  int Settings::save()
  {
    return 0;
  }

#endif

}
