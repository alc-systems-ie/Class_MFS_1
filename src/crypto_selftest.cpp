#include <cerrno>
#include <cstring>

#include <zephyr/logging/log.h>

#include "access_keys.hpp"
#include "access_vectors.hpp"
#include "crypto_selftest.hpp"

LOG_MODULE_REGISTER(crypto_selftest, LOG_LEVEL_INF);

namespace alc::crypto
{

  int RunSelfTest()
  {
    using namespace alc::access;

    uint8_t dayKey[M_DAY_KEY_BYTES] {};
    uint8_t onAir[protocol::M_UUID_BYTES] {};
    uint8_t plaintext[protocol::M_PLAINTEXT_BYTES] {};
    uint32_t unixSeconds { 0 };
    uint8_t index { 0 };

    for (const vectors::CommandVector& vector : vectors::M_COMMANDS) {
      if (DeriveDayKey(vectors::M_SECRET, vectors::M_DEVICE_ID, vector.day, vector.slot, dayKey) != 0 ||
          memcmp(dayKey, vector.dayKey, sizeof(dayKey)) != 0) {
        LOG_ERR("Self-test vector %u: day key mismatch!", index);
        return -EBADMSG;
      }
      if (SealCommand(dayKey, vectors::M_DEVICE_ID, vector.day, vector.slot, vector.n, vector.plaintext, onAir) != 0 ||
          memcmp(onAir, vector.onAir, sizeof(onAir)) != 0) {
        LOG_ERR("Self-test vector %u: sealed command mismatch - CCM disagrees with the app!", index);
        return -EBADMSG;
      }
      if (OpenCommand(dayKey, vectors::M_DEVICE_ID, vector.day, vector.slot, vector.n, vector.onAir, plaintext) != 0 ||
          memcmp(plaintext, vector.plaintext, sizeof(plaintext)) != 0) {
        LOG_ERR("Self-test vector %u: open failed!", index);
        return -EBADMSG;
      }

      // A corrupted tag must be REJECTED. A backend that accepts anything would
      // pass every check above.
      memcpy(onAir, vector.onAir, sizeof(onAir));
      onAir[protocol::M_OFFSET_TAG] ^= 0x01;
      if (OpenCommand(dayKey, vectors::M_DEVICE_ID, vector.day, vector.slot, vector.n, onAir, plaintext) != -EBADMSG) {
        LOG_ERR("Self-test vector %u: a corrupted tag was not rejected!", index);
        return -EBADMSG;
      }
      index++;
    }

    if (!OpenTimeSync(vectors::M_PROVISION_KEY, vectors::M_DEVICE_ID, vectors::M_TIME_SYNC_ON_AIR, unixSeconds) ||
        unixSeconds != vectors::M_TIME_SYNC_UNIX) {
      LOG_ERR("Self-test: time sync vector failed!");
      return -EBADMSG;
    }

    LOG_INF("Crypto self-test passed: %u command vectors and the time-sync vector match.", index);
    return 0;
  }

}
