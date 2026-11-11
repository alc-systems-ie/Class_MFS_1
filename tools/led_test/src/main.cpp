#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(led_test, LOG_LEVEL_INF);

namespace alc {

namespace {

struct TestPin {
  gpio_dt_spec spec;
  const char* name;
};

#define M_USER_NODE DT_PATH(zephyr_user)

// Walk order: the three LEDs, then FIRE_1 and FIRE_2. Only one pin is ever asserted at a time.
const TestPin s_pins[] {
  { GPIO_DT_SPEC_GET_BY_IDX(M_USER_NODE, led_gpios, 0), "LED1   (P2.02)" },
  { GPIO_DT_SPEC_GET_BY_IDX(M_USER_NODE, led_gpios, 1), "LED2   (P2.03)" },
  { GPIO_DT_SPEC_GET_BY_IDX(M_USER_NODE, led_gpios, 2), "LED3   (P2.04)" },
  { GPIO_DT_SPEC_GET_BY_IDX(M_USER_NODE, fire_gpios, 0), "FIRE_1 (P2.05)" },
  { GPIO_DT_SPEC_GET_BY_IDX(M_USER_NODE, fire_gpios, 1), "FIRE_2 (P2.09)" },
};

const i2c_dt_spec s_pmic = I2C_DT_SPEC_GET(DT_NODELABEL(npm2100));

// nPM2100 TIMER.TASKS_STOP (datasheet section 7.2.6) - the boot monitor is the TIMER block.
constexpr uint8_t M_REG_TIMER_TASKS_STOP { 0xB1 };
constexpr uint8_t M_TASK_TRIGGER { 0x01 };

constexpr int32_t M_ON_TIME_MS { 1000 };
constexpr int32_t M_PASS_GAP_MS { 1000 };

// Stops the nPM2100 boot monitor. Without this the PMIC power-cycles the SoC a few seconds after every boot.
int stopBootMonitor()
{
  if (!i2c_is_ready_dt(&s_pmic)) {
    LOG_ERR("nPM2100 I2C bus not ready!");
    return -ENODEV;
  }

  int result { i2c_reg_write_byte_dt(&s_pmic, M_REG_TIMER_TASKS_STOP, M_TASK_TRIGGER) };
  if (result < 0) {
    LOG_ERR("Failed to stop nPM2100 boot monitor: %d!", result);
    return result;
  }

  LOG_INF("nPM2100 boot monitor stopped.");
  return 0;
}

void logResetCause()
{
  uint32_t cause { 0 };

  int result { hwinfo_get_reset_cause(&cause) };
  if (result < 0) {
    LOG_WRN("Reset cause unavailable: %d", result);
    return;
  }

  LOG_INF("Reset cause 0x%08x%s%s%s%s%s", cause, (cause & RESET_PIN) ? " PIN" : "", (cause & RESET_SOFTWARE) ? " SOFTWARE" : "",
          (cause & RESET_POR) ? " POWER-ON" : "", (cause & RESET_WATCHDOG) ? " WATCHDOG" : "", (cause & RESET_DEBUG) ? " DEBUG" : "");
  hwinfo_clear_reset_cause();
}

int configurePins()
{
  for (const TestPin& pin : s_pins) {
    if (!gpio_is_ready_dt(&pin.spec)) {
      LOG_ERR("GPIO port for %s not ready!", pin.name);
      return -ENODEV;
    }

    int result { gpio_pin_configure_dt(&pin.spec, GPIO_OUTPUT_INACTIVE) };
    if (result < 0) {
      LOG_ERR("Failed to configure %s: %d!", pin.name, result);
      return result;
    }
  }

  return 0;
}

int run()
{
  uint32_t pass { 0 };

  LOG_INF("LED test boot.");
  logResetCause();

  // Carry on even if this fails: the LED walk is still useful, and the log will show the resets.
  stopBootMonitor();

  int result { configurePins() };
  if (result < 0) { return result; }

  LOG_INF("LED test started: %u pins, %d ms each.", static_cast<unsigned>(ARRAY_SIZE(s_pins)), M_ON_TIME_MS);

  while (true) {
    LOG_INF("--- Pass %u ---", pass);

    for (const TestPin& pin : s_pins) {
      result = gpio_pin_set_dt(&pin.spec, 1);
      LOG_INF("%s ON  (set result %d)", pin.name, result);
      k_msleep(M_ON_TIME_MS);

      result = gpio_pin_set_dt(&pin.spec, 0);
      LOG_INF("%s OFF (set result %d)", pin.name, result);
    }

    pass++;
    k_msleep(M_PASS_GAP_MS);
  }

  return 0;
}

} // namespace

} // namespace alc

int main()
{
  return alc::run();
}
