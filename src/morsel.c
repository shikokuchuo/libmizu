/* morsel.c — the map morsel-claim protocol: the language-agnostic half of
   a binding's parallel map (mizu's map.c and pymizu's map.c kept two copies
   in sync by convention). The region format and protocol invariants are
   mizu_ext.h's morsel section; this TU is the header layout/validation, the
   morsel-state word arithmetic, the generation-fenced claim + AIMD batch
   sizing + cursor issue of a batch transition, the reset/trim/cancel words,
   and the lost-set scan. It sees no language object: the descriptor codec,
   the x-section element I/O, the batch loop, and the gather stay with the
   binding. */

#include <stdlib.h>
#include <string.h>

#include "mizu_ext.h"

// State words -------------------------------------------------------------------

static MIZU_ATOMIC(uint32_t) *morsel_cancel_word(void *base,
                                                const mizu_morsel_hdr *h) {
  return (MIZU_ATOMIC(uint32_t) *)
    ((unsigned char *) base + h->state_off + MIZU_MORSEL_CANCEL_OFF);
}

static MIZU_ATOMIC(uint32_t) *morsel_gen_word(void *base,
                                             const mizu_morsel_hdr *h) {
  return (MIZU_ATOMIC(uint32_t) *)
    ((unsigned char *) base + h->state_off + MIZU_MORSEL_GEN_OFF);
}

static MIZU_ATOMIC(uint64_t) *morsel_cursor_word(void *base,
                                                const mizu_morsel_hdr *h) {
  return (MIZU_ATOMIC(uint64_t) *)
    ((unsigned char *) base + h->state_off + MIZU_MORSEL_CURSOR_OFF);
}

static MIZU_ATOMIC(uint32_t) *morsel_claim_word(void *base,
                                               const mizu_morsel_hdr *h,
                                               uint32_t r) {
  return (MIZU_ATOMIC(uint32_t) *)
    ((unsigned char *) base + h->state_off + MIZU_MORSEL_CLAIM_OFF +
     (uint64_t) r * 4);
}

// Header layout and validation -----------------------------------------------------

uint64_t mizu_morsel_layout(mizu_morsel_hdr *h, uint32_t magic, uint64_t n,
                           uint64_t morsel_size, uint64_t desc_len,
                           uint32_t x_type, uint64_t x_len,
                           uint32_t out_type, uint64_t out_m,
                           uint32_t claim_n) {
  if (n < 1 || n > ((uint64_t) 1 << 48)) return 0;
  if (morsel_size < 1 || morsel_size > n) return 0;
  if (desc_len < 1) return 0;
  if (claim_n < 1 || claim_n > (1u << 16)) return 0;
  memset(h, 0, sizeof *h);
  h->magic = magic;
  h->version = MIZU_ABI_VERSION;
  h->n = n;
  h->desc_off = sizeof(mizu_morsel_hdr);
  h->desc_len = desc_len;
  h->morsel_size = morsel_size;
  h->n_morsels = (n + morsel_size - 1) / morsel_size;
  h->claim_n = claim_n;
  uint64_t off = MIZU_ALIGN64(sizeof(mizu_morsel_hdr) + desc_len);
  if (x_type != 0) {
    size_t elt = mizu_type_elt_size((int) x_type);
    if (elt == 0 || x_len != n * elt) return 0;
    h->x_kind = MIZU_MORSEL_X_RAW;
    h->x_type = x_type;
    h->x_off = off;
    h->x_len = x_len;
    off = MIZU_ALIGN64(off + x_len);
  }
  /* the morsel state sits between the descriptor / x sections and the
     output area; a fresh region is zero-filled, so cancel, generation,
     cursor and every CLAIM word ((0 << 2) | IDLE) start armed for
     generation 0 */
  h->state_off = off;
  off = MIZU_ALIGN64(off + MIZU_MORSEL_CLAIM_OFF + (uint64_t) claim_n * 4);
  if (out_type != 0) {
    size_t elt = mizu_type_elt_size((int) out_type);
    if (elt == 0 || out_m < 1 || out_m > ((uint64_t) 1 << 32)) return 0;
    if (n > (((uint64_t) 1 << 46) - off) / (out_m * elt)) return 0;
    h->out_type = out_type;
    h->out_elt = (uint32_t) elt;
    h->out_m = out_m;
    h->out_off = off;
    off += n * out_m * elt;
  }
  if (off > ((uint64_t) 1 << 46)) return 0;
  return off;
}

const char *mizu_morsel_hdr_check(const void *base, size_t size,
                                 uint32_t magic, mizu_morsel_hdr *out) {
  if (size < sizeof(mizu_morsel_hdr))
    return "region is smaller than a map header";
  mizu_morsel_hdr h;
  memcpy(&h, base, sizeof h);
  if (h.magic != magic)
    return "bad magic: not this binding's map region";
  if (h.version != MIZU_ABI_VERSION)
    return "ABI version mismatch: worker and submitter were built against "
           "different mizu wire formats";
  if (h.n == 0 || h.n > ((uint64_t) 1 << 48))
    return "element count out of range";
  uint64_t end = MIZU_ALIGN64(sizeof(mizu_morsel_hdr) + h.desc_len);
  if (h.desc_off != sizeof(mizu_morsel_hdr) || h.desc_len == 0 ||
      h.desc_len > size - h.desc_off)
    return "descriptor lies outside the region";
  if (h.morsel_size == 0 ||
      h.n_morsels != (h.n + h.morsel_size - 1) / h.morsel_size)
    return "morsel geometry is inconsistent";
  if (h.x_kind == MIZU_MORSEL_X_RAW) {
    size_t elt = mizu_type_elt_size((int) h.x_type);
    /* x_off > size before the subtraction: an ALIGN64'd end can round past
       the mapping size */
    if (elt == 0 || h.x_off != end || h.x_off > size ||
        h.x_len != h.n * elt || h.x_len > size - h.x_off)
      return "x section lies outside the region";
    end = MIZU_ALIGN64(h.x_off + h.x_len);
  } else if (h.x_kind != MIZU_MORSEL_X_DESC) {
    return "unknown x section kind";
  }
  if (h.claim_n == 0 || h.claim_n > (1u << 16) ||
      h.state_off != end || (h.state_off & 63) != 0 ||
      h.state_off > size ||
      MIZU_MORSEL_CLAIM_OFF + (uint64_t) h.claim_n * 4 > size - h.state_off)
    return "morsel state section lies outside the region";
  end = MIZU_ALIGN64(h.state_off + MIZU_MORSEL_CLAIM_OFF +
                    (uint64_t) h.claim_n * 4);
  if (h.out_type != 0) {
    size_t elt = mizu_type_elt_size((int) h.out_type);
    if (elt == 0 || h.out_elt != elt || h.out_m == 0 ||
        h.out_m > ((uint64_t) 1 << 32) || h.out_off != end ||
        h.out_off > size ||
        h.n > (size - h.out_off) / (h.out_m * elt))
      return "output area lies outside the region";
  } else if (h.out_elt != 0 || h.out_m != 0 || h.out_off != 0) {
    return "output area lies outside the region";
  }
  if (out != NULL) *out = h;
  return NULL;
}

// The batch transition --------------------------------------------------------------

void mizu_morsel_sizer_init(mizu_morsel_sizer *sz) {
  sz->run_r = -1;
  sz->run_gen = 0;
  sz->k = 1;
  sz->k_last = 0;
  sz->t_last = 0;
  sz->cost = 0;
  sz->skip = 0;
}

int mizu_morsel_next(void *base, const mizu_morsel_hdr *h,
                    mizu_morsel_sizer *sz, uint32_t r, uint32_t gen,
                    const mizu_pool_sig *sig, uint64_t pin_k, double now,
                    uint64_t *m_out, uint64_t *k_out, int *help_out) {
  gen &= MIZU_MORSEL_GEN_MASK;

  /* first transition: CAS (gen << 2)|IDLE -> RUNNING — the one atomic that
     both claims the lane and fences the generation. It fails alike against
     ABANDONED (lost to the trim) and against a word re-armed with a newer
     generation; RUNNING at our generation means this very task already
     claimed it (each ordinal rides exactly one payload per generation), so
     later transitions — and a run resumed through an aliased ctx — fall
     straight through. */
  MIZU_ATOMIC(uint32_t) *cw = morsel_claim_word(base, h, r);
  uint32_t running = (gen << 2) | MIZU_MORSEL_RUNNING;
  uint32_t w = atomic_load_explicit(cw, memory_order_acquire);
  if (w == ((gen << 2) | MIZU_MORSEL_IDLE) &&
      atomic_compare_exchange_strong_explicit(cw, &w, running,
                                              memory_order_seq_cst,
                                              memory_order_acquire))
    w = running;
  if (w != running) return 0;

  if (sz->run_r != (int32_t) r || sz->run_gen != gen) {
    /* run boundary through this ctx: relearn over a fresh ramp */
    sz->run_r = (int32_t) r;
    sz->run_gen = gen;
    sz->k = 1;
    sz->k_last = 0;
    sz->cost = 0;
    sz->skip = 0;
  }

  if (atomic_load_explicit(morsel_cancel_word(base, h),
                           memory_order_acquire) != 0)
    return 0;

  int help = 0;
  if (sig != NULL) {
    /* a runner is the one place a worker sits for a whole map without
       touching its step loop, where these words are consumed: stop unwinds
       it there within ~a batch instead of at cursor exhaustion */
    if (atomic_load_explicit(sig->shutdown, memory_order_relaxed) != 0 ||
        atomic_load_explicit(sig->owner_dead, memory_order_relaxed) != 0)
      return 0;
    help = atomic_load_explicit(sig->help_wanted, memory_order_relaxed) != 0;
  }

  uint64_t k;
  if (pin_k != 0) {
    k = pin_k;
  } else {
    if (sz->k_last > 0) {
      if (sz->skip) {
        sz->skip = 0;   /* interval contained a helped foreign task */
      } else {
        double per = (now - sz->t_last) / (double) sz->k_last;
        sz->cost = per > 1e-9 ? per : 1e-9;   /* clock-floor trivial f */
      }
      if (sz->cost > 0) {
        double want = MIZU_MORSEL_T_TARGET / sz->cost;
        uint64_t wk = want >= 1 ? (uint64_t) want : 1;
        /* grow at most 2x per step toward the target; shrink immediately on
           overshoot; clamp to the batch cap */
        sz->k = wk >= sz->k * 2 ? sz->k * 2 : wk;
        if (sz->k > MIZU_MORSEL_BATCH_CAP) sz->k = MIZU_MORSEL_BATCH_CAP;
      }
    }
    k = sz->k;
  }

  /* relaxed issue: atomicity (unique claim) is all the shared state
     provides; ordering rides the task claim/publish chain. */
  uint64_t m = atomic_fetch_add_explicit(morsel_cursor_word(base, h), k,
                                         memory_order_relaxed);
  if (m >= h->n_morsels) return 0;
  if (k > h->n_morsels - m) k = h->n_morsels - m;   /* final grant */
  sz->k_last = k;
  sz->t_last = now;
  if (help) sz->skip = 1;

  *m_out = m;
  *k_out = k;
  *help_out = help;
  return 1;
}

// Reset / trim / cancel --------------------------------------------------------------

uint32_t mizu_morsel_reset(void *base, const mizu_morsel_hdr *h) {
  uint32_t gen = (atomic_fetch_add_explicit(morsel_gen_word(base, h), 1u,
                                            memory_order_seq_cst) + 1) &
    MIZU_MORSEL_GEN_MASK;
  for (uint32_t r = 0; r < h->claim_n; r++)
    atomic_store_explicit(morsel_claim_word(base, h, r),
                          (gen << 2) | MIZU_MORSEL_IDLE,
                          memory_order_seq_cst);
  atomic_store_explicit(morsel_cursor_word(base, h), 0,
                        memory_order_seq_cst);
  atomic_store_explicit(morsel_cancel_word(base, h), 0u,
                        memory_order_seq_cst);
  return gen;
}

int mizu_morsel_abandon(void *base, const mizu_morsel_hdr *h, uint32_t r,
                       uint32_t gen) {
  gen &= MIZU_MORSEL_GEN_MASK;
  MIZU_ATOMIC(uint32_t) *cw = morsel_claim_word(base, h, r);
  /* a no-op ("idle" refusal) unless the cursor is exhausted or the cancel
     word is set; a won IDLE -> ABANDONED CAS at the current generation
     proves that runner never started and never will do work */
  int armed =
    atomic_load_explicit(morsel_cursor_word(base, h), memory_order_acquire) >=
      h->n_morsels ||
    atomic_load_explicit(morsel_cancel_word(base, h),
                         memory_order_acquire) != 0;
  uint32_t w = atomic_load_explicit(cw, memory_order_acquire);
  if (armed)
    while (w == ((gen << 2) | MIZU_MORSEL_IDLE))
      if (atomic_compare_exchange_strong_explicit(
            cw, &w, (gen << 2) | MIZU_MORSEL_ABANDONED,
            memory_order_seq_cst, memory_order_acquire))
        return MIZU_MORSEL_ABANDONED;
  return (int) (w & 3u);
}

void mizu_morsel_cancel_set(void *base, const mizu_morsel_hdr *h) {
  atomic_store_explicit(morsel_cancel_word(base, h), 1u,
                        memory_order_seq_cst);
}

int mizu_morsel_cancel_get(const void *base, const mizu_morsel_hdr *h) {
  return atomic_load_explicit(morsel_cancel_word((void *) base, h),
                              memory_order_acquire) != 0;
}

uint32_t mizu_morsel_generation(const void *base, const mizu_morsel_hdr *h) {
  return atomic_load_explicit(morsel_gen_word((void *) base, h),
                              memory_order_acquire) & MIZU_MORSEL_GEN_MASK;
}

uint64_t mizu_morsel_cursor(const void *base, const mizu_morsel_hdr *h) {
  uint64_t cur = atomic_load_explicit(morsel_cursor_word((void *) base, h),
                                      memory_order_acquire);
  return cur > h->n_morsels ? h->n_morsels : cur;
}

uint32_t mizu_morsel_claim(const void *base, const mizu_morsel_hdr *h,
                          uint32_t r) {
  return atomic_load_explicit(morsel_claim_word((void *) base, h, r),
                              memory_order_acquire);
}

// Ranges ----------------------------------------------------------------------------

void mizu_morsel_span_of(const mizu_morsel_hdr *h, uint64_t m, uint64_t k,
                        uint64_t *lo, uint64_t *hi) {
  *lo = m * h->morsel_size;
  *hi = (m + k) * h->morsel_size;
  if (*hi > h->n) *hi = h->n;
}

static int morsel_span_cmp(const void *a, const void *b) {
  uint64_t x = ((const mizu_morsel_span *) a)->lo;
  uint64_t y = ((const mizu_morsel_span *) b)->lo;
  return (x > y) - (x < y);
}

size_t mizu_morsel_lost(mizu_morsel_span *spans, size_t n, uint64_t bound,
                       mizu_morsel_span *out) {
  if (n > 0) qsort(spans, n, sizeof *spans, morsel_span_cmp);
  size_t g = 0;
  uint64_t pos = 0;
  for (size_t i = 0; i < n; i++) {
    /* one gap per step: before span i (clamped to the issued bound), then
       the tail before it */
    uint64_t next = spans[i].lo > bound ? bound : spans[i].lo;
    if (next > pos) {
      out[g].lo = pos;
      out[g].hi = next;
      g++;
    }
    if (spans[i].hi > pos) pos = spans[i].hi;
  }
  if (pos < bound) {
    out[g].lo = pos;
    out[g].hi = bound;
    g++;
  }
  return g;
}
