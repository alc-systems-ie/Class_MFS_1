#pragma once
#include <cstdint>
#include <cstddef>

/** @file npm2100.hpp
 *  @brief Driver for Nordic nPM2100 PMIC. Zephyr-free; transport via callbacks.
 *
 *  Example:
 *  @code
 *    static int my_i2c_read(uint8_t addr, uint8_t reg, uint8_t* data, size_t len, void* ctx);
 *    static int my_i2c_write(uint8_t addr, const uint8_t* data, size_t len, void* ctx);
 *
 *    alc::Npm2100::I2cTransport transport {
 *        .Read = my_i2c_read, .Write = my_i2c_write,
 *        .Log = nullptr, .ctx = nullptr };
 *    alc::Npm2100 pmic { transport };
 *    if (pmic.Init() == 0) { pmic.BoostSetVoltage(3000); pmic.LdoSwEnable(); }
 *  @endcode
 */

namespace alc
{
  class Npm2100
  {
    public:
      struct I2cTransport
      {
          int (*Read)(uint8_t devAddr, uint8_t regAddr, uint8_t* data, size_t len, void* ctx);
          int (*Write)(uint8_t devAddr, const uint8_t* data, size_t len, void* ctx);
          void (*Log)(int level, const char* fmt, ...);
          void* ctx;
      };

      static constexpr uint8_t M_I2C_ADDRESS { 0x74 };

      enum class ResetReason : uint8_t {
        ColdPowerUp     = 0,
        ThermalShutdown = 1,
        BootMonitor     = 2,
        Button          = 3,
        WatchdogReset   = 4,
        WatchdogPwrCyc  = 5,
        SoftwareReset   = 6,
        HiberPin        = 7,
        HiberTimer      = 8,
        HiberPtPin      = 9,
        HiberPtTimer    = 10,
        PowerOffButton  = 11,
        ShipExit        = 12,
        OvercurrentProt = 13,
        Unknown         = 0xFF
      };

      /**
       * @brief Construct with an I²C transport. Cheap; no I/O performed.
       *
       * @param transport Function-pointer struct describing how to talk to the
       *                  bus. Copied by value, so caller's struct may go out
       *                  of scope after construction.
       * @note You must call Init() before any other method.
       */
      explicit Npm2100(const I2cTransport& transport);

      /**
       * @brief Probe the chip and cache its boot-time state.
       *
       * Reads RESET.SYSGDENSTATUS to confirm the chip ACKs, then reads
       * RESET.RESET to populate the reset-reason and brown-out caches.
       *
       * @param clearResetReason If true (default), also writes RESET.TASKS_CLR
       *                         so the next boot sees a clean reason register.
       * @return 0 on success; -EIO on transport failure.
       * @note May be called multiple times; each call re-probes and re-caches.
       */
      int Init(bool clearResetReason = true);

      /**
       * @brief Clear the chip's latched reset-reason register.
       *
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() hasn't been called.
       * @note Does not affect the cache populated by Init().
       *       Subsequent calls to GetResetReason() return the cached value.
       */
      int ClearResetReason();

      /**
       * @brief Last reset reason, cached at Init() time.
       *
       * @return The decoded ResetReason. ResetReason::Unknown if Init() has
       *         not yet succeeded, or if the chip reported a reserved code.
       * @note Cached value; reading is free and does not touch the bus.
       */
      ResetReason GetResetReason() const { return m_reset_reason; }

      /**
       * @brief Whether the most recent reset was accompanied by a brown-out.
       *
       * @return true if the BOR bit was set in RESET.RESET at Init() time.
       * @note Cached value; reading is free and does not touch the bus.
       */
      bool BrownOutOccurred() const { return m_brown_out; }

      /**
       * @brief Trigger an immediate full software reset of the PMIC.
       *
       * Writes RESET.TASKS_RESET, which restarts the chip exactly as if a
       * cold power-up had occurred.
       *
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() hasn't been called.
       * @note The chip will reset within microseconds; the caller's I²C bus
       *       state thereafter is undefined until the chip re-enumerates.
       */
      int SoftwareReset();

      // ──────────────────── RESET (button, scratch, sticky) ────────────────────

      /**
       * @brief Selects which physical pin acts as the reset button input.
       *
       * PgReset — the dedicated PG/RESET pin (default).
       * ShpHld  — share the SHPHLD pin so a single push-button can both wake
       *           from Ship and trigger a long-press reset.
       */
      enum class ResetButtonPin : uint8_t { PgReset = 0, ShpHld = 1 };

      /**
       * @brief Long-press duration required to trigger a reset.
       *
       * Encoded as the 2-bit TIME field of RESET.DEBOUNCE.
       */
      enum class ResetButtonHoldS : uint8_t { S10 = 0, S5 = 1, S20 = 2, S30 = 3 };

      /**
       * @brief What happens to the LDOSW rail on a watchdog-triggered reset.
       *
       * StayOn  — leave the LDOSW rail enabled across the reset (default).
       * TurnOff — disable the LDOSW rail when the watchdog forces a reset.
       */
      enum class WdResetLdoSwMode : uint8_t { StayOn = 0, TurnOff = 1 };

      /**
       * @brief Configure the long-press reset button (pin, hold time, enable).
       *
       * Writes RESET.BUTTON, RESET.PIN, and RESET.DEBOUNCE in sequence.
       *
       * @param pin              Which input pin services the button.
       * @param holdTime         Long-press duration before reset fires.
       * @param longPressEnabled If true, long-press reset is armed; if false,
       *                         the long-press path is disabled (RESET.BUTTON
       *                         LONGPRESS bit is set).
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int ConfigureResetButton(ResetButtonPin pin, ResetButtonHoldS holdTime, bool longPressEnabled);

      /**
       * @brief Choose whether the LDOSW rail stays on across a watchdog reset.
       *
       * Writes RESET.ALTCONFIG.
       *
       * @param mode See WdResetLdoSwMode.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int SetWdResetLdoSwMode(WdResetLdoSwMode mode);

      /**
       * @brief Write a byte to scratch register A and strobe it into latched form.
       *
       * RESET.WRITE holds the staged value; RESET.STROBE commits it. The latched
       * value survives Hibernate / Pass-Through and can be retrieved on the next
       * boot via ReadScratchA().
       *
       * @param value Byte to latch.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int WriteScratchA(uint8_t value);

      /**
       * @brief Read the latched scratch-A byte from RESET.READ.
       *
       * @param value Out: latched byte.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int ReadScratchA(uint8_t& value);

      /**
       * @brief Write a byte to the un-strobed scratch-B register.
       *
       * Unlike scratch A, scratch B is a single register with no strobe step:
       * the written value is immediately readable via ReadScratchB().
       *
       * @param value Byte to store.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int WriteScratchB(uint8_t value);

      /**
       * @brief Read the scratch-B byte.
       *
       * @param value Out: stored byte.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int ReadScratchB(uint8_t& value);

      /**
       * @brief Configure the boot-monitor sticky bits.
       *
       * Read-modify-writes the sticky-bit register so the PWRBUTTON sticky bit
       * is preserved while the BOOTMONSEL and BOOTMONEN bits are updated. Uses
       * the chip's two-step sticky-write sequence: load value into WRITESTICKY,
       * then strobe via STROBESTICKY.
       *
       * @param useRegister If true, the boot monitor uses the register-based
       *                    selector (BOOTMONSEL bit set).
       * @param enabled     If true, the boot monitor is enabled (BOOTMONEN bit
       *                    set).
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int ConfigureBootMonitor(bool useRegister, bool enabled);

      /**
       * @brief Read the live boot-monitor active flag.
       *
       * Reads RESET.SYSGDENSTATUS and reports the BOOTMON bit.
       *
       * @param active Out: true if the boot monitor is currently active.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int IsBootMonitorActive(bool& active);

      /**
       * @brief Set or clear the power-off button disable sticky bit.
       *
       * Read-modify-writes the sticky-bit register so the BOOTMONSEL and
       * BOOTMONEN bits are preserved while the PWRBUTTON bit is updated. Uses
       * the chip's two-step sticky-write sequence.
       *
       * @param disabled If true, the power-off button is disabled (PWRBUTTON
       *                 sticky bit set); if false, it is re-enabled.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int DisablePowerOffButton(bool disabled);

      // ──────────────────── BOOST ────────────────────

      /**
       * @brief Requested operating mode for the boost converter.
       *
       * Auto      — chip selects HP/LP/ULP/PT automatically (default).
       * ForcedHp  — pin-permanent High-Power PWM mode.
       * ForcedLp  — pin-permanent Low-Power hysteretic mode.
       * ForcedPt  — pin-permanent Pass-Through mode.
       * PreventHp — auto, but never enter HP (LP/ULP/PT only).
       */
      enum class BoostMode : uint8_t { Auto = 0, ForcedHp = 1, ForcedLp = 2, ForcedPt = 3, PreventHp = 4 };

      /**
       * @brief Active operating mode reported by the chip (read-only state).
       *
       * Differs from BoostMode in that PT and DPS are observable runtime
       * states, and ULP is reported separately from LP.
       */
      enum class BoostActiveMode : uint8_t { Hp = 0, Lp = 1, Ulp = 2, Pt = 3, Dps = 4 };

      /**
       * @brief Set the boost output voltage target.
       *
       * Programs BOOST.VOUT with the closest legal step to @p millivolts and
       * sets BOOST.VOUTSEL so the register value (not the SEL pins) controls
       * the output.
       *
       * @param millivolts Desired output, 1800..3300 mV. Values inside this
       *                   range are snapped to the nearest 50 mV step.
       * @return 0 on success; -EINVAL if @p millivolts is outside the legal
       *         range; -EIO on transport failure; -ENODEV if Init() not called.
       * @note The snapped value can be retrieved via BoostGetVoltageSetting().
       */
      int BoostSetVoltage(uint16_t millivolts);

      /**
       * @brief Last voltage written via BoostSetVoltage(), or value seeded
       *        from BOOST.VOUT at Init() time.
       *
       * @return Snapped voltage in millivolts. Reading is free (cached).
       */
      uint16_t BoostGetVoltageSetting() const { return m_boost_millivolts; }

      /**
       * @brief Select the boost converter operating mode.
       *
       * Writes BOOST.OPER.MODE via read-modify-write, leaving other bits in
       * BOOST.OPER untouched.
       *
       * @param mode Requested mode. See BoostMode.
       * @return 0 on success; -EINVAL if @p mode is out of range;
       *         -EIO on transport failure; -ENODEV if Init() not called.
       */
      int BoostSetMode(BoostMode mode);

      /**
       * @brief Read the boost converter's currently active mode.
       *
       * @param mode Out: the active mode reported by BOOST.STATUS0.MODE.
       * @return 0 on success; -EIO on transport failure or reserved code;
       *         -ENODEV if Init() not called.
       * @note In Auto mode this can change dynamically as load varies.
       */
      int BoostGetActiveMode(BoostActiveMode& mode);

      /**
       * @brief Whether the boost output has reached its target voltage.
       *
       * @param atTarget Out: true if BOOST.STATUS1.VOUTLVL is set.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() not called.
       */
      int BoostIsAtTarget(bool& atTarget);

      /**
       * @brief Set the VBAT low-voltage threshold and route it via VBATMINHSEL.
       *
       * Programs BOOST.VBATMINH with the snapped step and asserts the
       * VBATMINHSEL bit so the threshold is active.
       *
       * @param millivolts Threshold, 650..3150 mV (snapped to nearest 50 mV).
       * @return 0 on success; -EINVAL if @p millivolts out of range;
       *         -EIO on transport failure; -ENODEV if Init() not called.
       */
      int BoostSetVbatLowThreshold(uint16_t millivolts);

      /**
       * @brief Route the VBAT comparator to the VBATMINH-derived threshold.
       *
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() not called.
       * @note Pairs with BoostSetVbatLowThreshold(); without enabling, the
       *       threshold is programmed but the comparator does not use it.
       */
      int BoostEnableVbatLowMonitor();

      /**
       * @brief Disable the VBAT low-voltage comparator routing.
       *
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() not called.
       */
      int BoostDisableVbatLowMonitor();

      // ──────────────────── LDOSW ────────────────────

      /**
       * @brief Output configuration for the LDO/load-switch block.
       *
       * Ldo    — operate as a low-dropout regulator (programmable VOUT).
       * LoadSw — operate as a simple load switch (VOUT register ignored).
       */
      enum class LdoSwOutputMode : uint8_t { Ldo = 0, LoadSw = 1 };

      /**
       * @brief Power-mode selection for the LDOSW block.
       *
       * Auto — chip selects between ULP and HP based on load (default).
       * Ulp  — pin-permanent Ultra-Low-Power mode.
       * Hp   — pin-permanent High-Power mode.
       */
      enum class LdoSwPowerMode : uint8_t { Auto = 0, Ulp = 1, Hp = 2 };

      /**
       * @brief Select between LDO and load-switch operation.
       *
       * Writes LDOSW.SEL.MODE via read-modify-write so the OPER field is
       * preserved.
       *
       * @param mode See LdoSwOutputMode.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() not called.
       * @note Set the desired output voltage (LdoSwSetVoltage) and output
       *       mode before enabling the block to avoid an inrush at the
       *       wrong setting.
       */
      int LdoSwSetOutputMode(LdoSwOutputMode mode);

      /**
       * @brief Select the LDOSW power-mode (Auto / Ulp / Hp).
       *
       * Writes LDOSW.SEL.OPER via read-modify-write so the MODE field is
       * preserved.
       *
       * @param mode See LdoSwPowerMode.
       * @return 0 on success; -EINVAL if @p mode is out of range;
       *         -EIO on transport failure; -ENODEV if Init() not called.
       */
      int LdoSwSetPowerMode(LdoSwPowerMode mode);

      /**
       * @brief Set the LDO output voltage target.
       *
       * Programs LDOSW.VOUT with the closest legal step to @p millivolts.
       *
       * @param millivolts Desired output, 800..3000 mV. Values inside this
       *                   range are snapped to the nearest 50 mV step.
       * @return 0 on success; -EINVAL if @p millivolts is outside the legal
       *         range; -EIO on transport failure; -ENODEV if Init() not called.
       * @note The voltage register is honoured only in LDO mode; in load-switch
       *       mode the register is ignored. The snapped value can be
       *       retrieved via LdoSwGetVoltageSetting().
       */
      int LdoSwSetVoltage(uint16_t millivolts);

      /**
       * @brief Last voltage written via LdoSwSetVoltage(), or value seeded
       *        from LDOSW.VOUT at Init() time.
       *
       * @return Snapped voltage in millivolts. Reading is free (cached).
       */
      uint16_t LdoSwGetVoltageSetting() const { return m_ldo_sw_millivolts; }

      /**
       * @brief Enable the LDOSW block (assert LDOSW.LDOSW.ENABLE).
       *
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() not called.
       * @note Configure output mode, power mode, and voltage first to avoid
       *       glitching the rail at the wrong setting.
       */
      int LdoSwEnable();

      /**
       * @brief Disable the LDOSW block (clear LDOSW.LDOSW.ENABLE).
       *
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() not called.
       */
      int LdoSwDisable();

      /**
       * @brief Read whether the LDOSW block is currently enabled.
       *
       * @param enabled Out: true if LDOSW.LDOSW.ENABLE is set.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() not called.
       */
      int LdoSwIsEnabled(bool& enabled);

      /**
       * @brief Read the LDOSW over-current-protection latch.
       *
       * @param ocpActive Out: true if LDOSW.STATUS.OCP is set.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() not called.
       */
      int LdoSwReadOcpFault(bool& ocpActive);

      // ──────────────────── ADC ────────────────────

      /**
       * @brief Trigger a single-shot VBAT measurement and return the result.
       *
       * Selects the INSVBAT mode in ADC.CONFIG, fires ADC.TASKS_ADC, then polls
       * MAIN.EVENTS_ADC_SET for the VBATRDY bit up to ~20 times before giving
       * up. Each poll is one I²C read; the underlying conversion takes ~100 µs
       * so on a healthy bus 1–2 polls suffice.
       *
       * @param volts Out: measured battery voltage in volts.
       * @return 0 on success; -ETIMEDOUT if VBATRDY did not assert within the
       *         poll budget (~3 ms at 400 kHz I²C); -EIO on transport failure;
       *         -ENODEV if Init() not called.
       * @note Blocks the caller for the full conversion time. Conversion
       *       formula: volts = raw × 3.2 / 255.
       */
      int AdcReadVbat(float& volts);

      /**
       * @brief Trigger a single-shot VOUT measurement and return the result.
       *
       * Selects the VOUT mode in ADC.CONFIG, fires ADC.TASKS_ADC, then polls
       * MAIN.EVENTS_ADC_SET for the VOUTRDY bit up to ~20 times before giving
       * up. Each poll is one I²C read; the underlying conversion takes ~100 µs
       * so on a healthy bus 1–2 polls suffice.
       *
       * @param volts Out: measured boost output voltage in volts.
       * @return 0 on success; -ETIMEDOUT if VOUTRDY did not assert within the
       *         poll budget (~3 ms at 400 kHz I²C); -EIO on transport failure;
       *         -ENODEV if Init() not called.
       * @note Blocks the caller for the full conversion time. Conversion
       *       formula: volts = 1.8 + raw × 1.5 / 255.
       */
      int AdcReadVout(float& volts);

      /**
       * @brief Trigger a single-shot die-temperature measurement.
       *
       * Selects the DIETEMP mode in ADC.CONFIG, fires ADC.TASKS_ADC, then polls
       * MAIN.EVENTS_ADC_SET for the DIETRDY bit up to ~20 times before giving
       * up. Each poll is one I²C read; the underlying conversion takes ~100 µs
       * so on a healthy bus 1–2 polls suffice.
       *
       * @param celsius Out: measured die temperature in degrees Celsius.
       * @return 0 on success; -ETIMEDOUT if DIETRDY did not assert within the
       *         poll budget (~3 ms at 400 kHz I²C); -EIO on transport failure;
       *         -ENODEV if Init() not called.
       * @note Blocks the caller for the full conversion time. Conversion
       *       formula: celsius = 389.5 − 2.12 × raw.
       */
      int AdcReadDieTemp(float& celsius);

      // ──────────────────── TIMER ────────────────────

      /**
       * @brief Operating mode for the 24-bit TIMER block.
       *
       * GeneralPurpose — fires the Timer event when the target is reached.
       * WatchdogReset  — chip resets if not kicked before target expires.
       * WatchdogPwrCyc — chip power-cycles if not kicked before target expires.
       * Wakeup         — wakes from Hibernate when the target is reached.
       */
      enum class TimerMode : uint8_t { GeneralPurpose = 0, WatchdogReset = 1, WatchdogPwrCyc = 2, Wakeup = 3 };

      /**
       * @brief System-group event bits in MAIN.EVENTS_SYSTEM / INTEN_SYSTEM.
       *
       * Bit positions match the nrfx convention (bit N == 1u << N). Only the
       * Timer / TimerPreWarn / TimerFree bits are wired up by the TIMER API
       * surface; the remaining values are exposed for use by later blocks
       * (button, power-good, die-temp warning, etc.).
       */
      enum class SystemEvent : uint8_t {
        DieTempWarn  = 0,
        ShpHldFall   = 1,
        ShpHldRise   = 2,
        PgResetFall  = 3,
        PgResetRise  = 4,
        Timer        = 5,
        TimerPreWarn = 6,
        TimerFree    = 7
      };

      /**
       * @brief Stop the timer (writes TIMER.TASKS_STOP).
       *
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       * @note Safe to call when the timer is already stopped.
       */
      int TimerStop();

      /**
       * @brief Select the timer's operating mode.
       *
       * Writes TIMER.CONFIG.MODE via read-modify-write so other bits in
       * TIMER.CONFIG are preserved.
       *
       * @param mode See TimerMode.
       * @return 0 on success; -EINVAL if @p mode is out of range;
       *         -EIO on transport failure; -ENODEV if Init() not called.
       * @note The chip latches the mode at TimerStart(); call TimerStop()
       *       first if the timer is currently running.
       */
      int TimerSetMode(TimerMode mode);

      /**
       * @brief Set the timer target as a duration in milliseconds.
       *
       * Programs the 24-bit target register triplet (TARGETHI / TARGETMID /
       * TARGETLO) as a single 3-byte burst write. The duration is converted
       * to ticks using a 15.625 ms tick period: ticks = round(ms / 15.625),
       * then the chip's N-1 convention is applied (value = ticks - 1).
       *
       * @param milliseconds Desired duration. Minimum 16 ms (≈one tick); max
       *                     ~262 144 000 ms (~3 days), set by the 24-bit field.
       * @return 0 on success; -EINVAL if @p milliseconds is below the minimum
       *         tick or exceeds the 24-bit range; -EIO on transport failure;
       *         -ENODEV if Init() not called.
       * @note Half-up rounding: 16 ms snaps to one tick, 16 000 ms to 1024
       *       ticks, etc. The new target takes effect on the next TimerStart().
       */
      int TimerSetDurationMs(uint32_t milliseconds);

      /**
       * @brief Start the timer (writes TIMER.TASKS_START).
       *
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() not called.
       * @note The mode and duration must be programmed first.
       */
      int TimerStart();

      /**
       * @brief Kick the watchdog (writes TIMER.TASKS_KICK).
       *
       * Resets the timer's running count back to zero without stopping it.
       * Required at intervals shorter than the duration when running in
       * WatchdogReset or WatchdogPwrCyc mode.
       *
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() not called.
       */
      int TimerKickWatchdog();

      /**
       * @brief Poll the Timer expiry event flag.
       *
       * Reads MAIN.EVENTS_SYSTEM_SET and reports whether the Timer bit is
       * latched. Does not clear the event — call TimerClearExpiredEvent().
       *
       * @param expired Out: true if the Timer event is latched.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() not called.
       */
      int TimerIsExpired(bool& expired);

      /**
       * @brief Clear the Timer expiry event flag.
       *
       * Writes the Timer bit to MAIN.EVENTS_SYSTEM_CLR.
       *
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() not called.
       */
      int TimerClearExpiredEvent();

      /**
       * @brief Enable timer-related interrupts on the INT pin.
       *
       * Writes MAIN.INTEN_SYSTEM_SET. The Timer expiry interrupt is always
       * enabled; the pre-warning and free-running events are opt-in.
       *
       * @param preWarn   If true, also enable the TimerPreWarn interrupt.
       * @param freeEvent If true, also enable the TimerFree interrupt.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() not called.
       */
      int TimerEnableInterrupt(bool preWarn = false, bool freeEvent = false);

      /**
       * @brief Disable all three timer-related interrupts on the INT pin.
       *
       * Writes Timer | TimerPreWarn | TimerFree to MAIN.INTEN_SYSTEM_CLR.
       *
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() not called.
       */
      int TimerDisableInterrupt();

      // ──────────────────── SHIP & Break-to-Wake ────────────────────

      /**
       * @brief Edge selection for the SHPHLD wake-from-Ship pin.
       *
       * Falling — wake on a high-to-low transition (default factory setting).
       * Rising  — wake on a low-to-high transition.
       */
      enum class ShipWakeEdge : uint8_t { Falling = 0, Rising = 1 };

      /**
       * @brief Internal pull resistor on the SHPHLD pin.
       *
       * PullUp   — internal pull-up active (default).
       * None     — no internal pull resistor.
       * PullDown — internal pull-down active.
       */
      enum class ShpHldResistor : uint8_t { PullUp = 0, None = 1, PullDown = 2 };

      /**
       * @brief Current strength for the SHPHLD pull resistor.
       *
       * Weak / Low / Moderate / High — chip-defined steps; consult the datasheet
       * for absolute current values.
       */
      enum class ShpHldPullCurrent : uint8_t { Weak = 0, Low = 1, Moderate = 2, High = 3 };

      /**
       * @brief Enter Ship mode (writes SHIP.TASKS_SHIP).
       *
       * Powers down all rails. The chip wakes on the configured SHPHLD edge,
       * which is reported as ResetReason::ShipExit on the next boot.
       *
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       * @note Configure the wake edge and SHPHLD resistor/pull settings before
       *       entering Ship mode.
       */
      int ShipEnter();

      /**
       * @brief Set the SHPHLD pin edge that wakes the chip from Ship/Hibernate.
       *
       * Writes SHIP.WAKEUP.EDGE via read-modify-write so other bits
       * (HIBERNATE) are preserved.
       *
       * @param edge See ShipWakeEdge.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int ShipSetWakeEdge(ShipWakeEdge edge);

      /**
       * @brief Configure the SHPHLD break-to-wake pin's electrical behaviour.
       *
       * Programs SHIP.SHPHLD with the resistor type, pull current strength,
       * and weak-pull-up enable in one write.
       *
       * @param resistor          Internal pull resistor selection.
       * @param current           Current strength for the active pull.
       * @param weakPullUpEnabled If true, enables the supplementary weak pull-up.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int ShipConfigureBreakToWake(ShpHldResistor resistor, ShpHldPullCurrent current, bool weakPullUpEnabled);

      /**
       * @brief Enable SHPHLD-pin wake from Hibernate.
       *
       * Clears SHIP.WAKEUP.HIBERNATE so the SHPHLD pin is allowed to wake the
       * chip from Hibernate. Read-modify-write preserves the EDGE bit.
       *
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int ShpHldEnableHibernateWake();

      /**
       * @brief Disable SHPHLD-pin wake from Hibernate (timer-only wake).
       *
       * Sets SHIP.WAKEUP.HIBERNATE so the SHPHLD pin no longer wakes the chip
       * from Hibernate. Read-modify-write preserves the EDGE bit.
       *
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int ShpHldDisableHibernateWake();

      // ──────────────────── HIBERNATE ────────────────────

      /**
       * @brief SHPHLD wake-debounce duration for the Hibernate exit path.
       *
       * Encoded as a 3-bit step in HIBERNATE.DEBOUNCE.TIME. Values are the
       * minimum SHPHLD assertion required to count as a wake event.
       */
      enum class HibernateDebounceMs : uint8_t { Ms10 = 0, Ms30 = 1, Ms60 = 2, Ms100 = 3, Ms300 = 4, Ms600 = 5, Ms1000 = 6, Ms3000 = 7 };

      /**
       * @brief Enter Hibernate mode (writes HIBERNATE.TASKS_HIBER).
       *
       * Stops the boost converter and enters the low-power Hibernate state.
       * Wake sources: SHPHLD pin (if enabled via ShpHldEnableHibernateWake)
       * and the wake timer (if armed in TimerMode::Wakeup). On wake the chip
       * resumes execution with all configuration registers preserved.
       *
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int HibernateEnter();

      /**
       * @brief Enter Hibernate Pass-Through mode (writes HIBERNATE.TASKS_HIBERPT).
       *
       * Like HibernateEnter() but enters Pass-Through mode for the boost
       * converter on wake.
       *
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       * @note On wake from Hibernate Pass-Through the chip resets a subset of
       *       its configuration registers — re-apply application configuration
       *       after this style of wake. Consult the datasheet for the exact
       *       list of registers cleared.
       */
      int HibernateEnterPassThrough();

      /**
       * @brief Set the SHPHLD wake-debounce time and enable debounce.
       *
       * Writes HIBERNATE.DEBOUNCE with the encoded TIME field plus the ENABLE
       * bit set. SHPHLD must remain asserted for the selected duration for
       * the wake to be accepted.
       *
       * @param setting Debounce duration. See HibernateDebounceMs.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int HibernateSetWakeDebounce(HibernateDebounceMs setting);

      /**
       * @brief Disable SHPHLD wake-debounce (any-edge wake).
       *
       * Clears HIBERNATE.DEBOUNCE.ENABLE via read-modify-write so the TIME
       * field is preserved for later re-enable.
       *
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int HibernateDisableWakeDebounce();

      // ──────────────────── GPIO ────────────────────

      /**
       * @brief Selects which physical GPIO pin a method targets.
       *
       * Gpio0 — pin GPIO0.
       * Gpio1 — pin GPIO1.
       */
      enum class GpioPin : uint8_t { Gpio0 = 0, Gpio1 = 1 };

      /**
       * @brief Pin function selection (GPIO vs interrupt output).
       *
       * Gpio        — pin acts as a general-purpose I/O.
       * InterruptLo — pin drives low when the interrupt is asserted.
       * InterruptHi — pin drives high when the interrupt is asserted.
       */
      enum class GpioUsage : uint8_t { Gpio = 0, InterruptLo = 1, InterruptHi = 2 };

      /**
       * @brief Output drive strength for a GPIO configured as an output.
       *
       * Normal — standard drive (default).
       * Strong — increased drive current for heavy loads.
       */
      enum class GpioDrive : uint8_t { Normal = 0, Strong = 1 };

      /**
       * @brief Per-pin GPIO configuration applied by GpioConfigure().
       *
       * Defaults model the chip's reset state for each field where applicable:
       * input enabled with a pull-down resistor, normal drive, no debounce.
       */
      struct GpioConfig
      {
          bool inputEnabled { true };
          bool outputEnabled { false };
          bool openDrain { false };
          bool pullUp { false };
          bool pullDown { true };
          GpioDrive drive { GpioDrive::Normal };
          bool debounce { false };
      };

      /**
       * @brief Configure the electrical behaviour of one GPIO pin.
       *
       * Writes the appropriate GPIO.CONFIG[01] register with the field bits
       * encoded from @p cfg.
       *
       * @param pin Which physical pin to configure.
       * @param cfg Bundled configuration (input/output, pulls, drive, debounce).
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int GpioConfigure(GpioPin pin, const GpioConfig& cfg);

      /**
       * @brief Choose between GPIO and interrupt-output usage for a pin.
       *
       * Writes GPIO.USAGE[01] with the encoded usage value.
       *
       * @param pin   Which physical pin to repurpose.
       * @param usage See GpioUsage.
       * @return 0 on success; -EINVAL if @p usage is out of range;
       *         -EIO on transport failure; -ENODEV if Init() has not been called.
       */
      int GpioSetUsage(GpioPin pin, GpioUsage usage);

      /**
       * @brief Drive a GPIO output high or low.
       *
       * Writes GPIO.OUTPUT[01]. Has effect only when the pin is configured as
       * an output via GpioConfigure().
       *
       * @param pin  Which physical pin to drive.
       * @param high If true, drive logic high; if false, drive logic low.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int GpioWrite(GpioPin pin, bool high);

      /**
       * @brief Read the live logic level of one GPIO pin.
       *
       * Reads GPIO.READ and extracts the bit corresponding to @p pin.
       *
       * @param pin  Which physical pin to sample.
       * @param high Out: true if the pin reads high.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int GpioRead(GpioPin pin, bool& high);

      /**
       * @brief Read the raw GPIO.READ register exposing both pins at once.
       *
       * Bit 0 corresponds to GPIO0; bit 1 corresponds to GPIO1.
       *
       * @param bits Out: raw register value.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int GpioReadAll(uint8_t& bits);

      // ──────────────────── Live status & die-temp monitoring ────────────────────

      /**
       * @brief Read the live state of the SHPHLD pin.
       *
       * Reads MAIN.STATUS and reports the SHPHLD bit.
       *
       * @param high Out: true if SHPHLD is currently high.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int IsShpHldHigh(bool& high);

      /**
       * @brief Read the live state of the PG/RESET pin.
       *
       * Reads MAIN.STATUS and reports the PGRESET bit.
       *
       * @param high Out: true if PG/RESET is currently high.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int IsPgResetHigh(bool& high);

      /**
       * @brief Read whether the die-temperature warning threshold is exceeded.
       *
       * Reads MAIN.STATUS and reports the DIETEMP bit. Reflects the
       * continuously-monitored output of the die-temperature comparator;
       * requires die-temp monitoring to have been enabled.
       *
       * @param warning Out: true if the die-temp warning threshold is exceeded.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int IsDieTempWarning(bool& warning);

      /**
       * @brief Enable continuous die-temperature monitoring.
       *
       * Writes MAIN.REQUESTSET to set the DIETEMP request bit and, optionally,
       * the DIETEMPENA gating bit that restricts monitoring to periods when
       * the boost converter is running in HP mode.
       *
       * @param whileBoostInHpOnly If true (default), die-temp monitoring is
       *                           gated to HP-mode boost operation only; if
       *                           false, monitoring runs unconditionally.
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int EnableDieTempMonitoring(bool whileBoostInHpOnly = true);

      /**
       * @brief Disable continuous die-temperature monitoring.
       *
       * Writes MAIN.REQUESTCLR to clear both the DIETEMP and DIETEMPENA bits.
       *
       * @return 0 on success; -EIO on transport failure;
       *         -ENODEV if Init() has not been called.
       */
      int DisableDieTempMonitoring();

      // ──────────────────── Events & Interrupts (per-group) ────────────────────
      //
      // The MAIN block exposes five event groups: System, Adc, Gpio, Boost, and
      // LdoSw. Each group has paired SET / CLR registers for both the latched
      // event flags (EVENTS_*) and the per-bit interrupt-mask (INTEN_*). Bit
      // positions for each group are given by the corresponding *Event enum;
      // callers compose a bitmask by OR'ing `1u << static_cast<uint8_t>(value)`.
      //
      // The 20 group methods below all share the same signature and Doxygen
      // contract:
      //
      //   - GetXxxEventsPending(uint8_t& bits) — read latched-event SET register.
      //   - ClearXxxEvents(uint8_t bits)       — write bits to CLR register.
      //   - EnableXxxInterrupts(uint8_t bits)  — write bits to INTEN_*_SET.
      //   - DisableXxxInterrupts(uint8_t bits) — write bits to INTEN_*_CLR.
      //
      // All four return 0 on success, -EIO on transport failure, and -ENODEV
      // if Init() has not been called.

      /** @brief Bit positions for ADC-group events / interrupts. */
      enum class AdcEvent : uint8_t { VbatRdy = 0, DieTempRdy = 1, DroopRdy = 2, VoutRdy = 3 };

      /** @brief Bit positions for GPIO-group events / interrupts. */
      enum class GpioEvent : uint8_t { Gpio0Fall = 0, Gpio0Rise = 1, Gpio1Fall = 2, Gpio1Rise = 3 };

      /** @brief Bit positions for BOOST-group events / interrupts. */
      enum class BoostEvent : uint8_t {
        VbatWarnFall = 0,
        VbatWarnRise = 1,
        VoutMin      = 2,
        VoutWarnFall = 3,
        VoutWarnRise = 4,
        VoutDpsFall  = 5,
        VoutDpsRise  = 6,
        VoutOk       = 7
      };

      /** @brief Bit positions for LDOSW-group events / interrupts. */
      enum class LdoSwEvent : uint8_t { Ocp = 0, VintFail = 1 };

      /** @brief Read latched system-group events from MAIN.EVENTS_SYSTEM_SET. */
      int GetSystemEventsPending(uint8_t& bits);
      /** @brief Clear the supplied system-group event bits via MAIN.EVENTS_SYSTEM_CLR. */
      int ClearSystemEvents(uint8_t bits);
      /** @brief Enable the supplied system-group interrupt bits via MAIN.INTEN_SYSTEM_SET. */
      int EnableSystemInterrupts(uint8_t bits);
      /** @brief Disable the supplied system-group interrupt bits via MAIN.INTEN_SYSTEM_CLR. */
      int DisableSystemInterrupts(uint8_t bits);

      /** @brief Read latched ADC-group events from MAIN.EVENTS_ADC_SET. */
      int GetAdcEventsPending(uint8_t& bits);
      /** @brief Clear the supplied ADC-group event bits via MAIN.EVENTS_ADC_CLR. */
      int ClearAdcEvents(uint8_t bits);
      /** @brief Enable the supplied ADC-group interrupt bits via MAIN.INTEN_ADC_SET. */
      int EnableAdcInterrupts(uint8_t bits);
      /** @brief Disable the supplied ADC-group interrupt bits via MAIN.INTEN_ADC_CLR. */
      int DisableAdcInterrupts(uint8_t bits);

      /** @brief Read latched GPIO-group events from MAIN.EVENTS_GPIO_SET. */
      int GetGpioEventsPending(uint8_t& bits);
      /** @brief Clear the supplied GPIO-group event bits via MAIN.EVENTS_GPIO_CLR. */
      int ClearGpioEvents(uint8_t bits);
      /** @brief Enable the supplied GPIO-group interrupt bits via MAIN.INTEN_GPIO_SET. */
      int EnableGpioInterrupts(uint8_t bits);
      /** @brief Disable the supplied GPIO-group interrupt bits via MAIN.INTEN_GPIO_CLR. */
      int DisableGpioInterrupts(uint8_t bits);

      /** @brief Read latched BOOST-group events from MAIN.EVENTS_BOOST_SET. */
      int GetBoostEventsPending(uint8_t& bits);
      /** @brief Clear the supplied BOOST-group event bits via MAIN.EVENTS_BOOST_CLR. */
      int ClearBoostEvents(uint8_t bits);
      /** @brief Enable the supplied BOOST-group interrupt bits via MAIN.INTEN_BOOST_SET. */
      int EnableBoostInterrupts(uint8_t bits);
      /** @brief Disable the supplied BOOST-group interrupt bits via MAIN.INTEN_BOOST_CLR. */
      int DisableBoostInterrupts(uint8_t bits);

      /** @brief Read latched LDOSW-group events from MAIN.EVENTS_LDOSW_SET. */
      int GetLdoSwEventsPending(uint8_t& bits);
      /** @brief Clear the supplied LDOSW-group event bits via MAIN.EVENTS_LDOSW_CLR. */
      int ClearLdoSwEvents(uint8_t bits);
      /** @brief Enable the supplied LDOSW-group interrupt bits via MAIN.INTEN_LDOSW_SET. */
      int EnableLdoSwInterrupts(uint8_t bits);
      /** @brief Disable the supplied LDOSW-group interrupt bits via MAIN.INTEN_LDOSW_CLR. */
      int DisableLdoSwInterrupts(uint8_t bits);

      /**
       * @brief Clear every latched event across all five MAIN event groups.
       *
       * Writes 0xFF to MAIN.EVENTS_SYSTEM_CLR, MAIN.EVENTS_ADC_CLR,
       * MAIN.EVENTS_GPIO_CLR, MAIN.EVENTS_BOOST_CLR, and MAIN.EVENTS_LDOSW_CLR
       * in that order. Useful at boot or after re-arming the chip from a sleep
       * state to discard stale latched flags before enabling interrupts.
       *
       * @return 0 on success; -EIO on transport failure (returned from the
       *         first failing register write — preceding writes have already
       *         been committed); -ENODEV if Init() has not been called.
       */
      int ClearAllPendingEvents();

    private:
      int readRegister(uint8_t regAddr, uint8_t& value);
      int writeRegister(uint8_t regAddr, uint8_t value);
      int updateRegister(uint8_t regAddr, uint8_t value, uint8_t mask);
      int writeTask(uint8_t regAddr);
      int readBurst(uint8_t firstReg, uint8_t* dst, size_t len);
      int writeBurst(uint8_t firstReg, const uint8_t* src, size_t len);

      int adcSingleShot(uint8_t mode, uint8_t readyBit, uint8_t resultReg, uint8_t& raw);

      int writeStickyBits(uint8_t value);

      int eventGet(uint8_t setReg, uint8_t& bits) { return readRegister(setReg, bits); }
      int eventClear(uint8_t clrReg, uint8_t bits) { return writeRegister(clrReg, bits); }
      int eventEnable(uint8_t intenSetReg, uint8_t bits) { return writeRegister(intenSetReg, bits); }
      int eventDisable(uint8_t intenClrReg, uint8_t bits) { return writeRegister(intenClrReg, bits); }

      static ResetReason parseResetReason(uint8_t raw);

      I2cTransport m_transport;
      bool m_initialised { false };

      ResetReason m_reset_reason { ResetReason::Unknown };
      bool m_brown_out { false };

      uint16_t m_boost_millivolts { 0 };
      uint16_t m_ldo_sw_millivolts { 0 };
  };
}
