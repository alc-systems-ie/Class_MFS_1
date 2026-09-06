// Bench advertiser for the MFS_1 engineer toggle.
//
// Press the button: advertise the toggle payload for M_ADVERTISE_MS, then stop.
// The burst must be LONGER THAN ONE FULL MFS_1 SCAN PERIOD or it will almost
// never be seen — MFS_1 listens for only 100 ms in every 6000 ms, so a short
// burst overlaps a scan window about 1.7% of the time. See README.md.
//
// Board-agnostic by construction: the button and the LED come from the sw0 and
// led0 aliases, so the tool builds unchanged for any board that defines them.
// Which physical button and which colour that gives is a per-board question and
// is answered in boards/<board>.overlay - see the Thingy:53 one, where led0 is
// remapped because the board's only LED is an RGB part and led0 would be red.

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(toggle_dongle, LOG_LEVEL_INF);

namespace
{

  // Must exceed MFS_1's CONFIG_MFS_SCAN_PERIOD_MS (6000) so at least one scan
  // window lands inside the burst. MFS_1's own command cooldown must in turn
  // exceed this, so one press cannot be caught by two windows.
  constexpr uint32_t M_ADVERTISE_MS { 8000 };
  constexpr uint32_t M_BUTTON_POLL_MS { 25 };
  constexpr uint32_t M_BUTTON_DEBOUNCE_MS { 50 };

  // Payload must match alc::CommandScanner in ../../src/command_scanner.cpp.
  constexpr uint8_t M_COMMAND_TOGGLE_ARM { 0x01 };

  // clang-format off
  // One row per protocol field, laid out to match the payload table in README.md
  // and the CommandPayload struct it must agree with. clang-format would put each
  // byte on its own line and lose that correspondence.
  const uint8_t s_manufacturer_data[] {
    0xFF, 0xFF,           // Company ID 0xFFFF (reserved for test), little-endian.
    'M',  'F',            // Magic.
    0x01,                 // Protocol version.
    M_COMMAND_TOGGLE_ARM, // Command.
  };
  // clang-format on

  const struct bt_data s_advertising_data[] {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_MANUFACTURER_DATA, s_manufacturer_data, sizeof(s_manufacturer_data)),
  };

  // Taken from the aliases, never from a raw pin. All three boards in use declare
  // their button GPIO_ACTIVE_LOW with a pull-up, so gpio_pin_get_dt() reads 1 when
  // it is pressed and gpio_pin_configure_dt(GPIO_INPUT) keeps those devicetree
  // flags. The Dongle's LED is GPIO_ACTIVE_LOW too, which the _dt calls absorb.
  const struct gpio_dt_spec s_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
  const struct gpio_dt_spec s_led    = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

  int sendToggleBurst()
  {
    int result { bt_le_adv_start(BT_LE_ADV_NCONN, s_advertising_data, ARRAY_SIZE(s_advertising_data), nullptr, 0) };

    if (result < 0) {
      LOG_ERR("bt_le_adv_start failed: %d!", result);
      return result;
    }

    gpio_pin_set_dt(&s_led, 1);
    LOG_INF("Advertising toggle for %u ms.", M_ADVERTISE_MS);
    k_msleep(M_ADVERTISE_MS);

    gpio_pin_set_dt(&s_led, 0);
    result = bt_le_adv_stop();
    if (result < 0) {
      LOG_ERR("bt_le_adv_stop failed: %d!", result);
      return result;
    }

    LOG_INF("Burst complete.");
    return 0;
  }

} // namespace

int main()
{
  int result { 0 };

  if (!gpio_is_ready_dt(&s_button) || !gpio_is_ready_dt(&s_led)) {
    LOG_ERR("Button or LED GPIO not ready!");
    return -ENODEV;
  }

  result = gpio_pin_configure_dt(&s_button, GPIO_INPUT);
  if (result == 0) { result = gpio_pin_configure_dt(&s_led, GPIO_OUTPUT_INACTIVE); }
  if (result < 0) {
    LOG_ERR("GPIO configure failed: %d!", result);
    return result;
  }

  result = bt_enable(nullptr);
  if (result < 0) {
    LOG_ERR("bt_enable failed: %d!", result);
    return result;
  }

  LOG_INF("MFS_1 toggle tool ready. Press the button to send a toggle.");

  while (true) {
    if (gpio_pin_get_dt(&s_button) > 0) {
      k_msleep(M_BUTTON_DEBOUNCE_MS);
      if (gpio_pin_get_dt(&s_button) > 0) {
        sendToggleBurst();

        // Wait for release so a held button sends one burst, not a stream.
        while (gpio_pin_get_dt(&s_button) > 0) {
          k_msleep(M_BUTTON_POLL_MS);
        }
      }
    }
    k_msleep(M_BUTTON_POLL_MS);
  }

  return 0;
}
