/* Fuzz tier: the map region's morsel header validator
   (mizu_morsel_hdr_check) — the 128-byte header every runner reads off a
   region another process wrote, so bytes a crashed submitter left torn
   must fail cleanly before any section offset is used. libFuzzer
   harness; run as short fixed-seed ASan+UBSan bursts via
   `make test-fuzz`. */

#include <stddef.h>
#include <stdint.h>

#include "mizu_ext.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  mizu_morsel_hdr out;
  (void) mizu_morsel_hdr_check(data, size, &out);
  return 0;
}
