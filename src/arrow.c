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
   ordered dictionary declines). date32 / timestamp / duration cross as
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

/* A batch's exact null count (Arrow's -1 "not computed" resolved). */
static int64_t arrow_batch_nulls(const ArrowArray *a) {
  if (a->null_count == 0 || a->buffers[0] == NULL) return 0;
  if (a->null_count > 0) return a->null_count;
  const uint8_t *bm = (const uint8_t *) a->buffers[0];
  int64_t n = 0;
  for (int64_t k = 0; k < a->length; k++) {
    int64_t i = a->offset + k;
    n += !((bm[i >> 3] >> (i & 7)) & 1);
  }
  return n;
}

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

/* Convert a run of n valid elements. Per-element memcpy keeps every
   access alignment-safe (a contiguous buffer can still be
   element-misaligned — np.frombuffer with a byte offset); fixed-size
   memcpys compile to single load/store pairs. */
static void cvt_run(uint8_t *dst, const uint8_t *src, size_t n,
                    const mizu_cvt_row *row, mizu_arrow_warn *warn) {
  switch (row->cvt) {
  case MIZU_CVT_COPY:
    memcpy(dst, src, n * row->w_out);
    break;
  case MIZU_CVT_I8_INT:
    for (size_t i = 0; i < n; i++) {
      int8_t v;
      memcpy(&v, src + i, 1);
      int32_t o = (int32_t) v;
      memcpy(dst + 4 * i, &o, 4);
    }
    break;
  case MIZU_CVT_I16_INT:
    for (size_t i = 0; i < n; i++) {
      int16_t v;
      memcpy(&v, src + 2 * i, 2);
      int32_t o = v;
      memcpy(dst + 4 * i, &o, 4);
    }
    break;
  case MIZU_CVT_U16_INT:
    for (size_t i = 0; i < n; i++) {
      uint16_t v;
      memcpy(&v, src + 2 * i, 2);
      int32_t o = v;
      memcpy(dst + 4 * i, &o, 4);
    }
    break;
  case MIZU_CVT_U32_REAL:
    for (size_t i = 0; i < n; i++) {
      uint32_t v;
      memcpy(&v, src + 4 * i, 4);
      double o = (double) v;
      memcpy(dst + 8 * i, &o, 8);
    }
    break;
  case MIZU_CVT_U64_REAL:
    for (size_t i = 0; i < n; i++) {
      uint64_t v;
      memcpy(&v, src + 8 * i, 8);
      if (v > 9007199254740992ULL) {
        mizu_store_na_real(dst + 8 * i);
        warn->n_range++;
      } else {
        double o = (double) v;
        memcpy(dst + 8 * i, &o, 8);
      }
    }
    break;
  case MIZU_CVT_F16_REAL:
    for (size_t i = 0; i < n; i++) {
      uint16_t v;
      memcpy(&v, src + 2 * i, 2);
      double o = half_to_double(v);
      memcpy(dst + 8 * i, &o, 8);
    }
    break;
  case MIZU_CVT_F32_REAL:
    for (size_t i = 0; i < n; i++) {
      float v;
      memcpy(&v, src + 4 * i, 4);
      double o = v;
      memcpy(dst + 8 * i, &o, 8);
    }
    break;
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

/* Fill n lanes with the wire type's missing sentinel. */
static void cvt_fill_na(uint8_t *dst, size_t n, int wire) {
  if (wire == MIZU_TYPE_REAL) {
    for (size_t i = 0; i < n; i++) mizu_store_na_real(dst + 8 * i);
  } else if (wire == MIZU_TYPE_INT64) {
    const int64_t na = MIZU_NA_INT64;   /* reads as NA_integer64_ in R */
    for (size_t i = 0; i < n; i++) memcpy(dst + 8 * i, &na, 8);
  } else {
    const int32_t na = MIZU_NA_INT32;   /* INT and LGL share the sentinel */
    for (size_t i = 0; i < n; i++) memcpy(dst + 4 * i, &na, 4);
  }
}

/* Load lanes (<= 64) validity bits starting at bit position pos (Arrow
   LSB-first bit order), reading only the bytes that cover them. */
static uint64_t bitmap_word(const uint8_t *bm, uint64_t pos, size_t lanes) {
  const uint8_t *p = bm + (pos >> 3);
  unsigned sh = (unsigned) (pos & 7);
  if (sh == 0 && lanes == 64) {
    uint64_t w;
    memcpy(&w, p, 8);
    return w;
  }
  size_t nbytes = (sh + lanes + 7) >> 3;
  uint64_t lo = 0, hi = 0;
  for (size_t j = 0; j < nbytes; j++) {
    if (j < 8) lo |= (uint64_t) p[j] << (8 * j);
    else hi = p[j];   /* j == 8: only when sh > 0 and lanes == 64 */
  }
  uint64_t w = sh != 0 ? (lo >> sh) | (hi << (64 - sh)) : lo;
  if (lanes < 64) w &= ((uint64_t) 1 << lanes) - 1;
  return w;
}

/* The verbatim rows share the identity i64 row's contents: the pointer
   compare keys them out of the n_int64 count (a cap-peer reads their
   lanes exactly, so an INT64_MIN pattern there is a genuine value). */
static const mizu_cvt_row CVT_ROW_I64_V;

/* The masked variant (Arrow nulls): word-wise over the validity bitmap —
   an all-ones word a 64-lane convert, a zero word a 64-sentinel fill,
   mixed the per-lane select. off is the element offset: a bit offset
   into the bitmap (the word reads absorb it). Genuine sentinel values
   are counted on the int32 and int64 identity rows (they read as
   NA_integer_ / NA_integer64_ in R); the other rows cannot produce one. */
static void convert_masked(uint8_t *dst, const uint8_t *src,
                           const uint8_t *valid, uint64_t off, size_t n,
                           const mizu_cvt_row *row, mizu_arrow_warn *warn) {
  const size_t wo = row->w_out, wi = row->w_in;
  const int count_min = row->wire == MIZU_TYPE_INT &&
    row->cvt == MIZU_CVT_COPY;
  const int count_min64 = row->wire == MIZU_TYPE_INT64 &&
    row->cvt == MIZU_CVT_COPY && row != &CVT_ROW_I64_V;
  size_t i = 0;
  while (i < n) {
    size_t lanes = n - i < 64 ? n - i : 64;
    uint64_t w = bitmap_word(valid, off + i, lanes);
    uint64_t full = lanes == 64 ? UINT64_MAX : ((uint64_t) 1 << lanes) - 1;
    if (w == full) {
      if (count_min) {
        /* copy, then scan the destination (dst is int32-aligned on every
           tier: the inline payload is 16-byte aligned, arena chunks and
           region data areas 64). The count rides a local — a warn->
           store per lane would alias-block vectorization */
        cvt_run(dst + i * wo, src + i * wi, lanes, row, warn);
        const int32_t *d = (const int32_t *) (dst + i * wo);
        uint64_t cnt = 0;
        for (size_t j = 0; j < lanes; j++) cnt += d[j] == INT32_MIN;
        warn->n_intmin += cnt;
      } else if (count_min64) {
        cvt_run(dst + i * wo, src + i * wi, lanes, row, warn);
        const int64_t *d = (const int64_t *) (dst + i * wo);
        uint64_t cnt = 0;
        for (size_t j = 0; j < lanes; j++) cnt += d[j] == INT64_MIN;
        warn->n_int64 += cnt;
      } else {
        cvt_run(dst + i * wo, src + i * wi, lanes, row, warn);
      }
    } else if (w == 0) {
      cvt_fill_na(dst + i * wo, lanes, row->wire);
    } else {
      for (size_t j = 0; j < lanes; j++) {
        if ((w >> j) & 1) {
          cvt_run(dst + (i + j) * wo, src + (i + j) * wi, 1, row, warn);
          if (count_min) {
            int32_t v;
            memcpy(&v, dst + (i + j) * wo, 4);
            warn->n_intmin += v == INT32_MIN;
          } else if (count_min64) {
            int64_t v;
            memcpy(&v, dst + (i + j) * wo, 8);
            warn->n_int64 += v == INT64_MIN;
          }
        } else {
          cvt_fill_na(dst + (i + j) * wo, 1, row->wire);
        }
      }
    }
    i += lanes;
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
      (lanes == 64 ? UINT64_MAX : ((uint64_t) 1 << lanes) - 1);
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
static const mizu_cvt_row CVT_ROW_U64 = { MIZU_TYPE_REAL, MIZU_CVT_U64_REAL, 8, 8 };
static const mizu_cvt_row CVT_ROW_F16 = { MIZU_TYPE_REAL, MIZU_CVT_F16_REAL, 2, 8 };
static const mizu_cvt_row CVT_ROW_F32 = { MIZU_TYPE_REAL, MIZU_CVT_F32_REAL, 4, 8 };
static const mizu_cvt_row CVT_ROW_F64 = { MIZU_TYPE_REAL, MIZU_CVT_COPY, 8, 8 };
static const mizu_cvt_row CVT_ROW_BOOLBIT = { MIZU_TYPE_LGL, MIZU_CVT_BIT_LGL, 0, 4 };

/* Key the table by an Arrow C Data Interface format string (every
   supported row is a single character). */
const mizu_cvt_row *mizu_cvt_for_arrow(const char *f) {
  if (f[1] != '\0') return NULL;
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

/* An intv value, or the 0x02 scalar at length 1. Vector elements must fit
   int32 (the callers' contract); the scalar form keeps the full i64. */
static void arrow_ixe_intv(arrow_ixe *e, int64_t n, const int64_t *vals,
                           const int *na) {
  if (n == 1) {
    int64_t v = na != NULL && na[0] ? INT64_MIN : vals[0];
    ARROW_IXE_PUT(e, mizu_ix_put_int(ARROW_IXE_DST(e), v));
    return;
  }
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
  double ts_scale;      /* TS/TD */
  char unit;            /* TS/TD: the Arrow unit s/m/u/n */
  int ts_verbatim;      /* TS/TD: the ns INT64 leaf verdict (unit n and the
                           peer reads MIZU_CAP_MIZL_TSN), resolved at
                           classify */
  char tz[64];          /* TS: the classified tzone ("UTC" when naive) */
  const char *tz_o;     /* TS: the tzone override (NULL: the classified) */
  int idx_w;            /* DICT: the index width */
  int idx_signed;       /* DICT */
  int64_t nlev;         /* DICT: the canonical level count (batch 0's) */
  int64_t str_bytes;    /* STR: the packed byte total (the open's walk) */
  int u64_verbatim;     /* CVT 'L': the MIZU_CAP_MIZL_U64V verdict,
                           resolved at classify */
} arrow_pcol;

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
  const void *root_blob; /* the root-blob override (borrowed) */
  size_t root_blob_len;
  int any_remote;
  int any_str;
  int sized;
  size_t attrs_off;
  size_t root_size;      /* the root blob's byte count */
  size_t total;
};


/* The factor dict {levels, class = "factor"} — the inline attr shape's
   form exactly. The levels walk batch 0's dictionary (validated at open:
   every record resolves, so the arrow_str_at verdict is exactly == 1);
   a zero-batch column writes the empty strv. */
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
    arrow_ixe_class(&e, cls, 1);
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
      mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                          "column '%s': an ordered dictionary column has no "
                          "portable home (polars Enum: cast to "
                          "pl.Categorical)", nm);
      return -1;
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
  if (f[0] == 't' && f[1] == 's' && f[3] == ':' &&
      (f[2] == 's' || f[2] == 'm' || f[2] == 'u' || f[2] == 'n')) {
    pc->ts_scale = f[2] == 's' ? 1.0 : f[2] == 'm' ? 1e-3 :
      f[2] == 'u' ? 1e-6 : 1e-9;
    pc->unit = f[2];
    pc->ts_verbatim = f[2] == 'n' &&
      (a->peer_caps & MIZU_CAP_MIZL_TSN) != 0;
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
  if (f[0] == 't' && f[1] == 'D' && f[3] == '\0' &&
      (f[2] == 's' || f[2] == 'm' || f[2] == 'u' || f[2] == 'n')) {
    pc->ts_scale = f[2] == 's' ? 1.0 : f[2] == 'm' ? 1e-3 :
      f[2] == 'u' ? 1e-6 : 1e-9;
    pc->unit = f[2];
    pc->ts_verbatim = f[2] == 'n' &&
      (a->peer_caps & MIZU_CAP_MIZL_TSN) != 0;
    pc->kind = MIZU_ARROW_TD;
    return 0;
  }
  if (f[0] == 'L' && f[1] == '\0') {
    /* uint64: verbatim (an INT64 leaf + the u64 blob attr) for a peer
       advertising MIZU_CAP_MIZL_U64V, else the REAL conversion */
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

/* One batch's shape checks. 0 vetted, -1 with the decline in the
   thread-local slot. */
static int arrow_check_batch(mizu_arrow *a, const ArrowArray *b) {
  for (int i = 0; i < a->ncols; i++) {
    const ArrowArray *c = b->children[i];
    arrow_pcol *pc = &a->cols[i];
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
    if (pc->kind == MIZU_ARROW_DICT && c->dictionary == NULL) {
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
  const ArrowArray *dict = a->batches[0]->children[col]->dictionary;
  if (dict == NULL || dict->n_buffers < 3 || dict->buffers == NULL ||
      (dict->length > 0 &&
       (dict->buffers[1] == NULL || dict->buffers[2] == NULL))) {
    mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                        "column '%s': an invalid Arrow dictionary export", nm);
    return -1;
  }
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
    for (size_t b = 0; b < a->nb; b++) {
      const ArrowArray *ba = a->batches[b]->children[i];
      if (b != 0 && !arrow_levels_match(a, i, ba->dictionary)) {
        mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                            "column '%s': a dictionary column whose levels "
                            "differ across batches", nm);
        return -1;
      }
      const uint8_t *idx = (const uint8_t *) ba->buffers[1];
      for (int64_t k = 0; k < ba->length; k++) {
        if (!mizu_arrow_valid(ba, k)) continue;
        int64_t j = ba->offset + k, c;
        switch (pc->idx_w) {
        case 1:
          c = pc->idx_signed ? (int64_t) ((const int8_t *) idx)[j] :
            (int64_t) idx[j];
          break;
        case 2:
          c = pc->idx_signed ? (int64_t) ((const int16_t *) idx)[j] :
            (int64_t) ((const uint16_t *) idx)[j];
          break;
        case 4:
          c = pc->idx_signed ? (int64_t) ((const int32_t *) idx)[j] :
            (int64_t) ((const uint32_t *) idx)[j];
          break;
        default:
          c = ((const int64_t *) idx)[j];
          break;
        }
        if (c < 0 || c >= pc->nlev) {
          mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                              "column '%s': a dictionary index outside the "
                              "levels", nm);
          return -1;
        }
      }
    }
  }
  return 0;
}

// The size pass -------------------------------------------------------------------------

/* The packed string byte total: per batch the offset span when no nulls
   (a null's span must collapse, so a batch with nulls walks). Runs at
   open: a malformed record declines there, ahead of any emit. */
static int arrow_size_str(mizu_arrow *a, int col) {
  arrow_pcol *pc = &a->cols[col];
  int64_t bytes = 0;
  for (size_t b = 0; b < a->nb; b++) {
    const ArrowArray *ba = a->batches[b]->children[col];
    if (ba->length == 0) continue;
    if (pc->str_form != 3 && arrow_batch_nulls(ba) == 0) {
      if (pc->str_form == 1) {
        const int32_t *offs = (const int32_t *) ba->buffers[1];
        int32_t o0 = offs[ba->offset], o1 = offs[ba->offset + ba->length];
        if (o0 < 0 || o1 < o0) goto malformed;
        bytes += o1 - o0;
      } else {
        const int64_t *offs = (const int64_t *) ba->buffers[1];
        int64_t o0 = offs[ba->offset], o1 = offs[ba->offset + ba->length];
        if (o0 < 0 || o1 < o0) goto malformed;
        bytes += o1 - o0;
      }
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
  return 0;
malformed:
  mizu_err_record_tls(MIZU_ERRCAT_OTHER,
                      "column '%s': an invalid Arrow string export (a "
                      "malformed offset or view record)",
                      a->schema->children[col]->name);
  return -1;
}

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
    c->nulls += arrow_batch_nulls(a->batches[b]->children[col]);
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
    const uint8_t *valid = ba->null_count != 0 ?
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
      if (pc->str_form == 1) {
        const int32_t *so = (const int32_t *) ba->buffers[1];
        int32_t base0 = so[ba->offset];
        for (int64_t k = 0; k < ba->length; k++)
          offs[done + k + 1] = run + (so[ba->offset + k + 1] - base0);
        int32_t blen = so[ba->offset + ba->length] - base0;
        memcpy(data + run, (const uint8_t *) ba->buffers[2] + base0,
               (size_t) blen);
        run += blen;
      } else {
        const int64_t *so = (const int64_t *) ba->buffers[1];
        int64_t base0 = so[ba->offset];
        for (int64_t k = 0; k < ba->length; k++)
          offs[done + k + 1] = run + (so[ba->offset + k + 1] - base0);
        int64_t blen = so[ba->offset + ba->length] - base0;
        memcpy(data + run, (const uint8_t *) ba->buffers[2] + base0,
               (size_t) blen);
        run += blen;
      }
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

/* A dictionary column: the INT32 leaf of 1-based R codes (NA = INT_MIN). */
static void arrow_write_dict(uint8_t *dst, const mizu_arrow *a, int col) {
  const arrow_pcol *pc = &a->cols[col];
  int64_t done = 0;
  for (size_t b = 0; b < a->nb; b++) {
    const ArrowArray *ba = a->batches[b]->children[col];
    const uint8_t *idx = (const uint8_t *) ba->buffers[1];
    for (int64_t k = 0; k < ba->length; k++) {
      int32_t v;
      if (!mizu_arrow_valid(ba, k)) {
        v = MIZU_NA_INT32;
      } else {
        int64_t i = ba->offset + k, c;
        switch (pc->idx_w) {
        case 1:
          c = pc->idx_signed ? (int64_t) ((const int8_t *) idx)[i] :
            (int64_t) idx[i];
          break;
        case 2:
          c = pc->idx_signed ? (int64_t) ((const int16_t *) idx)[i] :
            (int64_t) ((const uint16_t *) idx)[i];
          break;
        case 4:
          c = pc->idx_signed ? (int64_t) ((const int32_t *) idx)[i] :
            (int64_t) ((const uint32_t *) idx)[i];
          break;
        default:
          c = ((const int64_t *) idx)[i];
          break;
        }
        v = (int32_t) (c + 1);
      }
      memcpy(dst + 4 * (done + k), &v, 4);
    }
    done += ba->length;
  }
}

/* A date32 column: the REAL leaf of epoch-day doubles (NaT = NA payload). */
static void arrow_write_date(uint8_t *dst, const mizu_arrow *a, int col) {
  int64_t done = 0;
  for (size_t b = 0; b < a->nb; b++) {
    const ArrowArray *ba = a->batches[b]->children[col];
    const int32_t *days = (const int32_t *) ba->buffers[1];
    for (int64_t k = 0; k < ba->length; k++) {
      if (!mizu_arrow_valid(ba, k)) {
        arrow_store_na_r(dst + 8 * (done + k));
      } else {
        double v = (double) days[ba->offset + k];
        memcpy(dst + 8 * (done + k), &v, 8);
      }
    }
    done += ba->length;
  }
}

/* A timestamp column: the REAL leaf of epoch-second doubles. */
static void arrow_write_ts(uint8_t *dst, const mizu_arrow *a, int col) {
  const arrow_pcol *pc = &a->cols[col];
  int64_t done = 0;
  for (size_t b = 0; b < a->nb; b++) {
    const ArrowArray *ba = a->batches[b]->children[col];
    const int64_t *counts = (const int64_t *) ba->buffers[1];
    for (int64_t k = 0; k < ba->length; k++) {
      if (!mizu_arrow_valid(ba, k)) {
        arrow_store_na_r(dst + 8 * (done + k));
      } else {
        double v = (double) counts[ba->offset + k] * pc->ts_scale;
        memcpy(dst + 8 * (done + k), &v, 8);
      }
    }
    done += ba->length;
  }
}

/* One leaf's validity bitmap into the section: verbatim for a single
   offset-0 batch, else memset-present and clear at the null positions. */
static void arrow_bitmap_write(uint8_t *dst, mizu_arrow *a, int col,
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
    if (ba->null_count != 0 && ba->buffers[0] != NULL) {
      for (int64_t k = 0; k < ba->length; k++) {
        if (!mizu_arrow_valid(ba, k)) {
          int64_t i = done + k;
          dst[i >> 3] &= (uint8_t) ~(1u << (i & 7));
        }
      }
    }
    done += ba->length;
  }
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
  memset(base + 24, 0, MIZU_HEADER_SIZE - 24);
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

  /* the header */
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

mizu_status mizu_arrow_open(mizu_arrow **out, const ArrowSchema *schema,
                            const ArrowArray *const *batches,
                            size_t n_batches, uint32_t peer_caps) {
  *out = NULL;
  if (schema == NULL || schema->format == NULL ||
      strcmp(schema->format, "+s") != 0)
    return arrow_decline("an Arrow frame's root schema is not a struct "
                         "(a table, not an array)");
  if (schema->n_children < 1 || schema->children == NULL)
    return arrow_decline("a zero-column Arrow frame has no portable home");
  mizu_arrow *a = calloc(1, sizeof(*a));
  if (a == NULL) {
    mizu_err_record_tls(MIZU_ERRCAT_OTHER, "out of memory");
    return MIZU_ERR;
  }
  a->schema = schema;
  a->batches = batches;
  a->nb = n_batches;
  a->peer_caps = peer_caps;
  a->ncols = (int) schema->n_children;
  /* one block (cols is the base, freed at close): the per-column plans and
     sizes, the remote-leaf overrides, the validity-table scratch — every
     member 8-aligned by construction (the structs pad to 8, tab is i64) */
  const size_t ncolsa = (size_t) a->ncols;
  uint8_t *blk = calloc(ncolsa, sizeof(*a->cols) + sizeof(*a->scols) +
                        sizeof(*a->remotes) + 2 * sizeof(*a->tab));
  if (blk == NULL) {
    mizu_arrow_close(a);
    mizu_err_record_tls(MIZU_ERRCAT_OTHER, "out of memory");
    return MIZU_ERR;
  }
  a->cols = (arrow_pcol *) blk;
  a->scols = (arrow_scol *) (blk + ncolsa * sizeof(*a->cols));
  a->remotes = (arrow_remote *) (blk + ncolsa * (sizeof(*a->cols) +
                                                 sizeof(*a->scols)));
  a->tab = (int64_t *) (blk + ncolsa * (sizeof(*a->cols) +
                                        sizeof(*a->scols) +
                                        sizeof(*a->remotes)));
  /* the names: every column named, no duplicates — an open-addressed
     table over the name hashes (strcmp on a hash hit only), not the
     quadratic pair scan a wide frame would pay per send */
  size_t ncap = 16;
  while (ncap < ncolsa * 2) ncap *= 2;
  int32_t *nslots = calloc(ncap, sizeof(*nslots));
  uint64_t *nhash = malloc(ncolsa * sizeof(*nhash));
  if (nslots == NULL || nhash == NULL) {
    free(nslots);
    free(nhash);
    mizu_arrow_close(a);
    mizu_err_record_tls(MIZU_ERRCAT_OTHER, "out of memory");
    return MIZU_ERR;
  }
  for (int i = 0; i < a->ncols; i++) {
    const char *nm = schema->children[i] != NULL ?
      schema->children[i]->name : NULL;
    if (nm == NULL) {
      free(nslots);
      free(nhash);
      mizu_arrow_close(a);
      return arrow_decline("an unnamed Arrow column has no portable home "
                           "(an R data.frame needs names)");
    }
    const uint64_t h = arrow_name_hash(nm);
    nhash[i] = h;
    size_t s = (size_t) h & (ncap - 1);
    while (nslots[s] != 0) {
      const int j = nslots[s] - 1;
      if (nhash[j] == h && strcmp(schema->children[j]->name, nm) == 0) {
        free(nslots);
        free(nhash);
        mizu_arrow_close(a);
        return arrow_decline("an Arrow frame with duplicate column names");
      }
      s = (s + 1) & (ncap - 1);
    }
    nslots[s] = i + 1;
  }
  free(nslots);
  free(nhash);
  for (int i = 0; i < a->ncols; i++)
    if (schema->children[i] == NULL ||
        arrow_classify(a, i, schema->children[i]) < 0)
      goto fail_classify;
  /* the batches: the struct shape, then the per-column vetting */
  for (size_t b = 0; b < n_batches; b++) {
    const ArrowArray *ba = batches[b];
    if (ba == NULL || ba->n_children != schema->n_children ||
        ba->children == NULL) {
      mizu_arrow_close(a);
      return arrow_decline("an Arrow batch whose column count differs "
                           "from the schema");
    }
    if (arrow_check_batch(a, ba) < 0) goto fail;
    a->rows += ba->length;
  }
  if (arrow_validate_dicts(a) < 0) goto fail;
  /* the string walks: the packed byte totals, and the one malformed-
     record check ahead of any emit */
  for (int i = 0; i < a->ncols; i++) {
    a->any_str |= a->cols[i].kind == MIZU_ARROW_STR;
    if (a->cols[i].kind == MIZU_ARROW_STR && arrow_size_str(a, i) < 0)
      goto fail;
  }
  *out = a;
  return MIZU_OK;
fail_classify:
fail:
  mizu_arrow_close(a);
  return MIZU_ERR;
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
  a->any_remote = 1;
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
  if (!a->sized) arrow_size_pass(a);
  mizu_stage_reap(h);
  mizu_shm *shm;
  if (mizu_stage_spill_get(h, a->total, &shm) != MIZU_OK) return MIZU_ERR;
  uint8_t *base = (uint8_t *) shm->addr;
  memset(warn, 0, sizeof(*warn));
  mizu_arrow_write(a, base, warn);
  /* the SHM_VEC frame: the region name, the type tag and the exact used
     byte count in aux */
  hdr->kind = MIZU_KIND_SHM_VEC;
  hdr->len = (uint32_t) shm->name_len;
  hdr->aux = mizu_aux_shm_vec(MIZU_TYPE_VEC, (uint64_t) a->total);
  memcpy(payload, shm->name, shm->name_len);
  mizu_stage_retain_zc(h, shm);
  return MIZU_OK;
}

void mizu_arrow_close(mizu_arrow *a) {
  if (a == NULL) return;
  free(a->cols);   /* the one block: scols / remotes / tab ride it */
  free(a);
}
