#include "led_sequencer.hpp"

namespace alc
{

  namespace
  {
    struct Step
    {
        uint16_t onMs;
        uint16_t offMs;
    };

    struct Definition
    {
        const Step* steps;
        uint8_t stepCount;
        uint16_t durationMs;
    };

    constexpr uint16_t M_LONG_PATTERN_MS { 3000 };

    constexpr Step M_ARMED[] { { 60, 65 } };
    constexpr Step M_DISARMED[] { { 500, 500 } };
    constexpr Step M_DELAY_CANCELLED[] { { 100, 100 }, { 100, 700 } };
    constexpr Step M_ARM_REFUSED[] { { 700, 300 } };
    constexpr Step M_SINGLE_BLINK[] { { 200, 0 } };
    constexpr Step M_DOUBLE_BLINK[] { { 200, 200 }, { 200, 0 } };

    constexpr uint16_t M_SINGLE_BLINK_MS { 200 };
    constexpr uint16_t M_DOUBLE_BLINK_MS { 600 };

    Definition definitionOf(LedPattern pattern)
    {
      switch (pattern) {
        case LedPattern::Armed:
          return { M_ARMED, 1, M_LONG_PATTERN_MS };
        case LedPattern::Disarmed:
          return { M_DISARMED, 1, M_LONG_PATTERN_MS };
        case LedPattern::DisarmedDelayCancelled:
          return { M_DELAY_CANCELLED, 2, M_LONG_PATTERN_MS };
        case LedPattern::ArmRefused:
          return { M_ARM_REFUSED, 1, M_LONG_PATTERN_MS };
        case LedPattern::SettingsApplied:
          return { M_SINGLE_BLINK, 1, M_SINGLE_BLINK_MS };
        case LedPattern::ModeChanged:
          return { M_DOUBLE_BLINK, 2, M_DOUBLE_BLINK_MS };
        default:
          return { nullptr, 0, 0 };
      }
    }
  }

  LedSequencer::LedSequencer()
      : m_pattern(LedPattern::None)
      , m_start_ms(0)
  {}

  void LedSequencer::Start(LedPattern pattern, int64_t nowMs)
  {
    m_pattern  = pattern;
    m_start_ms = nowMs;
  }

  bool LedSequencer::IsActive(int64_t nowMs) const
  {
    Definition definition { definitionOf(m_pattern) };
    int64_t elapsed { nowMs - m_start_ms };

    return definition.stepCount > 0 && elapsed >= 0 && elapsed < definition.durationMs;
  }

  bool LedSequencer::Level(int64_t nowMs) const
  {
    Definition definition { definitionOf(m_pattern) };
    uint32_t cycleMs { 0 };
    uint32_t position { 0 };

    if (!IsActive(nowMs)) { return false; }

    for (uint8_t index = 0; index < definition.stepCount; index++) {
      cycleMs += definition.steps[index].onMs + definition.steps[index].offMs;
    }

    position = static_cast<uint32_t>(nowMs - m_start_ms) % cycleMs;
    for (uint8_t index = 0; index < definition.stepCount; index++) {
      if (position < definition.steps[index].onMs) { return true; }
      position -= definition.steps[index].onMs;
      if (position < definition.steps[index].offMs) { return false; }
      position -= definition.steps[index].offMs;
    }
    return false;
  }

}
