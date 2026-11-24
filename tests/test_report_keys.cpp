#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "access_vectors.hpp"
#include "host_kmu.hpp"
#include "report_keys.hpp"

void run_report_key_tests()
{
  using namespace alc;
  using namespace alc::report;

  const crypto::KeyRef secret { crypto::KeyRef::Raw(access::vectors::M_SECRET, sizeof(access::vectors::M_SECRET)) };
  constexpr uint32_t M_DEVICE_ID { access::vectors::M_DEVICE_ID };

  // Vectors come from tools/gen_access_vectors.py (Python `cryptography`), not from this code.
  for (const access::vectors::ReportVector& vector : access::vectors::M_REPORTS) {
    uint8_t reportKey[M_REPORT_KEY_BYTES] {};
    uint8_t encKey[crypto::M_AES128_KEY_BYTES] {};
    uint8_t reportId[M_REPORT_ID_BYTES] {};
    uint8_t onAir[M_ON_AIR_BYTES] {};
    uint8_t plaintext[M_PLAINTEXT_BYTES] {};

    assert(DeriveReportKey(secret, M_DEVICE_ID, vector.generation, reportKey) == 0);
    assert(memcmp(reportKey, vector.reportKey, sizeof(reportKey)) == 0);

    assert(DeriveReportEncKey(reportKey, encKey) == 0);
    assert(memcmp(encKey, vector.encKey, sizeof(encKey)) == 0);

    assert(DeriveReportId(reportKey, vector.counter, reportId) == 0);
    assert(memcmp(reportId, vector.reportId, sizeof(reportId)) == 0);

    assert(SealReport(reportKey, M_DEVICE_ID, vector.generation, vector.counter, vector.plaintext, onAir) == 0);
    assert(memcmp(onAir, vector.onAir, sizeof(onAir)) == 0);

    assert(OpenReport(reportKey, M_DEVICE_ID, vector.generation, vector.counter, vector.onAir, plaintext) == 0);
    assert(memcmp(plaintext, vector.plaintext, sizeof(plaintext)) == 0);

    // A flipped bit in the ID, ciphertext or tag must fail.
    for (const uint8_t position : { uint8_t { 0 }, M_OFFSET_CIPHERTEXT, uint8_t { M_ON_AIR_BYTES - 1 } }) {
      memcpy(onAir, vector.onAir, sizeof(onAir));
      onAir[position] ^= 0x01;
      assert(OpenReport(reportKey, M_DEVICE_ID, vector.generation, vector.counter, onAir, plaintext) == -EBADMSG);
    }

    // The generation and counter are bound into the nonce.
    assert(OpenReport(reportKey, M_DEVICE_ID, static_cast<uint16_t>(vector.generation + 1), vector.counter, vector.onAir, plaintext) == -EBADMSG);
    assert(OpenReport(reportKey, M_DEVICE_ID, vector.generation, vector.counter + 1, vector.onAir, plaintext) == -EBADMSG);
  }

  // Battery tenths.
  assert(BatteryTenths(2949) == 29);
  assert(BatteryTenths(2950) == 30);
  assert(BatteryTenths(0) == 0);
  assert(BatteryTenths(-1) == M_BATTERY_UNKNOWN);
  assert(BatteryTenths(30000) == 254);

  // Plaintext layout.
  {
    constexpr uint8_t M_BATTERY_TENTHS { 30 };
    const uint8_t expected[M_PLAINTEXT_BYTES] { 0x11, M_BATTERY_TENTHS, 0, 0, 0, 0, 0, 0 };
    uint8_t plaintext[M_PLAINTEXT_BYTES] { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };

    EncodeReport(ReportEvent::FiredFromMotion, M_BATTERY_TENTHS, plaintext);
    assert(memcmp(plaintext, expected, sizeof(plaintext)) == 0);
  }

  // The nonce carries the 0xFF marker where a command nonce carries its slot.
  {
    uint8_t nonce[M_NONCE_BYTES] {};

    BuildReportNonce(0x01020304, 0x0506, 0x0708090A, nonce);
    const uint8_t expected[M_NONCE_BYTES] { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, M_NONCE_MARKER, 0x07, 0x08, 0x09, 0x0A };
    assert(memcmp(nonce, expected, sizeof(nonce)) == 0);
  }

  // A KMU secret under an unprovisioned slot is -ENOKEY and writes nothing.
  {
    constexpr uint8_t M_EMPTY_SLOT { 140 };
    uint8_t reportKey[M_REPORT_KEY_BYTES] {};

    crypto::host_kmu::Clear();
    assert(DeriveReportKey(crypto::KeyRef::Kmu(M_EMPTY_SLOT), M_DEVICE_ID, 1, reportKey) == -ENOKEY);
  }

  printf("report keys: OK\n");
}
