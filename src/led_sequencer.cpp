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
    constexpr uint16_t M_TRIPLE_PATTERN_MS { 1000 };
    constexpr uint16_t M_MISMATCH_PATTERN_MS { 5000 };

    constexpr Step M_RED_FLASH[] { { 60, 65 } };
    constexpr Step M_ORANGE_TRIPLE[] { { 150, 200 } };
    constexpr Step M_ORANGE_STEADY[] { { M_LONG_PATTERN_MS, 0 } };
    constexpr Step M_ORANGE_RAPID[] { { 100, 100 } };

    Definition definitionOf(LedPattern pattern)
    {
      switch (pattern) {
        case LedPattern::RedFlash:
          return { M_RED_FLASH, 1, M_LONG_PATTERN_MS };
        case LedPattern::OrangeTriple:
          return { M_ORANGE_TRIPLE, 1, M_TRIPLE_PATTERN_MS };
        case LedPattern::OrangeSteady:
          return { M_ORANGE_STEADY, 1, M_LONG_PATTERN_MS };
        case LedPattern::OrangeRapid:
          return { M_ORANGE_RAPID, 1, M_MISMATCH_PATTERN_MS };
        default:
          return { nullptr, 0, 0 };
      }
    }

    // Higher wins (spec section 3.3). None is lowest so it never blocks anything.
    constexpr uint8_t M_PRIORITY_NONE { 0 };
    constexpr uint8_t M_PRIORITY_CONFIRMATION { 1 };
    constexpr uint8_t M_PRIORITY_MISMATCH { 2 };
    constexpr uint8_t M_PRIORITY_RED_FLASH { 3 };
    constexpr uint8_t M_PRIORITY_WARNING { 4 };

    uint8_t priorityOf(LedPattern pattern)
    {
      switch (pattern) {
        case LedPattern::OrangeSteady:
          return M_PRIORITY_WARNING;
        case LedPattern::RedFlash:
          return M_PRIORITY_RED_FLASH;
        case LedPattern::OrangeRapid:
          return M_PRIORITY_MISMATCH;
        case LedPattern::OrangeTriple:
          return M_PRIORITY_CONFIRMATION;
        default:
          return M_PRIORITY_NONE;
      }
    }
  }

  LedColour ColourOf(LedPattern pattern)
  {
    switch (pattern) {
      case LedPattern::RedFlash:
        return LedColour::Red;
      case LedPattern::OrangeTriple:
      case LedPattern::OrangeSteady:
      case LedPattern::OrangeRapid:
        return LedColour::Orange;
      default:
        return LedColour::None;
    }
  }

  LedSequencer::LedSequencer()
      : m_pattern(LedPattern::None)
      , m_start_ms(0)
  {}

  bool LedSequencer::Start(LedPattern pattern, int64_t nowMs)
  {
    if (IsActive(nowMs) && priorityOf(pattern) < priorityOf(m_pattern)) { return false; }

    m_pattern  = pattern;
    m_start_ms = nowMs;
    return true;
  }

  bool LedSequencer::IsActive(int64_t nowMs) const
  {
    Definition definition { definitionOf(m_pattern) };
    int64_t elapsed { nowMs - m_start_ms };

    return definition.stepCount > 0 && elapsed >= 0 && elapsed < M_FRAME_GAP_MS + definition.durationMs + M_FRAME_GAP_MS;
  }

  bool LedSequencer::Level(int64_t nowMs) const
  {
    Definition definition { definitionOf(m_pattern) };
    int64_t elapsed { nowMs - m_start_ms };
    uint32_t cycleMs { 0 };
    uint32_t position { 0 };

    if (!IsActive(nowMs)) { return false; }

    // The dark frame either side of the steps - see M_FRAME_GAP_MS.
    if (elapsed < M_FRAME_GAP_MS || elapsed >= M_FRAME_GAP_MS + definition.durationMs) { return false; }

    for (uint8_t index = 0; index < definition.stepCount; index++) {
      cycleMs += definition.steps[index].onMs + definition.steps[index].offMs;
    }

    position = static_cast<uint32_t>(elapsed - M_FRAME_GAP_MS) % cycleMs;
    for (uint8_t index = 0; index < definition.stepCount; index++) {
      if (position < definition.steps[index].onMs) { return true; }
      position -= definition.steps[index].onMs;
      if (position < definition.steps[index].offMs) { return false; }
      position -= definition.steps[index].offMs;
    }
    return false;
  }

}
