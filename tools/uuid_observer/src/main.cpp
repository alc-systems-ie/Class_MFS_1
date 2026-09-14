#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdint.h>
#include <stdio.h>

LOG_MODULE_REGISTER(uuid_observer, LOG_LEVEL_INF);

// Bench diagnostic for the MFS_1 command path. Scans continuously and logs every
// complete or incomplete 128-bit service UUID list entry it hears, with the
// uptime, advertiser address and RSSI. Each line is one advert, so repeats show
// the phone's real advertising interval and a change of UUID shows exactly when
// a new command went on air.
namespace alc
{

  namespace
  {
    constexpr uint8_t M_UUID128_BYTES { 16 };
    constexpr uint16_t M_SCAN_INTERVAL { 0x0060 }; // 60 ms
    constexpr uint16_t M_SCAN_WINDOW { 0x0060 };   // 60 ms - continuous

    struct AdContext
    {
        char address[BT_ADDR_LE_STR_LEN];
        int8_t rssi;
    };

    bool parseAd(struct bt_data* data, void* userData)
    {
      AdContext* context { static_cast<AdContext*>(userData) };
      char hex[(2 * M_UUID128_BYTES) + 1] {};

      if (data->type != BT_DATA_UUID128_ALL && data->type != BT_DATA_UUID128_SOME) { return true; }

      for (uint8_t offset = 0; offset + M_UUID128_BYTES <= data->data_len; offset += M_UUID128_BYTES) {
        for (uint8_t index = 0; index < M_UUID128_BYTES; index++) {
          // On-air order, as MFS_1 logs and sees it.
          snprintf(&hex[2 * index], 3, "%02X", data->data[offset + index]);
        }
        LOG_INF("%s rssi %d uuid %s", context->address, context->rssi, hex);
      }
      return true;
    }

    void deviceFound(const bt_addr_le_t* address, int8_t rssi, uint8_t type, struct net_buf_simple* ad)
    {
      AdContext context {};

      ARG_UNUSED(type);
      bt_addr_le_to_str(address, context.address, sizeof(context.address));
      context.rssi = rssi;
      bt_data_parse(ad, parseAd, &context);
    }
  }

}

int main()
{
  const struct bt_le_scan_param scanParam {
    .type = BT_LE_SCAN_TYPE_PASSIVE, .options = BT_LE_SCAN_OPT_NONE, .interval = alc::M_SCAN_INTERVAL, .window = alc::M_SCAN_WINDOW,
  };
  int result { bt_enable(nullptr) };

  if (result < 0) {
    LOG_ERR("bt_enable failed: %d!", result);
    return result;
  }

  result = bt_le_scan_start(&scanParam, alc::deviceFound);
  if (result < 0) {
    LOG_ERR("bt_le_scan_start failed: %d!", result);
    return result;
  }

  LOG_INF("UUID observer scanning continuously.");
  return 0;
}
