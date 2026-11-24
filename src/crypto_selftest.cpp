#include <cerrno>
#include <cstring>

#include <zephyr/logging/log.h>

#include "access_keys.hpp"
#include "access_vectors.hpp"
#include "crypto_selftest.hpp"
#include "report_keys.hpp"

LOG_MODULE_REGISTER(crypto_selftest, LOG_LEVEL_INF);

namespace alc::crypto
{

  int RunSelfTest()
  {
    using namespace alc::access;

    const KeyRef secret { KeyRef::Raw(vectors::M_SECRET, sizeof(vectors::M_SECRET)) };
    const KeyRef provisionKey { KeyRef::Raw(vectors::M_PROVISION_KEY, sizeof(vectors::M_PROVISION_KEY)) };
    uint8_t dayKey[M_DAY_KEY_BYTES] {};
    uint8_t onAir[protocol::M_UUID_BYTES] {};
    uint8_t plaintext[protocol::M_PLAINTEXT_BYTES] {};
    uint32_t unixSeconds { 0 };
    uint8_t index { 0 };

    for (const vectors::CommandVector& vector : vectors::M_COMMANDS) {
      if (DeriveDayKey(secret, vectors::M_DEVICE_ID, vector.day, vector.slot, dayKey) != 0 || memcmp(dayKey, vector.dayKey, sizeof(dayKey)) != 0) {
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

    if (!OpenTimeSync(provisionKey, vectors::M_DEVICE_ID, vectors::M_TIME_SYNC_ON_AIR, unixSeconds) || unixSeconds != vectors::M_TIME_SYNC_UNIX) {
      LOG_ERR("Self-test: time sync vector failed!");
      return -EBADMSG;
    }

    index = 0;
    for (const vectors::ReportVector& vector : vectors::M_REPORTS) {
      uint8_t reportKey[report::M_REPORT_KEY_BYTES] {};

      if (report::DeriveReportKey(secret, vectors::M_DEVICE_ID, vector.generation, reportKey) != 0 ||
          memcmp(reportKey, vector.reportKey, sizeof(reportKey)) != 0) {
        LOG_ERR("Self-test report vector %u: report key mismatch!", index);
        return -EBADMSG;
      }
      if (report::SealReport(reportKey, vectors::M_DEVICE_ID, vector.generation, vector.counter, vector.plaintext, onAir) != 0 ||
          memcmp(onAir, vector.onAir, sizeof(onAir)) != 0) {
        LOG_ERR("Self-test report vector %u: sealed report mismatch - CCM disagrees with the Network Manager!", index);
        return -EBADMSG;
      }
      if (report::OpenReport(reportKey, vectors::M_DEVICE_ID, vector.generation, vector.counter, vector.onAir, plaintext) != 0 ||
          memcmp(plaintext, vector.plaintext, sizeof(plaintext)) != 0) {
        LOG_ERR("Self-test report vector %u: open failed!", index);
        return -EBADMSG;
      }

      memcpy(onAir, vector.onAir, sizeof(onAir));
      onAir[report::M_OFFSET_TAG] ^= 0x01;
      if (report::OpenReport(reportKey, vectors::M_DEVICE_ID, vector.generation, vector.counter, onAir, plaintext) != -EBADMSG) {
        LOG_ERR("Self-test report vector %u: a corrupted tag was not rejected!", index);
        return -EBADMSG;
      }
      index++;
    }

    LOG_INF("Crypto self-test passed: the command, report and time-sync vectors match.");
    return 0;
  }

}
