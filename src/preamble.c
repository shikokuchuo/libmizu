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

#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

static int mizu_pow2_u32(uint32_t v) {
  return v != 0 && (v & (v - 1)) == 0;
}

void mizu_preamble_write(void *region, const mizu_preamble *p) {
  memcpy(region, p, sizeof(*p));
}

const char *mizu_preamble_validate(const void *region, size_t region_size,
                                  mizu_preamble *out) {

  if (region_size < MIZU_FIXED_LAYOUT_SIZE)
    return "region is smaller than the fixed channel layout";

  mizu_preamble p;
  memcpy(&p, region, sizeof(p));

  if (p.magic != MIZU_MAGIC)
    return "bad magic: not a mizu channel region";
  if (p.version != MIZU_ABI_VERSION)
    return "ABI version mismatch: peer and host were built against "
           "different mizu wire formats";
  if (!mizu_pow2_u32(p.cap) || p.cap < 2)
    return "ring capacity is not a power of two";
  /* the create side's range, mirrored: slot < 64 would wrap inline_max
     (slot - sizeof(mizu_slot_hdr)) into a multi-gigabyte payload bound */
  if (!mizu_pow2_u32(p.slot) || p.slot < 64 || p.slot > (1u << 20))
    return "slot size is not a power of two between 64 and 2^20";
  if (p.arena_size % 128 != 0)
    return "arena size is not a multiple of 128";

  /* Extent checks use division/subtraction forms so no sum can wrap. */
  uint64_t ring_bytes = (uint64_t) p.cap * p.slot;
  uint64_t after_fixed = (uint64_t) region_size - MIZU_FIXED_LAYOUT_SIZE;
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
int mizu_token_valid(const char *token) {
  if (token == NULL || *token == '\0') return 0;
  for (const char *q = token; *q != '\0'; q++)
    if (!((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f') ||
          *q == '_'))
      return 0;
  return 1;
}

/* A wire-carried region name must be exactly the prefix plus
   "<hex>_<hex>": it is length-bounded only before concatenation into
   "/dev/shm%s" or handoff to the OS open, so a "/../" name would open
   an arbitrary same-user file. One hex run, one underscore, one hex
   run, NUL — nothing else. */
int mizu_name_valid(const char *name) {
  if (name == NULL) return 0;
  const char *p = MIZU_PREFIX_LITERAL;
  for (const char *q = p; *q != '\0'; q++)
    if (name[q - p] != *q) return 0;
  const char *q = name + strlen(p);
  if (*q == '\0') return 0;
  for (; *q != '\0'; q++)
    if (!((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f'))) break;
  if (*q != '_' || q == name + strlen(p)) return 0;
  q++;
  if (*q == '\0') return 0;
  for (; *q != '\0'; q++)
    if (!((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f'))) return 0;
  return 1;
}

/* token -> region name: "<prefix><token>", bounded. The channel/pool
   attach paths' shared prologue; TLS-recorded on either failure. */
int mizu_name_from_token(const char *token, char *name, size_t cap) {
  if (!mizu_token_valid(token)) {
    mizu_err_record_tls(MIZU_ERRCAT_OTHER, "malformed region-name suffix");
    return -1;
  }
  int nn = snprintf(name, cap, "%s%s", MIZU_PREFIX_LITERAL, token);
  if (nn <= 0 || (size_t) nn >= cap) {
    mizu_err_record_tls(MIZU_ERRCAT_OTHER, "malformed region-name suffix");
    return -1;
  }
  return 0;
}

/* The inverse: the join token is the region name past the prefix.
   TLS-recorded on truncation. */
mizu_status mizu_token_from_name(const char *name, char *buf, size_t cap) {
  const char *suffix = name + strlen(MIZU_PREFIX_LITERAL);
  int n = snprintf(buf, cap, "%s", suffix);
  if (n <= 0 || (size_t) n >= cap) {
    mizu_err_record_tls(MIZU_ERRCAT_OTHER, "token buffer too small");
    return MIZU_ERR;
  }
  return MIZU_OK;
}
