/* Fuzz tier: the MIZH and MIZS root validators (mizu_mizh_check,
   mizu_mizs_check) — the headers a consumer wraps a zero-copy view over
   (SHM_VEC, REF, a nested MIZL leaf's string block). A producer dying
   mid-write leaves them torn; every geometry check must fail cleanly.
   The MIZL directory has its own harness (fuzz_mizl). libFuzzer harness;
   run as short fixed-seed ASan+UBSan bursts via `make test-fuzz`. */

#include <stddef.h>
#include <stdint.h>

#include "mizu_ext.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  int wire_type = 0;
  int64_t n = 0, valid[2], str_size = 0, attrs_size = 0;
  (void) mizu_mizh_check(data, size, &wire_type, &n, valid);
  (void) mizu_mizs_check(data, size, &n, &str_size, &attrs_size);
  return 0;
}
