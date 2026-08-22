/* The magic + version preamble and its validator. Host-written before
   spawn, immutable thereafter; the peer validates before touching any
   shared atomic, so a mismatch or corrupt region fails before the ring
   protocol engages. */

/* _GNU_SOURCE: glibc hides O_CLOEXEC, MAP_POPULATE, pidfd_open, ...
   under strict -std=c11; define before any system header. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#include "internal.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

static int rei_pow2_u32(uint32_t v) {
  return v != 0 && (v & (v - 1)) == 0;
}

void rei_preamble_write(void *region, const rei_preamble *p) {
  memcpy(region, p, sizeof(*p));
}

const char *rei_preamble_validate(const void *region, size_t region_size,
                                  rei_preamble *out) {

  if (region_size < REI_FIXED_LAYOUT_SIZE)
    return "region is smaller than the fixed channel layout";

  rei_preamble p;
  memcpy(&p, region, sizeof(p));

  if (p.magic != REI_MAGIC)
    return "bad magic: not a rei channel region";
  if (p.version != REI_ABI_VERSION)
    return "ABI version mismatch: peer and host were built against "
           "different rei wire formats";
  if (!rei_pow2_u32(p.cap))
    return "ring capacity is not a power of two";
  if (!rei_pow2_u32(p.slot))
    return "slot size is not a power of two";
  if (p.arena_size % 64 != 0)
    return "arena size is not a multiple of 64";

  /* Extent checks use division/subtraction forms so no sum can wrap. */
  uint64_t ring_bytes = (uint64_t) p.cap * p.slot;
  uint64_t after_fixed = (uint64_t) region_size - REI_FIXED_LAYOUT_SIZE;
  if (ring_bytes > after_fixed / 2)
    return "rings exceed the mapped region";
  if (p.arena_size > (after_fixed - 2 * ring_bytes) / 2)
    return "arenas exceed the mapped region";
  if (p.drop_offset > region_size || p.drop_size > region_size - p.drop_offset)
    return "drop slot lies outside the mapped region";
  if (p.livedir_offset > region_size ||
      p.livedir_size > region_size - p.livedir_offset)
    return "liveness-dir string lies outside the mapped region";

  if (out != NULL) *out = p;
  return NULL;
}

/* The join token is the region name past the namespace prefix:
   "<pid hex>_<counter hex>". Validate the charset before it is
   snprintf'd into a shm name — NULL and empty are malformed. */
int rei_token_valid(const char *token) {
  if (token == NULL || *token == '\0') return 0;
  for (const char *q = token; *q != '\0'; q++)
    if (!((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f') ||
          *q == '_'))
      return 0;
  return 1;
}
