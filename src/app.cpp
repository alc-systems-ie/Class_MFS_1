#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "app.hpp"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

namespace alc
{

namespace
{

constexpr uint32_t M_IDLE_INTERVAL_MS { 1000 };

// These use `=` rather than brace initialisation: GPIO_DT_SPEC_GET already
// expands to a braced initialiser list, and wrapping it in further braces makes
// the compiler try to initialise the first member from the whole list.

// LED A — on while the device is Inactive. LED B — on while Active and triggered.
const struct gpio_dt_spec s_led_a = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
const struct gpio_dt_spec s_led_b = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

// nRF21540 control pins. Fitted on the bespoke board, unused by MFS_1.
const struct gpio_dt_spec s_fem_pdn   = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), fem_pdn_gpios);
const struct gpio_dt_spec s_fem_tx_en = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), fem_tx_en_gpios);
const struct gpio_dt_spec s_fem_rx_en = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), fem_rx_en_gpios);
const struct gpio_dt_spec s_fem_mode  = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), fem_mode_gpios);

const struct gpio_dt_spec* const s_fem_pins[] { &s_fem_pdn, &s_fem_tx_en, &s_fem_rx_en, &s_fem_mode };

}

App::App()
    : m_initialised(false)
{
}

int App::Run()
{
  int result { 0 };

  LOG_INF("MFS_1 starting, serial %s.", CONFIG_ALC_DEVICE_SERIAL);

  result = parkFrontEndModule();
  if (result < 0) {
    LOG_ERR("Failed to park the front-end module: %d!", result);
    return result;
  }

  result = initLeds();
  if (result < 0) {
    LOG_ERR("Failed to initialise the LEDs: %d!", result);
    return result;
  }

  m_initialised = true;
  LOG_INF("Scan period %d ms, window %d ms.", CONFIG_MFS_SCAN_PERIOD_MS, CONFIG_MFS_SCAN_WINDOW_MS);

  // Placeholder for the scan loop, TAN validation, arm state and ADXL367
  // handling. See docs/v1-scope.md section 1.
  while (true) {
    k_msleep(M_IDLE_INTERVAL_MS);
  }

  return 0;
}

int App::parkFrontEndModule()
{
  int result { 0 };

  for (const struct gpio_dt_spec* pin : s_fem_pins) {
    if (!gpio_is_ready_dt(pin)) {
      LOG_ERR("Front-end module GPIO not ready!");
      return -ENODEV;
    }

    result = gpio_pin_configure_dt(pin, GPIO_OUTPUT_INACTIVE);
    if (result < 0) {
      return result;
    }
  }

  LOG_INF("Front-end module parked in power-down.");
  return 0;
}

int App::initLeds()
{
  int result { 0 };

  if (!gpio_is_ready_dt(&s_led_a) || !gpio_is_ready_dt(&s_led_b)) {
    LOG_ERR("LED GPIO not ready!");
    return -ENODEV;
  }

  result = gpio_pin_configure_dt(&s_led_a, GPIO_OUTPUT_INACTIVE);
  if (result < 0) {
    return result;
  }

  return gpio_pin_configure_dt(&s_led_b, GPIO_OUTPUT_INACTIVE);
}

}
