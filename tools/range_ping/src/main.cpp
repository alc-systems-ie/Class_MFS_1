#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "range_protocol.hpp"

LOG_MODULE_REGISTER(range_ping, LOG_LEVEL_INF);

// Range test, ping side (Thingy:53, nRF21540 FEM at +20 dB).
//
// Once a second the sequence number in a Coded PHY (S8) advertisement steps on. The MFS_1
// running tools/range_pong answers each new sequence number with a pong carrying the RSSI it
// heard the ping at. The RGB LED reports each second, so the test needs no computer. A pong
// flashes a colour graded by the WEAKER of the two directions' RSSI (s_signal_colours):
//
//   green   - stronger than -70 dBm
//   yellow  - -70 to -85 dBm
//   red     - -85 to -95 dBm
//   magenta - -95 dBm or weaker: at the edge (Coded S8 sensitivity is about -103 dBm)
//   blue    - short blip: no pong for that ping; three longer blinks at boot
//   red, steady - the radio failed to start

namespace alc
{

  namespace
  {
    const gpio_dt_spec s_led_red   = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
    const gpio_dt_spec s_led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
    const gpio_dt_spec s_led_blue  = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

    constexpr int32_t M_PING_PERIOD_MS { 1000 };
    constexpr int32_t M_PONG_FLASH_MS { 200 };
    constexpr int32_t M_MISS_FLASH_MS { 40 };
    constexpr int32_t M_BOOT_BLINK_MS { 150 };
    constexpr uint8_t M_BOOT_BLINKS { 3 };
    constexpr uint16_t M_SUMMARY_EVERY_PINGS { 10 };
    constexpr uint32_t M_PERCENT { 100 };

    struct Colour
    {
        bool red;
        bool green;
        bool blue;
    };

    struct SignalColour
    {
        int8_t aboveDbm;
        Colour colour;
    };

    // Strongest first; the first entry the weaker RSSI is above wins, else M_EDGE_COLOUR.
    constexpr SignalColour s_signal_colours[] {
      { -70, { false, true, false } }, // Green.
      { -85, { true, true, false } },  // Yellow.
      { -95, { true, false, false } }, // Red.
    };
    constexpr Colour M_EDGE_COLOUR { true, false, true };   // Magenta.
    constexpr Colour M_MISS_COLOUR { false, false, true };  // Blue.
    constexpr Colour M_FAULT_COLOUR { true, false, false }; // Red, steady: radio init failed.

    Colour colourFor(int8_t rssiDbm)
    {
      for (const SignalColour& level : s_signal_colours) {
        if (rssiDbm > level.aboveDbm) { return level.colour; }
      }
      return M_EDGE_COLOUR;
    }

    struct PingState
    {
        uint16_t sequence;
        bool answered;
        int8_t rssiHereDbm;
        int8_t rssiThereDbm;
    };

    k_spinlock s_lock;
    PingState s_state { 0, false, 0, 0 };

    bt_le_ext_adv* s_adv { nullptr };
    uint8_t s_adv_buffer[range::M_PONG_LENGTH];

    void ledsOff(k_work* work)
    {
      ARG_UNUSED(work);
      gpio_pin_set_dt(&s_led_red, 0);
      gpio_pin_set_dt(&s_led_green, 0);
      gpio_pin_set_dt(&s_led_blue, 0);
    }

    K_WORK_DELAYABLE_DEFINE(s_leds_off_work, ledsOff);

    // Safe from any thread: the pins are set at once and the off is scheduled on the system work queue.
    // SYS_FOREVER_MS leaves the colour on.
    void flash(const Colour& colour, int32_t durationMs)
    {
      gpio_pin_set_dt(&s_led_red, colour.red ? 1 : 0);
      gpio_pin_set_dt(&s_led_green, colour.green ? 1 : 0);
      gpio_pin_set_dt(&s_led_blue, colour.blue ? 1 : 0);
      k_work_reschedule(&s_leds_off_work, SYS_TIMEOUT_MS(durationMs));
    }

    bool parseAdvertisingData(bt_data* data, void* userData)
    {
      auto* frame { static_cast<range::Frame*>(userData) };

      if (data->type != BT_DATA_MANUFACTURER_DATA) { return true; }
      if (range::Decode(data->data, data->data_len, *frame)) { return false; }
      return true;
    }

    // Bluetooth RX thread. Records the first pong for the current sequence and flashes the LED.
    void onScanReceive(const bt_le_scan_recv_info* info, net_buf_simple* buffer)
    {
      range::Frame frame { range::FrameType::Ping, 0, 0 };
      bool counted { false };

      bt_data_parse(buffer, parseAdvertisingData, &frame);
      if (frame.type != range::FrameType::Pong) { return; }

      K_SPINLOCK(&s_lock)
      {
        if (frame.sequence == s_state.sequence && !s_state.answered) {
          s_state.answered     = true;
          s_state.rssiHereDbm  = info->rssi;
          s_state.rssiThereDbm = frame.rssiDbm;
          counted              = true;
        }
      }

      if (!counted) { return; }
      flash(colourFor(MIN(info->rssi, frame.rssiDbm)), M_PONG_FLASH_MS);
    }

    bt_le_scan_cb s_scan_callbacks {
      .recv = onScanReceive,
    };

    int setPingData(uint16_t sequence)
    {
      const range::Frame frame { range::FrameType::Ping, sequence, 0 };
      bt_data advertisingData {
        .type     = BT_DATA_MANUFACTURER_DATA,
        .data_len = static_cast<uint8_t>(range::Encode(frame, s_adv_buffer)),
        .data     = s_adv_buffer,
      };

      return bt_le_ext_adv_set_data(s_adv, &advertisingData, 1, nullptr, 0);
    }

    int initLeds()
    {
      int result { 0 };

      if (!gpio_is_ready_dt(&s_led_red) || !gpio_is_ready_dt(&s_led_green) || !gpio_is_ready_dt(&s_led_blue)) {
        LOG_ERR("LED GPIO not ready!");
        return -ENODEV;
      }

      result = gpio_pin_configure_dt(&s_led_red, GPIO_OUTPUT_INACTIVE);
      if (result == 0) { result = gpio_pin_configure_dt(&s_led_green, GPIO_OUTPUT_INACTIVE); }
      if (result == 0) { result = gpio_pin_configure_dt(&s_led_blue, GPIO_OUTPUT_INACTIVE); }
      if (result < 0) {
        LOG_ERR("LED configure failed: %d!", result);
        return result;
      }

      for (uint8_t blink = 0; blink < M_BOOT_BLINKS; blink++) {
        gpio_pin_set_dt(&s_led_blue, 1);
        k_msleep(M_BOOT_BLINK_MS);
        gpio_pin_set_dt(&s_led_blue, 0);
        k_msleep(M_BOOT_BLINK_MS);
      }
      return 0;
    }

    int initRadio()
    {
      constexpr uint8_t M_ADV_SET_ID { 0 };
      const bt_le_adv_param advertisingParam {
        .id                 = BT_ID_DEFAULT,
        .sid                = M_ADV_SET_ID,
        .secondary_max_skip = 0,
        .options            = BT_LE_ADV_OPT_EXT_ADV | BT_LE_ADV_OPT_CODED | BT_LE_ADV_OPT_REQUIRE_S8_CODING,
        .interval_min       = range::M_ADV_INTERVAL,
        .interval_max       = range::M_ADV_INTERVAL,
        .peer               = nullptr,
      };
      bt_le_scan_param scanParam {};
      int result { bt_enable(nullptr) };

      if (result < 0) {
        LOG_ERR("Bluetooth init failed: %d!", result);
        return result;
      }

      result = bt_le_ext_adv_create(&advertisingParam, nullptr, &s_adv);
      if (result < 0) {
        LOG_ERR("Coded PHY advertising set create failed: %d!", result);
        return result;
      }

      result = setPingData(0);
      if (result == 0) { result = bt_le_ext_adv_start(s_adv, BT_LE_EXT_ADV_START_DEFAULT); }
      if (result < 0) {
        LOG_ERR("Ping advertising start failed: %d!", result);
        return result;
      }

      bt_le_scan_cb_register(&s_scan_callbacks);
      scanParam.type     = BT_LE_SCAN_TYPE_PASSIVE;
      scanParam.options  = BT_LE_SCAN_OPT_CODED | BT_LE_SCAN_OPT_NO_1M;
      scanParam.interval = range::M_SCAN_INTERVAL;
      scanParam.window   = range::M_SCAN_WINDOW;
      result             = bt_le_scan_start(&scanParam, nullptr);
      if (result < 0) {
        LOG_ERR("Coded PHY scan start failed: %d!", result);
        return result;
      }

      LOG_INF("Pinging on Coded PHY S8 every %d ms, scanning Coded PHY for pongs.", M_PING_PERIOD_MS);
      return 0;
    }

    int run()
    {
      PingState closed { 0, false, 0, 0 };
      uint32_t sent { 0 };
      uint32_t received { 0 };
      uint16_t windowSent { 0 };
      uint16_t windowReceived { 0 };
      uint16_t nextSequence { 0 };
      int64_t deadlineMs { 0 };
      int result { initLeds() };

      if (result < 0) { return result; }

      LOG_INF("Range ping (Thingy:53, nRF21540 at +20 dB) boot.");
      result = initRadio();
      if (result < 0) {
        flash(M_FAULT_COLOUR, SYS_FOREVER_MS);
        return result;
      }

      deadlineMs = k_uptime_get();
      while (true) {
        deadlineMs += M_PING_PERIOD_MS;
        k_sleep(K_TIMEOUT_ABS_MS(deadlineMs));

        K_SPINLOCK(&s_lock)
        { closed = s_state; }

        sent++;
        windowSent++;
        if (closed.answered) {
          received++;
          windowReceived++;
          LOG_INF("Ping %u: pong, RSSI here %d dBm, at MFS %d dBm.", closed.sequence, closed.rssiHereDbm, closed.rssiThereDbm);
        } else {
          LOG_INF("Ping %u: no pong.", closed.sequence);
          flash(M_MISS_COLOUR, M_MISS_FLASH_MS);
        }

        if (windowSent == M_SUMMARY_EVERY_PINGS) {
          LOG_INF("Last %u: %u answered. Total %u/%u (%u%%).", windowSent, windowReceived, received, sent, received * M_PERCENT / sent);
          windowSent     = 0;
          windowReceived = 0;
        }

        nextSequence = static_cast<uint16_t>(closed.sequence + 1);
        result       = setPingData(nextSequence);
        if (result < 0) { LOG_ERR("Ping data update failed: %d!", result); }

        K_SPINLOCK(&s_lock)
        {
          s_state.sequence = nextSequence;
          s_state.answered = false;
        }
      }

      return 0;
    }
  }

}

int main()
{ return alc::run(); }
