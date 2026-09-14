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
    // ways to move these lines are OutputSwitch::Enable(), Set() and Disable().
    // Do not add an accessor, do not pass these out, and do not declare a
    // second handle to the same pins elsewhere - in the product this output
    // switches a voltage.
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

    // Checks one pin at boot: plain input with no pull, read raw, then
    // disconnected. The disconnect is attempted whatever the configure or read
    // returned, so the pin is never left with a buffer connected. `level` is the
    // raw level read, or -1 if it could not be read.
    int bootCheckPin(const struct gpio_dt_spec& pin, int& level)
    {
      int result { gpio_pin_configure_dt(&pin, GPIO_INPUT) };
      int disconnectResult { 0 };

      level = -1;
      if (result == 0) {
        // Raw, not the _dt form: the point is what is physically on the pin.
        level = gpio_pin_get_raw(pin.port, pin.pin);
        if (level < 0) { result = level; }
      }

      disconnectResult = gpio_pin_configure_dt(&pin, GPIO_DISCONNECTED);
      if (result == 0) { result = disconnectResult; }
      return result;
    }

  }

  OutputSwitch::OutputSwitch()
      : m_initialised(false)
      , m_faulted(false)
      , m_asserted(false)
      , m_enabled(false)
      , m_readback_supported(false)
      , m_interlock(nullptr)
      , m_interlock_context(nullptr)
  {}

  void OutputSwitch::SetInterlock(InterlockFn interlock, void* context)
  {
    m_interlock         = interlock;
    m_interlock_context = context;
  }

  int OutputSwitch::Init()
  {
    int result1 { 0 };
    int result2 { 0 };
    int level1 { -1 };
    int level2 { -1 };

    if (!gpio_is_ready_dt(&s_fire1) || !gpio_is_ready_dt(&s_fire2)) {
      enterFaultState("fire GPIO port not ready", -ENODEV);
      return -ENODEV;
    }

    // NO DRIVER AT BOOT. The external 10k pull-downs hold both gates off from
    // reset, and this check never drives the pins: each is read as a plain input
    // and disconnected again. A driver is attached only by Enable(), as the last
    // step of arming. Both pins are always checked.
    result1 = bootCheckPin(s_fire1, level1);
    result2 = bootCheckPin(s_fire2, level2);

    if (result1 < 0 || result2 < 0) {
      LOG_ERR("Fire pin boot check could not complete: Fire1 %d, Fire2 %d!", result1, result2);
      enterFaultState("fire pin boot check failed", result1 < 0 ? result1 : result2);
      return result1 < 0 ? result1 : result2;
    }

    // A line reading high with no driver from this firmware is held on by
    // something else - a failed pull-down, a short or a misrouted net. Same
    // one-versus-both distinction as a failed clear: with the gates in SERIES,
    // one high gate is safe but has spent the redundancy, and two means the
    // device may be conducting right now.
    if (level1 != M_GATE_OFF && level2 != M_GATE_OFF) {
      LOG_ERR("BOTH FIRE GATES READ HIGH AT BOOT WITH NO DRIVER - THE DEVICE MAY BE FIRING!");
      enterFaultState("both fire pins read high at boot", -EIO);
      return -EIO;
    }

    if (level1 != M_GATE_OFF || level2 != M_GATE_OFF) {
      LOG_ERR("Fire gate %s read high at boot with no driver! Output is SAFE - the other MOSFET blocks - but redundancy is LOST!",
              level1 != M_GATE_OFF ? "1" : "2");
      enterFaultState("a fire pin read high at boot", -EIO);
      return -EIO;
    }

    m_initialised = true;
    m_asserted    = false;
    m_enabled     = false;

    LOG_INF("Fire output initialised: both pins read low at boot and are isolated (no driver).");
    return 0;
  }

  int OutputSwitch::Enable()
  {
    int result { 0 };

    if (!IsUsable()) {
      LOG_ERR("Refusing to enable the fire pins: switch is %s!", m_faulted ? "faulted" : "not initialised");
      return -EPERM;
    }

    // INACTIVE, never ACTIVE. The pins leave isolation already driven low, so
    // attaching the driver cannot energise a gate the pull-down was holding off.
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
      m_readback_supported = false;

      result = gpio_pin_configure_dt(&s_fire1, GPIO_OUTPUT_INACTIVE);
      if (result == 0) { result = gpio_pin_configure_dt(&s_fire2, GPIO_OUTPUT_INACTIVE); }
      if (result < 0) {
        enterFaultState("fire pin configure failed at enable", result);
        return result;
      }
    }

    // Enabled from here, so a failure below is isolated by enterFaultState().
    m_enabled  = true;
    m_asserted = false;

    // Belt and braces. gpio_pin_configure_dt() with GPIO_OUTPUT_INACTIVE has
    // already driven both low; this states it rather than assuming it.
    result = driveBoth(false);
    if (result < 0) {
      enterFaultState("could not drive fire pins low at enable", result);
      return result;
    }

    // Prove it rather than announce it. Reporting "de-energised" on the strength
    // of two writes that were never read back is the kind of claim this class
    // exists to avoid making.
    result = verifyBoth(false);
    if (result < 0) {
      enterFaultState("fire pins did not read back low at enable", result);
      return result;
    }

    LOG_INF("Fire pins enabled: outputs, de-energised and verified%s.", m_readback_supported ? "" : " (NO READ-BACK AVAILABLE - unverified)");
    return 0;
  }

  int OutputSwitch::Disable()
  {
    int result { isolateBoth() };

    // A failed isolation spends the guarantee that the pins have no driver while
    // the device is not armed. Refusing ever to enable again is the safe response.
    if (result < 0) {
      m_faulted = true;
      LOG_ERR("Fire output LATCHED FAULTY and will not assert again: fire pin disable failed (%d)!", result);
    }
    return result;
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
      // DISABLED: nothing to clear and nothing to verify. The pins have no
      // driver, and with the input buffer disconnected a read-back would read
      // garbage and falsely latch a fault.
      if (!m_enabled) {
        m_asserted = false;
        return 0;
      }

      int result1 { gpio_pin_set_dt(&s_fire1, M_GATE_OFF) };
      int result2 { gpio_pin_set_dt(&s_fire2, M_GATE_OFF) };

      m_asserted = false;

      // A failed clear - a write error or a read-back that is not low - LATCHES
      // the switch faulty and isolates both pins, whichever way it failed.
      // enterFaultState() leaves the switch disabled, so every later Set(false)
      // takes the disabled early return above: the fault is logged once, not
      // once per main-loop tick.
      if (result1 < 0 && result2 < 0) {
        // Nothing in software can stop the current now. Only the external gate
        // pull-downs remain.
        LOG_ERR("BOTH FIRE GATES FAILED TO CLEAR (%d, %d) - THE DEVICE MAY BE FIRING!", result1, result2);
        enterFaultState("both fire gates failed to clear", result1);
        return result1;
      }

      if (result1 < 0 || result2 < 0) {
        // Safe, because the surviving MOSFET blocks the circuit on its own. But
        // the series redundancy that made it safe is now spent, and a second
        // failure would fire the device.
        result = result1 < 0 ? result1 : result2;
        LOG_ERR("Fire gate %s failed to clear: %d! Output is SAFE - the other MOSFET blocks - but redundancy is LOST!", result1 < 0 ? "1" : "2",
                result);
        enterFaultState("a fire gate failed to clear", result);
        return result;
      }

      // The writes succeeded; prove the pins went low. verifyBoth() names a gate
      // still reading high with the same one-versus-both wording as above.
      result = verifyBoth(false);
      if (result < 0) {
        enterFaultState("fire pins did not read back low on clear", result);
        return result;
      }
      return 0;
    }

    if (!IsUsable()) {
      LOG_ERR("Refusing to assert the fire output: switch is %s!", m_faulted ? "faulted" : "not initialised");
      driveBoth(false);
      return -EPERM;
    }

    // Asserting a disabled switch means a caller bypassed the arming sequence,
    // which enables the pins only as its last step before Active. A bug, not a
    // routine condition.
    if (!m_enabled) {
      enterFaultState("assert requested while the fire pins are disabled - a caller bypassed the arming sequence", -EPERM);
      return -EPERM;
    }

    // SECOND LAYER. The caller should already have declined to ask, so being
    // refused here means the first layer failed - a bug, not a routine
    // condition. Latch faulty and say so loudly rather than quietly declining.
    if (m_interlock != nullptr && !m_interlock(m_interlock_context)) {
      driveBoth(false);
      enterFaultState("interlock refused the assertion - a caller bypassed the derivation point", -EPERM);
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
    //
    // Disabled, there is no driver to turn off and nothing is written.
    if (!m_enabled) {
      m_asserted = false;
      return 0;
    }

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
    bool clearing { !assert };
    int level1 { gpio_pin_get_raw(s_fire1.port, s_fire1.pin) };
    int level2 { gpio_pin_get_raw(s_fire2.port, s_fire2.pin) };

    if (level1 < 0) { return level1; }
    if (level2 < 0) { return level2; }

    // A clear that did not take is reported with the one-versus-both distinction.
    // The gates are in SERIES: one still high is safe but has spent the
    // redundancy, both still high means the device may be conducting right now.
    if (clearing && level1 != M_GATE_OFF && level2 != M_GATE_OFF) {
      LOG_ERR("BOTH FIRE GATES FAILED TO CLEAR (read back Fire1 %d Fire2 %d) - THE DEVICE MAY BE FIRING!", level1, level2);
      return -EIO;
    }

    if (clearing && (level1 != M_GATE_OFF || level2 != M_GATE_OFF)) {
      LOG_ERR("Fire gate %s failed to clear: read back high! Output is SAFE - the other MOSFET blocks - but redundancy is LOST!",
              level1 != M_GATE_OFF ? "1" : "2");
      return -EIO;
    }

    if (level1 != expected || level2 != expected) {
      // Named per channel. With two series MOSFETs, knowing WHICH gate misbehaved
      // is the difference between "one channel of protection is gone" and "the
      // device may be conducting", and those want different responses.
      LOG_ERR("Fire gate read-back mismatch: wanted %d, got Fire1 %d Fire2 %d!", expected, level1, level2);
      return -EIO;
    }

    return 0;
  }

  int OutputSwitch::isolateBoth()
  {
    if (!gpio_is_ready_dt(&s_fire1) || !gpio_is_ready_dt(&s_fire2)) {
      m_enabled  = false;
      m_asserted = false;
      LOG_ERR("Fire GPIO port not ready - cannot isolate the fire pins; only the external pull-downs remain!");
      return -ENODEV;
    }

    // Drive low FIRST, so a gate that was high is emptied at once rather than at
    // the pull-down's RC rate. On a pin that is already disconnected this only
    // sets the output latch, which is harmless. Every step is attempted on both
    // pins whatever an earlier one returned.
    int drive1 { gpio_pin_set_dt(&s_fire1, M_GATE_OFF) };
    int drive2 { gpio_pin_set_dt(&s_fire2, M_GATE_OFF) };

    m_asserted = false;

    // Then no driver at all: only the external pull-downs hold the lines.
    int disconnect1 { gpio_pin_configure_dt(&s_fire1, GPIO_DISCONNECTED) };
    int disconnect2 { gpio_pin_configure_dt(&s_fire2, GPIO_DISCONNECTED) };

    m_enabled = false;

    // A gate is only possibly still on if BOTH its clear and its disconnect
    // failed: either one alone leaves it low, driven or via its pull-down. The
    // gates are in SERIES, so the one-versus-both distinction applies as for
    // Set(false).
    bool stuck1 { drive1 < 0 && disconnect1 < 0 };
    bool stuck2 { drive2 < 0 && disconnect2 < 0 };

    if (stuck1 && stuck2) {
      LOG_ERR("BOTH FIRE GATES FAILED TO CLEAR (%d, %d) - THE DEVICE MAY BE FIRING!", drive1, drive2);
    } else if (stuck1 || stuck2) {
      LOG_ERR("Fire gate %s failed to clear: %d! Output is SAFE - the other MOSFET blocks - but redundancy is LOST!", stuck1 ? "1" : "2",
              stuck1 ? drive1 : drive2);
    } else if (drive1 < 0 || drive2 < 0 || disconnect1 < 0 || disconnect2 < 0) {
      LOG_ERR("Fire pin isolation incomplete (drive %d, %d; disconnect %d, %d)! Each gate is low, but the pins may NOT be isolated!", drive1, drive2,
              disconnect1, disconnect2);
    }

    if (drive1 < 0) { return drive1; }
    if (drive2 < 0) { return drive2; }
    if (disconnect1 < 0) { return disconnect1; }
    return disconnect2;
  }

  void OutputSwitch::enterFaultState(const char* reason, int errorCode)
  {
    m_faulted = true;

    // Go safe before announcing it: drive low, then disconnect, both pins. If
    // both gates fail there is nothing further software can do and the external
    // gate pull-downs are the last defence - isolateBoth() says so.
    (void)isolateBoth();

    LOG_ERR("Fire output LATCHED FAULTY and will not assert again: %s (%d)!", reason, errorCode);
  }

}
