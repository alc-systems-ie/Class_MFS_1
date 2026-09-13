#include <cstring>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/settings/settings.h>

#include "access_keys.hpp"
#include "access_store.hpp"
#include "app.hpp"
#include "credentials.hpp"
#include "crypto.hpp"
#include "crypto_selftest.hpp"
#include "npm2100_zephyr.hpp"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

namespace alc
{

  namespace
  {

    constexpr uint32_t M_POLL_INTERVAL_MS { 100 };

    // How often the persisted day floor is adopted from the clock with no command
    // involved. Keeps a device that receives no commands for days from later
    // accepting a stale captured provisioner sync against an old floor.
    constexpr int64_t M_ADVANCE_INTERVAL_SECS { 60 };

    // Stuck-AWAKE watchdog threshold, in 100 ms loop ticks. Generous multiple of
    // the configured inactivity period so normal sustained handling never trips it.
    constexpr uint32_t M_AWAKE_STUCK_TICKS { (CONFIG_MFS_ADXL_INACTIVITY_SECS * 10U * 6U) };

    // nPM2100 TIMER is specified to +-10%, so the deadline fallback in
    // serviceCooldown() must allow that much slack over the requested duration
    // before it can be trusted to mean the PMIC has gone silent.
    constexpr int64_t M_COOLDOWN_TOLERANCE_DIVISOR { 10 };

    // Extra fixed slack on top of the tolerance, covering scheduling jitter in the
    // 100 ms poll loop itself.
    constexpr int64_t M_COOLDOWN_GRACE_MS { 2000 };

    // Minimum spacing between re-arm retries once the cooldown has expired but
    // enableAccelerometer() keeps failing. One attempt per second bounds the
    // driver's own error logging to 1 Hz instead of the 10 Hz poll rate.
    constexpr int64_t M_COOLDOWN_RETRY_MS { 1000 };

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

    // BENCH credentials, parsed from Kconfig (credentials.conf) at boot. Production
    // moves both keys into the KMU - see docs/tan-scheme.md section 8.
    uint32_t s_device_id { 0 };
    uint8_t s_device_secret[access::M_SECRET_BYTES] {};
    uint8_t s_provision_key[access::M_SECRET_BYTES] {};

    const char* verdictName(AccessControl::Verdict verdict)
    {
      switch (verdict) {
        case AccessControl::Verdict::Accepted:
          return "Accepted";
        case AccessControl::Verdict::NotForUs:
          return "NotForUs";
        case AccessControl::Verdict::ClockInvalid:
          return "ClockInvalid";
        case AccessControl::Verdict::LockedOut:
          return "LockedOut";
        case AccessControl::Verdict::AuthFailed:
          return "AuthFailed";
        case AccessControl::Verdict::Malformed:
          return "Malformed";
        case AccessControl::Verdict::Stale:
          return "Stale";
        case AccessControl::Verdict::PersistFailed:
          return "PersistFailed";
        case AccessControl::Verdict::CryptoError:
          return "CryptoError";
      }
      return "Unknown"; // Unreachable while every Verdict is handled above - -Wswitch warns if a new one is added.
    }

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
      , m_clock()
      , m_access(0, s_device_secret, &access_store::Persist, nullptr)
      , m_last_advance_secs(0)
      , m_access_ready(false)
      , m_output_switch()
      , m_arm_state(ArmState::Inactive)
      , m_ignore_stale_trigger(false)
      , m_output_active(false)
      , m_awake_ticks(0)
      , m_settings()
      , m_detection_met(false)
      , m_activation_count(0)
      , m_in_cooldown(false)
      , m_previous_awake(false)
      , m_cooldown_deadline_ms(0)
      , m_cooldown_rearm_failed(false)
      , m_cooldown_expired(false)
      , m_cooldown_next_retry_ms(0)
      , m_initialised(false)
  {}

  int App::Run()
  {
    int result { 0 };
    bool previousTriggered { false };
    bool ledA { false };
    bool ledB { false };
#if defined(CONFIG_MFS_BATTERY_TEST)
    uint32_t blinkTicks { 0 };
    uint32_t blinkOnTicks { 0 };
#endif

    LOG_INF("MFS_1 starting, serial %s.", CONFIG_ALC_DEVICE_SERIAL);

    // THE FIRE OUTPUT IS BROUGHT UP FIRST, before the I2C bus, the FEM or the
    // LEDs. The external 10k pull-downs hold both lines de-energised from reset,
    // and this takes active ownership of them at the earliest opportunity so the
    // window in which they depend on the pull-downs alone is as short as
    // possible. If it fails, the device refuses to run: a sensor that cannot
    // prove its output is safe has no business continuing to boot.
    result = m_output_switch.Init();
    if (result < 0) {
      LOG_ERR("Failed to initialise the fire output: %d!", result);
      return result;
    }

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

    // Not fatal. A device that cannot authenticate commands still boots, safe
    // and Inactive, so its hardware can be diagnosed over RTT.
    result = initAccess();
    if (result < 0) { LOG_ERR("Access control unavailable (%d) - commands will be ignored this boot!", result); }

    // After initAccess(), which initialises the settings subsystem. Defaults stand
    // if nothing is stored or the record is invalid.
    result = m_settings.Load();
    if (result < 0) { LOG_WRN("Settings not loaded (%d) - using defaults.", result); }
    LOG_INF("Settings: %u activations, %u s cooldown, %u LSB, %u s delay, mode %u.", m_settings.Activations(), m_settings.CooldownSeconds(),
            m_settings.ThresholdLsb(), m_settings.DelaySeconds(), static_cast<unsigned>(m_settings.OperatingMode()));

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
      serviceCandidates();
      serviceDayRollover();

#if defined(CONFIG_MFS_BATTERY_TEST)
      if (++blinkTicks >= M_BLINK_PERIOD_TICKS) {
        blinkTicks   = 0;
        blinkOnTicks = M_BLINK_ON_TICKS;
      }
      ledA = blinkOnTicks > 0;
      if (blinkOnTicks > 0) { --blinkOnTicks; }
#endif

      serviceCooldown();

      // The ONE place the output state is derived. See updateOutputState().
      updateOutputState();

      // Compute the LED states HERE, once, so the log below reports what is
      // actually written to the pins. Recomputing them inside the log statement
      // from the arm state produced messages that contradicted the build - a
      // battery-test build never drives LED A, but the log still claimed "LED A ON".
#if defined(CONFIG_MFS_DEBUG_LED)
#if !defined(CONFIG_MFS_BATTERY_TEST)
      ledA = (m_arm_state == ArmState::Inactive);
#endif
      // LED B shows DETECTION, in either arm state, for the 5 s ADXL loop period.
      // Inactive it simulates triggers while tuning; Active it confirms one. It is
      // a bench indicator, not an output consumer - OutputSwitch is the example
      // future consumers copy (design spec section 6.2).
      ledB = m_detection_met;
#endif

      // Log only on transitions. A periodic dump floods the 4 KB RTT buffer in
      // LOG_MODE_IMMEDIATE and silently drops the events that actually matter —
      // which is how the LED behaviour went unexplained for a whole test cycle.
      if (IsOutputActive() != previousTriggered) {
        previousTriggered = IsOutputActive();
        LOG_INF("Output %s. Arm %s, LED A %s, LED B %s.", previousTriggered ? "ASSERTED" : "cleared",
                m_arm_state == ArmState::Active ? "Active" : "Inactive", ledA ? "ON" : "off", ledB ? "ON" : "off");
      }

      result = applyLeds(ledA, ledB);
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

    // Probe only. The part is NOT configured here: a deactivated device holds it
    // in standby, and the loop engine is configured at the moment of arming - see
    // enableAccelerometer(). Cold start is Inactive, so it stays in standby now.
    result = m_accelerometer.Standby();
    if (result < 0) {
      LOG_ERR("Failed to put the accelerometer in standby: %d!", result);
      return result;
    }

    LOG_INF("ADXL367 held in standby until the device is activated.");
    return 0;
  }

  int App::enableAccelerometer()
  {
    // ================================================================
    //  ENABLE ORDER - SAFETY CRITICAL. See docs/v1-scope.md section 1.0.1.
    //
    //  Configure the part, prove it is reporting inactivity, and ONLY
    //  then let m_arm_state go Active. The device therefore cannot come
    //  up armed on motion that predates arming.
    // ================================================================
    int result { 0 };
    bool awake { true };

    // Configuring IS the clear: the datasheet's loop mode initialization routine
    // soft-resets the part and forces one activity/inactivity cycle, which drives
    // AWAKE low and captures a valid reference. Doing it per-arm also means the
    // reference is always taken in the orientation the device is actually left in.
    result = m_accelerometer.ConfigureLoopMode(m_settings.ThresholdLsb(), CONFIG_MFS_ADXL_ACTIVITY_SAMPLES, CONFIG_MFS_ADXL_INACTIVITY_THRESHOLD,
                                               CONFIG_MFS_ADXL_INACTIVITY_SECS);
    if (result < 0) {
      LOG_ERR("Accelerometer would not configure: %d!", result);
      return result;
    }

    // Confirm AWAKE from STATUS rather than from INT1. The register is what the
    // engine actually holds; the pin only mirrors it.
    result = m_accelerometer.ReadAwake(awake);
    if (result < 0) {
      LOG_ERR("Accelerometer AWAKE could not be read: %d!", result);
      return result;
    }

    // Every (re)configure starts with no inherited latch - a detection from
    // before this configure must never reach the output (v1-scope section 1.0.1).
    m_detection_met  = false;
    m_previous_awake = false;

    // Should already be clear. If handling the device has woken it again in the
    // moments since, that assertion still predates arming, so suppress it until
    // INT1 de-asserts and a fresh edge arrives.
    m_ignore_stale_trigger = awake;
    if (awake) { LOG_WRN("ADXL still awake after configuring - suppressing until it clears!"); }

    m_awake_ticks = 0;
    return 0;
  }

  int App::disableAccelerometer()
  {
    // ================================================================
    //  DISABLE ORDER - SAFETY CRITICAL.
    //
    //  The output is taken to 0 through the single derivation point
    //  BEFORE the part is stopped, so there is no instant at which a
    //  deactivated device still reads as triggered. m_arm_state has
    //  already been set Inactive by the caller - the output is DERIVED
    //  from it, so the boolean necessarily moves first and the
    //  derivation follows immediately, before the sensor is touched.
    // ================================================================
    int result { 0 };

    m_ignore_stale_trigger = false;
    m_awake_ticks          = 0;
    updateOutputState();

    // Standby stops the loop engine and de-asserts INT1. The rail stays up: LSOUT
    // is shared and power-cycling it would cost the ADXL367's fuse-load sequence
    // and the 100 ms settling delay on every arm.
    result = m_accelerometer.Standby();
    if (result < 0) {
      LOG_ERR("Failed to put the accelerometer in standby: %d!", result);
      return result;
    }

    return 0;
  }

  void App::updateOutputState()
  {
    // ================================================================
    //  THE SINGLE SOURCE OF TRUTH FOR THE DEVICE OUTPUT.
    //
    //  m_arm_state IS DEFINITIVE. The accelerometer is only ever ANDed
    //  with it. Nothing downstream may read INT1, the AWAKE bit, or the
    //  ADXL367 in any form and act on it directly - in the product this
    //  output switches a voltage, and a device that fires while
    //  deactivated is dangerous.
    //
    //  Every consumer must call IsOutputActive(). OutputSwitch (below) is the
    //  example future consumers - the alarm report, event counter, anything
    //  else - copy. If a future change needs a different condition, change it
    //  HERE so every consumer moves together.
    // ================================================================
    bool awake { gpio_pin_get_dt(&s_adxl_int1) > 0 };

    // Release the stale-trigger suppression only once the part has actually gone
    // back to sleep. See setArmState() - this is what makes arming edge-triggered.
    if (m_ignore_stale_trigger && !awake) {
      m_ignore_stale_trigger = false;
      LOG_INF("ADXL cleared after arming - device is now live.");
    }

    // RISING EDGES, not levels. AWAKE stays asserted for the whole inactivity
    // period, so counting the level would add one activation per loop tick.
    bool risingEdge { awake && !m_previous_awake && !m_ignore_stale_trigger && !m_in_cooldown };
    m_previous_awake = awake;

    if (risingEdge) {
      m_activation_count++;
      LOG_INF("Activation %u of %u.", m_activation_count, m_settings.Activations());

      if (m_activation_count >= m_settings.Activations()) {
        m_detection_met    = true;
        m_activation_count = 0;
        // No blanking here. Standing the ADXL down at the moment of trigger
        // would cut short the assertion that IS the output's 5 s duration.
      } else {
        // Result not checked here - beginCooldown() already logs its own
        // failure, and on failure it has itself restored detection.
        beginCooldown();
      }
    }

    // The trigger's own AWAKE running to completion is what clears detection.
    if (m_detection_met && !awake) { m_detection_met = false; }

    m_output_active = (m_arm_state == ArmState::Active) && m_detection_met;

    // The fire output is driven HERE, in the same breath as the condition is
    // derived, rather than from the main loop. A consumer that lives at the
    // derivation point cannot be forgotten by a future edit to the loop, and
    // there is no second call site that could disagree with this one.
    m_output_switch.Set(m_output_active);

    // Stuck-AWAKE watchdog. Defence in depth: if the accelerometer somehow holds
    // AWAKE far beyond its configured inactivity period, the device stops
    // triggering and - worse - does so SILENTLY, with no LED and no log. That is
    // an unacceptable failure mode for an alarm sensor, so recover rather than
    // sit dead. Re-running the loop configuration includes the bootstrap that
    // guarantees AWAKE clears.
    // Only while armed: a deactivated device holds the part in standby, where the
    // loop engine is stopped and AWAKE is necessarily clear.
    if (m_arm_state == ArmState::Active && awake) {
      if (++m_awake_ticks >= M_AWAKE_STUCK_TICKS) {
        m_awake_ticks = 0;
        LOG_ERR("ADXL stuck AWAKE for %u s - re-arming the loop engine!", M_AWAKE_STUCK_TICKS / 10U);
        if (m_accelerometer.ConfigureLoopMode(m_settings.ThresholdLsb(), CONFIG_MFS_ADXL_ACTIVITY_SAMPLES, CONFIG_MFS_ADXL_INACTIVITY_THRESHOLD,
                                              CONFIG_MFS_ADXL_INACTIVITY_SECS) < 0) {
          LOG_ERR("ADXL re-arm failed!");
        }
        m_ignore_stale_trigger = false;
        // A stuck level must not hold a detection - and so the output - open.
        m_detection_met = false;
      }
    } else {
      m_awake_ticks = 0;
    }
  }

  void App::setArmState(ArmState state)
  {
    // EDGE-TRIGGERED ARMING - SAFETY CRITICAL.
    //
    // The ADXL367 AWAKE bit is a LEVEL, not a latch: once motion has occurred it
    // stays asserted for the whole inactivity period and cannot be cleared by
    // reading STATUS. So arming a continuously-running part would take that
    // assertion - which belongs to motion from BEFORE arming - as an immediate
    // trigger, and the device would fire the instant it was armed.
    //
    // This is the common case, not an edge case: an engineer handling the device
    // in order to arm it IS motion, so AWAKE is very often asserted at that
    // moment. In the product the trigger switches a voltage, so a false fire on
    // activation is dangerous, not merely untidy.
    //
    // The part is therefore STOPPED while the device is deactivated and
    // configured afresh when it is activated. The configuration routine drives
    // AWAKE low, so there is no stale level to inherit. alc_drawer_master solves
    // the equivalent problem differently - it uses latched activity, so it clears
    // the latch immediately before arming (ReadActivityLatched) - but a latch
    // clear has no effect on a level.
    if (state == ArmState::Active) {
      // Sensor first, boolean second. A device that cannot configure its
      // accelerometer must NOT report itself armed: it would be a silent loss of
      // function. It stays Inactive with LED A lit, so the refusal is visible.
      if (enableAccelerometer() < 0) {
        LOG_ERR("Arm request rejected - device stays Inactive!");
        return;
      }
      m_arm_state = ArmState::Active;
    } else {
      // Boolean first, sensor second - see disableAccelerometer().
      m_arm_state = ArmState::Inactive;
      disableAccelerometer();

      // Whatever was counted or latched belongs to the armed session that just
      // ended. Cleared AFTER disableAccelerometer() because its re-derivation
      // tick can itself count an edge and start a cooldown - clearing first would
      // leave that tick's work in place.
      m_activation_count = 0;
      m_detection_met    = false;
      m_previous_awake   = false;
      if (m_in_cooldown) {
        m_in_cooldown      = false;
        m_cooldown_expired = false;
        if (m_pmic.TimerStop() < 0) { LOG_ERR("Failed to stop the cooldown timer on deactivation!"); }
      }
    }

    // Deliberately says nothing about the LEDs: the main loop logs their actual
    // applied values. An earlier version asserted "LED A ON" here from the arm
    // state alone, which was wrong in any build that does not drive LED A.
    LOG_INF("Arm state: %s (uptime %lld ms).", state == ArmState::Active ? "Active" : "Inactive", k_uptime_get());
  }

  int App::beginCooldown()
  {
    uint16_t seconds { m_settings.CooldownSeconds() };
    int result { 0 };

    if (seconds == 0) { return 0; }

    // Stand the accelerometer down for the window. Leaving it running would let
    // a continuous disturbance hold AWAKE asserted right through the blanking
    // period, so the re-arm would inherit a stale level - exactly the bug commit
    // 0a50910 fixed for the arming path.
    result = m_accelerometer.Standby();
    if (result < 0) {
      LOG_ERR("Failed to stand the ADXL down for cooldown: %d!", result);
      return result;
    }

    result = m_pmic.TimerStop();
    if (result == 0) { result = m_pmic.TimerSetMode(Npm2100::TimerMode::GeneralPurpose); }
    if (result == 0) { result = m_pmic.TimerSetDurationMs(static_cast<uint32_t>(seconds) * MSEC_PER_SEC); }
    if (result == 0) { result = m_pmic.TimerClearExpiredEvent(); }
    if (result == 0) { result = m_pmic.TimerStart(); }
    if (result < 0) {
      LOG_ERR("Failed to start the cooldown timer: %d!", result);

      // Fail TOWARD detecting - no blanking this time - rather than leave the
      // part standing down with nothing left to bring it back up.
      if (m_pmic.TimerStop() < 0) { LOG_WRN("Could not stop the cooldown timer after a failure."); }
      if (enableAccelerometer() < 0) { LOG_ERR("Could not restore detection after the cooldown timer failed!"); }
      return result;
    }

    m_in_cooldown            = true;
    m_cooldown_rearm_failed  = false;
    m_cooldown_expired       = false;
    m_cooldown_next_retry_ms = 0;

    // Forced over regardless of the PMIC - see m_cooldown_deadline_ms. The TIMER
    // block is +-10%, plus a fixed grace for loop scheduling jitter.
    m_cooldown_deadline_ms = k_uptime_get() + static_cast<int64_t>(seconds) * MSEC_PER_SEC +
                             (static_cast<int64_t>(seconds) * MSEC_PER_SEC) / M_COOLDOWN_TOLERANCE_DIVISOR + M_COOLDOWN_GRACE_MS;

    LOG_INF("Cooldown started: %u s.", seconds);
    return 0;
  }

  void App::serviceCooldown()
  {
    bool expired { false };
    bool pmicExpired { false };

    if (!m_in_cooldown) { return; }

    // Only consult the PMIC until the cooldown is confirmed over. Once
    // m_cooldown_expired is set the window itself has already ended - the timer's
    // expiry event is already cleared - and everything left is the re-arm retry,
    // which has nothing to do with the PMIC timer.
    if (!m_cooldown_expired) {
      // A TimerIsExpired() error counts as not-expired from the PMIC, but the
      // deadline below still applies - it is the fallback for exactly this case.
      pmicExpired = (m_pmic.TimerIsExpired(expired) == 0) && expired;
      expired     = pmicExpired || (k_uptime_get() >= m_cooldown_deadline_ms);
      if (!expired) { return; }

      // Logged only on this transition, not on every later retry tick.
      if (!pmicExpired) { LOG_WRN("Cooldown forced over by the deadline - the PMIC timer did not report expiry!"); }
      if (m_pmic.TimerClearExpiredEvent() < 0) { LOG_WRN("Failed to clear the cooldown timer expiry event!"); }

      m_cooldown_expired = true;
    }

    // Rate-limited: one attempt per M_COOLDOWN_RETRY_MS rather than every 100 ms
    // poll tick, so a persistently failing re-arm cannot flood RTT via the
    // driver's own logging in enableAccelerometer() / ConfigureLoopMode().
    if (k_uptime_get() < m_cooldown_next_retry_ms) { return; }

    // Full bootstrap, not a bare restart. Re-arming must confirm AWAKE is clear
    // so the engine cannot inherit an assertion from during the blanking window.
    // m_in_cooldown is left set on failure so the re-arm is retried, rather than
    // stranding the ADXL in standby forever.
    if (enableAccelerometer() < 0) {
      if (!m_cooldown_rearm_failed) {
        LOG_ERR("Failed to re-arm the ADXL after cooldown - retrying!");
        m_cooldown_rearm_failed = true;
      }
      m_cooldown_next_retry_ms = k_uptime_get() + M_COOLDOWN_RETRY_MS;
      return;
    }

    m_in_cooldown      = false;
    m_cooldown_expired = false;
    m_previous_awake   = false;
    LOG_INF("Cooldown elapsed - detection re-armed.");
  }

  int App::initAccess()
  {
    int result { 0 };
    AccessState restored {};

    if (!credentials::ParseDeviceId(CONFIG_MFS_DEVICE_ID, s_device_id) ||
        !credentials::ParseHexBytes(CONFIG_MFS_DEVICE_SECRET, s_device_secret, sizeof(s_device_secret)) ||
        !credentials::ParseHexBytes(CONFIG_MFS_PROVISION_KEY, s_provision_key, sizeof(s_provision_key))) {
      LOG_ERR("Credentials missing or malformed - build with -DEXTRA_CONF_FILE=credentials.conf!");
      return -EINVAL;
    }

    // An all-zero key is a template nobody filled in, and would be shared by every
    // such build. The two keys must also differ, or the provisioner holds entry.
    if (credentials::IsAllZero(s_device_secret, sizeof(s_device_secret)) || credentials::IsAllZero(s_provision_key, sizeof(s_provision_key)) ||
        memcmp(s_device_secret, s_provision_key, sizeof(s_device_secret)) == 0) {
      LOG_ERR("Device secret and provisioning key must be set and must differ!");
      return -EINVAL;
    }

    result = crypto::Init();
    if (result < 0) { return result; }

    result = crypto::RunSelfTest();
    if (result < 0) { return result; }

    result = settings_subsys_init();
    if (result < 0) {
      LOG_ERR("settings_subsys_init failed: %d!", result);
      return result;
    }

    result = access_store::Load(restored);
    if (result < 0) { return result; }

    // The device ID is known only now, so AccessControl's tables are rebuilt
    // against it. The restored day is the floor a provisioner sync may not go
    // below; the clock itself stays INVALID - there is no resume from NVS.
    m_access.SetDeviceId(s_device_id);
    if (result > 0) {
      m_access.Restore(restored);
      m_clock.RaiseFloorDay(restored.day);
      LOG_INF("Access state restored: day floor %u.", restored.day);
    } else {
      LOG_INF("No access state stored - first boot, no day floor.");
    }

    m_access_ready = true;
    LOG_INF("Device 0x%08X ready. Clock INVALID until a provisioner time sync.", s_device_id);
    return 0;
  }

  void App::serviceCandidates()
  {
    CommandScanner::Candidate candidate {};
    int64_t uptimeSecs { k_uptime_get() / MSEC_PER_SEC };
    uint32_t dropped { 0 };

    while (m_scanner.TakeCandidate(candidate)) {
      if (!m_access_ready) { continue; }

      if (!m_clock.IsValid()) {
        handleTimeSyncCandidate(candidate, uptimeSecs);
      } else {
        handleCommandCandidate(candidate, uptimeSecs);
      }
    }

    // Counted on the Bluetooth RX thread, logged here instead - see
    // CommandScanner::TakeDroppedCount().
    dropped = m_scanner.TakeDroppedCount();
    if (dropped > 0) { LOG_WRN("Candidate queue full - %u adverts dropped!", dropped); }
  }

  void App::handleTimeSyncCandidate(const CommandScanner::Candidate& candidate, int64_t uptimeSecs)
  {
    uint32_t unixSeconds { 0 };
    DeviceClock::SyncResult syncResult { DeviceClock::SyncResult::BeforeEpoch };

    // Almost every advert in range lands here and fails the tag. That is routine
    // and is not logged, or the RTT buffer would fill with other people's phones.
    if (!access::OpenTimeSync(s_provision_key, s_device_id, candidate.bytes, unixSeconds)) { return; }

    syncResult = m_clock.ApplyProvisionerSync(unixSeconds, uptimeSecs);
    if (syncResult != DeviceClock::SyncResult::Applied) {
      LOG_WRN("Authentic time sync refused: result %u, unix %u, floor %u!", static_cast<unsigned>(syncResult), unixSeconds, m_clock.FloorDay());
      return;
    }

    LOG_INF("Clock set by provisioner: unix %u, day %u, %02u:%02u UTC.", unixSeconds, m_clock.DayIndex(uptimeSecs),
            m_clock.MinuteOfDay(uptimeSecs) / 60U, m_clock.MinuteOfDay(uptimeSecs) % 60U);

    if (m_access.Advance(m_clock, uptimeSecs) < 0) { LOG_ERR("Day rollover could not be persisted after the time sync!"); }
  }

  void App::handleCommandCandidate(const CommandScanner::Candidate& candidate, int64_t uptimeSecs)
  {
    AccessControl::Evaluation evaluation { m_access.Evaluate(candidate.bytes, sizeof(candidate.bytes), m_clock, uptimeSecs) };

    if (evaluation.verdict == AccessControl::Verdict::NotForUs) { return; }

    if (evaluation.verdict != AccessControl::Verdict::Accepted) {
      // RTT only. Nothing on the radio, nothing on the LEDs.
      LOG_WRN("Command rejected: %s (failures %u).", verdictName(evaluation.verdict), m_access.ConsecutiveFailures());
      return;
    }

    // Decoded field by field on purpose: when a slider produces the wrong byte
    // this is where you see it, rather than inferring it from an LED.
    LOG_INF("Command slot %u n %u: arm %s, delay %u s, activations %u, mode %u, cooldown %u s, threshold %u LSB, minute %u.", evaluation.slot,
            evaluation.n, evaluation.command.armActive ? "ACTIVE" : "INACTIVE", protocol::DelayToSeconds(evaluation.command.delayCode),
            evaluation.command.activations, static_cast<unsigned>(evaluation.command.mode),
            protocol::CooldownToSeconds(evaluation.command.cooldownByte), protocol::SensitivityToThresholdLsb(evaluation.command.sensitivityByte),
            evaluation.command.minuteOfDay);

    // PHASE 2 SHIM, extended in Task 14 to carry the settings so the detection
    // engine can be bench-tested from the app. The clock trim, re-arm ordering and
    // LED patterns arrive in Task 16, which replaces this.
    m_settings.ApplyFrom(evaluation.command, evaluation.slot == access::M_SLOT_NETWORK_MANAGER);
    setArmState(evaluation.command.armActive ? ArmState::Active : ArmState::Inactive);
  }

  void App::serviceDayRollover()
  {
    int64_t uptimeSecs { k_uptime_get() / MSEC_PER_SEC };

    if (!m_access_ready || !m_clock.IsValid()) { return; }
    if (uptimeSecs - m_last_advance_secs < M_ADVANCE_INTERVAL_SECS) { return; }

    m_last_advance_secs = uptimeSecs;
    if (m_access.Advance(m_clock, uptimeSecs) < 0) { LOG_ERR("Day rollover could not be persisted!"); }
  }

  int App::applyLeds(bool ledA, bool ledB)
  {
    int result { gpio_pin_set_dt(&s_led_a, ledA ? 1 : 0) };

    if (result < 0) { return result; }
    return gpio_pin_set_dt(&s_led_b, ledB ? 1 : 0);
  }

}
