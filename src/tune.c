/* glibc malloc tuning, applied by the binding at load. Large payloads
   arrive as fresh vectors; under glibc's defaults (mmap at 128 KB,
   aggressive heap trim) each is mapped, faulted in page by page, and
   unmapped at free — paid twice per large round trip. Raising the
   thresholds keeps big vectors on the main arena, pages resident.
   glibc-only: musl's mallopt is a stub; the macOS/Windows allocators
   already recycle large blocks. A process's own GLIBC_TUNABLES win. */

#include "internal.h"

#ifdef __GLIBC__
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#endif

void rei_tune(void) {
#ifdef __GLIBC__
  const char *gt = getenv("GLIBC_TUNABLES");
  if (gt != NULL && strstr(gt, "glibc.malloc") != NULL)
    return;
  mallopt(M_MMAP_THRESHOLD, 32 << 20);
  mallopt(M_TRIM_THRESHOLD, 128 << 20);
#endif
}
