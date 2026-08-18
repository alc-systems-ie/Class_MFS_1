#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app.hpp"
#include "npm2100_zephyr.hpp"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

namespace alc
{

  namespace
  {

    constexpr uint32_t M_POLL_INTERVAL_MS { 100 };

#if defined(CONFIG_MFS_BATTERY_TEST)
    // Liveness blink for the battery test, at the scan period.
    //
    // NOTE: this is NOT phase-locked to the radio's scan window. The controller
    // duty-cycles the scan itself from the interval/window pair and gives the
    // application no callback at window start, so this is a software tick of the
    // same period running independently. It shows the device is alive and running
    // its 6 s cycle; it does not mark the exact instant the receiver opens.
    constexpr uint32_t M_BLINK_PERIOD_TICKS { CONFIG_MFS_SCAN_PERIOD_MS / M_POLL_INTERVAL_MS };
    constexpr uint32_t M_BLINK_ON_TICKS { CONFIG_MFS_BLINK_MS / M_POLL_INTERVAL_MS };
#endif

    // ADXL367 supply rail. The part must stay on LSOUT at 1.8 V: its INT2 pin is
    // wired to the PMIC SHPHLD pin, which has a 1.9 V absolute maximum, so an ADXL
    // running from the ~3.0 V boost output would damage the PMIC.
    constexpr uint16_t M_LSOUT_MILLIVOLTS { 1800 };

    // LSOUT is actively discharged to AVSS1 when LDOSW is disabled; this is the time
    // allowed for the rail and the ADXL367's decoupling to reach 0 V, which the
    // ADXL367 datasheet recommends when power cycling.
    constexpr uint32_t M_LSOUT_DISCHARGE_MS { 50 };

    // Time from LSOUT rising to the ADXL367 answering on I2C. It must load fuses and
    // enter standby first. Diagnosed 2026-08-18: after an overnight power-down the
    // probe ran 0.57 ms after the rail came up and the part did not respond. Earlier
    // runs only worked because they were warm resets with LSOUT already live.
    //
    // Raised 20 -> 100 ms after a cold boot reported "answered only on probe
    // attempt 4", i.e. the part needed ~50 ms and the retries were masking a
    // marginal delay. Boot is ~0.25 s, so the headroom is free, and a supply that
    // settles slowly on a cold or depleted cell is exactly where this would
    // otherwise fail in the field. The probe retries remain as a safety net and
    // will warn if even this proves tight.
    constexpr uint32_t M_ADXL_POWER_ON_MS { 100 };

    // These use `=` rather than brace initialisation: GPIO_DT_SPEC_GET already
    // expands to a braced initialiser list, and wrapping it in further braces makes
    // the compiler try to initialise the first member from the whole list.

    // LED A — on while Inactive. LED B — on while Active and triggered.
    const struct gpio_dt_spec s_led_a = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
    const struct gpio_dt_spec s_led_b = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

    // ADXL367 INT1. Declared GPIO_ACTIVE_LOW in the overlay, so gpio_pin_get_dt()
    // returns 1 when the pin is electrically LOW, i.e. when AWAKE is asserted.
    const struct gpio_dt_spec s_adxl_int1 = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), adxl_int1_gpios);

    // nRF21540 control pins. Fitted on the bespoke board, unused by MFS_1.
    const struct gpio_dt_spec s_fem_pdn   = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), fem_pdn_gpios);
    const struct gpio_dt_spec s_fem_tx_en = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), fem_tx_en_gpios);
    const struct gpio_dt_spec s_fem_rx_en = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), fem_rx_en_gpios);
    const struct gpio_dt_spec s_fem_mode  = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), fem_mode_gpios);

    const struct gpio_dt_spec* const s_fem_pins[] { &s_fem_pdn, &s_fem_tx_en, &s_fem_rx_en, &s_fem_mode };

    const char* resetReasonName(Npm2100::ResetReason reason)
    {
      switch (reason) {
        case Npm2100::ResetReason::ColdPowerUp:
          return "ColdPowerUp";
        case Npm2100::ResetReason::ThermalShutdown:
          return "ThermalShutdown";
        case Npm2100::ResetReason::BootMonitor:
          return "BootMonitor";
        case Npm2100::ResetReason::Button:
          return "Button";
        case Npm2100::ResetReason::WatchdogReset:
          return "WatchdogReset";
        case Npm2100::ResetReason::WatchdogPwrCyc:
          return "WatchdogPwrCyc";
        case Npm2100::ResetReason::SoftwareReset:
          return "SoftwareReset";
        case Npm2100::ResetReason::HiberPin:
          return "HiberPin";
        case Npm2100::ResetReason::HiberTimer:
          return "HiberTimer";
        case Npm2100::ResetReason::HiberPtPin:
          return "HiberPtPin";
        case Npm2100::ResetReason::HiberPtTimer:
          return "HiberPtTimer";
        case Npm2100::ResetReason::PowerOffButton:
          return "PowerOffButton";
        case Npm2100::ResetReason::ShipExit:
          return "ShipExit";
        case Npm2100::ResetReason::OvercurrentProt:
          return "OvercurrentProt";
        default:
          return "Unknown";
      }
    }

  }

  App::App()
      : m_i2c_bus(DEVICE_DT_GET(DT_NODELABEL(i2c21)))
      , m_pmic(npm2100_zephyr::MakeZephyrTransport(DEVICE_DT_GET(DT_NODELABEL(i2c21))))
      , m_accelerometer(DEVICE_DT_GET(DT_NODELABEL(i2c21)))
      , m_scanner()
      , m_arm_state(ArmState::Inactive)
      , m_initialised(false)
  {}

  int App::Run()
  {
    int result { 0 };
    bool triggered { false };
    bool previousTriggered { false };
    bool ledA { false };
#if defined(CONFIG_MFS_BATTERY_TEST)
    uint32_t blinkTicks { 0 };
    uint32_t blinkOnTicks { 0 };
#endif

    LOG_INF("MFS_1 starting, serial %s.", CONFIG_ALC_DEVICE_SERIAL);

    if (!device_is_ready(m_i2c_bus)) {
      LOG_ERR("i2c21 not ready!");
      return -ENODEV;
    }

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

    result = initPmic();
    if (result < 0) {
      LOG_ERR("Failed to initialise the PMIC: %d!", result);
      return result;
    }

    result = initAccelerometer();
    if (result < 0) {
      LOG_ERR("Failed to initialise the accelerometer: %d!", result);
      return result;
    }

    result = lowerLsoutToUlp();
    if (result < 0) { return result; }

    // Cold start defaults to Inactive — see docs/v1-scope.md section 6.
    setArmState(ArmState::Inactive);

    result = m_scanner.Start();
    if (result < 0) {
      LOG_ERR("Failed to start the command scanner: %d!", result);
      return result;
    }

    m_initialised = true;
    m_accelerometer.LogDiagnostics();
    LOG_INF("INT1 raw %d (physical), dt %d (logical).", gpio_pin_get_raw(s_adxl_int1.port, s_adxl_int1.pin), gpio_pin_get_dt(&s_adxl_int1));

    while (true) {
      if (m_scanner.TakePendingCommand() == CommandScanner::Command::ToggleArm) { toggleArmState(); }

#if defined(CONFIG_MFS_BATTERY_TEST)
      if (++blinkTicks >= M_BLINK_PERIOD_TICKS) {
        blinkTicks   = 0;
        blinkOnTicks = M_BLINK_ON_TICKS;
      }
      ledA = blinkOnTicks > 0;
      if (blinkOnTicks > 0) { --blinkOnTicks; }
#endif

      // INT1 tracks the ADXL367 AWAKE bit, so the 5 s LED B timeout is the part's
      // own loop period rather than a software timer.
      triggered = gpio_pin_get_dt(&s_adxl_int1) > 0;

      // Log only on transitions. A periodic dump floods the 4 KB RTT buffer in
      // LOG_MODE_IMMEDIATE and silently drops the events that actually matter —
      // which is how the LED behaviour went unexplained for a whole test cycle.
      if (triggered != previousTriggered) {
        LOG_INF("Motion %s. Arm state %s, LED A %s, LED B %s.", triggered ? "detected" : "timed out",
                m_arm_state == ArmState::Active ? "Active" : "Inactive", (m_arm_state == ArmState::Inactive) ? "ON" : "off",
                ((m_arm_state == ArmState::Active) && triggered) ? "ON" : "off");
        previousTriggered = triggered;
      }

      result = refreshLeds(ledA, triggered);
      if (result < 0) { LOG_ERR("LED update failed: %d!", result); }

      k_msleep(M_POLL_INTERVAL_MS);
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
      if (result < 0) { return result; }
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
    if (result < 0) { return result; }
    return gpio_pin_configure_dt(&s_led_b, GPIO_OUTPUT_INACTIVE);
  }

  int App::initPmic()
  {
    int result { m_pmic.Init() };
    bool bootMonitorActive { false };

    if (result < 0) {
      LOG_ERR("nPM2100 probe failed: %d!", result);
      return result;
    }

    LOG_INF("nPM2100 reset reason %s, brown-out %s.", resetReasonName(m_pmic.GetResetReason()), m_pmic.BrownOutOccurred() ? "yes" : "no");

    // DISABLE THE BOOT MONITOR FIRST - it is on a timer.
    //
    // The nPM2100 arms its boot monitor after every power cycle ("Boot monitor is
    // activated after each power cycle unless disabled using SYSGDEN") and
    // power-cycles the device if the host never reports a successful boot.
    // Disabling it here IS that report - it is the intended handshake, not a
    // workaround. Skipping it means the PMIC resets the SoC every few seconds
    // forever.
    //
    // Diagnosed 2026-08-18: the device ran exactly four 2 s heartbeats and reset,
    // with GetResetReason() returning BootMonitor rather than ColdPowerUp. It
    // presented as LEDs blinking on a ~6 s cycle, an arm state that would not
    // stick, and RTT going silent after boot as the viewer lost sync on each
    // reset.
    result = m_pmic.IsBootMonitorActive(bootMonitorActive);
    if (result == 0 && bootMonitorActive) { LOG_INF("nPM2100 boot monitor is configured on - stopping it."); }

    // The boot monitor IS the TIMER block, and TASKS_STOP is the documented way to
    // stop it: "Software can stop the boot monitor by activating the timer stop
    // task in TASKS_STOP to avoid the power cycle." Stopping it is the intended
    // "the host booted successfully" handshake, not a workaround.
    //
    // The sticky BOOTMONSEL/BOOTMONEN bits are a separate selector and do NOT stop
    // a monitor that is already running. Nor does SYSGDENSTATUS report running
    // state - bit 0 means "boot monitor is active unless SYSGDENSTATE=0", i.e.
    // configuration, so it is logged for information and never used as a pass/fail.
    //
    // Boot monitor cannot be re-enabled over TWI once stopped, so this is one-way
    // until the next power cycle.
    //
    // Diagnosed 2026-08-18: without this the device ran four 2 s heartbeats and
    // power-cycled, with GetResetReason() returning BootMonitor. It presented as
    // LEDs blinking on a ~6 s cycle and an arm state that would not stick.
    result = m_pmic.TimerStop();
    if (result < 0) {
      LOG_ERR("Failed to stop the PMIC boot monitor timer: %d!", result);
      return result;
    }
    LOG_INF("nPM2100 boot monitor stopped.");

    // Disable the SHPHLD power-off path. ADXL367 INT2 is hard-wired to SHPHLD on
    // this board; a sustained assertion followed by release is the ship-mode
    // gesture and would power the device off. The ADXL is configured never to
    // assert INT2 (see Adxl367::ConfigureLoopMode) — this is the second line of
    // defence. See docs/v1-scope.md section 2.
    result = m_pmic.DisablePowerOffButton(true);
    if (result < 0) {
      LOG_ERR("Failed to disable the PMIC power-off button: %d!", result);
      return result;
    }
    LOG_INF("PMIC power-off button disabled - SHPHLD cannot ship the device.");

    // LSOUT supplies the ADXL367 at 1.8 V.
    //
    // POWER-CYCLE IT. nPM2100 registers survive an SoC reset, so LdoSwEnable() on an
    // already-live rail is a no-op and would leave the ADXL in whatever mode the
    // previous run ended in — including autosleep's wake-up mode, where the part
    // samples intermittently. Disabling LDOSW actively discharges LSOUT to AVSS1,
    // and the ADXL367 datasheet recommends a full discharge to 0 V when power
    // cycling, so the delay below is a discharge time, not a guess.
    result = m_pmic.LdoSwDisable();
    if (result < 0) {
      LOG_ERR("Failed to disable LSOUT for the power cycle: %d!", result);
      return result;
    }
    k_msleep(M_LSOUT_DISCHARGE_MS);

    // Bring the rail up in HIGH POWER, not Ultra-Low Power. The ADXL367 datasheet
    // requires supply current above 250 uA during power-up for correct fuse
    // loading; ULP is built for uA-level loads and is the wrong mode to power up
    // into. LDOSW drops to ULP once the accelerometer is configured — see
    // App::Run() — so the idle saving is kept.
    result = m_pmic.LdoSwSetOutputMode(Npm2100::LdoSwOutputMode::Ldo);
    if (result == 0) { result = m_pmic.LdoSwSetVoltage(M_LSOUT_MILLIVOLTS); }
    if (result == 0) { result = m_pmic.LdoSwSetPowerMode(Npm2100::LdoSwPowerMode::Hp); }
    if (result == 0) { result = m_pmic.LdoSwEnable(); }
    if (result < 0) {
      LOG_ERR("Failed to bring up LSOUT: %d!", result);
      return result;
    }

    // Fuse loading plus entry to standby before the part will answer on I2C.
    k_msleep(M_ADXL_POWER_ON_MS);

    LOG_INF("LSOUT power-cycled, now %u mV in High Power mode.", m_pmic.LdoSwGetVoltageSetting());
    return 0;
  }

  int App::lowerLsoutToUlp()
  {
    // The ADXL367 is configured and drawing ~1 uA, so LDOSW no longer needs High
    // Power. ULP still supplies up to 2 mA. Auto is not used: it follows the device
    // mode, and MFS_1 stays in Active mode permanently, so Auto would hold High
    // Power for the device's whole life.
    int result { m_pmic.LdoSwSetPowerMode(Npm2100::LdoSwPowerMode::Ulp) };

    if (result < 0) {
      LOG_ERR("Failed to drop LSOUT to Ultra-Low Power: %d!", result);
      return result;
    }

    LOG_INF("LSOUT dropped to Ultra-Low Power mode.");
    return 0;
  }

  int App::initAccelerometer()
  {
    int result { 0 };

    if (!gpio_is_ready_dt(&s_adxl_int1)) {
      LOG_ERR("ADXL367 INT1 GPIO not ready!");
      return -ENODEV;
    }

    result = gpio_pin_configure_dt(&s_adxl_int1, GPIO_INPUT);
    if (result < 0) {
      LOG_ERR("ADXL367 INT1 configure failed: %d!", result);
      return result;
    }

    result = m_accelerometer.Init();
    if (result < 0) { return result; }

    return m_accelerometer.ConfigureLoopMode(CONFIG_MFS_ADXL_THRESHOLD, CONFIG_MFS_ADXL_ACTIVITY_SAMPLES, CONFIG_MFS_ADXL_INACTIVITY_SECS);
  }

  void App::setArmState(ArmState state)
  {
    m_arm_state = state;
    LOG_INF("Arm state: %s (uptime %lld ms). LED A %s.", state == ArmState::Active ? "Active" : "Inactive", k_uptime_get(),
            state == ArmState::Inactive ? "ON" : "off");
  }

  void App::toggleArmState()
  {
    setArmState(m_arm_state == ArmState::Active ? ArmState::Inactive : ArmState::Active);
  }

  int App::refreshLeds(bool ledAState, bool triggered)
  {
    bool ledA { false };
    bool ledB { false };
    int result { 0 };

#if defined(CONFIG_MFS_DEBUG_LED)
#if defined(CONFIG_MFS_BATTERY_TEST)
    // Battery test: LED A is a liveness blink driven by the caller, not an
    // Inactive indicator. LED B is unchanged.
    ledA = ledAState;
#else
    ARG_UNUSED(ledAState);
    ledA = (m_arm_state == ArmState::Inactive);
#endif
    ledB = (m_arm_state == ArmState::Active) && triggered;
#else
    ARG_UNUSED(ledAState);
    ARG_UNUSED(triggered);
#endif

    result = gpio_pin_set_dt(&s_led_a, ledA ? 1 : 0);
    if (result < 0) { return result; }
    return gpio_pin_set_dt(&s_led_b, ledB ? 1 : 0);
  }

}
