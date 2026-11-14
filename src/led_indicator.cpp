#include "led_indicator.hpp"

namespace alc
{

  LedLevels BaseLevels(const LedInputs& inputs, int64_t nowMs)
  {
    LedLevels levels { false, false, false };

    switch (inputs.state) {
      case ArmState::Active:
        return levels;

      case ArmState::Arming:
        levels.red = true;
        return levels;

      case ArmState::Inactive:
        break;
    }

    // Before the time sync the device only waits for its clock: simulated firing
    // starts once the clock is set (owner, 2026-11-12).
    if (inputs.lowBattery) {
      levels.orange = static_cast<uint64_t>(nowMs) % M_LOW_BATTERY_FLASH_PERIOD_MS < M_LOW_BATTERY_FLASH_ON_MS;
    } else if (inputs.faultLatched) {
      levels.orange = true;
    } else if (!inputs.clockValid) {
      levels.green = true;
    } else if (inputs.simulatedFiring) {
      levels.red = true;
    } else {
      levels.green = static_cast<uint64_t>(nowMs) % M_GREEN_FLASH_PERIOD_MS < M_GREEN_FLASH_ON_MS;
    }
    return levels;
  }

  LedLevels OverlayLevels(LedPattern pattern, bool on)
  {
    LedLevels levels { false, false, false };

    switch (ColourOf(pattern)) {
      case LedColour::Red:
        levels.red = on;
        break;

      case LedColour::Orange:
        levels.orange = on;
        break;

      case LedColour::Green:
        levels.green = on;
        break;

      case LedColour::None:
        break;
    }
    return levels;
  }

  bool LivenessBlink::Tick()
  {
    bool lit { false };

    if (++m_ticks >= m_period_ticks) {
      m_ticks           = 0;
      m_remaining_ticks = m_on_ticks;
    }

    // Read before the decrement, so a one-tick blink is lit for its one tick.
    lit = m_remaining_ticks > 0;
    if (lit) { --m_remaining_ticks; }
    return lit;
  }

}
