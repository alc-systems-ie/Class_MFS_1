#pragma once

namespace alc
{

  /**
   * @brief The device's fire output — two GPIOs asserted together as one channel.
   *
   * **Hardware topology, which drives every decision below.** Fire1 and Fire2 are
   * the gates of **two MOSFETs in series**, one either side of the switch. Current
   * flows only when **both** are on. Two consequences follow, and both run against
   * the intuition that "half asserted" is the thing to fear:
   *
   * - **A half-asserted output does not fire.** The unasserted MOSFET blocks the
   *   circuit, so a partial drive is safe, merely wrong.
   * - **Failing to clear one gate is not an emergency; failing to clear both is.**
   *   These are therefore logged differently. If every failure shouts equally the
   *   one that matters is lost among the ones that do not.
   *
   * **The dangerous failure is the latent one** — a single channel silently stuck
   * on, leaving the device apparently protected by two MOSFETs when only one is
   * doing any work. It stays safe until an unrelated second failure fires it. That
   * is what the read-back verification is for: it cannot prove a MOSFET conducts,
   * but it can catch a gate that is not where it was told to be.
   *
   * **In the product this output switches a voltage.** The rest of the class is
   * arranged so firing is hard to do by accident and clearing is hard to prevent:
   *
   * - The pin handles are file-scope `static` in `output_switch.cpp`, so no other
   *   translation unit can take a handle and drive them. The only ways to move
   *   these lines are Enable(), Set() and Disable(). This is the same
   *   containment `s_adxl_int1` uses in `app.cpp`, applied to a more dangerous
   *   signal.
   * - Fire1 and Fire2 are **one logical channel**, never addressable separately.
   *   Both writes are always attempted, so a gate is never left energised because
   *   an earlier call bailed out.
   * - A failure **latches the switch faulty** and it refuses to assert again.
   *   Refusing to fire is the safe failure. A lost clear also latches, because the
   *   series redundancy it depended on is then spent.
   * - **The pins have no driver unless the switch is enabled.** The gates carry
   *   external 10 kΩ pull-downs, so both MOSFETs are off from reset and the
   *   hardware is already fail-safe. While the device is not Active both pins are
   *   `GPIO_DISCONNECTED` - input and output buffers off - and only the resistors
   *   hold the lines. Enable() makes them outputs (`GPIO_OUTPUT_INACTIVE`, never
   *   `GPIO_OUTPUT_ACTIVE`) as the last step of arming; Disable() drives both low
   *   and disconnects both again. See the arming sequence amendment section 1.
   * - **Boot check.** Init() briefly makes each pin a plain input (no pull), reads
   *   it - it must read low, held by its pull-down - and disconnects it. A line
   *   that reads high is held on by something other than this firmware, so the
   *   switch latches faulty and the device can never arm.
   *
   * @warning **Verification stops at the gate.** Nothing here senses the load, so
   *          a MOSFET that fails short is invisible to this class. Proving the
   *          switch itself would need load-side feedback the board does not have.
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
       * @brief The boot check. Leaves both fire pins isolated (`GPIO_DISCONNECTED`).
       *
       * Each pin is configured as a plain input with no pull, read raw - it must
       * read low - and then disconnected. Both pins are always checked and always
       * disconnected, whatever the first one did.
       *
       * @return 0 when both read low; -ENODEV if the port is not ready; -EIO if a
       *         pin read high; negative errno if a configure or read failed. On
       *         failure the switch is latched faulty, so Enable() will refuse and
       *         the device can never arm.
       */
      int Init();

      /**
       * @brief Make both fire pins outputs, de-energised, verified low. The last step of arming.
       *
       * Attempts to enable the input buffer alongside the output so that Set()
       * can read back what it drove. If the SoC will not do both at once the
       * configuration falls back to output-only and read-back verification is
       * disabled until the next Enable() - recorded rather than silently skipped,
       * because an unverifiable fire output is worth knowing about.
       *
       * @return 0 on success; -EPERM if the switch is faulted or its boot check
       *         did not pass; negative errno if a configure, write or read-back
       *         failed. On failure the pins are disabled again and the switch is
       *         latched faulty.
       */
      int Enable();

      /**
       * @brief Drive both fire pins low, then disconnect both. The first action of every disarm.
       *
       * Driving low first empties a gate that was high at once, rather than at
       * the pull-down's RC rate. Every step is attempted on both pins whatever an
       * earlier step returned, and the switch is disabled afterwards regardless.
       * Safe to call in any state, repeatedly.
       *
       * @return 0 on success; negative errno if any write or disconnect failed, in
       *         which case the pins may not be isolated and the switch is latched
       *         faulty.
       */
      int Disable();

      /**
       * @brief Drive the fire output.
       *
       * @param assert True to energise both lines, false to de-energise both.
       * @return 0 on success; -EPERM if asked to assert while faulty,
       *         uninitialised or disabled; negative errno if a pin write or
       *         read-back failed.
       *
       * @note Set(false) while disabled does nothing and returns 0: the pins have
       *       no driver to clear and no input buffer to verify against, so a
       *       read-back would read garbage and falsely latch a fault.
       * @note Set(true) while disabled means a caller bypassed the arming
       *       sequence. It is refused and latches the switch faulty.
       * @note Asserting while faulty is refused and both lines are forced low.
       *       Clearing an enabled switch is always attempted, whatever the state
       *       of the object.
       */
      int Set(bool assert);

      /**
       * @brief Drive both lines low, for fault and shutdown paths.
       *
       * Does nothing and returns 0 while disabled - the pins then have no driver
       * to drive low. Safe to call before Init() and safe to call repeatedly.
       *
       * @return 0 on success; negative errno if a pin write failed.
       */
      int ForceSafe();

      /**
       * @brief Predicate consulted immediately before the gates are driven high.
       *
       * Returning false REFUSES the assertion. This is a second, independent
       * layer: the caller is expected to have already declined to ask. Reaching
       * a refusal therefore means the first layer failed, which is a bug rather
       * than a routine condition, and is treated as one.
       */
      using InterlockFn = bool (*)(void* context);

      /** @brief Install the interlock. Passing nullptr removes it. */
      void SetInterlock(InterlockFn interlock, void* context);

      /** @brief Whether the output is currently energised. */
      bool IsAsserted() const { return m_asserted; }

      /** @brief True when the boot check passed and the switch has not latched faulty. */
      bool IsUsable() const { return m_initialised && !m_faulted; }

      /** @brief True while the pins are outputs - between a successful Enable() and the next Disable(). */
      bool IsEnabled() const { return m_enabled; }

      /** @brief True when a failure has permanently disabled asserting. */
      bool IsFaulted() const { return m_faulted; }

      /** @brief True when Set() is able to verify what it drove - decided at the last Enable(). */
      bool IsVerified() const { return m_readback_supported; }

    private:
      // Drives both lines to the same level. Returns the first error encountered,
      // having still attempted the second pin - a half-driven output must never
      // be left behind because the first write failed.
      int driveBoth(bool assert);

      // Confirms both pins read back at the level just driven. Returns 0 when
      // read-back is unsupported, so an unverifiable build still operates.
      int verifyBoth(bool assert);

      // Drives both lines low, then disconnects both, attempting every step on both
      // pins. Disables the switch regardless. Logs with the one-versus-both
      // distinction and returns the first error. Does not latch the fault.
      int isolateBoth();

      // Latches the fault, isolates both lines and logs. One place, so every
      // failure path behaves identically.
      void enterFaultState(const char* reason, int errorCode);

      bool m_initialised;
      bool m_faulted;
      bool m_asserted;
      bool m_enabled;
      bool m_readback_supported;
      InterlockFn m_interlock;
      void* m_interlock_context;
  };

}
