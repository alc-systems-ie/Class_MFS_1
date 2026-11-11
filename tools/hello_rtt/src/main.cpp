#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hello_rtt, LOG_LEVEL_INF);

namespace alc {

constexpr static int32_t M_HEARTBEAT_INTERVAL_MS { 1000 };

// The heartbeat also proves the kernel timer (GRTC on LFXO) is ticking: if the
// banner appears but the count never advances, suspect the 32.768 kHz crystal.
int run()
{
  uint32_t count { 0 };

  LOG_INF("Hello world from CLASS MFS_1 (%s).", CONFIG_BOARD_TARGET);

  while (true) {
    LOG_INF("Heartbeat %u", count);
    count++;
    k_msleep(M_HEARTBEAT_INTERVAL_MS);
  }

  return 0;
}

} // namespace alc

int main()
{
  return alc::run();
}
