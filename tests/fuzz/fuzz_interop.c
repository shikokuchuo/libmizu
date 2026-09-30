/* Fuzz tier: the interop cursor — the parser of every byte a foreign
   peer can send on the 'I' stream. A torn or hostile stream must be a
   clean cursor error, never a memory error. The input models the whole
   stream: when it starts with the magic it is parsed as-is (the version
   check runs); otherwise the 'I' + version header is prepended so short
   inputs still reach the value grammar. libFuzzer harness; run as short
   fixed-seed ASan+UBSan bursts via `make test-fuzz`. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mizu_ext.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  mizu_ix cur;
  mizu_ix_item it;
  if (size >= 2 && data[0] == MIZU_INTEROP_MAGIC) {
    if (mizu_ix_open(&cur, data, size) != MIZU_OK) return 0;
  } else {
    /* the runner caps inputs at 4096 bytes (-max_len in the Makefile) */
    if (size > 4096) return 0;
    uint8_t buf[4096 + 2];
    buf[0] = MIZU_INTEROP_MAGIC;
    buf[1] = MIZU_IX_VERSION;
    memcpy(buf + 2, data, size);
    if (mizu_ix_open(&cur, buf, size + 2) != MIZU_OK) return 0;
  }
  /* pull until the stream ends, completes, or faults; the cursor latches
     its error, so the loop is bounded */
  while (!cur.done && mizu_ix_next(&cur, &it) == MIZU_OK) {
  }
  (void) mizu_ix_end(&cur);
  return 0;
}
