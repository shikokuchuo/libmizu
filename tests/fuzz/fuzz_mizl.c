/* Fuzz tier: the MIZL directory parser (mizu_mizl_check + mizu_mizl_elem).
   A producer dying mid-layout-write leaves a torn directory; every entry
   check — alignment and extent, the attrs tail, the listed tag, the
   remote leaf's span bounds and claim rows — must fail cleanly.
   libFuzzer harness; run as short fixed-seed ASan+UBSan bursts via
   `make test-fuzz`. */

#include <stddef.h>
#include <stdint.h>

#include "mizu_ext.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  int64_t n = 0, aoff = 0, asz = 0, valid[2];
  if (mizu_mizl_check(data, size, &n, &aoff, &asz, valid) != 0)
    return 0;
  mizu_mizl_entry e;
  for (int64_t i = 0; i < n; i++)
    (void) mizu_mizl_elem(data, size, i, &e);
  return 0;
}
