/* Fuzz tier: the SHM_RAW / REF identifier resolution. A payload frame
   names an out-of-line region; the consumer opens that name through
   rei_read_region (open cache, then a read-only open by name). A peer
   can put any byte string there — it must be bounded (REI_NAME_MAX),
   fail cleanly, and set ctx->gone. One real region exists so the
   cache-hit path runs too. libFuzzer harness; run as short fixed-seed
   ASan+UBSan bursts via `make test-fuzz`. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

static rei_handle h;         /* bare stack handle: the open cache is all
                                this needs (test_spill.c's pattern) */
static rei_shm *known;       /* the harness's own region (its mapping is
                                separate from the cache's borrowed one) */
static char known_name[REI_NAME_MAX];
static uint32_t known_len;
static int ready;

static void teardown(void) {
  rei_oc_teardown(&h.oc);
  if (known != NULL) rei_shm_close(known, 1);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (!ready) {
    memset(&h, 0, sizeof(h));
    if (rei_shm_create(&known, 4096) != REI_OK) return 0;
    snprintf(known_name, sizeof(known_name), "%s", rei_shm_name(known));
    known_len = (uint32_t) strlen(known_name);
    atexit(teardown);
    ready = 1;
  }

  /* selector byte: 0 resolves the real region's name (open + store,
     then cache hits); anything else is the peer-controlled byte case */
  static const uint8_t empty;
  const uint8_t *name = &empty;
  uint32_t len = 0;
  if (size > 1 && data[0] == 0) {
    name = (const uint8_t *) known_name;
    len = known_len;
  } else if (size > 1) {
    name = data + 1;
    len = (uint32_t) (size - 1);
  }

  rei_read_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.size = (uint32_t) sizeof(ctx);
  ctx.handle = &h;
  rei_shm *shm = rei_read_region(&ctx, name, len);
  if (shm == NULL)
    return 0;                 /* vanished/invalid: gone must be set */
  /* a resolve must be the known region, by name */
  return strcmp(rei_shm_name(shm), known_name) == 0 ? 0 : 1;
}
