#pragma once

#include <cstdint>

namespace alc
{

  /**
   * @brief BLE observer that receives engineer commands from advertisements.
   *
   * MFS_1 never advertises and never connects (docs/v1-scope.md section 4). The
   * engineer's tool advertises; this scans passively for it. Scan duty cycling is
   * handled by the controller via the interval/window pair, so the SoC sleeps in
   * System ON idle between windows.
   *
   * v1 accepts a single plaintext toggle command, gated behind
   * CONFIG_MFS_INSECURE_TOGGLE. TAN validation and provisioner time sync replace
   * it later — see docs/tan-scheme.md.
   */
  class CommandScanner
  {
    public:
      /** @brief Commands carried in the advertising payload. */
      enum class Command : uint8_t { None = 0x00, ToggleArm = 0x01 };

      CommandScanner();

      /**
       * @brief Enable Bluetooth and start the passive scan.
       * @return 0 on success; negative errno from bt_enable() or bt_le_scan_start().
       */
      int Start();

      /**
       * @brief Consume the pending command, if any.
       *
       * Commands arrive on the Bluetooth RX thread; this hands them to the main loop
       * so no application work happens in that context.
       *
       * @return The pending command, or Command::None. Clears the pending slot.
       */
      Command TakePendingCommand();

    private:
      bool m_started;
  };

}
