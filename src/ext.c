/* ext.c — the exported (extern) half of mizu_ext.h's dual-form accessors.

   C TUs (core and binding alike) inline their own copy from the header;
   FFI consumers bind these symbols. MIZU_EXT_NO_INLINES keeps the
   header's static inlines out of this TU: internal and external linkage
   of one name cannot coexist in a TU. The bodies are the same offset
   math over the same macros — the macros, not the bodies, are the one
   definition.

   The amalgamation excludes this file: folding it into the single mizu.c
   TU would redefine the inlines it already carries. FFI consumers of
   the shared library get these symbols; C consumers of the amalgamation
   get the inlines from mizu_ext.h. */

#define MIZU_EXT_NO_INLINES
#include "mizu_ext.h"

uint32_t mizu_parker_snapshot(const mizu_parker *pk) {
  return atomic_load_explicit(pk->epoch, memory_order_acquire);
}

MIZU_ATOMIC(uint32_t) *mizu_zc_rc(void *base) {
  return (MIZU_ATOMIC(uint32_t) *) ((unsigned char *) base +
                                   MIZU_ZC_REFCOUNT_OFF);
}

MIZU_ATOMIC(uint32_t) *mizu_zc_flags_(void *base) {
  return (MIZU_ATOMIC(uint32_t) *) ((unsigned char *) base +
                                   MIZU_ZC_FLAGS_OFF);
}

double mizu_timeout_ms(double seconds) {
  if (!isfinite(seconds)) return -1;
  return seconds <= 0 ? 0 : seconds * 1000;
}

void *mizu_stage_raw(mizu_handle *h, uint64_t n, int wire_type,
                    mizu_slot_hdr *hdr, uint8_t *payload,
                    uint32_t inline_max) {
  if (n <= (uint64_t) inline_max) {
    hdr->kind = MIZU_KIND_RAWVEC;
    hdr->len = (uint32_t) n;
    hdr->aux = (uint64_t) (uint32_t) wire_type;
    return payload;
  }
  return mizu_stage_raw_spill(h, n, wire_type, hdr, payload, inline_max);
}

void mizu_store_na_real(void *dst) {
  const uint64_t bits = MIZU_NA_REAL_BITS;
  memcpy(dst, &bits, 8);
}

uint64_t mizu_aux_rawspill_pool(int type, uint32_t name_len) {
  return (uint64_t) (uint32_t) type | ((uint64_t) name_len << 8);
}

uint64_t mizu_aux_shm_vec(int type, uint64_t total) {
  return (uint64_t) (uint32_t) type | (total << 8);
}

int mizu_aux_type(uint64_t aux) {
  return (int) (aux & 0xff);
}

uint64_t mizu_aux_hi(uint64_t aux) {
  return aux >> 8;
}

void mizu_mizh_write(void *base, int wire_type, int64_t n_elems) {
  const uint32_t magic = MIZU_MAGIC_VEC;
  const int32_t t32 = wire_type;
  const int64_t zero64 = 0;
  memcpy(base, &magic, 4);
  memcpy((unsigned char *) base + 4, &t32, 4);
  memcpy((unsigned char *) base + 8, &n_elems, 8);
  memcpy((unsigned char *) base + 16, &zero64, 8);
  memset((unsigned char *) base + 24, 0, MIZU_HEADER_SIZE - 24);
}

int mizu_mizh_check(const void *base, size_t size,
                   int *wire_type, int64_t *n_elems) {
  if (size < MIZU_HEADER_SIZE) return -1;
  uint32_t magic;
  int32_t t32;
  int64_t len, attrs;
  memcpy(&magic, base, 4);
  if (magic != MIZU_MAGIC_VEC) return -1;
  memcpy(&t32, (const unsigned char *) base + 4, 4);
  memcpy(&len, (const unsigned char *) base + 8, 8);
  memcpy(&attrs, (const unsigned char *) base + 16, 8);
  const size_t elt = mizu_type_elt_size(t32);
  if (elt == 0 || len < 0 || attrs < 0 ||
      len > ((int64_t) size - (int64_t) MIZU_HEADER_SIZE) / (int64_t) elt ||
      attrs > (int64_t) size - (int64_t) MIZU_HEADER_SIZE -
              len * (int64_t) elt)
    return -1;
  *wire_type = t32;
  *n_elems = len;
  return 0;
}
