/* Fuzz tier: the Arrow frame writer's classify-and-vet layer
   (mizu_arrow_open) over fuzz-derived schema and batch shapes, with
   mizu_arrow_write driven for inputs that pass vetting (the offset
   arithmetic is the OOB hunting ground). The harness crafts internally
   consistent buffers in a static arena — data/offsets/view/bitmap spans
   always in-bounds — so any sanitizer hit is a writer bug; the malformed
   shapes (buffer counts, names, child counts, null_count range, a uint8
   column's nulls, a bad dictionary) exercise the declines. libFuzzer
   harness; run as short fixed-seed ASan+UBSan bursts via
   `make test-fuzz`. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/* The format table: every classified kind, plus unmapped and malformed
   entries for the declines (the short ones reach the NUL-length guards
   in arrow_classify / mizu_cvt_for_arrow). */
static const char *const FMTS[] = {
  "b", "c", "s", "S", "C", "i", "I", "l", "L", "e", "f", "g",
  "u", "U", "vu", "tdD", "tsu:", "tsm:Europe/Paris", "tDs", "tDm",
  "w", "+l", "Z", "x:2", "t", "ts", "tD", "", "tsx"
};
#define NFMTS (sizeof(FMTS) / sizeof(FMTS[0]))

static const char *const NAMES[] = { "a", "b", "a", NULL };

/* The arena: every crafted buffer, spaced so the crafted geometry (a few
   hundred bytes per slot) stays in-bounds by construction. */
static uint8_t arena[16384];
#define SLOT 2048

static void mk_schema(ArrowSchema *s, const char *fmt, const char *name,
                      ArrowSchema **children, int64_t n_children,
                      ArrowSchema *dict, int64_t flags) {
  memset(s, 0, sizeof(*s));
  s->format = fmt;
  s->name = name;
  s->flags = flags;
  s->n_children = n_children;
  s->children = children;
  s->dictionary = dict;
}

static void mk_array(ArrowArray *a, int64_t len, int64_t nulls, int64_t off,
                     int64_t n_buffers, const void **buffers,
                     ArrowArray **children, ArrowArray *dict) {
  memset(a, 0, sizeof(*a));
  a->length = len;
  a->null_count = nulls;
  a->offset = off;
  a->n_buffers = n_buffers;
  a->buffers = buffers;
  a->n_children = -1;
  a->children = children;
  a->dictionary = dict;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 12) return 0;
  const int ncols = 1 + (data[0] & 3);
  const uint8_t flags = data[1];
  uint8_t pick[1024];
  for (size_t i = 0; i < sizeof pick; i++) pick[i] = data[(2 + i) % size];

  ArrowSchema cols_s[4], dict_s[4], root;
  ArrowSchema *kids[4];
  ArrowArray cols_a[4], dict_a[4], batch;
  ArrowArray *bkids[4];
  const void *bufs[4][4];
  const void *dbufs[4][3];

  const char *fmt[4];
  int dict[4] = { 0, 0, 0, 0 };
  for (int i = 0; i < ncols; i++) {
    fmt[i] = FMTS[pick[i * 8] % NFMTS];
    dict[i] = (flags & (1u << i)) != 0;
    const char *name = NAMES[pick[i * 8 + 1] & 3];
    if (dict[i]) {
      mk_schema(&dict_s[i], "u", NULL, NULL, 0, NULL, 0);
      mk_schema(&cols_s[i], "c", name, NULL, 0, &dict_s[i],
                (int64_t) (pick[i * 8 + 2] & 1));
    } else {
      mk_schema(&cols_s[i], fmt[i], name, NULL, 0, NULL, 0);
    }
    kids[i] = &cols_s[i];
  }
  mk_schema(&root, "+s", NULL, kids, ncols, NULL, 0);

  /* the batch: each column 0-7 rows, offset 0 or 1, nulls per a byte */
  const int64_t len = (int64_t) (pick[40] & 7);
  for (int i = 0; i < ncols; i++) {
    uint8_t *slot = arena + (size_t) i * SLOT;
    const int64_t off = (int64_t) (pick[41 + i * 4] & 1);
    const int64_t n = len;
    const int nc = pick[42 + i * 4];
    const int64_t null_count =
      (nc & 3) == 0 ? -1 : (nc & 3) == 1 ? 0 : (nc & 3) == 2 ? 1 : n + 1;
    /* the slot layout: [0,64) the data buffer, [64,72) the bitmap,
       [128,192) the offsets array, [256,400) the levels/views, [384,392)
       the view sizes entry */
    uint8_t *col_data = slot;
    uint8_t *bm = slot + 64;
    for (size_t k = 0; k < 64; k++)
      col_data[k] = pick[48 + i * 16 + (int) (k % 8)];
    for (size_t k = 0; k < 8; k++) bm[k] = pick[96 + i * 8 + (int) k];
    if (dict[i]) {
      /* three 1-byte levels; fuzz-valued one-byte indices (the range
         check fires past the levels) */
      uint8_t *levs = slot + 128;
      levs[0] = 'x';
      levs[1] = 'y';
      levs[2] = 'z';
      int32_t *loffs = (int32_t *) (slot + 256);
      loffs[0] = 0;
      loffs[1] = 1;
      loffs[2] = 2;
      loffs[3] = 3;
      dbufs[i][0] = NULL;
      dbufs[i][1] = loffs;
      dbufs[i][2] = levs;
      mk_array(&dict_a[i], 3, 0, 0, 3, dbufs[i], NULL, NULL);
      bufs[i][0] = bm;
      bufs[i][1] = col_data;
      mk_array(&cols_a[i], n, null_count, off, 2, bufs[i], NULL,
               &dict_a[i]);
    } else {
      int form = strcmp(fmt[i], "u") == 0 ? 1 :
        strcmp(fmt[i], "U") == 0 ? 2 : strcmp(fmt[i], "vu") == 0 ? 3 : 0;
      if (form == 1 || form == 2) {
        int32_t *so = (int32_t *) (slot + 128);
        int64_t *lo = (int64_t *) (slot + 128);
        if (pick[43 + i * 4] & 4) {
          /* raw fuzz-valued offsets, bounded to the 64-byte data span:
             non-monotonic interiors reach the open vet's offset walk */
          for (int64_t k = 0; k <= off + n; k++) {
            if (form == 1)
              so[k] = (int32_t) (pick[43 + i * 4 + (int) (k & 1)] & 63);
            else
              lo[k] = (int64_t) (pick[43 + i * 4 + (int) (k & 1)] & 63);
          }
        } else {
          /* crafted monotonic offsets, spans 0-3 bytes */
          int32_t run = 0;
          for (int64_t k = 0; k <= off + n; k++) {
            run += pick[43 + i * 4 + (int) (k & 1)] & 3;
            if (form == 1) so[k] = run;
            else lo[k] = run;
          }
        }
        bufs[i][1] = slot + 128;
        bufs[i][2] = col_data;
      } else if (form == 3) {
        /* views: inline (<= 12-byte strings) or long with a fuzz-valued
           buffer index and offset — bi == ndata (the sizes array's slot)
           and off + len past the declared size exercise the view record's
           bounds; slot [384,392) is the one sizes entry */
        uint8_t *vw = slot + 256;
        int64_t *sz = (int64_t *) (slot + 384);
        sz[0] = 64;
        for (int64_t k = 0; k < off + n; k++) {
          const uint8_t p = pick[44 + i * 4 + (int) (k % 3)];
          if (p & 8) {
            const int32_t l = 13 + (int32_t) (p & 7);
            const int32_t bi = (int32_t) (pick[45 + i * 4 + (int) (k & 1)] & 3);
            const int32_t bo = (int32_t) (pick[46 + i * 4 + (int) (k & 1)] & 63);
            memcpy(vw + 16 * k, &l, 4);
            memcpy(vw + 16 * k + 8, &bi, 4);
            memcpy(vw + 16 * k + 12, &bo, 4);
          } else {
            const int32_t l = (int32_t) (p & 7);
            memcpy(vw + 16 * k, &l, 4);
            memset(vw + 16 * k + 4, 'a' + (int) (k % 26), (size_t) l);
          }
        }
        bufs[i][1] = vw;
        bufs[i][2] = col_data;
        bufs[i][3] = sz;
      } else {
        bufs[i][1] = col_data;
      }
      bufs[i][0] = bm;
      int64_t need = form == 3 ? 4 : form != 0 ? 3 : 2;
      if (pick[45 + i * 4] & 1) need--;   /* a short buffer count */
      mk_array(&cols_a[i], n, null_count, off, need, bufs[i], NULL, NULL);
    }
    bkids[i] = &cols_a[i];
  }
  /* the struct batch's own shape (offset, null count, an optional
     bitmap): the vet declines a struct offset or a null bitmap */
  uint8_t sbm = 0xFF;
  const void *sbufs[1] = { &sbm };
  const int64_t snc = (int64_t) (pick[124] & 3);
  mk_array(&batch, len, snc == 3 ? -1 : snc,
           (int64_t) ((pick[124] >> 2) & 1), 1,
           (pick[124] & 8) != 0 ? sbufs : NULL, bkids, NULL);
  batch.n_children = (flags & 0x10) != 0 ? ncols - 1 : ncols;

  /* an optional second batch: the same crafted buffers, each
     dictionary re-shaped per the fuzz bytes (NULL buffers, a short
     count, a levels mismatch, no dictionary — the later-batch
     dictionary vet) */
  ArrowArray cols_a2[4], dict_a2[4], batch2;
  ArrowArray *bkids2[4];
  const void *dbufs2[4][3];
  const ArrowArray *batches[2];
  int nb = 1;
  batches[0] = &batch;
  if (flags & 0x20) {
    for (int i = 0; i < ncols; i++) {
      cols_a2[i] = cols_a[i];
      bkids2[i] = &cols_a2[i];
      if (dict[i]) {
        dict_a2[i] = dict_a[i];
        cols_a2[i].dictionary = &dict_a2[i];
        switch (pick[125 + i] % 5) {
        case 1: dict_a2[i].buffers = NULL; break;
        case 2: dict_a2[i].n_buffers = 2; break;
        case 3:   /* the same shape, other level bytes (a mismatch) */
          dbufs2[i][0] = NULL;
          dbufs2[i][1] = dict_a[i].buffers[1];
          dbufs2[i][2] = arena + (size_t) i * SLOT + 384;
          dict_a2[i].buffers = dbufs2[i];
          break;
        case 4: cols_a2[i].dictionary = NULL; break;
        default: break;   /* identical levels */
        }
      }
    }
    mk_array(&batch2, len, 0, 0, 1, NULL, bkids2, NULL);
    batch2.n_children = ncols;
    batches[1] = &batch2;
    nb = 2;
  }

  mizu_arrow *a = NULL;
  if (mizu_arrow_open(&a, &root, batches, (size_t) nb, 0) != MIZU_OK)
    return 0;
  size_t total = mizu_arrow_size(a);
  if (total < (size_t) 1 << 20) {
    uint8_t *buf = malloc(total);
    if (buf != NULL) {
      mizu_arrow_warn warn;
      memset(&warn, 0, sizeof warn);
      mizu_arrow_write(a, buf, &warn);
      int64_t n = 0, aoff = 0, asz = 0, valid[2];
      if (mizu_mizl_check(buf, total, &n, &aoff, &asz, valid) == 0) {
        mizu_mizl_entry e;
        for (int64_t i = 0; i < n; i++)
          (void) mizu_mizl_elem(buf, total, i, &e);
      }
      free(buf);
    }
  }
  mizu_arrow_close(a);
  return 0;
}
