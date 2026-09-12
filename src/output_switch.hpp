#pragma once

namespace alc
{

  /**
   * @brief The device's fire output — two GPIOs asserted together as one channel.
   *
   * **In the product this output switches a voltage.** Everything about this class
   * is arranged so that firing is hard to do by accident and clearing is hard to
   * prevent:
   *
   * - The pin handles are file-scope `static` in `output_switch.cpp`, so no other
   *   translation unit can take a handle and drive them. The only way to move
   *   these lines is Set(). This is the same containment `s_adxl_int1` uses in
   *   `app.cpp`, applied to a more dangerous signal.
   * - Fire1 and Fire2 are **one logical channel**, never addressable separately.
   *   A half-asserted output is not a state this class can be left in: if either
   *   line fails to drive, both are taken low.
   * - Any failure **latches the switch faulty** and it refuses to assert again.
   *   Refusing to fire is the safe failure; refusing to clear is not, so a failed
   *   clear is retried and logged at error level rather than latched away.
   * - The pins carry external 10 kΩ pull-downs, so the hardware is already
   *   fail-safe through reset. Init() configures them `GPIO_OUTPUT_INACTIVE` and
   *   this class never uses `GPIO_OUTPUT_ACTIVE`, so firmware cannot undo that.
   *
   * The caller supplies the condition and this class supplies none of it. The
   * only sanctioned source is `App::IsOutputActive()` — see `docs/v1-scope.md`
   * section 1.0. Nothing here reads INT1, the AWAKE bit or the arm state.
   */
  class OutputSwitch
  {
    public:
      OutputSwitch();

      /**
       * @brief Configure both fire pins as outputs, de-energised.
       *
       * Attempts to enable the input buffer alongside the output so that Set()
       * can read back what it drove. If the SoC will not do both at once the
       * configuration falls back to output-only and read-back verification is
       * disabled for the life of the object — recorded rather than silently
       * skipped, because an unverifiable fire output is worth knowing about.
       *
       * @return 0 on success; negative errno if either pin is unavailable. On
       *         failure the switch is latched faulty and will never assert.
       */
      int Init();

      /**
       * @brief Drive the fire output.
       *
       * @param assert True to energise both lines, false to de-energise both.
       * @return 0 on success; -EPERM if asked to assert while faulty or
       *         uninitialised; negative errno if a pin write or read-back failed.
       *
       * @note Asserting while faulty is refused and both lines are forced low.
       *       Clearing is always attempted, whatever the state of the object.
       */
      int Set(bool assert);

      /**
       * @brief Drive both lines low unconditionally, for fault and shutdown paths.
       *
       * Safe to call before Init() and safe to call repeatedly.
       *
       * @return 0 on success; negative errno if a pin write failed.
       */
      int ForceSafe();

      /** @brief Whether the output is currently energised. */
      bool IsAsserted() const { return m_asserted; }

      /** @brief True when the switch is initialised and has not latched faulty. */
      bool IsUsable() const { return m_initialised && !m_faulted; }

      /** @brief True when a failure has permanently disabled asserting. */
      bool IsFaulted() const { return m_faulted; }

      /** @brief True when Set() is able to verify what it drove. */
      bool IsVerified() const { return m_readback_supported; }

    private:
      // Drives both lines to the same level. Returns the first error encountered,
      // having still attempted the second pin - a half-driven output must never
      // be left behind because the first write failed.
      int driveBoth(bool assert);

      // Confirms both pins read back at the level just driven. Returns 0 when
      // read-back is unsupported, so an unverifiable build still operates.
      int verifyBoth(bool assert);

      // Latches the fault, forces both lines low and logs. One place, so every
      // failure path behaves identically.
      void enterFaultState(const char* reason, int errorCode);

      bool m_initialised;
      bool m_faulted;
      bool m_asserted;
      bool m_readback_supported;
  };

}
