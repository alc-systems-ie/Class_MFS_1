#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "access_keys.hpp"
#include "access_vectors.hpp"

void run_access_key_tests()
{
  using namespace alc;
  using namespace alc::access;

  // Every vector was produced by tools/gen_access_vectors.py using Python's
  // `cryptography` package - an implementation that is neither this one nor the
  // app's. Matching it is the only evidence the two sides agree.
  for (const vectors::CommandVector& vector : vectors::M_COMMANDS) {
    uint8_t dayKey[M_DAY_KEY_BYTES] {};
    uint8_t encKey[crypto::M_AES128_KEY_BYTES] {};
    uint8_t rotatingId[protocol::M_ROTATING_ID_BYTES] {};
    uint8_t onAir[protocol::M_UUID_BYTES] {};
    uint8_t plaintext[protocol::M_PLAINTEXT_BYTES] {};

    assert(DeriveDayKey(vectors::M_SECRET, vectors::M_DEVICE_ID, vector.day, vector.slot, dayKey) == 0);
    assert(memcmp(dayKey, vector.dayKey, sizeof(dayKey)) == 0);

    assert(DeriveEncKey(dayKey, encKey) == 0);
    assert(memcmp(encKey, vector.encKey, sizeof(encKey)) == 0);

    assert(DeriveRotatingId(dayKey, vector.n, rotatingId) == 0);
    assert(memcmp(rotatingId, vector.rotatingId, sizeof(rotatingId)) == 0);

    assert(SealCommand(dayKey, vectors::M_DEVICE_ID, vector.day, vector.slot, vector.n, vector.plaintext, onAir) == 0);
    assert(memcmp(onAir, vector.onAir, sizeof(onAir)) == 0);

    assert(OpenCommand(dayKey, vectors::M_DEVICE_ID, vector.day, vector.slot, vector.n, vector.onAir, plaintext) == 0);
    assert(memcmp(plaintext, vector.plaintext, sizeof(plaintext)) == 0);

    // A single flipped bit anywhere authenticated must fail - ID, ciphertext or tag.
    const uint8_t positions[] { protocol::M_OFFSET_ROTATING_ID, protocol::M_OFFSET_CIPHERTEXT, protocol::M_OFFSET_TAG };
    for (uint8_t position : positions) {
      memcpy(onAir, vector.onAir, sizeof(onAir));
      onAir[position] ^= 0x01;
      assert(OpenCommand(dayKey, vectors::M_DEVICE_ID, vector.day, vector.slot, vector.n, onAir, plaintext) == -EBADMSG);
    }

    // The right bytes opened as the WRONG sequence number, slot or day must fail.
    // Each is bound into the nonce, which is what stops a command being replayed
    // under another n.
    assert(OpenCommand(dayKey, vectors::M_DEVICE_ID, vector.day, vector.slot, vector.n + 1, vector.onAir, plaintext) == -EBADMSG);
    assert(OpenCommand(dayKey, vectors::M_DEVICE_ID, vector.day, static_cast<uint8_t>(vector.slot ^ 1), vector.n, vector.onAir, plaintext) ==
           -EBADMSG);
    assert(OpenCommand(dayKey, vectors::M_DEVICE_ID, static_cast<uint16_t>(vector.day + 1), vector.slot, vector.n, vector.onAir, plaintext) ==
           -EBADMSG);
  }

  // Distinct slots on the same day must have unrelated day keys.
  {
    uint8_t slotZero[M_DAY_KEY_BYTES] {};
    uint8_t slotOne[M_DAY_KEY_BYTES] {};
    assert(DeriveDayKey(vectors::M_SECRET, vectors::M_DEVICE_ID, 256, 0, slotZero) == 0);
    assert(DeriveDayKey(vectors::M_SECRET, vectors::M_DEVICE_ID, 256, 1, slotOne) == 0);
    assert(memcmp(slotZero, slotOne, sizeof(slotZero)) != 0);
  }

  // Time sync.
  {
    uint8_t onAir[protocol::M_UUID_BYTES] {};
    uint32_t unixSeconds { 0 };

    assert(BuildTimeSync(vectors::M_PROVISION_KEY, vectors::M_DEVICE_ID, vectors::M_TIME_SYNC_UNIX, onAir) == 0);
    assert(memcmp(onAir, vectors::M_TIME_SYNC_ON_AIR, sizeof(onAir)) == 0);

    assert(OpenTimeSync(vectors::M_PROVISION_KEY, vectors::M_DEVICE_ID, vectors::M_TIME_SYNC_ON_AIR, unixSeconds));
    assert(unixSeconds == vectors::M_TIME_SYNC_UNIX);

    // Changing the time without the key must fail - this is the whole point.
    memcpy(onAir, vectors::M_TIME_SYNC_ON_AIR, sizeof(onAir));
    onAir[0] ^= 0x01;
    unixSeconds = 0;
    assert(!OpenTimeSync(vectors::M_PROVISION_KEY, vectors::M_DEVICE_ID, onAir, unixSeconds));
    assert(unixSeconds == 0);

    // The DEVICE SECRET must not authorise a time sync. The two keys are
    // deliberately separate: a provisioner can set the clock and nothing else.
    assert(!OpenTimeSync(vectors::M_SECRET, vectors::M_DEVICE_ID, vectors::M_TIME_SYNC_ON_AIR, unixSeconds));

    // Nor may a sync for one device work on another.
    assert(!OpenTimeSync(vectors::M_PROVISION_KEY, vectors::M_DEVICE_ID + 1, vectors::M_TIME_SYNC_ON_AIR, unixSeconds));
  }

  printf("access keys: OK\n");
}
