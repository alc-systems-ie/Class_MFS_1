#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(tamper_test, LOG_LEVEL_INF);

namespace alc {

namespace {

#define M_USER_NODE DT_PATH(zephyr_user)

const gpio_dt_spec s_tamper = GPIO_DT_SPEC_GET(M_USER_NODE, tamper_gpios);
const gpio_dt_spec s_led3 = GPIO_DT_SPEC_GET(M_USER_NODE, led3_gpios);
const gpio_dt_spec s_fire1 = GPIO_DT_SPEC_GET(M_USER_NODE, fire1_gpios);
const gpio_dt_spec s_fire2 = GPIO_DT_SPEC_GET(M_USER_NODE, fire2_gpios);
const i2c_dt_spec s_pmic = I2C_DT_SPEC_GET(DT_NODELABEL(npm2100));

// nPM2100 TIMER.TASKS_STOP (datasheet section 7.2.6) - the boot monitor is the TIMER block.
constexpr uint8_t M_REG_TIMER_TASKS_STOP { 0xB1 };
constexpr uint8_t M_TASK_TRIGGER { 0x01 };

// Poll period doubles as the debounce: a level must hold for M_DEBOUNCE_SAMPLES polls to count.
constexpr int32_t M_POLL_INTERVAL_MS { 10 };
constexpr uint8_t M_DEBOUNCE_SAMPLES { 3 };

// Firing order: FIRE_1, then FIRE_2 this long after.
constexpr int32_t M_FIRE_STAGGER_MS { 10 };

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

// Drives FIRE_1, then FIRE_2 M_FIRE_STAGGER_MS later. Pins are configured as outputs here, never before.
int assertFire()
{
  int result { gpio_pin_configure_dt(&s_fire1, GPIO_OUTPUT_ACTIVE) };
  if (result < 0) {
    LOG_ERR("Failed to assert FIRE_1: %d!", result);
    return result;
  }

  k_msleep(M_FIRE_STAGGER_MS);

  result = gpio_pin_configure_dt(&s_fire2, GPIO_OUTPUT_ACTIVE);
  if (result < 0) {
    LOG_ERR("Failed to assert FIRE_2: %d!", result);
    return result;
  }

  return 0;
}

// Drives both fire pins low, then leaves them undriven so the external pull-downs hold them low.
int releaseFire()
{
  int returnCode { 0 };
  const gpio_dt_spec* firePins[] { &s_fire1, &s_fire2 };

  for (const gpio_dt_spec* pin : firePins) {
    int result { gpio_pin_configure_dt(pin, GPIO_OUTPUT_INACTIVE) };
    if (result == 0) { result = gpio_pin_configure_dt(pin, GPIO_DISCONNECTED); }

    if (result < 0) {
      LOG_ERR("Failed to release fire pin P2.%02u: %d!", pin->pin, result);
      returnCode = result;
    }
  }

  return returnCode;
}

int configurePins()
{
  if (!gpio_is_ready_dt(&s_tamper) || !gpio_is_ready_dt(&s_led3) || !gpio_is_ready_dt(&s_fire1) || !gpio_is_ready_dt(&s_fire2)) {
    LOG_ERR("GPIO port not ready!");
    return -ENODEV;
  }

  int result { gpio_pin_configure_dt(&s_tamper, GPIO_INPUT) };
  if (result < 0) {
    LOG_ERR("Failed to configure ANTI_TAMP: %d!", result);
    return result;
  }

  result = gpio_pin_configure_dt(&s_led3, GPIO_OUTPUT_INACTIVE);
  if (result < 0) {
    LOG_ERR("Failed to configure LED3: %d!", result);
    return result;
  }

  return releaseFire();
}

int run()
{
  bool pressed { false };
  uint8_t stableCount { 0 };
  uint32_t pressCount { 0 };

  LOG_INF("Anti-tamper test boot.");
  logResetCause();

  // Carry on even if this fails: the button test is still useful, and the log will show the resets.
  stopBootMonitor();

  int result { configurePins() };
  if (result < 0) { return result; }

  LOG_INF("Anti-tamper test started: ANTI_TAMP P1.02 (active low) -> LED3 P2.04 + FIRE_1 P2.05, FIRE_2 P2.09 (+10 ms). Initial raw level %d.", gpio_pin_get_raw(s_tamper.port, s_tamper.pin));

  while (true) {
    result = gpio_pin_get_dt(&s_tamper);
    if (result < 0) {
      LOG_ERR("Failed to read ANTI_TAMP: %d!", result);
      return result;
    }

    bool level { result == 1 };
    if (level == pressed) {
      stableCount = 0;
    } else if (++stableCount >= M_DEBOUNCE_SAMPLES) {
      pressed = level;
      stableCount = 0;

      if (pressed) {
        pressCount++;
        gpio_pin_set_dt(&s_led3, 1);
        result = assertFire();
        LOG_INF("ANTI_TAMP pressed (count %u) - LED3 on, FIRE_1 then FIRE_2 on (result %d).", pressCount, result);
      } else {
        result = releaseFire();
        gpio_pin_set_dt(&s_led3, 0);
        LOG_INF("ANTI_TAMP released - FIRE_1 and FIRE_2 off, LED3 off (result %d).", result);
      }
    }

    k_msleep(M_POLL_INTERVAL_MS);
  }

  return 0;
}

} // namespace

} // namespace alc

int main()
{
  return alc::run();
}
