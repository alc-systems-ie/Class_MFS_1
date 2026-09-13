#include <cassert>
#include <cstdio>

#include "mfs_protocol.hpp"

void run_protocol_tests()
{
  using namespace alc::protocol;

  // Table anchors, straight from the design spec section 5.
  assert(M_THRESHOLD_TABLE[0] == 4000);  // 1000 mg, least sensitive
  assert(M_THRESHOLD_TABLE[143] == 302); // ~75 mg, the present default
  assert(M_THRESHOLD_TABLE[255] == 40);  // 10 mg, most sensitive

  assert(M_COOLDOWN_TABLE[0] == 0); // reserved: no cooldown
  assert(M_COOLDOWN_TABLE[1] == 1);
  assert(M_COOLDOWN_TABLE[128] == 60); // the clean midpoint
  assert(M_COOLDOWN_TABLE[255] == 3600);

  // Sensitivity must be MONOTONICALLY DECREASING in threshold: a higher byte
  // means more sensitive, which means a lower number. Getting this backwards
  // would make the app's slider work in reverse, which is easy to miss on the
  // bench because the device still triggers - just at the wrong setting.
  for (int i = 1; i < 256; i++) {
    assert(M_THRESHOLD_TABLE[i] <= M_THRESHOLD_TABLE[i - 1]);
  }

  // Cooldown must be monotonically increasing from index 1 upward.
  for (int i = 2; i < 256; i++) {
    assert(M_COOLDOWN_TABLE[i] >= M_COOLDOWN_TABLE[i - 1]);
  }

  // Delay: the two seams are where a piecewise encoding goes wrong, so they are
  // asserted explicitly rather than left to the monotonicity check.
  assert(M_DELAY_TABLE[0] == 0);       // default - no delay
  assert(M_DELAY_TABLE[59] == 59);     // 59 s
  assert(M_DELAY_TABLE[60] == 60);     // 1 min - contiguous with 59 s
  assert(M_DELAY_TABLE[118] == 3540);  // 59 min
  assert(M_DELAY_TABLE[119] == 3600);  // 1 h - contiguous with 59 min
  assert(M_DELAY_TABLE[127] == 32400); // 9 h, the maximum

  // Strictly increasing: no delay code may mean the same as another.
  for (int i = 1; i < 128; i++) {
    assert(M_DELAY_TABLE[i] > M_DELAY_TABLE[i - 1]);
  }

  // The ADXL367 threshold register is 13-bit. A table entry that overflowed it
  // would be silently truncated by writeThreshold() into a DIFFERENT threshold.
  for (int i = 0; i < 256; i++) {
    assert(M_THRESHOLD_TABLE[i] <= 0x1FFF);
    assert(M_THRESHOLD_TABLE[i] >= 1);
  }

  printf("protocol tables: OK\n");
}
