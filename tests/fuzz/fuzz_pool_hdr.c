/* Fuzz tier: the pool header validator — the attach path's counterpart
   of the channel preamble check, run before any registry word is read.
   Bytes a crashed controller left torn must fail cleanly. libFuzzer
   harness; run as short fixed-seed ASan+UBSan bursts via
   `make test-fuzz`. */

#include <stddef.h>
#include <stdint.h>

#include "internal.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  rei_pool_hdr out;
  /* region_size is the fstat truth: the input models the whole region */
  (void) rei_pool_hdr_validate(data, size, &out);
  return 0;
}
