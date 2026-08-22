/* Fuzz tier: the channel preamble validator — the parse a peer runs on
   a host's control region before touching any shared atomic, so bytes a
   crashed host left torn must fail cleanly, never crash or over-read.
   region_size is the fstat truth, so the input models the whole mapped
   region. libFuzzer harness; run as short fixed-seed ASan+UBSan bursts
   via `make test-fuzz`. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "internal.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  rei_preamble out;
  if (size >= REI_FIXED_LAYOUT_SIZE) {
    /* the input models the whole mapped region; validate reads
       sizeof(rei_preamble) only past its size guard, so passing the
       true size can never over-read the input */
    (void) rei_preamble_validate(data, size, &out);
  } else {
    /* a minimum-size control region: the input is its prefix and the
       rest reads as the zero-fill a fresh region carries — this keeps
       short inputs reaching the field checks */
    uint8_t region[REI_FIXED_LAYOUT_SIZE];
    memset(region, 0, sizeof(region));
    memcpy(region, data, size);
    (void) rei_preamble_validate(region, sizeof(region), &out);
  }
  return 0;
}
