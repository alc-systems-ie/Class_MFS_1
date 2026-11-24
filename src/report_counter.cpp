#include <cerrno>
#include <limits>

#include "report_counter.hpp"

namespace alc
{

  namespace
  {
    constexpr uint32_t M_COUNTER_MAX { std::numeric_limits<uint32_t>::max() };
  }

  ReportCounter::ReportCounter(PersistFn persist, void* context)
      : m_persist(persist)
      , m_context(context)
  {}

  int ReportCounter::Begin(uint32_t storedFloor)
  {
    int result { 0 };

    m_begun          = false;
    m_next           = 0;
    m_usable_end     = 0;
    m_reserved_up_to = 0;

    if (storedFloor > M_COUNTER_MAX - M_BLOCK) { return -EOVERFLOW; }

    result = m_persist(storedFloor + M_BLOCK, m_context);
    if (result < 0) { return result; }

    m_next           = storedFloor;
    m_reserved_up_to = storedFloor + M_BLOCK;
    m_usable_end     = m_reserved_up_to;
    m_begun          = true;
    return 0;
  }

  bool ReportCounter::Take(uint32_t& counter)
  {
    if (!m_begun || m_next >= m_usable_end) { return false; }

    counter = m_next;
    m_next++;
    return true;
  }

  bool ReportCounter::NeedsRefill() const
  { return m_begun && (m_usable_end - m_next) <= M_REFILL_AT; }

  int ReportCounter::Refill()
  {
    int result { 0 };

    if (!m_begun) { return -EINVAL; }
    if (m_reserved_up_to > M_COUNTER_MAX - M_BLOCK) { return -EOVERFLOW; }

    result = m_persist(m_reserved_up_to + M_BLOCK, m_context);
    if (result < 0) { return result; }

    m_reserved_up_to += M_BLOCK;
    m_usable_end = m_reserved_up_to;
    return 0;
  }

}
