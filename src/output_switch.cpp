#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "output_switch.hpp"

LOG_MODULE_REGISTER(output_switch, LOG_LEVEL_INF);

namespace alc
{

  namespace
  {

    // THE FIRE PINS LIVE HERE AND NOWHERE ELSE.
    //
    // File-scope static, so no other translation unit can reach them. The only
    // way to move these lines is OutputSwitch::Set(). Do not add an accessor, do
    // not pass these out, and do not declare a second handle to the same pins
    // elsewhere - in the product this output switches a voltage.
    //
    // `=` rather than brace initialisation: GPIO_DT_SPEC_GET already expands to a
    // braced initialiser list, and wrapping it in further braces makes the
    // compiler try to initialise the first member from the whole list.
    const struct gpio_dt_spec s_fire1 = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), fire1_gpios);
    const struct gpio_dt_spec s_fire2 = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), fire2_gpios);

  }

  OutputSwitch::OutputSwitch()
      : m_initialised(false)
      , m_faulted(false)
      , m_asserted(false)
      , m_readback_supported(false)
  {}

  int OutputSwitch::Init()
  {
    int result { 0 };

    if (!gpio_is_ready_dt(&s_fire1) || !gpio_is_ready_dt(&s_fire2)) {
      enterFaultState("fire GPIO port not ready", -ENODEV);
      return -ENODEV;
    }

    // INACTIVE, never ACTIVE. The external 10k pull-downs hold both lines
    // de-energised from reset until this runs; configuring them any other way
    // would throw away a fail-safe the hardware gives us for free.
    //
    // GPIO_INPUT is requested alongside so Set() can read back what it drove. Not
    // every SoC will connect the input buffer on an output pin, so a refusal here
    // is tolerated and read-back is disabled rather than treated as a fault.
    result = gpio_pin_configure_dt(&s_fire1, GPIO_OUTPUT_INACTIVE | GPIO_INPUT);
    if (result == 0) { result = gpio_pin_configure_dt(&s_fire2, GPIO_OUTPUT_INACTIVE | GPIO_INPUT); }

    if (result == 0) {
      m_readback_supported = true;
    } else {
      LOG_WRN("Fire pins will not take GPIO_INPUT (%d) - driving without read-back verification.", result);

      result = gpio_pin_configure_dt(&s_fire1, GPIO_OUTPUT_INACTIVE);
      if (result == 0) { result = gpio_pin_configure_dt(&s_fire2, GPIO_OUTPUT_INACTIVE); }
      if (result < 0) {
        enterFaultState("fire pin configure failed", result);
        return result;
      }
    }

    m_initialised = true;
    m_asserted    = false;

    // Belt and braces. gpio_pin_configure_dt() with GPIO_OUTPUT_INACTIVE has
    // already driven both low; this states it rather than assuming it.
    result = driveBoth(false);
    if (result < 0) {
      enterFaultState("could not drive fire pins low at init", result);
      return result;
    }

    // Prove it rather than announce it. Reporting "de-energised" on the strength
    // of two writes that were never read back is the kind of claim this class
    // exists to avoid making.
    result = verifyBoth(false);
    if (result < 0) {
      enterFaultState("fire pins did not read back low at init", result);
      return result;
    }

    LOG_INF("Fire output initialised, de-energised and verified%s.", m_readback_supported ? "" : " (NO READ-BACK AVAILABLE - unverified)");
    return 0;
  }

  int OutputSwitch::Set(bool assert)
  {
    int result { 0 };

    // Clearing is always attempted, whatever state this object is in. A faulted
    // or uninitialised switch must still be able to go safe.
    if (!assert) {
      result     = driveBoth(false);
      m_asserted = false;
      if (result < 0) {
        // Deliberately NOT latched as a fault and returned quietly: a fire output
        // that will not clear is the dangerous direction, and the operator needs
        // to see it every time it happens.
        LOG_ERR("FIRE OUTPUT WOULD NOT CLEAR: %d!", result);
        return result;
      }
      return verifyBoth(false);
    }

    if (!IsUsable()) {
      LOG_ERR("Refusing to assert the fire output: switch is %s!", m_faulted ? "faulted" : "not initialised");
      driveBoth(false);
      return -EPERM;
    }

    result = driveBoth(true);
    if (result < 0) {
      enterFaultState("fire pin write failed while asserting", result);
      return result;
    }

    result = verifyBoth(true);
    if (result < 0) {
      enterFaultState("fire pins did not read back as asserted", result);
      return result;
    }

    m_asserted = true;
    return 0;
  }

  int OutputSwitch::ForceSafe()
  {
    int result { driveBoth(false) };

    m_asserted = false;
    if (result < 0) { LOG_ERR("FIRE OUTPUT WOULD NOT CLEAR: %d!", result); }
    return result;
  }

  int OutputSwitch::driveBoth(bool assert)
  {
    int value { assert ? 1 : 0 };

    // Both writes are attempted even if the first fails. Returning early would
    // leave one line driven and the other not - a half-asserted fire output,
    // which is the one state this class exists to make unreachable.
    int result1 { gpio_pin_set_dt(&s_fire1, value) };
    int result2 { gpio_pin_set_dt(&s_fire2, value) };

    if (result1 < 0) { return result1; }
    return result2;
  }

  int OutputSwitch::verifyBoth(bool assert)
  {
    if (!m_readback_supported) { return 0; }

    // Raw, not the _dt form. These pins are ACTIVE_HIGH so the two agree today,
    // but raw is what is actually on the pin and the point of a read-back is to
    // learn what the hardware did, not what the logical layer believes.
    int expected { assert ? 1 : 0 };
    int level1 { gpio_pin_get_raw(s_fire1.port, s_fire1.pin) };
    int level2 { gpio_pin_get_raw(s_fire2.port, s_fire2.pin) };

    if (level1 < 0) { return level1; }
    if (level2 < 0) { return level2; }

    if (level1 != expected || level2 != expected) {
      LOG_ERR("Fire pin read-back mismatch: wanted %d, got Fire1 %d Fire2 %d!", expected, level1, level2);
      return -EIO;
    }

    return 0;
  }

  void OutputSwitch::enterFaultState(const char* reason, int errorCode)
  {
    m_faulted = true;

    // Go safe before announcing it. If this write also fails there is nothing
    // further software can do, and the external pull-downs are the last defence.
    if (gpio_pin_set_dt(&s_fire1, 0) < 0 || gpio_pin_set_dt(&s_fire2, 0) < 0) {
      LOG_ERR("FIRE OUTPUT WOULD NOT CLEAR WHILE FAULTING - relying on the external pull-downs!");
    }
    m_asserted = false;

    LOG_ERR("Fire output LATCHED FAULTY and will not assert again: %s (%d)!", reason, errorCode);
  }

}
