#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "npm2100.hpp"
#include "npm2100_zephyr.hpp"
#include "range_protocol.hpp"

LOG_MODULE_REGISTER(range_pong, LOG_LEVEL_INF);

// Range test, pong side (CLASS MFS_1, nRF21540 FEM at +10 dB, LNA on receive).
//
// Scans the Coded PHY continuously. Each new ping sequence number from tools/range_ping
// flashes LED2 and is answered: the pong advertisement (Coded PHY S8) is updated to echo
// that sequence number and the RSSI the ping arrived at. The pong advertises until
// M_PONG_HOLD_MS passes with no ping, then stops.
//
// The fire pins are never touched: they stay at their reset state, held off by the
// external pull-downs.

namespace alc
{

  namespace
  {
    const struct device* const s_pmic_bus { DEVICE_DT_GET(DT_NODELABEL(i2c21)) };
    const gpio_dt_spec s_led2 = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), led2_gpios);

    constexpr int32_t M_LED_FLASH_MS { 100 };
    constexpr int32_t M_PONG_HOLD_MS { 5000 };
    constexpr int32_t M_SUMMARY_INTERVAL_MS { 10000 };

    struct PendingPing
    {
        uint16_t sequence;
        int8_t rssiDbm;
    };

    k_spinlock s_lock;
    PendingPing s_pending { 0, 0 };
    bool s_have_last { false };
    uint16_t s_last_sequence { 0 };
    uint32_t s_pings_heard { 0 };

    bt_le_ext_adv* s_adv { nullptr };
    bool s_advertising { false };
    uint8_t s_adv_buffer[range::M_PONG_LENGTH];

    void ledOff(k_work* work)
    {
      ARG_UNUSED(work);
      gpio_pin_set_dt(&s_led2, 0);
    }

    // System work queue: owns the advertising set, so start, update and stop never race.
    void sendPong(k_work* work)
    {
      PendingPing ping { 0, 0 };
      range::Frame frame { range::FrameType::Pong, 0, 0 };
      bt_data advertisingData {};
      int result { 0 };

      ARG_UNUSED(work);
      K_SPINLOCK(&s_lock)
      { ping = s_pending; }

      frame.sequence           = ping.sequence;
      frame.rssiDbm            = ping.rssiDbm;
      advertisingData.type     = BT_DATA_MANUFACTURER_DATA;
      advertisingData.data_len = static_cast<uint8_t>(range::Encode(frame, s_adv_buffer));
      advertisingData.data     = s_adv_buffer;

      result = bt_le_ext_adv_set_data(s_adv, &advertisingData, 1, nullptr, 0);
      if (result < 0) {
        LOG_ERR("Pong data update failed: %d!", result);
        return;
      }

      if (!s_advertising) {
        result = bt_le_ext_adv_start(s_adv, BT_LE_EXT_ADV_START_DEFAULT);
        if (result < 0) {
          LOG_ERR("Pong advertising start failed: %d!", result);
          return;
        }
        s_advertising = true;
      }

      LOG_INF("Ping %u at %d dBm - pong sent.", ping.sequence, ping.rssiDbm);
    }

    void stopPong(k_work* work)
    {
      int result { 0 };

      ARG_UNUSED(work);
      if (!s_advertising) { return; }

      result = bt_le_ext_adv_stop(s_adv);
      if (result < 0) {
        LOG_ERR("Pong advertising stop failed: %d!", result);
        return;
      }
      s_advertising = false;
      LOG_INF("No ping for %d ms - pong stopped.", M_PONG_HOLD_MS);
    }

    K_WORK_DELAYABLE_DEFINE(s_led_off_work, ledOff);
    K_WORK_DEFINE(s_pong_work, sendPong);
    K_WORK_DELAYABLE_DEFINE(s_pong_stop_work, stopPong);

    bool parseAdvertisingData(bt_data* data, void* userData)
    {
      auto* frame { static_cast<range::Frame*>(userData) };

      if (data->type != BT_DATA_MANUFACTURER_DATA) { return true; }
      if (range::Decode(data->data, data->data_len, *frame)) { return false; }
      return true;
    }

    // Bluetooth RX thread. The ping repeats about ten times per sequence number; only the first is answered.
    void onScanReceive(const bt_le_scan_recv_info* info, net_buf_simple* buffer)
    {
      range::Frame frame { range::FrameType::Pong, 0, 0 };
      bool fresh { false };

      bt_data_parse(buffer, parseAdvertisingData, &frame);
      if (frame.type != range::FrameType::Ping) { return; }

      K_SPINLOCK(&s_lock)
      {
        if (!s_have_last || frame.sequence != s_last_sequence) {
          s_have_last        = true;
          s_last_sequence    = frame.sequence;
          s_pending.sequence = frame.sequence;
          s_pending.rssiDbm  = info->rssi;
          s_pings_heard++;
          fresh = true;
        }
      }

      if (!fresh) { return; }
      gpio_pin_set_dt(&s_led2, 1);
      k_work_reschedule(&s_led_off_work, K_MSEC(M_LED_FLASH_MS));
      k_work_submit(&s_pong_work);
      k_work_reschedule(&s_pong_stop_work, K_MSEC(M_PONG_HOLD_MS));
    }

    bt_le_scan_cb s_scan_callbacks {
      .recv = onScanReceive,
    };

    // The nPM2100 boot monitor resets the SoC about 9 s after boot unless it is stopped.
    void stopBootMonitor()
    {
      Npm2100 pmic { npm2100_zephyr::MakeZephyrTransport(s_pmic_bus) };
      int result { pmic.Init() };

      if (result == 0) { result = pmic.TimerStop(); }
      if (result < 0) {
        LOG_ERR("Failed to stop the nPM2100 boot monitor: %d!", result);
        return;
      }
      LOG_INF("nPM2100 boot monitor stopped.");
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

      LOG_INF("Listening for pings on Coded PHY; pongs at +10 dBm via the nRF21540, S8.");
      return 0;
    }

    int run()
    {
      uint32_t heard { 0 };
      uint32_t lastHeard { 0 };
      int result { 0 };

      if (!device_is_ready(s_pmic_bus) || !gpio_is_ready_dt(&s_led2)) {
        LOG_ERR("PMIC bus or LED2 GPIO not ready!");
        return -ENODEV;
      }

      LOG_INF("Range pong (MFS_1) boot.");
      stopBootMonitor();

      result = gpio_pin_configure_dt(&s_led2, GPIO_OUTPUT_INACTIVE);
      if (result < 0) {
        LOG_ERR("LED2 configure failed: %d!", result);
        return result;
      }

      result = initRadio();
      if (result < 0) { return result; }

      while (true) {
        k_msleep(M_SUMMARY_INTERVAL_MS);

        K_SPINLOCK(&s_lock)
        { heard = s_pings_heard; }
        LOG_INF("Pings heard: %u in the last %d s, %u total.", heard - lastHeard, M_SUMMARY_INTERVAL_MS / MSEC_PER_SEC, heard);
        lastHeard = heard;
      }

      return 0;
    }
  }

}

int main()
{ return alc::run(); }
