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

    // MOSFET gate drive levels. Named so that a call site reads as intent rather
    // than as a bare 1 or 0 on the most dangerous line in the device.
    constexpr int M_GATE_ON { 1 };
    constexpr int M_GATE_OFF { 0 };

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
    //
    // The two gates are in SERIES, so clearing either one stops the device
    // firing. That makes a single failed clear a fault but not an emergency, and
    // the two cases are reported differently on purpose: if both alarms read the
    // same, the one that matters gets lost among the ones that do not.
    if (!assert) {
      int result1 { gpio_pin_set_dt(&s_fire1, M_GATE_OFF) };
      int result2 { gpio_pin_set_dt(&s_fire2, M_GATE_OFF) };

      m_asserted = false;

      if (result1 < 0 && result2 < 0) {
        // Nothing in software can stop the current now. Only the external gate
        // pull-downs remain.
        LOG_ERR("BOTH FIRE GATES FAILED TO CLEAR (%d, %d) - THE DEVICE MAY BE FIRING!", result1, result2);
        return result1;
      }

      if (result1 < 0 || result2 < 0) {
        // Safe, because the surviving MOSFET blocks the circuit on its own. But
        // the series redundancy that made it safe is now spent, and a second
        // failure would fire the device.
        LOG_ERR("Fire gate %s failed to clear: %d! Output is SAFE - the other MOSFET blocks - but redundancy is LOST!", result1 < 0 ? "1" : "2",
                result1 < 0 ? result1 : result2);
        m_faulted = true;
        return result1 < 0 ? result1 : result2;
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
    // Same one-versus-both distinction as Set(false). Turning either gate off is
    // enough to keep the device from firing, so only a double failure is an
    // emergency - and saying so is what keeps the emergency legible.
    int result1 { gpio_pin_set_dt(&s_fire1, M_GATE_OFF) };
    int result2 { gpio_pin_set_dt(&s_fire2, M_GATE_OFF) };

    m_asserted = false;

    if (result1 < 0 && result2 < 0) {
      LOG_ERR("BOTH FIRE GATES FAILED TO CLEAR (%d, %d) - THE DEVICE MAY BE FIRING!", result1, result2);
      return result1;
    }

    if (result1 < 0 || result2 < 0) {
      LOG_ERR("Fire gate %s failed to clear: %d! Output is SAFE - the other MOSFET blocks - but redundancy is LOST!", result1 < 0 ? "1" : "2",
              result1 < 0 ? result1 : result2);
      m_faulted = true;
      return result1 < 0 ? result1 : result2;
    }

    return 0;
  }

  int OutputSwitch::driveBoth(bool assert)
  {
    int value { assert ? M_GATE_ON : M_GATE_OFF };

    // Both writes are attempted even if the first fails.
    //
    // Note what this is and is not protecting against. The gates are in SERIES,
    // so a half-driven output does NOT fire - the unasserted MOSFET blocks the
    // circuit. Attempting the second write is therefore not about preventing a
    // dangerous state; it is about never leaving a gate energised that nobody
    // asked for, and about making the failure visible in one pass rather than
    // discovering the second gate's condition on some later call.
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
    int expected { assert ? M_GATE_ON : M_GATE_OFF };
    int level1 { gpio_pin_get_raw(s_fire1.port, s_fire1.pin) };
    int level2 { gpio_pin_get_raw(s_fire2.port, s_fire2.pin) };

    if (level1 < 0) { return level1; }
    if (level2 < 0) { return level2; }

    if (level1 != expected || level2 != expected) {
      // Named per channel. With two series MOSFETs, knowing WHICH gate misbehaved
      // is the difference between "one channel of protection is gone" and "the
      // device may be conducting", and those want different responses.
      LOG_ERR("Fire gate read-back mismatch: wanted %d, got Fire1 %d Fire2 %d!", expected, level1, level2);
      return -EIO;
    }

    return 0;
  }

  void OutputSwitch::enterFaultState(const char* reason, int errorCode)
  {
    m_faulted = true;

    // Go safe before announcing it. Both are attempted: the gates are in series,
    // so getting either one low is enough to keep the device from firing. If both
    // fail there is nothing further software can do and the external gate
    // pull-downs are the last defence.
    int result1 { gpio_pin_set_dt(&s_fire1, M_GATE_OFF) };
    int result2 { gpio_pin_set_dt(&s_fire2, M_GATE_OFF) };

    if (result1 < 0 && result2 < 0) {
      LOG_ERR("BOTH FIRE GATES FAILED TO CLEAR WHILE FAULTING - only the external pull-downs remain!");
    } else if (result1 < 0 || result2 < 0) {
      LOG_ERR("Fire gate %s failed to clear while faulting - output SAFE via the other MOSFET.", result1 < 0 ? "1" : "2");
    }
    m_asserted = false;

    LOG_ERR("Fire output LATCHED FAULTY and will not assert again: %s (%d)!", reason, errorCode);
  }

}
