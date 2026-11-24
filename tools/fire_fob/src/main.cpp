// Fire fob - Thingy:53 demo tool. BENCH / DEMO ONLY (tools/fire_fob/README.md).
//
// Each press of the button sends a FIRE to each MFS in secrets/fire_fob_keys.h, 10 s
// apart, as the engineer slot in that header. Fire and forget: the MFS sends no receipt,
// so the fob does not listen. One GREEN flash per unit once its FIRE is advertised
// ("sent", not "received"), one long RED flash per unit whose FIRE could not be sent.
// BLUE while a run is in progress (presses are ignored). An MFS acts on a FIRE about
// 100 ms after hearing it, with no countdown: a disarmed MFS rehearses (red LED); an
// ARMED MFS FIRES ITS OUTPUTS and cannot be stopped.
//
// Rules carried over from docs/tan-scheme.md: each sequence number is saved to flash
// BEFORE its FIRE is advertised, so a reset never reuses an AES-CCM nonce; every FIRE
// carries the current UTC minute, so the fob's clock (generation time + uptime) must be
// right to within the MFS's 10 min tolerance. A second boot on the same header means
// that clock is lost: the fob then refuses (red triple flash) until regenerated and
// reflashed. Keys expire at 04:00 UTC: after that it refuses too.
#include <cstring>

#include <psa/crypto.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include "fire_fob_keys.h"

LOG_MODULE_REGISTER(fire_fob, LOG_LEVEL_INF);

namespace alc
{

  namespace
  {
    using fob::keys::M_DEVICE_COUNT;
    using fob::keys::M_DEVICES;

    constexpr uint32_t M_EPOCH_UNIX { 1767225600 };
    constexpr uint32_t M_DAY_BOUNDARY_SECS { 4 * 3600 };
    constexpr uint32_t M_SECONDS_PER_DAY { 86400 };
    constexpr uint32_t M_SECONDS_PER_MINUTE { 60 };
    constexpr uint8_t M_PROTOCOL_VERSION { 0x05 };
    constexpr uint8_t M_UUID_BYTES { 16 };
    constexpr uint8_t M_ID_BYTES { 4 };
    constexpr uint8_t M_NONCE_BYTES { 11 };
    constexpr uint8_t M_TAG_BYTES { 4 };
    constexpr uint8_t M_PLAINTEXT_BYTES { 8 };
    constexpr uint8_t M_SEALED_BYTES { M_PLAINTEXT_BYTES + M_TAG_BYTES };

    constexpr int64_t M_STAGGER_MS { 10000 };
    // 30 s per FIRE, as the app: the MFS listens 100 ms every 7.245 s.
    constexpr int64_t M_ADV_WINDOW_MS { 30000 };
    constexpr uint16_t M_ADV_TIMEOUT_10MS { 3000 };
    constexpr uint16_t M_ADV_INTERVAL_UNITS { 80 };
    constexpr int32_t M_POLL_MS { 20 };
    constexpr int32_t M_FLASH_ON_MS { 150 };
    constexpr int32_t M_FLASH_OFF_MS { 150 };
    constexpr int32_t M_LONG_FLASH_MS { 1000 };
    constexpr uint8_t M_TRIPLE { 3 };
    constexpr uint8_t M_SINGLE { 1 };

    const gpio_dt_spec s_red    = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
    const gpio_dt_spec s_green  = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
    const gpio_dt_spec s_blue   = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
    const gpio_dt_spec s_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

    // Persistent state (settings "fob/..."): the key day the counters belong to, the next
    // sequence number per device, and the header generation time already booted once.
    uint16_t s_stored_day { 0 };
    uint32_t s_next[M_DEVICE_COUNT] {};
    uint32_t s_booted_generation { 0 };

    psa_key_id_t s_keys[M_DEVICE_COUNT] {};
    struct bt_le_ext_adv* s_sets[M_DEVICE_COUNT] {};

    int settingsSet(const char* name, size_t length, settings_read_cb readCb, void* cbArg)
    {
      const char* next { nullptr };

      if (settings_name_steq(name, "day", &next) && !next && length == sizeof(s_stored_day)) {
        return readCb(cbArg, &s_stored_day, sizeof(s_stored_day)) < 0 ? -EIO : 0;
      }
      if (settings_name_steq(name, "booted", &next) && !next && length == sizeof(s_booted_generation)) {
        return readCb(cbArg, &s_booted_generation, sizeof(s_booted_generation)) < 0 ? -EIO : 0;
      }
      for (uint8_t index = 0; index < M_DEVICE_COUNT; index++) {
        char key[8] {};
        snprintf(key, sizeof(key), "n%u", index);
        if (settings_name_steq(name, key, &next) && !next && length == sizeof(uint32_t)) {
          return readCb(cbArg, &s_next[index], sizeof(uint32_t)) < 0 ? -EIO : 0;
        }
      }
      return 0;
    }

    SETTINGS_STATIC_HANDLER_DEFINE(fob, "fob", nullptr, settingsSet, nullptr, nullptr);

    int saveNext(uint8_t index)
    {
      char key[16] {};
      snprintf(key, sizeof(key), "fob/n%u", index);
      return settings_save_one(key, &s_next[index], sizeof(uint32_t));
    }

    uint32_t nowUnix()
    { return fob::keys::M_GENERATED_UNIX + static_cast<uint32_t>(k_uptime_get() / MSEC_PER_SEC); }

    bool keysValidNow()
    {
      const uint32_t now { nowUnix() };
      return now >= M_EPOCH_UNIX + M_DAY_BOUNDARY_SECS && (now - M_EPOCH_UNIX - M_DAY_BOUNDARY_SECS) / M_SECONDS_PER_DAY == fob::keys::M_DAY;
    }

    void writeU32Be(uint8_t* out, uint32_t value)
    {
      out[0] = static_cast<uint8_t>(value >> 24);
      out[1] = static_cast<uint8_t>(value >> 16);
      out[2] = static_cast<uint8_t>(value >> 8);
      out[3] = static_cast<uint8_t>(value);
    }

    // FIRE for device `index`, sequence `n`, UTC minute of day `minute`: rotatingId(n) | ciphertext | tag.
    int seal(uint8_t index, uint32_t n, uint16_t minute, uint8_t (&uuid)[M_UUID_BYTES])
    {
      const fob::keys::Device& device { M_DEVICES[index] };
      uint8_t nonce[M_NONCE_BYTES] {};
      uint8_t aad[M_ID_BYTES + 1] {};
      // Fire-command amendment section 2.2: 'O', type 00, 'M', 'G', minute (LE), 0, 0.
      const uint8_t plaintext[M_PLAINTEXT_BYTES] { 0x4F, 0x00, 0x4D, 0x47, static_cast<uint8_t>(minute & 0xFF), static_cast<uint8_t>(minute >> 8),
                                                   0x00, 0x00 };
      size_t sealedLength { 0 };

      writeU32Be(&nonce[0], device.deviceId);
      nonce[4] = static_cast<uint8_t>(fob::keys::M_DAY >> 8);
      nonce[5] = static_cast<uint8_t>(fob::keys::M_DAY);
      nonce[6] = fob::keys::M_SLOT;
      writeU32Be(&nonce[7], n);
      memcpy(aad, device.rotatingIds[n], M_ID_BYTES);
      aad[M_ID_BYTES] = M_PROTOCOL_VERSION;
      memcpy(uuid, device.rotatingIds[n], M_ID_BYTES);

      psa_status_t status { psa_aead_encrypt(s_keys[index], PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, M_TAG_BYTES), nonce, sizeof(nonce), aad,
                                             sizeof(aad), plaintext, sizeof(plaintext), &uuid[M_ID_BYTES], M_SEALED_BYTES, &sealedLength) };
      if (status != PSA_SUCCESS || sealedLength != M_SEALED_BYTES) {
        LOG_ERR("FIRE seal failed for %s: %d!", device.serial, static_cast<int>(status));
        return -EIO;
      }
      return 0;
    }

    void flash(const gpio_dt_spec& led, uint8_t times, int32_t onMs)
    {
      const int blue { gpio_pin_get_dt(&s_blue) };

      gpio_pin_set_dt(&s_blue, 0);
      for (uint8_t count = 0; count < times; count++) {
        gpio_pin_set_dt(&led, 1);
        k_msleep(onMs);
        gpio_pin_set_dt(&led, 0);
        k_msleep(M_FLASH_OFF_MS);
      }
      gpio_pin_set_dt(&s_blue, blue > 0 ? 1 : 0);
    }

    int initKeys()
    {
      psa_status_t status { psa_crypto_init() };
      if (status != PSA_SUCCESS) {
        LOG_ERR("PSA crypto init failed: %d!", static_cast<int>(status));
        return -EIO;
      }
      for (uint8_t index = 0; index < M_DEVICE_COUNT; index++) {
        psa_key_attributes_t attributes = psa_key_attributes_init();
        psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
        psa_set_key_bits(&attributes, 128);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
        psa_set_key_algorithm(&attributes, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, M_TAG_BYTES));
        status = psa_import_key(&attributes, M_DEVICES[index].encKey, sizeof(M_DEVICES[index].encKey), &s_keys[index]);
        if (status != PSA_SUCCESS) {
          LOG_ERR("Key import failed for %s: %d!", M_DEVICES[index].serial, static_cast<int>(status));
          return -EIO;
        }

        // Self-test against the Python reference: FIRE, n 0, minute 0.
        uint8_t test[M_UUID_BYTES] {};
        if (seal(index, 0, 0, test) < 0 || memcmp(test, M_DEVICES[index].testSealed, M_UUID_BYTES) != 0) {
          LOG_ERR("Self-test FAILED for %s - the fob's FIRE would not authenticate!", M_DEVICES[index].serial);
          return -EIO;
        }
      }
      LOG_INF("Crypto self-test passed for all %u devices.", M_DEVICE_COUNT);
      return 0;
    }

    int initState(bool& clockTrusted)
    {
      int result { settings_subsys_init() };
      if (result == 0) { result = settings_load_subtree("fob"); }
      if (result < 0) {
        LOG_ERR("Settings failed: %d!", result);
        return result;
      }

      // Counters belong to one key day: a new day's header starts every device at 0.
      if (s_stored_day != fob::keys::M_DAY) {
        LOG_INF("New key day %u (stored %u): sequence numbers restart at 0.", fob::keys::M_DAY, s_stored_day);
        s_stored_day = fob::keys::M_DAY;
        for (uint8_t index = 0; index < M_DEVICE_COUNT; index++) {
          s_next[index] = 0;
          result        = saveNext(index);
          if (result < 0) { return result; }
        }
        result = settings_save_one("fob/day", &s_stored_day, sizeof(s_stored_day));
        if (result < 0) { return result; }
      }

      // The clock is generation time + uptime: valid on the first boot after a flash only.
      clockTrusted        = s_booted_generation != fob::keys::M_GENERATED_UNIX;
      s_booted_generation = fob::keys::M_GENERATED_UNIX;
      return settings_save_one("fob/booted", &s_booted_generation, sizeof(s_booted_generation));
    }

    int initRadio()
    {
      const struct bt_le_adv_param param BT_LE_ADV_PARAM_INIT(0, M_ADV_INTERVAL_UNITS, M_ADV_INTERVAL_UNITS, nullptr);
      int result { bt_enable(nullptr) };

      if (result < 0) {
        LOG_ERR("Bluetooth enable failed: %d!", result);
        return result;
      }
      for (uint8_t index = 0; index < M_DEVICE_COUNT; index++) {
        result = bt_le_ext_adv_create(&param, nullptr, &s_sets[index]);
        if (result < 0) {
          LOG_ERR("Advertising set %u create failed: %d!", index, result);
          return result;
        }
      }
      return 0;
    }

    int sendFire(uint8_t index)
    {
      static uint8_t s_uuid[M_DEVICE_COUNT][M_UUID_BYTES] {};
      static struct bt_data s_ad[M_DEVICE_COUNT] {};
      const fob::keys::Device& device { M_DEVICES[index] };
      const uint32_t n { s_next[index] };
      const uint32_t now { nowUnix() };
      const uint16_t minute { static_cast<uint16_t>((now % M_SECONDS_PER_DAY) / M_SECONDS_PER_MINUTE) };
      struct bt_le_ext_adv_start_param start {};
      int result { 0 };

      if (n >= fob::keys::M_SEQUENCE_COUNT) {
        LOG_ERR("%s: all %u sequence numbers used today - regenerate with a larger --count tomorrow!", device.serial, n);
        return -ENOSPC;
      }

      // Save n + 1 BEFORE advertising: a reset must never reuse n (it is the CCM nonce).
      s_next[index] = n + 1;
      result        = saveNext(index);
      if (result < 0) {
        LOG_ERR("%s: saving the sequence number failed (%d) - FIRE not sent!", device.serial, result);
        return result;
      }
      result = seal(index, n, minute, s_uuid[index]);
      if (result < 0) { return result; }

      s_ad[index].type     = BT_DATA_UUID128_ALL;
      s_ad[index].data_len = M_UUID_BYTES;
      s_ad[index].data     = s_uuid[index];
      result               = bt_le_ext_adv_set_data(s_sets[index], &s_ad[index], 1, nullptr, 0);
      if (result == 0) {
        start.timeout = M_ADV_TIMEOUT_10MS;
        result        = bt_le_ext_adv_start(s_sets[index], &start);
      }
      if (result < 0) {
        LOG_ERR("%s: FIRE advert failed: %d!", device.serial, result);
        return result;
      }
      LOG_INF("FIRE -> %s (0x%08X) slot %u n %u minute %u.", device.serial, device.deviceId, fob::keys::M_SLOT, n, minute);
      return 0;
    }

    // One press: FIRE to each device, M_STAGGER_MS apart, a green flash as each is sent. The run
    // (and the blue LED) lasts until the last advert has run its 30 s window, so a second press
    // cannot restart an advertising set that is still sending.
    void runSequence()
    {
      const int64_t startMs { k_uptime_get() };
      bool sent[M_DEVICE_COUNT] {};
      bool done[M_DEVICE_COUNT] {};
      uint8_t remaining { M_DEVICE_COUNT };

      gpio_pin_set_dt(&s_blue, 1);
      LOG_INF("Button: FIRE sequence started.");
      while (remaining > 0) {
        const int64_t nowMs { k_uptime_get() };

        for (uint8_t index = 0; index < M_DEVICE_COUNT; index++) {
          const int64_t dueMs { startMs + index * M_STAGGER_MS };

          if (!sent[index] && nowMs >= dueMs) {
            sent[index] = true;
            if (sendFire(index) < 0) {
              done[index] = true;
              remaining--;
              flash(s_red, M_SINGLE, M_LONG_FLASH_MS);
            } else {
              flash(s_green, M_SINGLE, M_FLASH_ON_MS);
            }
          } else if (sent[index] && !done[index] && nowMs >= dueMs + M_ADV_WINDOW_MS) {
            done[index] = true;
            remaining--;
          }
        }
        k_msleep(M_POLL_MS);
      }
      gpio_pin_set_dt(&s_blue, 0);
      LOG_INF("FIRE sequence finished.");
    }
  }

  int run()
  {
    bool clockTrusted { false };
    bool ready { false };
    int lastButton { 0 };

    gpio_pin_configure_dt(&s_red, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&s_green, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&s_blue, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&s_button, GPIO_INPUT);

    LOG_INF("Fire fob: slot %u, key day %u, %u devices, generated unix %u.", fob::keys::M_SLOT, fob::keys::M_DAY, M_DEVICE_COUNT,
            fob::keys::M_GENERATED_UNIX);
    ready = initKeys() == 0 && initState(clockTrusted) == 0 && initRadio() == 0;
    if (ready && !clockTrusted) {
      LOG_ERR("Second boot on this key header - the clock is lost. Regenerate and reflash (README.md)!");
      ready = false;
    }
    if (ready) {
      for (uint8_t index = 0; index < M_DEVICE_COUNT; index++) {
        LOG_INF("%s next n %u.", M_DEVICES[index].serial, s_next[index]);
      }
      LOG_INF("Ready. Press the button to FIRE.");
      flash(s_green, 1, M_LONG_FLASH_MS);
    } else {
      flash(s_red, M_TRIPLE, M_FLASH_ON_MS);
    }

    while (true) {
      const int button { gpio_pin_get_dt(&s_button) };

      if (button > 0 && lastButton == 0) {
        if (!ready || !keysValidNow()) {
          LOG_WRN("Refused: %s.", ready ? "keys expired at 04:00 UTC - regenerate and reflash" : "fob not ready (see boot log)");
          flash(s_red, M_TRIPLE, M_FLASH_ON_MS);
        } else {
          runSequence();
        }
      }
      lastButton = button;
      k_msleep(M_POLL_MS);
    }
    return 0;
  }

}

int main()
{ return alc::run(); }
