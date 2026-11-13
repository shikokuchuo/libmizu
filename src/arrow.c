/* arrow.c — the Arrow C Data Interface -> MIZL frame writer and the
   fixed-width conversion kernels.

   The writer is the mirror of the bindings' view-layer layout writes: an
   attr-qualified Arrow frame past the zero-copy floor stages as one MIZL
   region — the 64-byte header, one 32-byte directory entry per column,
   the column bodies at 64-byte-aligned offsets, the leaf blobs, the root
   frame blob, then the validity tail — framed SHM_VEC with the exact used
   byte count. Promoted from pymizu's shmframe.c (the code moves, the wire
   bytes are pinned by pymizu's cross-language suite); the bindings keep
   only their stream pull and their size-tier pre-gate.

   The policy (the bindings' previously separate, drifted rules in one
   home): the producer's validity bitmap is authoritative — copied into
   the tail verbatim for a single offset-0 batch, rebuilt honouring
   batch->offset otherwise — and the data carries the R sentinels at null
   slots, patched in the copy (Arrow NaN is a value and stays NaN). A
   uint8 column's nulls decline (R raw has no NA). Dictionary columns of
   strings cross as factors (1-based INT32 codes + a levels blob; an
   ordered dictionary crosses as an ordered factor — the class strv's
   two-element form — for a peer advertising MIZU_CAP_ORDERED, and
   declines for one without). date32 / timestamp / duration cross as
   REAL leaves + the Date / POSIXct / difftime blobs. Strings take the
   producer's UTF-8 on its word (Arrow's contract); the catastrophic cases
   (negative or non-monotonic offsets, a view's buffer index past the
   array) decline. The write is always the foreign form (bulk CE_UTF8
   marks + the validity tail), so the bytes are peer-agnostic: an R reader
   takes the sentinels and ignores the tail, a Python reader takes the
   bitmap.

   Staging is transactional: the writer is pure C over caller-owned memory
   until the layout write lands in the region, and mizu_arrow_stage
   touches no shared state before its gate passes. */

#include <stdio.h>
#include <stdlib.h>

#include "internal.h"

/* The MIZL remote-leaf directory tag (mizu.h's wire format). */
#define MIZU_ARROW_TAG_REMOTE 33

/* R's verbatim NA_real_ bits, for the Date / POSIXct / difftime leaves'
   missing sentinels (the corpus's pin; the quiet-bit-set twin
   MIZU_NA_REAL_BITS reads as NA too, but a relay compares payloads
   bitwise). */
static void arrow_store_na_r(uint8_t *dst) {
  const uint64_t bits = 0x7FF00000000007A2ULL;
  memcpy(dst, &bits, 8);
}

// The Arrow element helpers: the validity bit and the per-element string
// access are the ext tier's dual-form mizu_arrow_valid / mizu_arrow_str_at
// (mizu_ext.h) ----------------------------------------------------------------

// The conversion kernels ------------------------------------------------------------

/* IEEE 754 half -> double, exact: half's 5-bit exponent / 10-bit
   mantissa decode into float's wider same-shaped fields (the subnormal
   branch normalizes), the float widens losslessly. */
static double half_to_double(uint16_t h) {
  const uint32_t sign = (uint32_t) (h >> 15) << 31;
  uint32_t exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
  uint32_t f;
  if (exp == 0x1F) {
    f = sign | 0x7F800000u | (mant << 13);       /* inf/nan */
  } else if (exp == 0) {
    if (mant == 0) {
      f = sign;                                  /* ±0 */
    } else {
      int e = 0;
      do {
        mant <<= 1;
        e--;
      } while (!(mant & 0x400));
      mant &= 0x3FF;
      f = sign | (uint32_t) (113 + e) << 23 | (mant << 13);
    }
  } else {
    f = sign | ((exp + 112) << 23) | (mant << 13);
  }
  float fv;
  memcpy(&fv, &f, 4);
  return (double) fv;
}

/* The verbatim rows share the identity i64 row's contents: the pointer
   compare keys them out of the n_int64 count (a cap-peer reads their
   lanes exactly, so an INT64_MIN pattern there is a genuine value).
   Declared ahead of cvt_run, which fuses the count. */
static const mizu_cvt_row CVT_ROW_I64_V;

/* Run bytes past which the int64 sentinel count fuses into the copy loop
   instead of riding a second pass: below it the count's re-read of src
   hits the LLC (the memcpy's non-temporal stores keep dst out of the
   way); past it src is DRAM-resident and the fused single pass wins.
   Measured on 4-16 MB LLC hosts at the 8/64 MB shapes. */
#define MIZU_CVT_FUSE_MIN ((size_t) 16 << 20)

/* Convert a run of n valid elements. Per-element memcpy keeps every
   access alignment-safe (a contiguous buffer can still be
   element-misaligned — np.frombuffer with a byte offset); fixed-size
   memcpys compile to single load/store pairs. */
/* Internal conversion rows: dictionary codes widen and add one; temporal
   counts widen to doubles at their declared scale. Their null lanes use
   R's verbatim NA payload rather than the wire's quiet-bit-set twin. */
enum {
  ARROW_CVT_DICT_I8 = 256,
  ARROW_CVT_DICT_U8,
  ARROW_CVT_DICT_I16,
  ARROW_CVT_DICT_U16,
  ARROW_CVT_DICT_I32,
  ARROW_CVT_DICT_U32,
  ARROW_CVT_DICT_I64,
  ARROW_CVT_DICT_U64,
  ARROW_CVT_DATE,
  ARROW_CVT_TS_S,
  ARROW_CVT_TS_MS,
  ARROW_CVT_TS_US,
  ARROW_CVT_TS_NS
};

/* The widening rows are one shape: fixed-size memcpys (single load/store
   pairs) around a per-lane conversion. U64_INT64 (the range check + warn),
   BOOL8_LGL, C64_CPLX and COPY stay explicit below. */
#define ARROW_CVT_WIDEN(NAME, ST, SW, DT, DW, CONV)                          \
  case NAME:                                                                 \
    for (size_t i = 0; i < n; i++) {                                         \
      ST v;                                                                  \
      memcpy(&v, src + (SW) * i, (SW));                                      \
      DT o = (CONV);                                                         \
      memcpy(dst + (DW) * i, &o, (DW));                                      \
    }                                                                        \
    break;

static void cvt_run(uint8_t *dst, const uint8_t *src, size_t n,
                    const mizu_cvt_row *row, mizu_arrow_warn *warn) {
  switch (row->cvt) {
  case MIZU_CVT_COPY:
    /* Fuse the genuine-sentinel count into the copy: the int32/int64
       identity rows read a sentinel as NA in R, so their count rides the
       run rather than re-reading src in a second pass. The verbatim i64
       row (a cap-peer's agreed values) is keyed out by pointer. The count
       rides a local — a warn-> store per lane would alias-block
       vectorization. */
    if (warn != NULL && row != &CVT_ROW_I64_V) {
      uint64_t cnt = 0;
      if (row->wire == MIZU_TYPE_INT) {
        /* The int32 count is lane-bound (2x lanes per byte moved), so the
           fused single pass wins at every size measured; element-misaligned
           runs keep the alignment-safe pair. */
        if (((uintptr_t) dst | (uintptr_t) src) % 4 == 0) {
          int32_t *restrict d = (int32_t *) (void *) dst;
          const int32_t *restrict s = (const int32_t *) (const void *) src;
          for (size_t i = 0; i < n; i++) {
            const int32_t v = s[i];
            d[i] = v;
            cnt += v == INT32_MIN;
          }
        } else {
          memcpy(dst, src, n * 4);
          for (size_t i = 0; i < n; i++) {
            int32_t v;
            memcpy(&v, src + 4 * i, 4);
            cnt += v == INT32_MIN;
          }
        }
        warn->n_intmin += cnt;
      } else if (row->wire == MIZU_TYPE_INT64) {
        /* Below the fuse gate the memcpy + count pair wins: the count's
           re-read of src hits the LLC and the monolithic memcpy earns
           non-temporal stores. Past it (src DRAM-resident) the pair pays
           a second full src stream and the fused single pass wins. */
        if (n * 8 >= MIZU_CVT_FUSE_MIN &&
            ((uintptr_t) dst | (uintptr_t) src) % 8 == 0) {
          int64_t *restrict d = (int64_t *) (void *) dst;
          const int64_t *restrict s = (const int64_t *) (const void *) src;
          for (size_t i = 0; i < n; i++) {
            const int64_t v = s[i];
            d[i] = v;
            cnt += v == INT64_MIN;
          }
        } else {
          memcpy(dst, src, n * 8);
          for (size_t i = 0; i < n; i++) {
            int64_t v;
            memcpy(&v, src + 8 * i, 8);
            cnt += v == INT64_MIN;
          }
        }
        warn->n_int64 += cnt;
      } else {
        memcpy(dst, src, n * row->w_out);
      }
    } else {
      memcpy(dst, src, n * row->w_out);
    }
    break;
  ARROW_CVT_WIDEN(MIZU_CVT_I8_INT, int8_t, 1, int32_t, 4, (int32_t) v)
  ARROW_CVT_WIDEN(MIZU_CVT_I16_INT, int16_t, 2, int32_t, 4, v)
  ARROW_CVT_WIDEN(MIZU_CVT_U16_INT, uint16_t, 2, int32_t, 4, v)
  ARROW_CVT_WIDEN(MIZU_CVT_U32_REAL, uint32_t, 4, double, 8, (double) v)
  case MIZU_CVT_U64_INT64:
    for (size_t i = 0; i < n; i++) {
      uint64_t v;
      memcpy(&v, src + 8 * i, 8);
      if (v > (uint64_t) INT64_MAX) {
        const int64_t na = MIZU_NA_INT64;
        memcpy(dst + 8 * i, &na, 8);
        warn->n_range++;
      } else {
        const int64_t o = (int64_t) v;
        memcpy(dst + 8 * i, &o, 8);
      }
    }
    break;
  ARROW_CVT_WIDEN(MIZU_CVT_F16_REAL, uint16_t, 2, double, 8, half_to_double(v))
  ARROW_CVT_WIDEN(MIZU_CVT_F32_REAL, float, 4, double, 8, v)
#define ARROW_CVT_DICT(NAME, ST, SW)                                          \
  case NAME:                                                                  \
    for (size_t i = 0; i < n; i++) {                                          \
      ST v;                                                                   \
      memcpy(&v, src + (SW) * i, (SW));                                       \
      uint32_t o = (uint32_t) v + 1u;  /* null lanes may hold any bits */     \
      memcpy(dst + 4 * i, &o, 4);                                             \
    }                                                                         \
    break;
  ARROW_CVT_DICT(ARROW_CVT_DICT_I8, int8_t, 1)
  ARROW_CVT_DICT(ARROW_CVT_DICT_U8, uint8_t, 1)
  ARROW_CVT_DICT(ARROW_CVT_DICT_I16, int16_t, 2)
  ARROW_CVT_DICT(ARROW_CVT_DICT_U16, uint16_t, 2)
  ARROW_CVT_DICT(ARROW_CVT_DICT_I32, int32_t, 4)
  ARROW_CVT_DICT(ARROW_CVT_DICT_U32, uint32_t, 4)
  ARROW_CVT_DICT(ARROW_CVT_DICT_I64, int64_t, 8)
  ARROW_CVT_DICT(ARROW_CVT_DICT_U64, uint64_t, 8)
#undef ARROW_CVT_DICT
  ARROW_CVT_WIDEN(ARROW_CVT_DATE, int32_t, 4, double, 8, (double) v)
  ARROW_CVT_WIDEN(ARROW_CVT_TS_S, int64_t, 8, double, 8, (double) v * 1.0)
  ARROW_CVT_WIDEN(ARROW_CVT_TS_MS, int64_t, 8, double, 8, (double) v * 1e-3)
  ARROW_CVT_WIDEN(ARROW_CVT_TS_US, int64_t, 8, double, 8, (double) v * 1e-6)
  ARROW_CVT_WIDEN(ARROW_CVT_TS_NS, int64_t, 8, double, 8, (double) v * 1e-9)
  case MIZU_CVT_BOOL8_LGL:
    for (size_t i = 0; i < n; i++) {
      int32_t o = src[i] != 0;
      memcpy(dst + 4 * i, &o, 4);
    }
    break;
  case MIZU_CVT_C64_CPLX:
    for (size_t i = 0; i < n; i++) {
      float re, im;
      memcpy(&re, src + 8 * i, 4);
      memcpy(&im, src + 8 * i + 4, 4);
      double o[2] = { re, im };
      memcpy(dst + 16 * i, o, 16);
    }
    break;
  default:
    break;
  }
}
#undef ARROW_CVT_WIDEN

/* Fill nbytes (a multiple of 4) with an 8-byte pattern, a 4-byte sentinel
   given doubled. Per-lane stores stay scalar; whole-block copies of the
   replicated pattern compile to vector stores. */
static void cvt_fill_pattern(uint8_t *dst, size_t nbytes, uint64_t pat) {
  uint64_t blk[8];
  for (int j = 0; j < 8; j++) blk[j] = pat;
  size_t i = 0;
  for (; i + 64 <= nbytes; i += 64) memcpy(dst + i, blk, 64);
  memcpy(dst + i, blk, nbytes - i);
}

/* Fill n lanes with the wire type's missing sentinel, at the type's
   lane width. */
static void cvt_fill_na(uint8_t *dst, size_t n, int wire) {
  uint64_t pat;
  switch (wire) {
  case MIZU_TYPE_REAL:
  case MIZU_TYPE_CPLX:   /* NA_complex_: both parts the real sentinel */
    mizu_store_na_real((uint8_t *) &pat);
    cvt_fill_pattern(dst, n * (wire == MIZU_TYPE_CPLX ? 16 : 8), pat);
    break;
  case MIZU_TYPE_INT64:   /* reads as NA_integer64_ in R */
    cvt_fill_pattern(dst, n * 8, (uint64_t) MIZU_NA_INT64);
    break;
  case MIZU_TYPE_RAW:    /* no sentinel: a zero byte per lane */
    memset(dst, 0, n);
    break;
  default: {
    const uint32_t na = (uint32_t) MIZU_NA_INT32;   /* INT and LGL share it */
    cvt_fill_pattern(dst, n * 4, (uint64_t) na << 32 | na);
    break;
  }
  }
}

static void cvt_fill(uint8_t *dst, size_t n, const mizu_cvt_row *row) {
  if (row->cvt >= ARROW_CVT_DATE && row->cvt <= ARROW_CVT_TS_NS) {
    uint64_t pat;
    arrow_store_na_r((uint8_t *) &pat);
    cvt_fill_pattern(dst, n * 8, pat);
  } else {
    cvt_fill_na(dst, n, row->wire);
  }
}

/* All-ones over `lanes` lanes (the shift alone is UB at 64). */
static uint64_t arrow_lanes_mask(size_t lanes) {
  return lanes == 64 ? UINT64_MAX : ((uint64_t) 1 << lanes) - 1;
}

/* Load lanes (<= 64) validity bits starting at bit position pos (Arrow
   LSB-first bit order), reading only the bytes that cover them. */
static uint64_t bitmap_word(const uint8_t *bm, uint64_t pos, size_t lanes) {
  const uint8_t *p = bm + (pos >> 3);
  unsigned sh = (unsigned) (pos & 7);
  if (lanes == 64) {
    uint64_t w;
    memcpy(&w, p, 8);
    /* a bit offset spills the last lanes into a ninth byte */
    return sh == 0 ? w : (w >> sh) | ((uint64_t) p[8] << (64 - sh));
  }
  size_t nbytes = (sh + lanes + 7) >> 3;
  uint64_t lo = 0, hi = 0;
  for (size_t j = 0; j < nbytes; j++) {
    if (j < 8) lo |= (uint64_t) p[j] << (8 * j);
    else hi = p[j];   /* j == 8: only when sh + lanes > 64 */
  }
  uint64_t w = sh != 0 ? (lo >> sh) | (hi << (64 - sh)) : lo;
  w &= arrow_lanes_mask(lanes);
  return w;
}

/* A batch's exact null count (Arrow's -1 "not computed" resolved), counted
   a word at a time: bitmap_word (it masks and shifts) for the head lanes up
   to a byte boundary and for the tail, plain loads between. */
static int64_t arrow_batch_nulls(const ArrowArray *a) {
  if (a->null_count == 0 || a->buffers[0] == NULL) return 0;
  if (a->null_count > 0) return a->null_count;
  const uint8_t *bm = (const uint8_t *) a->buffers[0];
  int64_t n = 0;
  int64_t k = 0;
  const int64_t head = -a->offset & 7;
  if (head < a->length) {
    if (head != 0) {
      n = head - __builtin_popcountll(
        bitmap_word(bm, (uint64_t) a->offset, (size_t) head));
      k = head;
    }
    const uint8_t *p = bm + ((a->offset + head) >> 3);
    const int64_t words = (a->length - head) >> 6;
    uint64_t ones = 0;
    for (int64_t j = 0; j < words; j++) {
      uint64_t w;
      memcpy(&w, p + 8 * j, 8);
      ones += (uint64_t) __builtin_popcountll(w);
    }
    k += words * 64;
    n += words * 64 - (int64_t) ones;
  }
  for (; k < a->length;) {
    const size_t lanes = a->length - k < 64 ? (size_t) (a->length - k) : 64;
    const uint64_t w = bitmap_word(bm, (uint64_t) (a->offset + k), lanes);
    n += (int64_t) lanes - (int64_t) __builtin_popcountll(w);
    k += (int64_t) lanes;
  }
  return n;
}

/* The masked variant (Arrow nulls): word-wise over the validity bitmap,
   run-coalesced — consecutive all-ones words accumulate into one convert,
   consecutive zero words into one sentinel fill. A mixed word converts all
   64 lanes in one pass, then patches its null lanes. A null lane's value is
   undefined, so the warning counts cover valid lanes only: whatever a null
   lane added in cvt_run (a genuine int32/int64 sentinel, a uint64 past the
   int64 range) is subtracted again, or a dense word counts its valid lanes
   itself. off is the bit offset into validity. */
static void convert_masked(uint8_t *dst, const uint8_t *src,
                           const uint8_t *valid, uint64_t off, size_t n,
                           const mizu_cvt_row *row, mizu_arrow_warn *warn) {
  const size_t wo = row->w_out, wi = row->w_in;
  /* the row's sentinel, held in registers for the mixed-word patch (wo is
     4, 8, or 16 as two 8-byte halves; RAW's 1 is a zero byte) */
  uint64_t na8;
  uint32_t na4;
  {
    uint8_t na[16] = { 0 };
    cvt_fill(na, 1, row);
    memcpy(&na8, na, 8);
    memcpy(&na4, na, 4);
  }
  /* which warning count a null lane's (undefined) value may have bumped */
  const int fix = warn == NULL ? 0 :
    row->cvt == MIZU_CVT_U64_INT64 ? 3 :
    row->cvt != MIZU_CVT_COPY || row == &CVT_ROW_I64_V ? 0 :
    row->wire == MIZU_TYPE_INT ? 1 : row->wire == MIZU_TYPE_INT64 ? 2 : 0;
  size_t i = 0;
  while (i < n) {
    const size_t lanes = n - i < 64 ? n - i : 64;
    const uint64_t w = bitmap_word(valid, off + i, lanes);
    if (w == 0 || w == arrow_lanes_mask(lanes)) {
      const int is_valid = w != 0;
      size_t run = lanes;
      while (i + run < n) {
        const size_t l2 = n - (i + run) < 64 ? n - (i + run) : 64;
        const uint64_t w2 = bitmap_word(valid, off + i + run, l2);
        if (is_valid ? w2 != arrow_lanes_mask(l2) : w2 != 0) break;
        run += l2;
      }
      if (is_valid)
        cvt_run(dst + i * wo, src + i * wi, run, row, warn);
      else
        cvt_fill(dst + i * wo, run, row);
      i += run;
    } else {
      uint64_t nulls = ~w & arrow_lanes_mask(lanes);
      if (fix == 0) {
        cvt_run(dst + i * wo, src + i * wi, lanes, row, warn);
      } else if (fix != 3 && __builtin_popcountll(nulls) >= 8) {
        /* dense: convert uncounted and count the valid lanes, cheaper than
           uncounting every null lane */
        cvt_run(dst + i * wo, src + i * wi, lanes, row, NULL);
        uint64_t c = 0;
        if (fix == 1) {
          for (size_t j = 0; j < lanes; j++) {
            int32_t v;
            memcpy(&v, src + (i + j) * 4, 4);
            c += ((w >> j) & 1) & (uint64_t) (v == INT32_MIN);
          }
          warn->n_intmin += c;
        } else {
          for (size_t j = 0; j < lanes; j++) {
            int64_t v;
            memcpy(&v, src + (i + j) * 8, 8);
            c += ((w >> j) & 1) & (uint64_t) (v == INT64_MIN);
          }
          warn->n_int64 += c;
        }
      } else {
        cvt_run(dst + i * wo, src + i * wi, lanes, row, warn);
        for (uint64_t m = nulls; m != 0; m &= m - 1) {
          const size_t j = i + (size_t) __builtin_ctzll(m);
          if (fix == 1) {
            int32_t v;
            memcpy(&v, src + j * 4, 4);
            warn->n_intmin -= v == INT32_MIN;
          } else {
            int64_t v;
            memcpy(&v, src + j * 8, 8);
            if (fix == 2) warn->n_int64 -= v == INT64_MIN;
            else warn->n_range -= v < 0;
          }
        }
      }
      if (wo == 4 && __builtin_popcountll(nulls) >= 8) {
        /* dense 4-byte lanes: a branchless select beats per-null stores */
        for (size_t j = 0; j < lanes; j++) {
          uint32_t d;
          memcpy(&d, dst + (i + j) * 4, 4);
          const uint32_t m = (uint32_t) 0 - (uint32_t) ((w >> j) & 1);
          d = (d & m) | (na4 & ~m);
          memcpy(dst + (i + j) * 4, &d, 4);
        }
        i += lanes;
        continue;
      }
      while (nulls != 0) {
        const size_t j = i + (size_t) __builtin_ctzll(nulls);
        nulls &= nulls - 1;
        if (wo == 8) {
          memcpy(dst + j * 8, &na8, 8);
        } else if (wo == 4) {
          memcpy(dst + j * 4, &na4, 4);
        } else if (wo == 16) {
          memcpy(dst + j * 16, &na8, 8);
          memcpy(dst + j * 16 + 8, &na8, 8);
        } else {
          dst[j] = 0;   /* RAW: no sentinel */
        }
      }
      i += lanes;
    }
  }
}

/* Arrow bool: the data buffer is bit-packed too, so off is a bit offset
   into both bitmaps; a lane's value is its data bit ANDed with validity. */
static void convert_bit_lgl(uint8_t *dst, const uint8_t *data,
                            const uint8_t *valid, uint64_t off, size_t n) {
  size_t i = 0;
  while (i < n) {
    size_t lanes = n - i < 64 ? n - i : 64;
    uint64_t dbits = bitmap_word(data, off + i, lanes);
    uint64_t vbits = valid != NULL ? bitmap_word(valid, off + i, lanes) :
      arrow_lanes_mask(lanes);
    uint64_t vals = dbits & vbits;
    for (size_t j = 0; j < lanes; j++) {
      int32_t o = (vbits >> j) & 1 ? (int32_t) ((vals >> j) & 1) : INT32_MIN;
      memcpy(dst + 4 * (i + j), &o, 4);
    }
    i += lanes;
  }
}

static const mizu_cvt_row CVT_ROW_U8 = { MIZU_TYPE_RAW, MIZU_CVT_COPY, 1, 1 };
static const mizu_cvt_row CVT_ROW_I8 = { MIZU_TYPE_INT, MIZU_CVT_I8_INT, 1, 4 };
static const mizu_cvt_row CVT_ROW_I16 = { MIZU_TYPE_INT, MIZU_CVT_I16_INT, 2, 4 };
static const mizu_cvt_row CVT_ROW_U16 = { MIZU_TYPE_INT, MIZU_CVT_U16_INT, 2, 4 };
static const mizu_cvt_row CVT_ROW_I32 = { MIZU_TYPE_INT, MIZU_CVT_COPY, 4, 4 };
static const mizu_cvt_row CVT_ROW_U32 = { MIZU_TYPE_REAL, MIZU_CVT_U32_REAL, 4, 8 };
static const mizu_cvt_row CVT_ROW_I64 = { MIZU_TYPE_INT64, MIZU_CVT_COPY, 8, 8 };
static const mizu_cvt_row CVT_ROW_I64_V = { MIZU_TYPE_INT64, MIZU_CVT_COPY, 8, 8 };
static const mizu_cvt_row CVT_ROW_U64 = { MIZU_TYPE_INT64, MIZU_CVT_U64_INT64, 8, 8 };
static const mizu_cvt_row CVT_ROW_F16 = { MIZU_TYPE_REAL, MIZU_CVT_F16_REAL, 2, 8 };
static const mizu_cvt_row CVT_ROW_F32 = { MIZU_TYPE_REAL, MIZU_CVT_F32_REAL, 4, 8 };
static const mizu_cvt_row CVT_ROW_F64 = { MIZU_TYPE_REAL, MIZU_CVT_COPY, 8, 8 };
static const mizu_cvt_row CVT_ROW_BOOLBIT = { MIZU_TYPE_LGL, MIZU_CVT_BIT_LGL, 0, 4 };

/* Key the table by an Arrow C Data Interface format string (every
   supported row is a single character). */
const mizu_cvt_row *mizu_cvt_for_arrow(const char *f) {
  if (f[0] == '\0' || f[1] != '\0') return NULL;
  switch (f[0]) {
  case 'C': return &CVT_ROW_U8;
  case 'c': return &CVT_ROW_I8;
  case 's': return &CVT_ROW_I16;
  case 'S': return &CVT_ROW_U16;
  case 'i': return &CVT_ROW_I32;
  case 'I': return &CVT_ROW_U32;
  case 'l': return &CVT_ROW_I64;
  case 'L': return &CVT_ROW_U64;
  case 'e': return &CVT_ROW_F16;
  case 'f': return &CVT_ROW_F32;
  case 'g': return &CVT_ROW_F64;
  case 'b': return &CVT_ROW_BOOLBIT;
  default: break;
  }
  return NULL;
}

void mizu_cvt_convert(uint8_t *dst, const uint8_t *src,
                      const uint8_t *valid, uint64_t off, size_t n,
                      const mizu_cvt_row *row, mizu_arrow_warn *warn) {
  if (row->cvt == MIZU_CVT_BIT_LGL) {
    convert_bit_lgl(dst, src, valid, off, n);
  } else if (valid != NULL) {
    convert_masked(dst, src, valid, off, n, row, warn);
  } else {
    /* the null-free path counts too: a genuine sentinel there reads as NA
       in R exactly like a masked one's — the count rides cvt_run's fused
       copy here as on the masked runs */
    cvt_run(dst, src, n, row, warn);
  }
}

// The attribute-blob emitters (complete 'I' streams) --------------------------------

/* The counting writer: the put helpers count with a NULL dst, so one
   function sizes (dst NULL) and writes behind the size pass's count. */
typedef struct {
  uint8_t *dst;
  size_t total;
} arrow_ixe;

#define ARROW_IXE_PUT(x, call) do { (x)->total += (call); } while (0)
#define ARROW_IXE_DST(x) ((x)->dst != NULL ? (x)->dst + (x)->total : NULL)

/* The vector-tag wire header (tag + u64 count) for the emit-bytes forms. */
static size_t arrow_ixe_vec_begin(uint8_t *dst, int tag, uint64_t n) {
  if (dst != NULL) {
    dst[0] = (uint8_t) tag;
    memcpy(dst + 1, &n, 8);
  }
  return 9;
}

/* A strv value, or the 0x04 scalar at length 1 (the corpus's scalar-form
   pin — attr dicts included). A len of -1 is NA (strs[i] then unused). */
static void arrow_ixe_strv(arrow_ixe *e, int64_t n, const char *const *strs,
                           const int64_t *lens) {
  if (n == 1) {
    ARROW_IXE_PUT(e, mizu_ix_put_str(ARROW_IXE_DST(e), strs[0],
                                     (int32_t) lens[0]));
    return;
  }
  ARROW_IXE_PUT(e, mizu_ix_put_strv_begin(ARROW_IXE_DST(e), (uint64_t) n));
  for (int64_t i = 0; i < n; i++)
    ARROW_IXE_PUT(e, mizu_ix_put_strelt(ARROW_IXE_DST(e), strs[i],
                                        (int32_t) lens[i]));
}

/* An intv value. Vector elements must fit int32 (the caller's contract). */
static void arrow_ixe_intv(arrow_ixe *e, int64_t n, const int64_t *vals,
                           const int *na) {
  ARROW_IXE_PUT(e, arrow_ixe_vec_begin(ARROW_IXE_DST(e), MIZU_IX_TAG_INTV,
                                       (uint64_t) n));
  if (e->dst != NULL) {
    uint8_t *dst = e->dst + e->total;
    for (int64_t i = 0; i < n; i++) {
      int32_t v = na != NULL && na[i] ? MIZU_NA_INT32 : (int32_t) vals[i];
      memcpy(dst + 4 * i, &v, 4);
    }
  }
  e->total += (size_t) n * 4;
}

static void arrow_ixe_key(arrow_ixe *e, const char *key) {
  ARROW_IXE_PUT(e, mizu_ix_put_key(ARROW_IXE_DST(e), key,
                                   (uint32_t) strlen(key)));
}

/* The class attribute: strv form, scalar at length 1. */
static void arrow_ixe_class(arrow_ixe *e, const char *const *names,
                            int64_t n) {
  const char *strs[2] = { NULL, NULL };
  int64_t lens[2] = { 0, 0 };
  for (int64_t i = 0; i < n; i++) {
    strs[i] = names[i];
    lens[i] = (int64_t) strlen(names[i]);
  }
  arrow_ixe_strv(e, n, strs, lens);
}

/* The u64 marker dict {u64 = TRUE} — the verbatim uint64 form's leaf:
   INT64 lanes the reader interprets unsigned. The bare dict serves the
   per-column blob writer; the blob adds the stream header. */
static size_t arrow_u64_dict(uint8_t *dst) {
  arrow_ixe e = { dst, 0 };
  ARROW_IXE_PUT(&e, mizu_ix_put_dict_begin(ARROW_IXE_DST(&e), 1));
  arrow_ixe_key(&e, MIZU_IX_ATTR_U64);
  ARROW_IXE_PUT(&e, mizu_ix_put_lgl(ARROW_IXE_DST(&e), 1));
  return e.total;
}

static size_t arrow_blob_u64(uint8_t *dst) {
  size_t n = mizu_ix_put_header(dst);
  return n + arrow_u64_dict(dst != NULL ? dst + n : NULL);
}

/* The frame dict's names value: the corpus's scalar form at length 1. */
static void arrow_ixe_names(arrow_ixe *e, const ArrowSchema *schema, int n) {
  if (n == 1) {
    const char *nm = schema->children[0]->name;
    ARROW_IXE_PUT(e, mizu_ix_put_str(ARROW_IXE_DST(e), nm,
                                     (int32_t) strlen(nm)));
    return;
  }
  ARROW_IXE_PUT(e, mizu_ix_put_strv_begin(ARROW_IXE_DST(e), (uint64_t) n));
  for (int i = 0; i < n; i++) {
    const char *nm = schema->children[i]->name;
    ARROW_IXE_PUT(e, mizu_ix_put_strelt(ARROW_IXE_DST(e), nm,
                                        (int32_t) strlen(nm)));
  }
}

static size_t arrow_date_dict(uint8_t *dst) {
  static const char *cls[1] = { MIZU_IX_CLASS_DATE };
  arrow_ixe e = { dst, 0 };
  ARROW_IXE_PUT(&e, mizu_ix_put_dict_begin(ARROW_IXE_DST(&e), 1));
  arrow_ixe_key(&e, MIZU_IX_ATTR_CLASS);
  arrow_ixe_class(&e, cls, 1);
  return e.total;
}

static size_t arrow_blob_date(uint8_t *dst) {
  size_t n = mizu_ix_put_header(dst);
  return n + arrow_date_dict(dst != NULL ? dst + n : NULL);
}

/* tsn: the verbatim ns form's extra pair — the i64 counts' scale label. */
static size_t arrow_ts_dict(uint8_t *dst, const char *tz, int tsn) {
  static const char *cls[2] = { MIZU_IX_CLASS_POSIXCT, MIZU_IX_CLASS_POSIXT };
  arrow_ixe e = { dst, 0 };
  ARROW_IXE_PUT(&e, mizu_ix_put_dict_begin(ARROW_IXE_DST(&e), tsn ? 3 : 2));
  arrow_ixe_key(&e, MIZU_IX_ATTR_CLASS);
  arrow_ixe_class(&e, cls, 2);
  arrow_ixe_key(&e, MIZU_IX_ATTR_TZONE);
  ARROW_IXE_PUT(&e, mizu_ix_put_str(ARROW_IXE_DST(&e), tz,
                                    (int32_t) strlen(tz)));
  if (tsn) {
    arrow_ixe_key(&e, MIZU_IX_ATTR_TSCALE);
    ARROW_IXE_PUT(&e, mizu_ix_put_str(ARROW_IXE_DST(&e), "ns", 2));
  }
  return e.total;
}

static size_t arrow_blob_ts(uint8_t *dst, const char *tz, int tsn) {
  size_t n = mizu_ix_put_header(dst);
  return n + arrow_ts_dict(dst != NULL ? dst + n : NULL, tz, tsn);
}

/* The difftime dict {class = "difftime", units = "secs"} — the layout
   column's doubles are seconds, so the blob is unit-fixed (tscale names
   the verbatim form's i64 counts instead). */
static size_t arrow_difftime_dict(uint8_t *dst, int tsn) {
  static const char *cls[1] = { MIZU_IX_CLASS_DIFFTIME };
  arrow_ixe e = { dst, 0 };
  ARROW_IXE_PUT(&e, mizu_ix_put_dict_begin(ARROW_IXE_DST(&e), tsn ? 3 : 2));
  arrow_ixe_key(&e, MIZU_IX_ATTR_CLASS);
  arrow_ixe_class(&e, cls, 1);
  arrow_ixe_key(&e, MIZU_IX_ATTR_UNITS);
  ARROW_IXE_PUT(&e, mizu_ix_put_str(ARROW_IXE_DST(&e), MIZU_IX_UNIT_SECS,
                                    (int32_t) (sizeof MIZU_IX_UNIT_SECS - 1)));
  if (tsn) {
    arrow_ixe_key(&e, MIZU_IX_ATTR_TSCALE);
    ARROW_IXE_PUT(&e, mizu_ix_put_str(ARROW_IXE_DST(&e), "ns", 2));
  }
  return e.total;
}

static size_t arrow_blob_difftime(uint8_t *dst, int tsn) {
  size_t n = mizu_ix_put_header(dst);
  return n + arrow_difftime_dict(dst != NULL ? dst + n : NULL, tsn);
}

/* The frame dict {names, class = "data.frame", row.names}: row.names the
   automatic c(NA, -n) (a caller's custom form rides the root-blob
   override). The bare dict serves the inline frame form
   (mizu_arrow_write_ix); the blob adds the stream header. */
static size_t arrow_frame_dict(uint8_t *dst, const ArrowSchema *schema,
                               int ncols, int64_t rows) {
  arrow_ixe e = { dst, 0 };
  ARROW_IXE_PUT(&e, mizu_ix_put_dict_begin(ARROW_IXE_DST(&e), 3));
  arrow_ixe_key(&e, MIZU_IX_ATTR_NAMES);
  arrow_ixe_names(&e, schema, ncols);
  arrow_ixe_key(&e, MIZU_IX_ATTR_CLASS);
  {
    static const char *cls[1] = { MIZU_IX_CLASS_DATAFRAME };
    arrow_ixe_class(&e, cls, 1);
  }
  arrow_ixe_key(&e, MIZU_IX_ATTR_ROWNAMES);
  {
    int64_t vals[2] = { 0, -rows };
    int na[2] = { 1, 0 };
    arrow_ixe_intv(&e, 2, vals, na);
  }
  return e.total;
}

static size_t arrow_blob_frame(uint8_t *dst, const ArrowSchema *schema,
                               int ncols, int64_t rows) {
  size_t n = mizu_ix_put_header(dst);
  return n + arrow_frame_dict(dst != NULL ? dst + n : NULL, schema, ncols,
                              rows);
}

// The plan --------------------------------------------------------------------------

typedef struct {
  int kind;              /* MIZU_ARROW_* */
  const mizu_cvt_row *row;   /* CVT */
  int str_form;         /* STR/DICT: 1 utf8, 2 large_utf8, 3 view */
  char unit;            /* TS/TD: the Arrow unit s/m/u/n */
  int ts_verbatim;      /* TS/TD: the ns INT64 leaf verdict (unit n and the
                           peer reads MIZU_CAP_MIZL_TSN), resolved at
                           classify */
  char tz[64];          /* TS: the classified tzone ("UTC" when naive) */
  const char *tz_o;     /* TS: the tzone override (NULL: the classified) */
  int idx_w;            /* DICT: the index width */
  int idx_signed;       /* DICT */
  int ordered;          /* DICT: the ORDERED flag's class-strv verdict,
                           resolved at classify (a cap-less peer declined
                           there, so this is never a strip) */
  int64_t nlev;         /* DICT: the canonical level count (batch 0's) */
  int64_t str_bytes;    /* STR: the packed byte total (the string walk) */
  int str_walked;       /* STR: the walk ran (at open, or at the lazy
                           open's mizu_arrow_validate) */
  int u64_verbatim;     /* CVT 'L': the MIZU_CAP_MIZL_U64V verdict,
                           resolved at classify */
} arrow_pcol;

/* The shared TS/TD unit decode: unit char and the ns-verbatim verdict
   (unit n and a peer advertising MIZU_CAP_MIZL_TSN). */
static void arrow_ts_unit(char u, uint32_t caps, arrow_pcol *pc) {
  pc->unit = u;
  pc->ts_verbatim = u == 'n' && (caps & MIZU_CAP_MIZL_TSN) != 0;
}

typedef struct {
  int64_t body;      /* the leaf's data bytes, blob excluded */
  int64_t blob;      /* the leaf attrs blob size (0 for plain columns) */
  int64_t nulls;     /* Arrow nulls across the column's batches */
  const char *tz;    /* TS: the tzone the leaf blob writes */
} arrow_scol;

typedef struct {
  char span[MIZU_NAME_MAX + 14];  /* name + "[leaf+1]" */
  size_t span_len;
  void *ref_base;      /* the referenced mapping (the REFHELD OR target) */
  int64_t ref_attrs;   /* the referenced leaf's attrs blob size */
  int64_t ref_claim;   /* the validity claim (-1 known-NA-free, else 0) */
  int used;
} arrow_remote;

struct mizu_arrow {
  const ArrowSchema *schema;         /* borrowed */
  const ArrowArray *const *batches;  /* borrowed */
  size_t nb;
  uint32_t peer_caps;
  int ncols;
  int64_t rows;
  arrow_pcol *cols;
  arrow_scol *scols;
  arrow_remote *remotes;
  int64_t *tab;          /* the validity table scratch (2 * ncols) */
  int64_t *batch_nulls;  /* resolved counts, column-major [col * nb + b] */
  const void *root_blob; /* the root-blob override (borrowed) */
  size_t root_blob_len;
  int any_str;
  int sized;
  size_t attrs_off;
  size_t root_size;      /* the root blob's byte count */
  size_t total;
};

static int64_t arrow_nulls(const mizu_arrow *a, int col, size_t b) {
  return a->batch_nulls[(size_t) col * a->nb + b];
}


/* The factor dict {levels, class} — the inline attr shape's form exactly:
   class the scalar "factor", or the two-element strv c("ordered",
   "factor") when the column classified ordered. The levels walk batch 0's
   dictionary (validated at open: every record resolves, so the
   arrow_str_at verdict is exactly == 1); a zero-batch column writes the
   empty strv. */
static size_t arrow_factor_dict(uint8_t *dst, const mizu_arrow *a, int col) {
  const arrow_pcol *pc = &a->cols[col];
  const ArrowArray *dict = a->nb != 0 ?
    a->batches[0]->children[col]->dictionary : NULL;
  arrow_ixe e = { dst, 0 };
  ARROW_IXE_PUT(&e, mizu_ix_put_dict_begin(ARROW_IXE_DST(&e), 2));
  arrow_ixe_key(&e, MIZU_IX_ATTR_LEVELS);
  if (dict == NULL) {
    /* the zero-batch column: the empty strv */
    ARROW_IXE_PUT(&e, mizu_ix_put_strv_begin(ARROW_IXE_DST(&e), 0));
  } else if (pc->nlev == 1) {
    const uint8_t *s = NULL;
    int32_t len = 0;
    (void) mizu_arrow_str_at(dict, 0, pc->str_form, &s, &len);
    ARROW_IXE_PUT(&e, mizu_ix_put_str(ARROW_IXE_DST(&e), s, len));
  } else {
    ARROW_IXE_PUT(&e, mizu_ix_put_strv_begin(ARROW_IXE_DST(&e),
                                             (uint64_t) pc->nlev));
    for (int64_t i = 0; i < pc->nlev; i++) {
      const uint8_t *s = NULL;
      int32_t len = 0;
      (void) mizu_arrow_str_at(dict, i, pc->str_form, &s, &len);
      ARROW_IXE_PUT(&e, mizu_ix_put_strelt(ARROW_IXE_DST(&e), s, len));
    }
  }
  arrow_ixe_key(&e, MIZU_IX_ATTR_CLASS);
  {
    static const char *cls[1] = { MIZU_IX_CLASS_FACTOR };
    static const char *ocls[2] = { MIZU_IX_CLASS_ORDERED,
                                   MIZU_IX_CLASS_FACTOR };
    arrow_ixe_class(&e, pc->ordered ? ocls : cls, pc->ordered ? 2 : 1);
  }
  return e.total;
}

/* The factor blob: the dict as a complete stream. */
static size_t arrow_blob_factor(uint8_t *dst, const mizu_arrow *a, int col) {
  size_t n = mizu_ix_put_header(dst);
  return n + arrow_factor_dict(dst != NULL ? dst + n : NULL, a, col);
}

// Classification ---------------------------------------------------------------------

/* A decline from the writer: the thread-local slot, MIZU_ERR returned. */
static mizu_status arrow_decline(const char *what) {
  mizu_err_record_tls(MIZU_ERRCAT_OTHER, "%s", what);
  return MIZU_ERR;
}

/* Classify one schema child into a plan column. */
static int arrow_classify(mizu_arrow *a, int col, const ArrowSchema *sc) {
  arrow_pcol *pc = &a->cols[col];
  memset(pc, 0, sizeof(*pc));
  const char *nm = a->schema->children[col]->name;
  const char *f = sc->format;
  if (f == NULL) {
    mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                        "column '%s': an Arrow column with no format", nm);
    return -1;
  }
  if (sc->dictionary != NULL) {
    if (sc->flags & MIZU_ARROW_FLAG_DICTIONARY_ORDERED) {
      /* ordered factors: metadata is data — a peer short of the
         capability gets the decline, never a stripped class */
      if ((a->peer_caps & MIZU_CAP_ORDERED) == 0) {
        mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                            "column '%s': an ordered dictionary column "
                            "needs a peer with the ordered factor "
                            "capability (polars Enum: cast to "
                            "pl.Categorical)", nm);
        return -1;
      }
      pc->ordered = 1;
    }
    const char *vf = sc->dictionary->format;
    if (vf == NULL ||
        (strcmp(vf, "u") != 0 && strcmp(vf, "U") != 0 &&
         strcmp(vf, "vu") != 0)) {
      mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                          "column '%s': a dictionary column of non-string "
                          "values has no portable home", nm);
      return -1;
    }
    pc->str_form = vf[0] == 'v' ? 3 : vf[0] == 'U' ? 2 : 1;
    if (f[1] != '\0') goto bad_index;
    switch (f[0]) {
    case 'c': pc->idx_w = 1; pc->idx_signed = 1; break;
    case 'C': pc->idx_w = 1; break;
    case 's': pc->idx_w = 2; pc->idx_signed = 1; break;
    case 'S': pc->idx_w = 2; break;
    case 'i': pc->idx_w = 4; pc->idx_signed = 1; break;
    case 'I': pc->idx_w = 4; break;
    case 'l': pc->idx_w = 8; pc->idx_signed = 1; break;
    case 'L': pc->idx_w = 8; break;
    default:
    bad_index:
      mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                          "column '%s': a dictionary column with a "
                          "non-integer index has no portable home", nm);
      return -1;
    }
    pc->kind = MIZU_ARROW_DICT;
    return 0;
  }
  if (strcmp(f, "u") == 0) {
    pc->kind = MIZU_ARROW_STR;
    pc->str_form = 1;
    return 0;
  }
  if (strcmp(f, "U") == 0) {
    pc->kind = MIZU_ARROW_STR;
    pc->str_form = 2;
    return 0;
  }
  if (strcmp(f, "vu") == 0) {
    pc->kind = MIZU_ARROW_STR;
    pc->str_form = 3;
    return 0;
  }
  if (strcmp(f, "tdD") == 0) {
    pc->kind = MIZU_ARROW_DATE;
    return 0;
  }
  /* the unit compare precedes the f[3] read: a short format ("ts", "tD")
     must not be indexed past its NUL */
  if (f[0] == 't' && f[1] == 's' &&
      (f[2] == 's' || f[2] == 'm' || f[2] == 'u' || f[2] == 'n') &&
      f[3] == ':') {
    arrow_ts_unit(f[2], a->peer_caps, pc);
    const char *tz = f + 4;
    if (tz[0] == '\0') tz = "UTC";
    const size_t tzlen = strlen(tz);
    if (tzlen >= sizeof(pc->tz)) {
      mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                          "column '%s': a timezone name past %zu bytes has "
                          "no portable home", nm, sizeof(pc->tz) - 1);
      return -1;
    }
    memcpy(pc->tz, tz, tzlen + 1);
    pc->kind = MIZU_ARROW_TS;
    return 0;
  }
  if (f[0] == 't' && f[1] == 'D' &&
      (f[2] == 's' || f[2] == 'm' || f[2] == 'u' || f[2] == 'n') &&
      f[3] == '\0') {
    arrow_ts_unit(f[2], a->peer_caps, pc);
    pc->kind = MIZU_ARROW_TD;
    return 0;
  }
  if (f[0] == 'L' && f[1] == '\0') {
    /* uint64: verbatim (an INT64 leaf + the u64 blob attr) for a peer
       advertising MIZU_CAP_MIZL_U64V, else the integer64 conversion
       (in-range exact, the int64 NA sentinel past INT64_MAX) */
    pc->kind = MIZU_ARROW_CVT;
    pc->row = &CVT_ROW_U64;
    pc->u64_verbatim = (a->peer_caps & MIZU_CAP_MIZL_U64V) != 0;
    return 0;
  }
  const mizu_cvt_row *row = mizu_cvt_for_arrow(f);
  if (row != NULL) {
    pc->kind = MIZU_ARROW_CVT;
    pc->row = row;
    return 0;
  }
  mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                      "column '%s': an Arrow type with no portable home "
                      "(format '%s')", nm, f);
  return -1;
}

/* A dictionary array's buffer shape: the levels vet, applied to every
   batch's dictionary ahead of the level match's reads. A string_view
   (form 3) dictionary with every level inline (<= 12 bytes) legally has
   no variadic data buffer — n_buffers == 2; a level past 12 bytes then
   fails the open's per-level resolve, so the form is vetted here and the
   spans there. */
static int arrow_dict_shape_ok(const ArrowArray *dict, int str_form) {
  if (dict == NULL || dict->buffers == NULL) return 0;
  /* the dictionary array's own offset and null count, range-checked as
     every column's are (arrow_check_batch): a negative offset would index
     ahead of the offsets buffer */
  if (dict->length < 0 || dict->offset < 0 || dict->null_count < -1 ||
      dict->null_count > dict->length)
    return 0;
  const int64_t need = str_form == 3 ? 2 : 3;
  if (dict->n_buffers < need) return 0;
  if (dict->length <= 0) return 1;
  return dict->buffers[1] != NULL &&
    (dict->n_buffers == 2 || dict->buffers[2] != NULL);
}

/* One batch's shape checks. 0 vetted, -1 with the decline in the
   thread-local slot. */
static int arrow_check_batch(mizu_arrow *a, const ArrowArray *b) {
  /* the struct's own validity and offset are never read: a batch
     carrying either would stage the children's slot values as rows */
  if (b->offset != 0 ||
      (b->null_count != 0 && b->buffers != NULL && b->buffers[0] != NULL)) {
    mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                        "an Arrow batch with null struct rows or a struct "
                        "offset has no portable home");
    return -1;
  }
  for (int i = 0; i < a->ncols; i++) {
    const ArrowArray *c = b->children[i];
    arrow_pcol *pc = &a->cols[i];
    if (c == NULL) {
      mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                          "an invalid Arrow C Data Interface batch");
      return -1;
    }
    if (c->length != b->length) {
      mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                          "an Arrow batch whose columns differ in length");
      return -1;
    }
    int64_t need = pc->kind == MIZU_ARROW_STR ?
      (pc->str_form == 3 ? 2 : 3) : 2;
    if (c->n_buffers < need || c->buffers == NULL || c->length < 0 ||
        c->offset < 0 || c->null_count < -1 ||
        c->null_count > c->length ||
        (c->buffers[0] == NULL && c->null_count > 0) ||
        (c->buffers[1] == NULL && c->length != 0)) {
      mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                          "an invalid Arrow C Data Interface batch");
      return -1;
    }
    if (pc->kind == MIZU_ARROW_STR && pc->str_form != 3 &&
        c->buffers[2] == NULL && c->length != 0) {
      mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                          "an invalid Arrow C Data Interface batch");
      return -1;
    }
    if (pc->kind == MIZU_ARROW_DICT &&
        !arrow_dict_shape_ok(c->dictionary, pc->str_form)) {
      mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                          "an invalid Arrow C Data Interface batch");
      return -1;
    }
    /* R raw vectors have no NA: the masked convert would write a 4-byte
       sentinel into a 1-byte column */
    if (pc->kind == MIZU_ARROW_CVT && pc->row->wire == MIZU_TYPE_RAW &&
        c->buffers[0] != NULL && c->null_count != 0) {
      mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                          "Arrow nulls cannot cross in a uint8 column (R "
                          "raw vectors have no NA)");
      return -1;
    }
  }
  return 0;
}

// The dictionary validation -------------------------------------------------------------

/* The dictionary levels of batch 0 (the canonical set later batches must
   match): validated in place — the borrowed dictionary ArrowArray serves
   every later read (the level-match and the factor blob walk it through
   arrow_str_at), so no offsets or bytes are copied out. */
static int arrow_levels_check(mizu_arrow *a, int col) {
  const char *nm = a->schema->children[col]->name;
  arrow_pcol *pc = &a->cols[col];
  if (a->nb == 0) {
    /* a zero-row stream serves no batch: the canonical levels are empty */
    pc->nlev = 0;
    return 0;
  }
  /* batch 0's dictionary: shape-vetted with every batch at open
     (arrow_check_batch), ahead of this walk */
  const ArrowArray *dict = a->batches[0]->children[col]->dictionary;
  int64_t blen = 0;
  for (int64_t i = 0; i < dict->length; i++) {
    const uint8_t *s;
    int32_t len;
    if (!mizu_arrow_valid(dict, i)) {
      /* a null level: decline (R factor levels are non-NA) */
      mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                          "column '%s': a dictionary column with a null "
                          "level has no portable home", nm);
      return -1;
    }
    if (mizu_arrow_str_at(dict, i, pc->str_form, &s, &len) != 1) {
      mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                          "column '%s': an invalid Arrow dictionary export",
                          nm);
      return -1;
    }
    if (blen + (int64_t) len > INT32_MAX) {
      mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                          "column '%s': dictionary levels past the int32 "
                          "offset range", nm);
      return -1;
    }
    blen += (int64_t) len;
  }
  pc->nlev = dict->length;
  return 0;
}

/* Min/max of n >= 1 indices from element i, validity ignored. The
   reduction stays in the index's own type (widening each lane to int64
   costs the narrow types their vector lanes). The unsigned 64-bit arm reads
   the bits as int64, so values past INT64_MAX decline as negative. */
static void arrow_dict_minmax(const uint8_t *idx, int64_t i, int64_t n,
                              const arrow_pcol *pc, int64_t *lo, int64_t *hi) {
  int64_t mn = INT64_MAX, mx = INT64_MIN;
#define ARROW_DICT_MINMAX(ST, W)                                               \
  {                                                                            \
    ST lo_, hi_;                                                               \
    memcpy(&lo_, idx + (W) * i, (W));                                          \
    hi_ = lo_;                                                                 \
    for (int64_t k = 1; k < n; k++) {                                          \
      ST v;                                                                    \
      memcpy(&v, idx + (W) * (i + k), (W));                                    \
      if (v < lo_) lo_ = v;                                                    \
      if (v > hi_) hi_ = v;                                                    \
    }                                                                          \
    mn = (int64_t) lo_;                                                        \
    mx = (int64_t) hi_;                                                        \
  }                                                                            \
  break;
  switch (pc->idx_w * 2 + (pc->idx_signed != 0)) {
  case 2: ARROW_DICT_MINMAX(uint8_t, 1)
  case 3: ARROW_DICT_MINMAX(int8_t, 1)
  case 4: ARROW_DICT_MINMAX(uint16_t, 2)
  case 5: ARROW_DICT_MINMAX(int16_t, 2)
  case 8: ARROW_DICT_MINMAX(uint32_t, 4)
  case 9: ARROW_DICT_MINMAX(int32_t, 4)
  default: ARROW_DICT_MINMAX(int64_t, 8)
  }
#undef ARROW_DICT_MINMAX
  *lo = mn;
  *hi = mx;
}

/* Every valid index of one batch inside [0, nlev). A null-free batch is one
   reduction; otherwise 4096 lanes at a time, narrowing to 64-lane words and
   then to validity only where the unmasked range fails (a null slot's value
   is undefined). */
static int arrow_dict_in_range(const ArrowArray *ba, const arrow_pcol *pc,
                               int64_t nulls) {
  const uint8_t *idx = (const uint8_t *) ba->buffers[1];
  const uint8_t *valid = (const uint8_t *) ba->buffers[0];
  int64_t lo, hi;
  for (int64_t k = 0; k < ba->length;) {
    const int64_t left = ba->length - k;
    const int64_t lanes = nulls == 0 || left < 4096 ? left : 4096;
    arrow_dict_minmax(idx, ba->offset + k, lanes, pc, &lo, &hi);
    if (lo < 0 || hi >= pc->nlev) {
      if (nulls == 0) return 0;
      for (int64_t q = 0; q < lanes; q += 64) {
        const int64_t l2 = lanes - q < 64 ? lanes - q : 64;
        arrow_dict_minmax(idx, ba->offset + k + q, l2, pc, &lo, &hi);
        if (lo >= 0 && hi < pc->nlev) continue;
        uint64_t w = bitmap_word(valid, (uint64_t) (ba->offset + k + q),
                                 (size_t) l2);
        for (; w != 0; w &= w - 1) {
          arrow_dict_minmax(idx, ba->offset + k + q + __builtin_ctzll(w), 1,
                            pc, &lo, &hi);
          if (lo < 0 || hi >= pc->nlev) return 0;
        }
      }
    }
    k += lanes;
  }
  return 1;
}

/* A later batch's dictionary must equal batch 0's canonical set. */
static int arrow_levels_match(mizu_arrow *a, int col,
                              const ArrowArray *dict) {
  const arrow_pcol *pc = &a->cols[col];
  const ArrowArray *d0 = a->batches[0]->children[col]->dictionary;
  if (dict == NULL || dict->length != pc->nlev) return 0;
  for (int64_t i = 0; i < pc->nlev; i++) {
    const uint8_t *s0, *s;
    int32_t l0, l;
    if (mizu_arrow_str_at(dict, i, pc->str_form, &s, &l) != 1) return 0;
    /* batch 0's record was validated at open: == 1 */
    if (mizu_arrow_str_at(d0, i, pc->str_form, &s0, &l0) != 1 || l0 != l ||
        memcmp(s0, s, (size_t) l) != 0)
      return 0;
  }
  return 1;
}

/* The dictionary columns' canonical levels (batch 0) and index bounds,
   validated before any emit. */
static int arrow_validate_dicts(mizu_arrow *a) {
  for (int i = 0; i < a->ncols; i++) {
    arrow_pcol *pc = &a->cols[i];
    if (pc->kind != MIZU_ARROW_DICT) continue;
    const char *nm = a->schema->children[i]->name;
    if (arrow_levels_check(a, i) < 0)
      return -1;
    /* the blob rides the directory entry's int32 attrs_size: each level
       costs framing bytes on top of the (already bounded) packed bytes,
       so a level count past ~2^29 would wrap the cast at write */
    if (arrow_blob_factor(NULL, a, i) > (size_t) INT32_MAX) {
      mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                          "column '%s': dictionary levels past the int32 "
                          "attrs range", nm);
      return -1;
    }
    for (size_t b = 0; b < a->nb; b++) {
      const ArrowArray *ba = a->batches[b]->children[i];
      if (b != 0 && !arrow_levels_match(a, i, ba->dictionary)) {
        mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                            "column '%s': a dictionary column whose levels "
                            "differ across batches", nm);
        return -1;
      }
      if (!arrow_dict_in_range(ba, pc, arrow_nulls(a, i, b))) {
        mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                            "column '%s': a dictionary index outside the "
                            "levels", nm);
        return -1;
      }
    }
  }
  return 0;
}

// The size pass -------------------------------------------------------------------------

/* One offsets-width span walk (the null-free span: the endpoints alone
   don't prove the interior monotone, so walk it; COND rejects a step). */
#define ARROW_STR_SIZE_SPAN(otype, ow, COND)                                 \
  {                                                                          \
    const uint8_t *offs = (const uint8_t *) ba->buffers[1];                  \
    otype first, prev;                                                       \
    memcpy(&first, offs + (ow) * ba->offset, (ow));                          \
    prev = first;                                                            \
    if (prev < 0) goto malformed;                                            \
    for (int64_t k = 0; k < ba->length; k++) {                               \
      otype o;                                                               \
      memcpy(&o, offs + (ow) * (ba->offset + k + 1), (ow));                  \
      if (COND) goto malformed;                                              \
      prev = o;                                                              \
    }                                                                        \
    bytes += prev - first;                                                   \
  }

/* The packed string byte total: per batch the offset span when no nulls
   (a null's span must collapse, so a batch with nulls walks). Runs at
   open, or at mizu_arrow_validate after a lazy one: a malformed record
   declines there, ahead of any emit. */
static int arrow_size_str(mizu_arrow *a, int col) {
  arrow_pcol *pc = &a->cols[col];
  int64_t bytes = 0;
  for (size_t b = 0; b < a->nb; b++) {
    const ArrowArray *ba = a->batches[b]->children[col];
    if (ba->length == 0) continue;
    if (pc->str_form != 3 && arrow_nulls(a, col, b) == 0) {
      /* the null-free span: the endpoints alone don't prove the
         intermediate offsets — the writer rebases per element, so a
         non-monotonic interior (or an i64 span past the int32 element
         bound) would size one way and gather another; walk them */
      if (pc->str_form == 1)
        ARROW_STR_SIZE_SPAN(int32_t, 4, o < prev)
      else
        ARROW_STR_SIZE_SPAN(int64_t, 8, o < prev || o - prev > INT32_MAX)
      continue;
    }
    for (int64_t k = 0; k < ba->length; k++) {
      const uint8_t *s;
      int32_t len;
      int rc = mizu_arrow_str_at(ba, k, pc->str_form, &s, &len);
      if (rc < 0) goto malformed;
      if (rc > 0) bytes += len;
    }
  }
  pc->str_bytes = bytes;
  pc->str_walked = 1;
  return 0;
malformed:
  mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                      "column '%s': an invalid Arrow string export (a "
                      "malformed offset or view record)",
                      a->schema->children[col]->name);
  return -1;
}
#undef ARROW_STR_SIZE_SPAN

static void arrow_size_col(mizu_arrow *a, int col, arrow_scol *c) {
  const arrow_pcol *pc = &a->cols[col];
  memset(c, 0, sizeof(*c));
  switch (pc->kind) {
  case MIZU_ARROW_CVT:
    c->body = a->rows * pc->row->w_out;
    if (pc->u64_verbatim) c->blob = (int64_t) arrow_blob_u64(NULL);
    break;
  case MIZU_ARROW_STR:
    c->body = mizu_mizs_geometry(a->rows).data + pc->str_bytes;
    break;
  case MIZU_ARROW_DICT:
    c->body = a->rows * 4;
    c->blob = (int64_t) arrow_blob_factor(NULL, a, col);
    break;
  case MIZU_ARROW_DATE:
    c->body = a->rows * 8;
    c->blob = (int64_t) arrow_blob_date(NULL);
    break;
  case MIZU_ARROW_TS:
    c->body = a->rows * 8;
    c->tz = pc->tz_o != NULL ? pc->tz_o : pc->tz;
    c->blob = (int64_t) arrow_blob_ts(NULL, c->tz, pc->ts_verbatim);
    break;
  case MIZU_ARROW_TD:
    c->body = a->rows * 8;
    c->blob = (int64_t) arrow_blob_difftime(NULL, pc->ts_verbatim);
    break;
  default:
    break;
  }
  for (size_t b = 0; b < a->nb; b++)
    c->nulls += arrow_nulls(a, col, b);
}

/* The size pass: per-column bodies and blobs, the root blob, the tail.
   Infallible — the batch vetting and the string walks ran at open. */
static void arrow_size_pass(mizu_arrow *a) {
  const int ncols = a->ncols;
  size_t cur = MIZU_ALIGN64(MIZU_HEADER_SIZE + 32 * (size_t) ncols);
  int any_atomic_nulls = 0;
  int any_remote_unknown = 0;
  for (int i = 0; i < ncols; i++) {
    if (a->remotes[i].used) {
      a->scols[i].body = (int64_t) a->remotes[i].span_len;
      a->scols[i].blob = 0;
      a->scols[i].nulls = 0;
      if (a->remotes[i].ref_claim != -1) any_remote_unknown = 1;
    } else {
      arrow_size_col(a, i, &a->scols[i]);
    }
    cur += MIZU_ALIGN64((size_t) (a->scols[i].body + a->scols[i].blob));
    if (a->cols[i].kind != MIZU_ARROW_STR && a->scols[i].nulls > 0)
      any_atomic_nulls = 1;
  }
  a->root_size = a->root_blob != NULL ? a->root_blob_len :
    arrow_blob_frame(NULL, a->schema, ncols, a->rows);
  a->attrs_off = cur;
  cur += MIZU_ALIGN64(a->root_size);
  /* the tail rides whenever a table is emitted: local nulls, or a remote
     column that cannot claim known-NA-free (its row is the claim alone —
     a remote column adds no bitmap and no count to the header validity) */
  if (any_atomic_nulls || any_remote_unknown) {
    for (int i = 0; i < ncols; i++) {
      if (a->cols[i].kind == MIZU_ARROW_STR || a->scols[i].nulls == 0)
        continue;
      cur = MIZU_ALIGN64(cur) + ((size_t) a->rows + 7) / 8;
    }
    cur = MIZU_ALIGN64(cur) + 16 * (size_t) ncols;
  }
  a->total = cur;
  a->sized = 1;
}

// The write pass -------------------------------------------------------------------------

static void arrow_write_cvt(uint8_t *dst, const mizu_arrow *a, int col,
                            const mizu_cvt_row *row, mizu_arrow_warn *warn) {
  size_t done = 0;
  for (size_t b = 0; b < a->nb; b++) {
    const ArrowArray *ba = a->batches[b]->children[col];
    size_t n = (size_t) ba->length;
    if (n == 0) continue;
    const uint8_t *data = (const uint8_t *) ba->buffers[1];
    const uint8_t *valid = arrow_nulls(a, col, b) != 0 ?
      (const uint8_t *) ba->buffers[0] : NULL;
    uint64_t off = (uint64_t) ba->offset;
    if (row->cvt == MIZU_CVT_BIT_LGL) {
      mizu_cvt_convert(dst + done, data, valid, off, n, row, warn);
    } else {
      mizu_cvt_convert(dst + done, data + off * row->w_in, valid,
                       off, n, row, warn);
    }
    done += n * row->w_out;
  }
}

/* One offsets-width batch copy (the null-free path: rebase the offsets
   per batch, the bytes one memcpy). */
#define ARROW_STR_COPY_SPAN(otype, ow, dtype)                                \
  {                                                                          \
    const uint8_t *so = (const uint8_t *) ba->buffers[1];                    \
    otype base0;                                                             \
    memcpy(&base0, so + (ow) * ba->offset, (ow));                            \
    for (int64_t k = 0; k < ba->length; k++) {                               \
      otype o;                                                               \
      memcpy(&o, so + (ow) * (ba->offset + k + 1), (ow));                    \
      offs[done + k + 1] = (dtype) (run + (o - base0));                      \
    }                                                                        \
    otype oend;                                                              \
    memcpy(&oend, so + (ow) * (ba->offset + ba->length), (ow));              \
    otype blen = oend - base0;                                               \
    memcpy(data + run, (const uint8_t *) ba->buffers[2] + base0,             \
           (size_t) blen);                                                   \
    run += blen;                                                             \
  }

/* The MIZS string block: validity bitmap (present bits), i64 offsets, the
   per-string encoding bytes, the packed bytes — each section 64-byte
   aligned from the block start. Present elements mark CE_UTF8, an NA
   spans zero bytes. */
static void arrow_write_str(uint8_t *block, mizu_arrow *a, int col,
                            int64_t rows, int has_nulls) {
  const arrow_pcol *pc = &a->cols[col];
  mizu_mizs_geom g = mizu_mizs_geometry(rows);
  uint8_t *validity = block + g.validity;
  int64_t *offs = (int64_t *) (block + g.offsets);
  uint8_t *enc = block + g.encoding;
  uint8_t *data = block + g.data;
  memset(validity, 0xFF, (size_t) (rows + 7) / 8);
  memset(enc, MIZU_CE_UTF8, (size_t) rows);
  int64_t done = 0, run = 0;
  offs[0] = 0;
  if (!has_nulls && pc->str_form != 3) {
    /* no nulls: per-batch rebase, the bytes one memcpy per batch */
    for (size_t b = 0; b < a->nb; b++) {
      const ArrowArray *ba = a->batches[b]->children[col];
      if (ba->length == 0) continue;
      if (pc->str_form == 1)
        ARROW_STR_COPY_SPAN(int32_t, 4, int64_t)
      else
        ARROW_STR_COPY_SPAN(int64_t, 8, int64_t)
      done += ba->length;
    }
    return;
  }
  /* the gather: string_view, or nulls (a null slot's span collapses to
     zero bytes), the encoding bytes and validity cleared in the walk */
  for (size_t b = 0; b < a->nb; b++) {
    const ArrowArray *ba = a->batches[b]->children[col];
    for (int64_t k = 0; k < ba->length; k++) {
      const uint8_t *s;
      int32_t len;
      /* the size walk already validated every element (a malformed
         record declined at open), so this is exactly == 1 */
      if (mizu_arrow_str_at(ba, k, pc->str_form, &s, &len) == 1) {
        memcpy(data + run, s, (size_t) len);
        run += len;
      } else {
        int64_t i = done + k;
        validity[i >> 3] &= (uint8_t) ~(1u << (i & 7));
        enc[i] = 0;
      }
      offs[done + k + 1] = run;
    }
    done += ba->length;
  }
}

/* The null-free utf8 / large_utf8 concat at int32 offsets (the
   binding-side column layout). The open's walk proved every span monotone
   and holds the byte total (gated to int32 here), so the write only
   rebases, a large_utf8 column's offsets narrowing as they go. */
int64_t mizu_arrow_col_utf8(const mizu_arrow *a, int64_t col, int32_t *offs,
                            uint8_t *data) {
  if (col < 0 || col >= a->ncols) return -1;
  const arrow_pcol *pc = &a->cols[col];
  if (pc->kind != MIZU_ARROW_STR || pc->str_form == 3 ||
      !pc->str_walked || pc->str_bytes > INT32_MAX)
    return -1;
  for (size_t b = 0; b < a->nb; b++)
    if (arrow_nulls(a, (int) col, b) != 0) return -1;
  if (offs == NULL) return pc->str_bytes;
  int64_t done = 0, run = 0;
  offs[0] = 0;
  for (size_t b = 0; b < a->nb; b++) {
    const ArrowArray *ba = a->batches[b]->children[col];
    if (ba->length == 0) continue;
    if (pc->str_form == 1)
      ARROW_STR_COPY_SPAN(int32_t, 4, int32_t)
    else
      ARROW_STR_COPY_SPAN(int64_t, 8, int32_t)
    done += ba->length;
  }
  return pc->str_bytes;
}
#undef ARROW_STR_COPY_SPAN

/* A dictionary column: the INT32 leaf of 1-based R codes (NA = INT_MIN).
   Open validated every valid index, so the widening rows can add one without
   a range check; their unsigned arithmetic also tolerates arbitrary null
   lanes when a mixed validity word converts all 64 lanes at once. */
static void arrow_write_dict(uint8_t *dst, const mizu_arrow *a, int col) {
  const arrow_pcol *pc = &a->cols[col];
  mizu_cvt_row row = { MIZU_TYPE_INT, 0, (uint8_t) pc->idx_w, 4 };
  switch (pc->idx_w) {
  case 1:
    row.cvt = pc->idx_signed ? ARROW_CVT_DICT_I8 : ARROW_CVT_DICT_U8;
    break;
  case 2:
    row.cvt = pc->idx_signed ? ARROW_CVT_DICT_I16 : ARROW_CVT_DICT_U16;
    break;
  case 4:
    row.cvt = pc->idx_signed ? ARROW_CVT_DICT_I32 : ARROW_CVT_DICT_U32;
    break;
  default:
    row.cvt = pc->idx_signed ? ARROW_CVT_DICT_I64 : ARROW_CVT_DICT_U64;
    break;
  }
  int64_t done = 0;
  for (size_t b = 0; b < a->nb; b++) {
    const ArrowArray *ba = a->batches[b]->children[col];
    const uint8_t *idx = (const uint8_t *) ba->buffers[1];
    const uint8_t *valid = arrow_nulls(a, col, b) != 0 ?
      (const uint8_t *) ba->buffers[0] : NULL;
    const uint64_t off = (uint64_t) ba->offset;
    mizu_cvt_convert(dst + 4 * done, idx + off * row.w_in, valid, off,
                     (size_t) ba->length, &row, NULL);
    done += ba->length;
  }
}

/* A date32 column: the REAL leaf of epoch-day doubles (NaT = NA payload). */
static void arrow_write_date(uint8_t *dst, const mizu_arrow *a, int col) {
  static const mizu_cvt_row row = {
    MIZU_TYPE_REAL, ARROW_CVT_DATE, 4, 8
  };
  arrow_write_cvt(dst, a, col, &row, NULL);
}

/* A timestamp column: the REAL leaf of epoch-second doubles. */
static void arrow_write_ts(uint8_t *dst, const mizu_arrow *a, int col) {
  const arrow_pcol *pc = &a->cols[col];
  const int cvt = pc->unit == 's' ? ARROW_CVT_TS_S :
    pc->unit == 'm' ? ARROW_CVT_TS_MS :
    pc->unit == 'u' ? ARROW_CVT_TS_US : ARROW_CVT_TS_NS;
  const mizu_cvt_row row = { MIZU_TYPE_REAL, cvt, 8, 8 };
  arrow_write_cvt(dst, a, col, &row, NULL);
}

/* Clear the null lanes of one validity word (lanes <= 64) at bit position
   pos of dst, touching only the bytes the lanes cover. */
static void arrow_bitmap_clear(uint8_t *dst, uint64_t pos, uint64_t nulls,
                               size_t lanes) {
  uint8_t *p = dst + (pos >> 3);
  const unsigned sh = (unsigned) (pos & 7);
  /* up to 8 bytes from the shifted word, a 9th only when the shift spills */
  const size_t nbytes = (sh + lanes + 7) >> 3;
  const uint64_t lo = nulls << sh;
  for (size_t j = 0; j < nbytes && j < 8; j++)
    p[j] &= (uint8_t) ~(lo >> (8 * j));
  if (nbytes == 9) p[8] &= (uint8_t) ~(nulls >> (64 - sh));
}

/* One leaf's validity bitmap into the section: verbatim for a single
   offset-0 batch, else memset-present and each batch with nulls laid over
   it. A batch's head lanes clear up to a destination byte boundary; from
   there its whole bytes are the batch's own, so they store directly (one
   memcpy when the source is byte-aligned there too, else shifted words),
   and the tail lanes clear. */
static void arrow_bitmap_write(uint8_t *dst, const mizu_arrow *a, int col,
                               int64_t rows) {
  const size_t nb = ((size_t) rows + 7) / 8;
  if (a->nb == 1) {
    const ArrowArray *ba = a->batches[0]->children[col];
    if (ba->offset == 0) {
      memcpy(dst, ba->buffers[0], nb);
      return;
    }
  }
  memset(dst, 0xFF, nb);
  int64_t done = 0;
  for (size_t b = 0; b < a->nb; b++) {
    const ArrowArray *ba = a->batches[b]->children[col];
    if (arrow_nulls(a, col, b) != 0) {
      const uint8_t *valid = (const uint8_t *) ba->buffers[0];
      int64_t k = -done & 7;
      if (k > ba->length) k = ba->length;
      if (k != 0)
        arrow_bitmap_clear(dst, (uint64_t) done,
                           arrow_lanes_mask((size_t) k) &
                           ~bitmap_word(valid, (uint64_t) ba->offset,
                                        (size_t) k), (size_t) k);
      uint8_t *p = dst + ((done + k) >> 3);
      if (((ba->offset + k) & 7) == 0) {
        const size_t whole = (size_t) (ba->length - k) >> 3;
        memcpy(p, valid + ((ba->offset + k) >> 3), whole);
        k += (int64_t) whole * 8;
      } else {
        for (; k + 64 <= ba->length; k += 64, p += 8) {
          const uint64_t w = bitmap_word(valid, (uint64_t) (ba->offset + k),
                                         64);
          memcpy(p, &w, 8);
        }
      }
      while (k < ba->length) {
        const size_t lanes = ba->length - k < 64 ?
          (size_t) (ba->length - k) : 64;
        arrow_bitmap_clear(dst, (uint64_t) (done + k),
                           arrow_lanes_mask(lanes) &
                           ~bitmap_word(valid, (uint64_t) (ba->offset + k),
                                        lanes), lanes);
        k += (int64_t) lanes;
      }
    }
    done += ba->length;
  }
}

int64_t mizu_arrow_col_validity(const mizu_arrow *a, int64_t col,
                                uint8_t *dst) {
  if (col < 0 || col >= a->ncols) return 0;
  int64_t nulls = 0;
  for (size_t b = 0; b < a->nb; b++) nulls += arrow_nulls(a, (int) col, b);
  if (dst != NULL) {
    if (nulls != 0)
      arrow_bitmap_write(dst, a, (int) col, a->rows);
    else
      memset(dst, 0xFF, ((size_t) a->rows + 7) / 8);
  }
  return nulls;
}

/* The MIZL directory entry (mizu.h's wire format). */
typedef struct {
  int64_t data_offset;   /* 64-byte aligned */
  int64_t data_size;
  int32_t sexptype;
  int32_t attrs_size;
  int64_t length;
} arrow_elem;

MIZU_STATIC_ASSERT(sizeof(arrow_elem) == 32,
                   "the MIZL directory entry is 32 bytes");

void mizu_arrow_write(mizu_arrow *a, void *base_, mizu_arrow_warn *warn) {
  if (!a->sized) arrow_size_pass(a);
  uint8_t *base = (uint8_t *) base_;
  const int ncols = a->ncols;
  const int64_t rows = a->rows;
  /* the reserved header band [24-63]: a recycled region carries stale
     words — the zc refcount included, which retain_zc stores after */
  memset(base + MIZU_ZC_REFCOUNT_OFF, 0, MIZU_HEADER_SIZE - MIZU_ZC_REFCOUNT_OFF);
  size_t cur = MIZU_ALIGN64(MIZU_HEADER_SIZE + 32 * (size_t) ncols);
  int64_t *tab = a->tab;
  for (int i = 0; i < ncols; i++) {
    const arrow_pcol *pc = &a->cols[i];
    const arrow_scol *c = &a->scols[i];
    arrow_elem entry;
    entry.data_offset = (int64_t) cur;
    entry.length = rows;
    uint8_t *dst = base + cur;
    if (a->remotes[i].used) {
      /* the remote leaf: the identifier span, the referenced leaf's attrs
         size as resolved — no local bytes */
      const arrow_remote *r = &a->remotes[i];
      entry.sexptype = MIZU_ARROW_TAG_REMOTE;
      entry.attrs_size = (int32_t) r->ref_attrs;
      entry.data_size = (int64_t) r->span_len;
      memcpy(dst, r->span, r->span_len);
      /* one REFHELD OR per distinct referenced region per send: the
         holder set widens beyond the direct peer */
      int seen = 0;
      for (int k = 0; k < i; k++)
        if (a->remotes[k].used && a->remotes[k].ref_base == r->ref_base)
          seen = 1;
      if (!seen)
        atomic_fetch_or_explicit(mizu_zc_flags_(r->ref_base),
                                 MIZU_ZC_FLAG_REFHELD, memory_order_acq_rel);
    } else {
      entry.attrs_size = (int32_t) c->blob;
      switch (pc->kind) {
      case MIZU_ARROW_CVT:
        if (pc->u64_verbatim) {
          /* uint64 verbatim: the identity i64 lanes (the excluded twin —
             an INT64_MIN pattern is a genuine value here), the u64
             marker the reader's unsigned interpretation */
          entry.sexptype = MIZU_TYPE_INT64;
          arrow_write_cvt(dst, a, i, &CVT_ROW_I64_V, warn);
          arrow_blob_u64(dst + c->body);
        } else {
          entry.sexptype = pc->row->wire;
          arrow_write_cvt(dst, a, i, pc->row, warn);
        }
        break;
      case MIZU_ARROW_STR:
        entry.sexptype = MIZU_TYPE_STR;
        arrow_write_str(dst, a, i, rows, c->nulls > 0);
        break;
      case MIZU_ARROW_DICT:
        entry.sexptype = MIZU_TYPE_INT;
        arrow_write_dict(dst, a, i);
        arrow_blob_factor(dst + c->body, a, i);
        break;
      case MIZU_ARROW_DATE:
        entry.sexptype = MIZU_TYPE_REAL;
        arrow_write_date(dst, a, i);
        arrow_blob_date(dst + c->body);
        break;
      case MIZU_ARROW_TS:
        if (pc->ts_verbatim) {
          /* the ns counts verbatim: the identity i64 row, NaT the INT64
             sentinel, the tscale pair the scale label */
          entry.sexptype = MIZU_TYPE_INT64;
          arrow_write_cvt(dst, a, i, &CVT_ROW_I64_V, warn);
          arrow_blob_ts(dst + c->body, c->tz, 1);
        } else {
          entry.sexptype = MIZU_TYPE_REAL;
          arrow_write_ts(dst, a, i);
          arrow_blob_ts(dst + c->body, c->tz, 0);
        }
        break;
      case MIZU_ARROW_TD:
        if (pc->ts_verbatim) {
          entry.sexptype = MIZU_TYPE_INT64;
          arrow_write_cvt(dst, a, i, &CVT_ROW_I64_V, warn);
          arrow_blob_difftime(dst + c->body, 1);
        } else {
          entry.sexptype = MIZU_TYPE_REAL;
          arrow_write_ts(dst, a, i);
          arrow_blob_difftime(dst + c->body, 0);
        }
        break;
      default:
        break;
      }
      entry.data_size = c->body + c->blob;
    }
    memcpy(base + MIZU_HEADER_SIZE + 32 * (size_t) i, &entry,
           sizeof(entry));
    cur += MIZU_ALIGN64((size_t) entry.data_size);
  }

  /* the root frame blob (names, class, row.names) — the override, or the
     automatic form */
  if (a->root_blob != NULL) {
    memcpy(base + cur, a->root_blob, a->root_blob_len);
  } else {
    arrow_blob_frame(base + cur, a->schema, ncols, rows);
  }
  cur += MIZU_ALIGN64(a->root_size);

  /* the validity tail: a bitmap per NA-ful local atomic leaf, then the
     table — a clean run collapses to the header's known-NA-free and no
     tail bytes, but only when every remote column claims known-NA-free
     too (the header never speaks for a remote column); a string leaf's
     nulls ride its block, its entry {0, 0} */
  int64_t total_nulls = 0;
  int any_remote_unknown = 0;
  for (int i = 0; i < ncols; i++) {
    const ptrdiff_t ti = 2 * (ptrdiff_t) i;
    if (a->remotes[i].used) {
      tab[ti] = 0;
      tab[ti + 1] = a->remotes[i].ref_claim;
      if (a->remotes[i].ref_claim != -1) any_remote_unknown = 1;
      continue;
    }
    if (a->cols[i].kind == MIZU_ARROW_STR) {
      tab[ti] = 0;
      tab[ti + 1] = 0;
      continue;
    }
    if (a->scols[i].nulls == 0) {
      tab[ti] = 0;
      tab[ti + 1] = -1;
      continue;
    }
    size_t off = MIZU_ALIGN64(cur);
    arrow_bitmap_write(base + off, a, i, rows);
    tab[ti] = (int64_t) off;
    tab[ti + 1] = a->scols[i].nulls;
    total_nulls += a->scols[i].nulls;
    cur = off + ((size_t) rows + 7) / 8;
  }
  if (total_nulls > 0 || any_remote_unknown) {
    size_t tab_off = MIZU_ALIGN64(cur);
    memcpy(base + tab_off, tab, 16 * (size_t) ncols);
    mizu_mizh_validity_set(base, (int64_t) tab_off, total_nulls);
  } else {
    mizu_mizh_validity_set(base, 0, -1);
  }

  {
    uint32_t magic = MIZU_MAGIC_LIST;
    int32_t n32 = (int32_t) ncols;
    int64_t ao64 = (int64_t) a->attrs_off;
    int64_t as64 = (int64_t) a->root_size;
    memcpy(base, &magic, 4);
    memcpy(base + 4, &n32, 4);
    memcpy(base + 8, &ao64, 8);
    memcpy(base + 16, &as64, 8);
  }
}

int64_t mizu_arrow_rows(const mizu_arrow *a) {
  return a->rows;
}

// The writer's public surface --------------------------------------------------------------

/* FNV-1a over the name bytes — the duplicate-name check's hash. */
static uint64_t arrow_name_hash(const char *s) {
  uint64_t h = 1469598103934665603ULL;
  for (; *s != '\0'; s++) {
    h ^= (uint8_t) *s;
    h *= 1099511628211ULL;
  }
  return h;
}

/* Every column named, no duplicates. Narrow frames use the stack; wide ones
   keep the open-addressed hash table (strcmp on a hash hit only). */
static mizu_status arrow_names_check(mizu_arrow *a) {
  enum { NARROW = 8, STACK_CAP = 16 };
  int32_t local_slots[STACK_CAP] = { 0 };
  uint64_t local_hash[NARROW];
  const size_t ncols = (size_t) a->ncols;
  size_t ncap = STACK_CAP;
  while (ncap < ncols * 2) ncap *= 2;
  const int heap = ncols > NARROW;
  int32_t *nslots = local_slots;
  uint64_t *nhash = local_hash;
  if (heap) {
    nslots = calloc(ncap, sizeof(*nslots));
    nhash = malloc(ncols * sizeof(*nhash));
    if (nslots == NULL || nhash == NULL) {
      free(nslots);
      free(nhash);
      mizu_err_record_tls(MIZU_ERRCAT_OTHER, "out of memory");
      return MIZU_ERR;
    }
  }
  mizu_status st = MIZU_OK;
  for (int i = 0; i < a->ncols; i++) {
    const char *nm = a->schema->children[i] != NULL ?
      a->schema->children[i]->name : NULL;
    if (nm == NULL) {
      st = arrow_decline("an unnamed Arrow column has no portable home "
                         "(an R data.frame needs names)");
      break;
    }
    const uint64_t h = arrow_name_hash(nm);
    nhash[i] = h;
    size_t s = (size_t) h & (ncap - 1);
    while (nslots[s] != 0) {
      const int j = nslots[s] - 1;
      if (nhash[j] == h &&
          strcmp(a->schema->children[j]->name, nm) == 0) {
        st = arrow_decline("an Arrow frame with duplicate column names");
        goto done;
      }
      s = (s + 1) & (ncap - 1);
    }
    nslots[s] = i + 1;
  }
done:
  if (heap) {
    free(nslots);
    free(nhash);
  }
  return st;
}

static mizu_status arrow_open(mizu_arrow **out, const ArrowSchema *schema,
                              const ArrowArray *const *batches,
                              size_t n_batches, uint32_t peer_caps, int frame,
                              int lazy) {
  *out = NULL;
  if (schema == NULL || schema->format == NULL ||
      strcmp(schema->format, "+s") != 0)
    return arrow_decline("an Arrow frame's root schema is not a struct "
                         "(a table, not an array)");
  if (schema->n_children < 1 || schema->children == NULL)
    return arrow_decline("a zero-column Arrow frame has no portable home");
  if (schema->n_children > INT32_MAX)
    return arrow_decline("an Arrow frame past the int32 column range has no "
                         "portable home");
  const size_t ncols = (size_t) schema->n_children;
  const size_t stride = sizeof(arrow_pcol) + sizeof(arrow_scol) +
    sizeof(arrow_remote) + 2 * sizeof(int64_t);
  if (ncols > (SIZE_MAX - sizeof(mizu_arrow)) / stride) {
    mizu_err_record_tls(MIZU_ERRCAT_OTHER, "out of memory");
    return MIZU_ERR;
  }
  size_t block = sizeof(mizu_arrow) + ncols * stride;
  if (n_batches != 0 &&
      ncols > (SIZE_MAX - block) / sizeof(int64_t) / n_batches) {
    mizu_err_record_tls(MIZU_ERRCAT_OTHER, "out of memory");
    return MIZU_ERR;
  }
  block += ncols * n_batches * sizeof(int64_t);
  /* One allocation: the handle, the per-column plans/sizes/remotes, the
     validity-table scratch, and the resolved per-batch null counts. Every
     member is 8-aligned by construction. */
  uint8_t *blk = calloc(1, block);
  if (blk == NULL) {
    mizu_err_record_tls(MIZU_ERRCAT_OTHER, "out of memory");
    return MIZU_ERR;
  }
  mizu_arrow *a = (mizu_arrow *) blk;
  a->schema = schema;
  a->batches = batches;
  a->nb = n_batches;
  a->peer_caps = peer_caps;
  a->ncols = (int) schema->n_children;
  a->cols = (arrow_pcol *) (blk + sizeof(*a));
  a->scols = (arrow_scol *)
    ((uint8_t *) a->cols + ncols * sizeof(*a->cols));
  a->remotes = (arrow_remote *)
    ((uint8_t *) a->scols + ncols * sizeof(*a->scols));
  a->tab = (int64_t *)
    ((uint8_t *) a->remotes + ncols * sizeof(*a->remotes));
  a->batch_nulls = a->tab + 2 * ncols;
  if (arrow_names_check(a) != MIZU_OK) goto fail;
  for (int i = 0; i < a->ncols; i++)
    if (schema->children[i] == NULL ||
        arrow_classify(a, i, schema->children[i]) < 0)
      goto fail;
  for (size_t b = 0; b < n_batches; b++) {
    const ArrowArray *ba = batches[b];
    if (ba == NULL || ba->n_children != schema->n_children ||
        ba->children == NULL) {
      mizu_arrow_close(a);
      return arrow_decline("an Arrow batch whose column count differs "
                           "from the schema");
    }
    if (arrow_check_batch(a, ba) < 0) goto fail;
    for (int i = 0; i < a->ncols; i++)
      a->batch_nulls[(size_t) i * n_batches + b] =
        arrow_batch_nulls(ba->children[i]);
    a->rows += ba->length;
  }
  /* the root dict's automatic row.names are the compact c(NA, -n) int32
     form — past 2^31 rows the emit would wrap, and the inline 'I' form
     has no root-blob override to escape it */
  if (frame && a->rows > INT32_MAX) {
    mizu_arrow_close(a);
    return arrow_decline("an Arrow frame past the int32 row range has no "
                         "portable home");
  }
  if (arrow_validate_dicts(a) < 0) goto fail;
  /* the string walks: the packed byte totals, and the one malformed-
     record check ahead of any emit (a lazy open leaves them to
     mizu_arrow_validate, past the caller's remote overrides) */
  for (int i = 0; i < a->ncols; i++) {
    a->any_str |= a->cols[i].kind == MIZU_ARROW_STR;
    if (!lazy && a->cols[i].kind == MIZU_ARROW_STR &&
        arrow_size_str(a, i) < 0)
      goto fail;
  }
  *out = a;
  return MIZU_OK;
fail:
  mizu_arrow_close(a);
  return MIZU_ERR;
}

mizu_status mizu_arrow_open(mizu_arrow **out, const ArrowSchema *schema,
                            const ArrowArray *const *batches,
                            size_t n_batches, uint32_t peer_caps) {
  return arrow_open(out, schema, batches, n_batches, peer_caps, 1, 0);
}

mizu_status mizu_arrow_open_lazy(mizu_arrow **out, const ArrowSchema *schema,
                                 const ArrowArray *const *batches,
                                 size_t n_batches, uint32_t peer_caps) {
  return arrow_open(out, schema, batches, n_batches, peer_caps, 1, 1);
}

mizu_status mizu_arrow_validate(mizu_arrow *a) {
  for (int i = 0; i < a->ncols; i++) {
    const arrow_pcol *pc = &a->cols[i];
    if (pc->kind != MIZU_ARROW_STR || pc->str_walked || a->remotes[i].used)
      continue;
    if (arrow_size_str(a, i) < 0) return MIZU_ERR;
    a->sized = 0;
  }
  return MIZU_OK;
}

size_t mizu_arrow_size_floor(const mizu_arrow *a) {
  const size_t rows = (size_t) a->rows;
  size_t cur = MIZU_HEADER_SIZE + 32 * (size_t) a->ncols;
  for (int i = 0; i < a->ncols; i++) {
    const arrow_pcol *pc = &a->cols[i];
    switch (pc->kind) {
    case MIZU_ARROW_CVT:
      cur += rows * (size_t) pc->row->w_out;
      break;
    case MIZU_ARROW_STR:
      cur += (size_t) mizu_mizs_geometry(a->rows).data;
      break;
    case MIZU_ARROW_DICT:
      cur += rows * 4;
      break;
    default:
      cur += rows * 8;
      break;
    }
  }
  return cur;
}

mizu_status mizu_arrow_open_vector(mizu_arrow **out, const ArrowSchema *schema,
                                   const ArrowArray *const *batches,
                                   size_t n_batches, uint32_t peer_caps) {
  return arrow_open(out, schema, batches, n_batches, peer_caps, 0, 0);
}

void mizu_arrow_col_info(const mizu_arrow *a, int64_t col,
                         mizu_arrow_col *out) {
  const arrow_pcol *pc = &a->cols[col];
  memset(out, 0, sizeof(*out));
  out->kind = pc->kind;
  out->row = pc->row;
  out->str_form = pc->str_form;
  out->u64_verbatim = pc->u64_verbatim;
}

size_t mizu_arrow_col_body(const mizu_arrow *a, int64_t col, uint8_t *dst,
                           mizu_arrow_warn *warn) {
  if (col < 0 || col >= a->ncols) return 0;
  const arrow_pcol *pc = &a->cols[col];
  const int64_t rows = a->rows;
  switch (pc->kind) {
  case MIZU_ARROW_CVT:
    if (dst != NULL) arrow_write_cvt(dst, a, (int) col, pc->row, warn);
    return (size_t) rows * pc->row->w_out;
  case MIZU_ARROW_DICT:
    if (dst != NULL) arrow_write_dict(dst, a, (int) col);
    return (size_t) rows * 4;
  case MIZU_ARROW_DATE:
    if (dst != NULL) arrow_write_date(dst, a, (int) col);
    return (size_t) rows * 8;
  case MIZU_ARROW_TS:
  case MIZU_ARROW_TD:
    if (dst != NULL) arrow_write_ts(dst, a, (int) col);
    return (size_t) rows * 8;
  default:
    /* STR: the 'I' string form is the binding's per-element emitter
       (mizu_arrow_str_at), the MIZS block region-only */
    return 0;
  }
}

size_t mizu_arrow_col_blob(const mizu_arrow *a, int64_t col, uint8_t *dst) {
  if (col < 0 || col >= a->ncols) return 0;
  const arrow_pcol *pc = &a->cols[col];
  switch (pc->kind) {
  case MIZU_ARROW_DICT:
    return arrow_factor_dict(dst, a, (int) col);
  case MIZU_ARROW_DATE:
    return arrow_date_dict(dst);
  case MIZU_ARROW_TS:
    return arrow_ts_dict(dst, pc->tz_o != NULL ? pc->tz_o : pc->tz, 0);
  case MIZU_ARROW_TD:
    return arrow_difftime_dict(dst, 0);
  default:
    /* CVT / STR: no attribute dict on the by-value tiers */
    return 0;
  }
}

/* One column's 'I' value form (the small-table path): the plain vectors
   bare, the attr-marked forms splicing mizu_arrow_col_blob's bare dict
   (it emits the dict without the stream header — exactly the inline attr
   shape). A string column walks its elements through mizu_arrow_str_at,
   a malformed record put as NA like a null (every record was validated
   at open, so == 1 in practice — the put never sees a garbage span).
   The MIZL-only leaves (the ns INT64 counts, the u64 lanes) never appear
   here: the body writers are the by-value forms. */
static size_t arrow_ix_col(const mizu_arrow *a, int64_t col, uint8_t *dst,
                           mizu_arrow_warn *warn) {
  const arrow_pcol *pc = &a->cols[col];
  const uint64_t rows = (uint64_t) a->rows;
  arrow_ixe e = { dst, 0 };
  switch (pc->kind) {
  case MIZU_ARROW_CVT:
    ARROW_IXE_PUT(&e, arrow_ixe_vec_begin(ARROW_IXE_DST(&e),
                                          mizu_ix_tag_of(pc->row->wire),
                                          rows));
    e.total += mizu_arrow_col_body(a, col, ARROW_IXE_DST(&e), warn);
    return e.total;
  case MIZU_ARROW_STR:
    ARROW_IXE_PUT(&e, mizu_ix_put_strv_begin(ARROW_IXE_DST(&e), rows));
    for (size_t b = 0; b < a->nb; b++) {
      const ArrowArray *ba = a->batches[b]->children[col];
      for (int64_t k = 0; k < ba->length; k++) {
        const uint8_t *s;
        int32_t len;
        if (mizu_arrow_str_at(ba, k, pc->str_form, &s, &len) != 1) {
          ARROW_IXE_PUT(&e, mizu_ix_put_strelt(ARROW_IXE_DST(&e), NULL, -1));
        } else {
          ARROW_IXE_PUT(&e, mizu_ix_put_strelt(ARROW_IXE_DST(&e), s, len));
        }
      }
    }
    return e.total;
  default:
    break;
  }
  /* the attr-marked forms (CVT and STR returned above): the 1-based codes
     an intv, the days/seconds a realv, then the bare dict */
  const int tag = pc->kind == MIZU_ARROW_DICT ? MIZU_IX_TAG_INTV :
    MIZU_IX_TAG_REALV;
  ARROW_IXE_PUT(&e, mizu_ix_put_attr(ARROW_IXE_DST(&e)));
  ARROW_IXE_PUT(&e, arrow_ixe_vec_begin(ARROW_IXE_DST(&e), tag, rows));
  e.total += mizu_arrow_col_body(a, col, ARROW_IXE_DST(&e), warn);
  e.total += mizu_arrow_col_blob(a, col, ARROW_IXE_DST(&e));
  return e.total;
}

size_t mizu_arrow_write_ix_col(const mizu_arrow *a, int64_t col,
                               uint8_t *dst, mizu_arrow_warn *warn) {
  if (col < 0 || col >= a->ncols) return 0;
  return arrow_ix_col(a, col, dst, warn);
}

size_t mizu_arrow_write_ix(const mizu_arrow *a, uint8_t *dst,
                           mizu_arrow_warn *warn) {
  arrow_ixe e = { dst, 0 };
  ARROW_IXE_PUT(&e, mizu_ix_put_header(ARROW_IXE_DST(&e)));
  ARROW_IXE_PUT(&e, mizu_ix_put_attr(ARROW_IXE_DST(&e)));
  ARROW_IXE_PUT(&e, mizu_ix_put_list_begin(ARROW_IXE_DST(&e),
                                           (uint64_t) a->ncols));
  for (int64_t i = 0; i < a->ncols; i++)
    e.total += arrow_ix_col(a, i, ARROW_IXE_DST(&e), warn);
  /* the root dict's automatic form; the root-blob override is the region
     writer's and does not apply to the inline form */
  e.total += arrow_frame_dict(ARROW_IXE_DST(&e), a->schema, a->ncols,
                              a->rows);
  return e.total;
}

int mizu_arrow_remote(mizu_arrow *a, int64_t col, const char *ident,
                      size_t ident_len, void *ref_base, size_t ref_size,
                      int64_t ref_leaf) {
  if (col < 0 || col >= a->ncols || ident == NULL || ident_len == 0 ||
      ident_len > MIZU_NAME_MAX || ref_base == NULL)
    return -1;
  mizu_mizl_entry re;
  if (mizu_mizl_elem(ref_base, ref_size, ref_leaf, &re) != 0) return -1;
  /* a length mismatch would write a tag-33 entry whose claim fails the
     reader's resolve — decline the override instead, the column stays
     local (the torn-tree precedent) */
  if (re.length != a->rows) return -1;
  const int32_t rtag = re.sexptype & ~(int32_t) MIZU_MIZL_S4;
  arrow_remote *r = &a->remotes[col];
  memcpy(r->span, ident, ident_len);
  r->span_len = ident_len + (size_t) snprintf(r->span + ident_len, 14,
                                              "[%lld]",
                                              (long long) (ref_leaf + 1));
  r->ref_base = ref_base;
  r->ref_attrs = re.attrs_size;
  r->ref_claim = mizu_type_elt_size(rtag) != 0 &&
    re.valid[0] == 0 && re.valid[1] == -1 ? -1 : 0;
  r->used = 1;
  a->sized = 0;   /* the override moves the span: re-run the size pass */
  return 0;
}

void mizu_arrow_root_blob(mizu_arrow *a, const void *blob, size_t len) {
  a->root_blob = blob;
  a->root_blob_len = len;
  a->sized = 0;
}

void mizu_arrow_tz(mizu_arrow *a, int64_t col, const char *tz) {
  if (col < 0 || col >= a->ncols) return;
  a->cols[col].tz_o = tz;
  a->sized = 0;
}

size_t mizu_arrow_size(mizu_arrow *a) {
  if (!a->sized) arrow_size_pass(a);
  return a->total;
}

mizu_status mizu_arrow_stage(mizu_handle *h, mizu_arrow *a,
                             mizu_slot_hdr *hdr, unsigned char *payload,
                             mizu_arrow_warn *warn) {
  /* the gate, touching no shared state */
  const uint32_t need = MIZU_CAP_MIZL | MIZU_CAP_ATTRS |
    (a->any_str ? MIZU_CAP_MIZS : 0u);
  if ((a->peer_caps & need) != need) {
    mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                        "the peer lacks the frame capability conjunction");
    return MIZU_ERR;
  }
  if (mizu_handle_churn(h)) {
    mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                        "the spill churn signal is raised");
    return MIZU_ERR;
  }
  /* a lazy open's pending string walks (a no-op past an eager open or an
     explicit validate): the size pass needs every local byte total */
  if (mizu_arrow_validate(a) != MIZU_OK) return MIZU_ERR;
  if (!a->sized) arrow_size_pass(a);
  mizu_stage_reap(h);
  mizu_shm *shm;
  if (mizu_stage_spill_get(h, a->total, &shm) != MIZU_OK) return MIZU_ERR;
  uint8_t *base = (uint8_t *) shm->addr;
  memset(warn, 0, sizeof(*warn));
  mizu_arrow_write(a, base, warn);
  hdr->kind = MIZU_KIND_SHM_VEC;
  hdr->len = (uint32_t) shm->name_len;
  hdr->aux = mizu_aux_shm_vec(MIZU_TYPE_VEC, (uint64_t) a->total);
  memcpy(payload, shm->name, shm->name_len);
  mizu_stage_retain_zc(h, shm);
  return MIZU_OK;
}

void mizu_arrow_close(mizu_arrow *a) {
  free(a);   /* the plans, sizes, remotes, tab and null counts ride it */
}
