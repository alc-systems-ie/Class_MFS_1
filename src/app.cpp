#include <cstring>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/policy.h>

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

    // How often serviceScanHealth() retries starting the scan if it is down.
    // Only the retry is throttled - the detection engine tracks scanner loss
    // during an armed delay on every tick - since the Bluetooth stack's own
    // stop/start churn is not free and a genuine outage does not need a 100 ms
    // retry rate to recover promptly.
    constexpr int64_t M_SCAN_SERVICE_INTERVAL_MS { 1000 };

    // How often the persisted day floor is adopted from the clock with no command
    // involved. Keeps a device that receives no commands for days from later
    // accepting a stale captured provisioner sync against an old floor.
    constexpr int64_t M_ADVANCE_INTERVAL_SECS { 60 };

    // The detection engine's configure retry spacing, for log messages.
    constexpr uint32_t M_ENGINE_RETRY_MS { static_cast<uint32_t>(DetectionEngine::M_COOLDOWN_RETRY_MS) };

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

    // LED A — on while Inactive. LED B (bench only) — on while detection is met,
    // Inactive or Active, and suppressed while Arming; see the disarmed test mode
    // amendment section 2 and the arming sequence amendment section 3.
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

    const char* armStateName(ArmState state)
    {
      switch (state) {
        case ArmState::Inactive:
          return "Inactive";
        case ArmState::Arming:
          return "Arming";
        case ArmState::Active:
          return "Active";
      }
      return "Unknown"; // Unreachable while every ArmState is handled above - -Wswitch warns if a new one is added.
    }

    // The warning reason for a failed arming step.
    const char* armingStepReason(ArmingStep step)
    {
      switch (step) {
        case ArmingStep::DisablePins:
          return "fire pins could not be isolated";
        case ArmingStep::RestartDetection:
          return "arming failed - detection would not restart armed";
        case ArmingStep::EnablePins:
          return "arming failed - fire pins would not enable";
      }
      return "unknown arming step"; // Unreachable while every ArmingStep is handled above - -Wswitch warns if a new one is added.
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
      , m_output_active(false)
      , m_settings()
      , m_delay_timer {}
      , m_delay_pm_lock_held(false)
      , m_last_scan_service_ms(0)
      , m_scan_outage_logged(false)
      , m_logging_cooldown(false)
      , m_engine(*this)
      , m_arming(*this)
      , m_led_sequencer()
      , m_led_timer {}
      , m_initialised(false)
  {}

  int App::Run()
  {
    int result { 0 };
    bool previousTriggered { false };
    bool ledA { false };
    bool ledB { false };
#if defined(CONFIG_MFS_DEBUG_LED)
    bool previousDetection { false };
    int64_t detectionStartMs { 0 };
#endif
#if defined(CONFIG_MFS_BATTERY_TEST)
    uint32_t blinkTicks { 0 };
    uint32_t blinkOnTicks { 0 };
#endif

    LOG_INF("MFS_1 starting, serial %s.", CONFIG_ALC_DEVICE_SERIAL);

    k_timer_init(&m_delay_timer, nullptr, nullptr);
    k_timer_init(&m_led_timer, &App::ledTimerHandler, nullptr);
    k_timer_user_data_set(&m_led_timer, this);

    // THE FIRE OUTPUT IS CHECKED FIRST, before the I2C bus, the FEM or the LEDs.
    // It is not driven: the external 10k pull-downs hold both gates off from
    // reset, and Init() only reads each pin as a plain input - it must read low
    // - and leaves both disconnected. The pins get a driver only as the last
    // step of arming (arming sequence amendment section 1).
    //
    // A failed check does NOT stop the boot. The switch is then latched faulty
    // with its pins isolated, which is safe, and every arming attempt fails safe
    // to the warning - while commands, the LEDs and RTT keep the device
    // diagnosable.
    result = m_output_switch.Init();
    if (result < 0) {
      LOG_ERR("Fire output failed its boot check (%d) - pins isolated and latched faulty; booting on, but the device will NOT arm!", result);
      signalWarning("fire pins failed the boot check", result);
    }

    // Installed before anything can ask the switch to assert.
    m_output_switch.SetInterlock(&App::interlockThunk, this);

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

    // Cold start defaults to Inactive — see docs/v1-scope.md section 6. The pins
    // are already isolated by Init(); this is the ordinary disarm path, so the
    // boot state is reached exactly as every later disarm reaches it. Inactive
    // is not idle: this configures the ADXL367 and starts the detection test at
    // the stored settings (disarmed test mode amendment, section 3).
    (void)disarmDevice();

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
      serviceScanHealth();

      // The exit delay. At the deadline, in one synchronous call: restart
      // detection armed -> enable the fire pins -> Active. Before the output is
      // derived below, so nothing ticks the engine or derives the output between
      // the armed restart and Active. Only a completed arming is acknowledged.
      if (m_arming.Service(k_uptime_get())) {
        LOG_INF("Arm state: Active - fire pins enabled (uptime %lld ms).", k_uptime_get());
        playLedPattern(LedPattern::Armed);
      }

      // A failed arming has already failed safe and raised the warning inside
      // Service(). No LED A acknowledgement.
      logArmingFailure();

#if defined(CONFIG_MFS_BATTERY_TEST)
      if (++blinkTicks >= M_BLINK_PERIOD_TICKS) {
        blinkTicks   = 0;
        blinkOnTicks = M_BLINK_ON_TICKS;
      }
      ledA = blinkOnTicks > 0;
      if (blinkOnTicks > 0) { --blinkOnTicks; }
#endif

      // The ONE place the output state is derived. See updateOutputState().
      updateOutputState(EngineTick::Run);

      // Firing is one of the only two ways out of the armed state. Acted on here,
      // not inside updateOutputState(), because the disarm path re-enters it. The
      // flag is taken on every tick; the engine only raises it for an ARMED
      // output, and a disarmed test carries on.
      if (m_engine.TakeTriggerComplete() && m_arming.State() == ArmState::Active) {
        LOG_WRN("Trigger complete - latched Inactive. Re-arming needs an engineer command.");
        (void)disarmDevice();
      }

      // Compute the LED states HERE, once, so the log below reports what is
      // actually written to the pins. Recomputing them inside the log statement
      // from the arm state produced messages that contradicted the build - a
      // battery-test build never drives LED A, but the log still claimed "LED A ON".
      int64_t ledNowMs { k_uptime_get() };
      bool ledSequencerActive { m_led_sequencer.IsActive(ledNowMs) };

      // While a pattern plays, the 10 ms LED timer owns LED A exclusively - in
      // EVERY build, battery-test included, or the loop's write below would fight
      // the timer's. ledA is still computed so the transition log reports what is
      // actually lit; the pin write itself is skipped further down.
      if (ledSequencerActive) {
        ledA = m_led_sequencer.Level(ledNowMs);
      } else {
#if !defined(CONFIG_MFS_BATTERY_TEST)
#if defined(CONFIG_MFS_DEBUG_LED)
        // Bench only, between patterns: LED A is lit while Inactive, as before,
        // and dark while Arming - nothing is shown until the arm is acknowledged.
        ledA = (m_arming.State() == ArmState::Inactive);
#else
        // Production: between patterns LED A must be explicitly turned off - left
        // unassigned, it would stick at whatever level the last pattern ended on,
        // costing ~2 mA and breaking covertness.
        ledA = false;
#endif
#endif
        // Battery-test builds set ledA from the liveness blink above; this branch
        // must not override it between patterns.
      }

#if defined(CONFIG_MFS_DEBUG_LED)
      // LED B shows DETECTION for the 5 s ADXL loop period. Inactive it shows
      // test triggers; Active it confirms one. While Arming it is SUPPRESSED - the
      // engineer is walking away and nothing may be visible; the test running
      // underneath is discarded when arming completes or is cancelled. It is a
      // bench indicator, not an output consumer - OutputSwitch is the example
      // future consumers copy (design spec section 6.2).
      ledB = m_engine.DetectionMet() && (m_arming.State() != ArmState::Arming);

      // Logged on transitions of LED B itself, so the log follows the LED. While
      // Inactive nothing else reports a test trigger - the Output line below only
      // fires armed - so without this a test shows "Activation 3 of 3." and then
      // silence, and LED B is the only evidence (bench, 2026-09-14).
      if (ledB != previousDetection) {
        previousDetection = ledB;
        if (ledB) {
          detectionStartMs = ledNowMs;
          LOG_INF("Detection met (%s) - LED B on.", m_arming.State() == ArmState::Active ? "armed" : "test");
        } else {
          LOG_INF("Detection cleared after %lld ms - LED B off.", ledNowMs - detectionStartMs);
        }
      }
#endif

      // Log only on transitions. A periodic dump floods the 4 KB RTT buffer in
      // LOG_MODE_IMMEDIATE and silently drops the events that actually matter —
      // which is how the LED behaviour went unexplained for a whole test cycle.
      if (IsOutputActive() != previousTriggered) {
        previousTriggered = IsOutputActive();
        LOG_INF("Output %s. Arm %s, LED A %s, LED B %s.", previousTriggered ? "ASSERTED" : "cleared", armStateName(m_arming.State()),
                ledA ? "ON" : "off", ledB ? "ON" : "off");
      }

      // LED A is written here ONLY while no pattern is playing - see
      // ledSequencerActive above. LED B has no second writer, so it is always
      // written from the loop.
      result = applyLedB(ledB);
      if (result == 0 && !ledSequencerActive) { result = applyLedA(ledA); }
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
    // into. LDOSW drops to ULP right after the probe below, before the
    // accelerometer is ever configured — see App::lowerLsoutToUlp(), called from
    // App::Run() straight after initAccelerometer() — so the idle saving is kept.
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
    // Called right after initAccelerometer(), which only probes the part and
    // parks it in standby - well before the detection engine's first loop-mode
    // configure in the boot disarmDevice() below. Standby current is already
    // uA-level, so LDOSW no longer needs High Power here; the loop-mode current
    // once configured stays within ULP's headroom too. ULP still supplies up to
    // 2 mA. Auto is not used: it follows the device mode, and MFS_1 stays in
    // Active mode permanently, so Auto would hold High Power for the device's
    // whole life.
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

    // Probe only, with both interrupt pins parked safe. The loop engine is not
    // configured here: the detection engine configures it afresh on every restart
    // - the first at boot, when the device starts Inactive and testing.
    result = m_accelerometer.Standby();
    if (result < 0) {
      LOG_ERR("Failed to put the accelerometer in standby: %d!", result);
      return result;
    }

    LOG_INF("ADXL367 probed and parked until the detection engine configures it.");
    return 0;
  }

  void App::updateOutputState(EngineTick tick)
  {
    // ================================================================
    //  THE SINGLE SOURCE OF TRUTH FOR THE DEVICE OUTPUT.
    //
    //  THE ARM STATE IS DEFINITIVE. The accelerometer is only ever ANDed
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

    // The detection engine counts, cools down, delays and runs the stuck-AWAKE
    // watchdog identically armed and disarmed - it never sees the output. While
    // Arming it runs disarmed: the test carries on invisibly. The
    // disarm path skips the tick: it must take the output low without counting
    // an edge or starting a cooldown on the way, and restarts the engine next.
    if (tick == EngineTick::Run) { m_engine.Tick(detectionSettings(), m_arming.State() == ArmState::Active, awake, k_uptime_get()); }

    // LAYER ONE of the delay interlock is the last term: while a delay is
    // pending - armed trigger or test - detection cannot reach the output.
    m_output_active = (m_arming.State() == ArmState::Active) && m_engine.DetectionMet() && delayPermitsFiring();

    // The fire output is driven HERE, in the same breath as the condition is
    // derived, rather than from the main loop. A consumer that lives at the
    // derivation point cannot be forgotten by a future edit to the loop, and
    // there is no second call site that could disagree with this one. While the
    // device is not Active the pins are disabled and Set(false) does nothing.
    m_output_switch.Set(m_output_active);

    // ONE-SHOT. The engine tracks the output App actually derived, and flags the
    // trigger complete once it has asserted and ended - acted on in the main loop.
    m_engine.NoteOutput(m_output_active);
  }

  bool App::disarmDevice()
  {
    // ================================================================
    //  DISARM ORDER - SAFETY CRITICAL (arming sequence amendment 2).
    //
    //  ArmingSequence::Disarm() disables the fire pins FIRST - driven
    //  low, then disconnected - then sets Inactive and cancels any
    //  arming, then calls RestartDetection(false), which re-derives the
    //  output through the single derivation point and only then restarts
    //  the engine. The pins go safe before any state changes. The pending
    //  state is deliberately not persisted, so a reset loses a pending
    //  trigger too - the fail-safe direction.
    // ================================================================
    bool cancelled { m_arming.Disarm() };

    logArmingFailure();

    // Deliberately says nothing about the LEDs: the main loop logs their actual
    // applied values.
    LOG_INF("Arm state: Inactive%s (uptime %lld ms).", cancelled ? " - arming cancelled" : "", k_uptime_get());
    return cancelled;
  }

  void App::logArmingFailure()
  {
    ArmingStep step { ArmingStep::DisablePins };
    int result { 0 };

    if (!m_arming.TakeFailure(step, result)) { return; }

    switch (step) {
      case ArmingStep::DisablePins:
        LOG_ERR("Fire pin disable failed (%d) - fire pins may NOT be isolated!", result);
        break;

      case ArmingStep::RestartDetection:
        LOG_ERR("Arming failed at the detection restart (%d) - device Inactive, fire pins disabled, no acknowledgement!", result);
        break;

      case ArmingStep::EnablePins:
        LOG_ERR("Arming failed at the fire pin enable (%d) - device Inactive, fire pins disabled, no acknowledgement!", result);
        break;
    }
  }

  void App::signalWarning(const char* reason, int result)
  {
    LOG_ERR("WARNING (light TBC): %s (%d)!", reason, result);
  }

  int App::DisableFirePins()
  {
    // Must not call back into m_arming - it runs inside the fail-safe itself.
    return m_output_switch.Disable();
  }

  int App::RestartDetection(bool armed)
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
    // The part runs in every arm state (disarmed test mode amendment), so the
    // mechanism is RECONFIGURE ON EVERY RESTART: every transition restarts the
    // detection engine from zero, which configures the part afresh through the
    // loop-mode bootstrap. That drives AWAKE low, so there is no stale level to
    // inherit, and any AWAKE still reported after it is suppressed until a fresh
    // edge. The restart also discards the test's count, cooldown, delay and
    // detection latch, so nothing from a test can reach the armed output
    // (amendment section 3.2). alc_drawer_master solves the equivalent problem
    // differently - it uses latched activity, so it clears the latch immediately
    // before arming (ReadActivityLatched) - but a latch clear has no effect on a
    // level.
    //
    // Must not call back into m_arming. Nothing below services the scanner queue,
    // so no command can be handled from inside it.
    int result { 0 };

    if (!armed) {
      // Disarm order step 3. The state is already Inactive, so the output
      // re-derives false without an engine tick - no edge counted, no cooldown
      // started - and Set(false) on the disabled switch does nothing.
      updateOutputState(EngineTick::Skip);
    }

    // Whatever was counted, latched or pending belongs to the session that just
    // ended. Armed, the state is still Arming, so nothing derives an output from
    // this session until the pins are enabled and the state is Active.
    result = restartEngine(armed);
    if (result < 0) {
      if (armed) {
        LOG_ERR("Arming: the accelerometer would not configure (%d) - arming fails safe!", result);
      } else {
        LOG_ERR("Detection test could not start - retrying the accelerometer every %u ms!", M_ENGINE_RETRY_MS);
      }
    }
    return result;
  }

  int App::EnableFirePins()
  {
    // The LAST step of arming. Must not call back into m_arming.
    return m_output_switch.Enable();
  }

  void App::SignalWarning(ArmingStep step, int result)
  {
    // Must not call back into m_arming.
    signalWarning(armingStepReason(step), result);
  }

  DetectionSettings App::detectionSettings() const
  {
    return DetectionSettings { m_settings.Activations(), m_settings.CooldownSeconds(), m_settings.DelaySeconds(), m_settings.ThresholdLsb() };
  }

  int App::restartEngine(bool armed)
  {
    // A restart discards any cooldown, so the next CooldownElapsed can only
    // belong to a configure retry unless a new cooldown starts first.
    m_logging_cooldown = false;
    return m_engine.Restart(detectionSettings(), armed, k_uptime_get());
  }

  bool App::delayPermitsFiring() const
  {
    return m_engine.DelayPermitsFiring();
  }

  bool App::interlockThunk(void* context)
  {
    return static_cast<const App*>(context)->delayPermitsFiring();
  }

  void App::serviceScanHealth()
  {
    int64_t uptimeMs { k_uptime_get() };

    // Throttled - the Bluetooth stack's own stop/start churn is not free, and a
    // genuine outage does not need a 100 ms retry rate to recover promptly.
    // Scanner loss during an armed delay is not tracked here: the detection
    // engine checks ScannerRunning() itself on every tick.
    if (uptimeMs - m_last_scan_service_ms < M_SCAN_SERVICE_INTERVAL_MS) { return; }
    m_last_scan_service_ms = uptimeMs;

    // Health and retry are keyed on the REQUESTED cadence, not merely
    // "running": a scan that is running but stuck at the fallback cadence
    // (a failed SetFastScan() whose fallback succeeded) is a silent failure
    // that IsScanning() alone would miss entirely.
    if (m_scanner.IsAtRequestedCadence()) {
      m_scan_outage_logged = false;
      return;
    }

    m_scanner.ServiceScan();
    if (!m_scanner.IsAtRequestedCadence() && !m_scan_outage_logged) {
      LOG_ERR("Scanner not at the requested cadence - retrying!");
      m_scan_outage_logged = true;
    }
  }

  int App::ConfigureAccelerometer(uint16_t thresholdLsb, bool& awake)
  {
    int result { 0 };

    // Configuring IS the clear: the datasheet's loop mode initialization routine
    // soft-resets the part and forces one activity/inactivity cycle, which drives
    // AWAKE low and captures a valid reference. Doing it on every restart also
    // means the reference is always taken in the orientation the device is
    // actually left in.
    result = m_accelerometer.ConfigureLoopMode(thresholdLsb, CONFIG_MFS_ADXL_ACTIVITY_SAMPLES, CONFIG_MFS_ADXL_INACTIVITY_THRESHOLD,
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

    return 0;
  }

  int App::StandbyAccelerometer()
  {
    // Standby stops the loop engine and de-asserts INT1. The rail stays up: LSOUT
    // is shared and power-cycling it would cost the ADXL367's fuse-load sequence
    // and the 100 ms settling delay on every configure.
    return m_accelerometer.Standby();
  }

  int App::StartCooldownTimer(uint32_t durationMs)
  {
    int result { m_pmic.TimerStop() };

    if (result == 0) { result = m_pmic.TimerSetMode(Npm2100::TimerMode::GeneralPurpose); }
    if (result == 0) { result = m_pmic.TimerSetDurationMs(durationMs); }
    if (result == 0) { result = m_pmic.TimerClearExpiredEvent(); }
    if (result == 0) { result = m_pmic.TimerStart(); }
    return result;
  }

  int App::StopCooldownTimer()
  {
    return m_pmic.TimerStop();
  }

  int App::CooldownTimerExpired(bool& expired)
  {
    return m_pmic.TimerIsExpired(expired);
  }

  int App::ClearCooldownTimerEvent()
  {
    return m_pmic.TimerClearExpiredEvent();
  }

  void App::StartDelayTimer(uint32_t durationMs)
  {
    // GRTC, not the PMIC timer. At +/-10% over temperature the PMIC would put a
    // 9-hour delay anywhere inside a 108-minute window.
    k_timer_start(&m_delay_timer, K_MSEC(durationMs), K_NO_WAIT);
  }

  void App::StopDelayTimer()
  {
    k_timer_stop(&m_delay_timer);
  }

  bool App::DelayTimerRunning() const
  {
    // k_timer_remaining_ticks() reads 0 both for "expired" and for "never
    // started"; the engine's deadline is what tells the two apart.
    return k_timer_remaining_ticks(&m_delay_timer) != 0;
  }

  int App::SetTriggerPendingScan(bool fast)
  {
    // Stay awake while an armed trigger is pending - only effective when
    // CONFIG_PM is enabled (it is off in this build; the continuous scan below is
    // what actually keeps a disarm heard promptly). Battery life is explicitly
    // not a factor while a trigger is pending.
    if (fast && !m_delay_pm_lock_held) {
      pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
      m_delay_pm_lock_held = true;
    } else if (!fast && m_delay_pm_lock_held) {
      pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
      m_delay_pm_lock_held = false;
    }

    // Scan continuously while armed and pending. The deactivate path is the most
    // important thing the device does while a trigger is pending, and at the
    // normal 6 s cadence an abort takes ~30 s to be heard with confidence. A
    // failure is reported by the engine (DelayFastScanFailed or
    // DelayScanRestoreFailed) and retried by serviceScanHealth().
    return m_scanner.SetFastScan(fast);
  }

  bool App::ScannerRunning() const
  {
    return m_scanner.IsScanning();
  }

  void App::OnDetectionEvent(const DetectionEvent& event)
  {
    // Every log line the detection engine produces. Called synchronously from
    // inside Tick() and Restart(), at the point in the sequence where App logged
    // the same thing before the extraction. A disarmed delay is a TEST and never
    // logs as a pending trigger.
    switch (event.type) {
      case DetectionEventType::StaleAwakeSuppressed:
        LOG_WRN("ADXL still awake after configuring - suppressing until it clears!");
        break;

      case DetectionEventType::StaleAwakeReleased:
        if (event.armed) {
          LOG_INF("ADXL cleared after arming - device is now live.");
        } else {
          LOG_INF("ADXL cleared after configuring - test is now live.");
        }
        break;

      case DetectionEventType::Activation:
        LOG_INF("Activation %u of %u.", event.count, event.limit);
        break;

      case DetectionEventType::CooldownStarted:
        m_logging_cooldown = true;
        LOG_INF("Cooldown started: %u s.", event.seconds);
        break;

      case DetectionEventType::CooldownStandbyFailed:
        LOG_ERR("Failed to stand the ADXL down for cooldown: %d - no cooldown, reconfiguring within %u ms!", static_cast<int>(event.result),
                M_ENGINE_RETRY_MS);
        break;

      case DetectionEventType::CooldownTimerFailed:
        LOG_ERR("Failed to start the cooldown timer: %d!", static_cast<int>(event.result));
        break;

      case DetectionEventType::CooldownTimerStopFailed:
        LOG_WRN("Could not stop the cooldown timer after a failure.");
        break;

      case DetectionEventType::CooldownRestoreFailed:
        LOG_ERR("Could not restore detection after the cooldown timer failed - retrying every %u ms!", M_ENGINE_RETRY_MS);
        break;

      case DetectionEventType::CooldownForced:
        LOG_WRN("Cooldown forced over by the deadline - the PMIC timer did not report expiry!");
        break;

      case DetectionEventType::CooldownClearEventFailed:
        LOG_WRN("Failed to clear the cooldown timer expiry event!");
        break;

      case DetectionEventType::RearmFailed:
        if (m_logging_cooldown) {
          LOG_ERR("Failed to re-arm the ADXL after cooldown - retrying!");
        } else {
          LOG_ERR("Failed to reconfigure the ADXL - retrying every %u ms!", M_ENGINE_RETRY_MS);
        }
        break;

      case DetectionEventType::CooldownElapsed:
        // The engine runs a configure retry through its cooldown path, so this
        // also ends a retry. Only a cooldown that actually started logs as one.
        if (m_logging_cooldown) {
          LOG_INF("Cooldown elapsed - detection re-armed.");
        } else {
          LOG_INF("ADXL reconfigured - detection restored.");
        }
        m_logging_cooldown = false;
        break;

      case DetectionEventType::DelayStarted:
        if (event.armed) {
          LOG_WRN("TRIGGER PENDING: firing in %u s. Deactivating cancels it.", event.seconds);
        } else {
          LOG_INF("TEST trigger pending: LED B in %u s.", event.seconds);
        }
        break;

      case DetectionEventType::DelayScanLost:
        LOG_ERR("Scanner not running at the start of a delay - a disarm may not be heard!");
        break;

      case DetectionEventType::DelayFastScanFailed:
        LOG_WRN("Continuous scan unavailable at the start of a delay - retrying; a disarm is still heard at the duty-cycled rate.");
        break;

      case DetectionEventType::DelayScanRestoreFailed:
        // serviceScanHealth() keeps retrying - a failed cadence change here must
        // not be silently lost.
        LOG_ERR("Failed to restore duty-cycled scanning: %d!", static_cast<int>(event.result));
        break;

      case DetectionEventType::DelayExpired:
        // Armed, the output assertion itself is what gets logged, as before.
        if (!event.armed) { LOG_INF("TEST delay elapsed - LED B on."); }
        break;

      case DetectionEventType::DelayExpiredScanLost:
        LOG_WRN("Trigger firing although the scanner was not running during the delay - a disarm may have been missed.");
        break;

      case DetectionEventType::WatchdogRearm:
        LOG_ERR("ADXL stuck AWAKE for %u s - re-arming the loop engine!", event.seconds);
        break;

      case DetectionEventType::WatchdogRearmFailed:
        // The engine hands over to its configure retry, so this is no cooldown.
        m_logging_cooldown = false;
        LOG_ERR("ADXL re-arm failed!");
        break;

      case DetectionEventType::RestartCooldownStopFailed:
        if (event.armed) {
          LOG_WRN("Could not stop the test cooldown timer before arming.");
        } else {
          LOG_ERR("Failed to stop the cooldown timer on restarting the test!");
        }
        break;

      case DetectionEventType::RestartStandbyFailed:
        if (event.armed) {
          LOG_WRN("Could not stand the accelerometer down after a refused arm.");
        } else {
          LOG_WRN("Could not stand the accelerometer down after a failed test restart.");
        }
        break;
    }
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
    // this is where you see it, rather than inferring it from an LED. Arm and
    // Disarm carry no settings fields to decode, so their line is just type and
    // minute.
    if (evaluation.command.type == protocol::CommandType::Settings) {
      LOG_INF("Command slot %u n %u: %s, delay %u s, activations %u, mode %u, cooldown %u s, threshold %u LSB, minute %u.", evaluation.slot,
              evaluation.n, protocol::CommandTypeName(evaluation.command.type), protocol::DelayToSeconds(evaluation.command.delayCode),
              evaluation.command.activations, static_cast<unsigned>(evaluation.command.mode),
              protocol::CooldownToSeconds(evaluation.command.cooldownByte), protocol::SensitivityToThresholdLsb(evaluation.command.sensitivityByte),
              evaluation.command.minuteOfDay);
    } else {
      LOG_INF("Command slot %u n %u: %s, minute %u.", evaluation.slot, evaluation.n, protocol::CommandTypeName(evaluation.command.type),
              evaluation.command.minuteOfDay);
    }

    applyCommand(evaluation, uptimeSecs);
  }

  void App::serviceDayRollover()
  {
    int64_t uptimeSecs { k_uptime_get() / MSEC_PER_SEC };

    if (!m_access_ready || !m_clock.IsValid()) { return; }
    if (uptimeSecs - m_last_advance_secs < M_ADVANCE_INTERVAL_SECS) { return; }

    m_last_advance_secs = uptimeSecs;
    if (m_access.Advance(m_clock, uptimeSecs) < 0) { LOG_ERR("Day rollover could not be persisted!"); }
  }

  void App::applyCommand(const AccessControl::Evaluation& evaluation, int64_t uptimeSecs)
  {
    const protocol::Command& command { evaluation.command };
    bool fromNetworkManager { evaluation.slot == access::M_SLOT_NETWORK_MANAGER };
    ArmDecision decision { DecideCommand(m_arming.State(), fromNetworkManager, command) };
    protocol::Mode previousMode { m_settings.OperatingMode() };
    bool armedDelayWasPending { m_engine.DelayPendingArmed() };
    int result { 0 };
    LedPattern pattern { LedPattern::None };

    // THE SINGLE PATH. Everything below acts on `decision` and on nothing else -
    // see DecideCommand(). On command, an armed or arming device only ever disarms.
    //
    // Ignore has two sources, told apart by type for the log only: a reserved
    // type, or an Arm or Settings during the exit delay. Neither changes
    // anything - no LED, no trim, no settings.
    if (decision.action == ArmAction::Ignore) {
      if (command.type == protocol::CommandType::Reserved) {
        LOG_WRN("Command slot %u n %u has a reserved type - ignored.", evaluation.slot, evaluation.n);
      } else {
        LOG_INF("Arming: %s from slot %u n %u ignored - only a disarm is accepted while arming.", protocol::CommandTypeName(command.type),
                evaluation.slot, evaluation.n);
      }
      return;
    }

    // Armed, and not a disarm: nothing changes - no settings, mode or trim - but
    // LED A replays Armed so an engineer who did not know the state learns it.
    // The command authenticated, so this is not a breach of silence on failure.
    if (decision.action == ArmAction::ReplayArmed) {
      LOG_INF("Armed: %s from slot %u n %u changes nothing - replaying Armed.", protocol::CommandTypeName(command.type), evaluation.slot,
              evaluation.n);
      playLedPattern(LedPattern::Armed);
      return;
    }

    if (decision.trimClock && m_clock.ApplyMinuteHint(command.minuteOfDay, uptimeSecs) == DeviceClock::TrimResult::Trimmed) {
      LOG_INF("Clock trimmed from slot %u: now minute %u.", evaluation.slot, m_clock.MinuteOfDay(uptimeSecs));
    }

    if (decision.applySettings) {
      bool applyMode { decision.applyMode };

      if (command.mode != protocol::Mode::TriggerOnly && !decision.applyMode) {
        LOG_WRN("Mode field from slot %u ignored - only the Network Manager may change the mode.", evaluation.slot);
      }

      // Report modes are not implemented yet (Task 18). A Network Manager
      // command asking for one is refused outright - applied with mode changes
      // disallowed, so the device stays Trigger only rather than storing a
      // mode it will never honour.
      if (decision.applyMode && command.mode != protocol::Mode::TriggerOnly) {
        LOG_WRN("Mode %u refused - reporting is not implemented yet; device stays Trigger only.", static_cast<unsigned>(command.mode));
        applyMode = false;
      }

      m_settings.ApplyFrom(command, applyMode);
    }

    switch (decision.action) {
      case ArmAction::Disarm:
        // Any state. From Active this is the only state change a command can
        // make; from Arming it cancels the exit delay; from Inactive it is the
        // same deactivation, which restarts the test from zero - the engineer's
        // way to reset a long cooldown or delay (amendment section 3.1). A
        // disarm carries no settings.
        //
        // The double blink means a pending TRIGGER was cancelled, so it plays
        // only for an ARMED delay, read before disarming. Cancelling a test
        // delay is not a cancelled trigger, and while Arming the engine runs
        // disarmed, so a cancelled arming plays the plain Disarmed flash.
        if (disarmDevice()) { LOG_INF("Arming cancelled by slot %u n %u.", evaluation.slot, evaluation.n); }
        pattern = armedDelayWasPending ? LedPattern::DisarmedDelayCancelled : LedPattern::Disarmed;
        if (armedDelayWasPending) { LOG_WRN("Disarmed with a trigger PENDING - the trigger is cancelled."); }
        break;

      case ArmAction::Arm:
        // The command carries no settings; arming uses m_settings exactly as
        // already stored, never the command's own settings fields.
        //
        // Only the exit delay starts here. The pins stay isolated and NOTHING is
        // acknowledged: LED A stays silent until the main loop's Service() has
        // restarted detection armed and enabled the pins. A failure then fails
        // safe to the warning, not to LED A.
        if (!m_arming.BeginArming(k_uptime_get())) {
          LOG_ERR("Arming not started - device was not Inactive!");
          return;
        }

        // Stop any pattern still playing (e.g. a Disarmed flash from an
        // earlier command) so LED A shows nothing for the whole exit delay -
        // same stop-then-touch order as playLedPattern().
        k_timer_stop(&m_led_timer);
        m_led_sequencer.Stop();

        LOG_INF("Arming: fire pins isolated, arming in %u s.", static_cast<unsigned>(ArmingSequence::M_EXIT_DELAY_MS / MSEC_PER_SEC));
        break;

      case ArmAction::Tune:
        // Restart the test from zero at the new settings. Always, not only on
        // change - sending the same settings again is the engineer's way to
        // reset a long cooldown or delay (amendment section 3.1).
        result = restartEngine(false);
        if (result < 0) { LOG_ERR("Could not start the engine for the test - retrying every %u ms!", M_ENGINE_RETRY_MS); }
        pattern = (m_settings.OperatingMode() != previousMode) ? LedPattern::ModeChanged : LedPattern::SettingsApplied;
        break;

      default:
        return;
    }

    // No "mode stored but not implemented" warning here any more: applyMode
    // above refuses any mode but Trigger only at the point of application, so
    // m_settings.OperatingMode() can no longer hold anything else.

    LOG_INF("Applied: arm %s, %u activations, %u s cooldown, %u LSB, %u s delay.", armStateName(m_arming.State()), m_settings.Activations(),
            m_settings.CooldownSeconds(), m_settings.ThresholdLsb(), m_settings.DelaySeconds());

    // An accepted Arm has nothing to show yet - see ArmAction::Arm above.
    if (pattern != LedPattern::None) { playLedPattern(pattern); }
  }

  void App::playLedPattern(LedPattern pattern)
  {
    constexpr k_timeout_t M_LED_TICK { K_MSEC(10) };

    // Stop the timer before touching the sequencer, so the handler never reads it
    // half-written. A new command replaces whatever was playing.
    k_timer_stop(&m_led_timer);
    m_led_sequencer.Start(pattern, k_uptime_get());
    k_timer_start(&m_led_timer, K_NO_WAIT, M_LED_TICK);
  }

  void App::ledTimerHandler(struct k_timer* timer)
  {
    App* self { static_cast<App*>(k_timer_user_data_get(timer)) };
    int64_t now { k_uptime_get() };

    if (!self->m_led_sequencer.IsActive(now)) {
      k_timer_stop(timer);
      return;
    }

    // ISR context. gpio_pin_set_dt() is ISR-safe on the nRF GPIO driver.
    gpio_pin_set_dt(&s_led_a, self->m_led_sequencer.Level(now) ? 1 : 0);
  }

  int App::applyLedA(bool ledA)
  {
    return gpio_pin_set_dt(&s_led_a, ledA ? 1 : 0);
  }

  int App::applyLedB(bool ledB)
  {
    return gpio_pin_set_dt(&s_led_b, ledB ? 1 : 0);
  }

}
