/* Bench: Arrow open/classification, per-column body conversion kernels, and
   a two-batch whole-frame write. Hand-built one-column struct frames, one warmup and the best of seven
   runs. Report-only (no timing asserts — runner timing is too variable);
   records are appended to bench/notes.md by hand with the commit SHA.
   `make bench`. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#define NROWS (8LL * 1024 * 1024)
#define NRUNS 7

enum column_kind { FLOAT64, INT64, FLOAT32, DATE32, TIMESTAMP_US, DICT_I32 };
enum null_kind { NULL_FREE, UNKNOWN_VALID, NULL_1PCT, NULL_50PCT, ALL_NULL };

struct column_case {
  enum column_kind kind;
  const char *name;
  const char *format;
  size_t out_width;
};

struct null_case {
  enum null_kind kind;
  const char *name;
  int64_t count;
};

static const struct column_case columns[] = {
  { FLOAT64, "float64_copy", "g", 8 },
  { INT64, "int64_copy", "l", 8 },
  { FLOAT32, "float32_widen", "f", 8 },
  { DATE32, "date32", "tdD", 8 },
  { TIMESTAMP_US, "timestamp_us", "tsu:", 8 },
  { DICT_I32, "dictionary_i32", "i", 4 }
};

static const struct null_case nulls[] = {
  { NULL_FREE, "null_free", 0 },
  { UNKNOWN_VALID, "unknown_all_valid", -1 },
  { NULL_1PCT, "null_1pct", (NROWS + 99) / 100 },
  { NULL_50PCT, "null_50pct", NROWS / 2 },
  { ALL_NULL, "all_null", NROWS }
};

static void fail(const char *what) {
  const char *detail = mizu_last_error_message();
  fprintf(stderr, "bench_arrow: %s%s%s\n", what,
          detail != NULL ? ": " : "", detail != NULL ? detail : "");
  exit(1);
}

static void *xmalloc(size_t n) {
  void *p = malloc(n);
  if (p == NULL) fail("allocation failed");
  return p;
}

static void mk_schema(ArrowSchema *s, const char *format, const char *name,
                      ArrowSchema **children, ArrowSchema *dictionary) {
  memset(s, 0, sizeof(*s));
  s->format = format;
  s->name = name;
  s->n_children = children != NULL ? 1 : 0;
  s->children = children;
  s->dictionary = dictionary;
}

static void mk_array(ArrowArray *a, int64_t length, int64_t null_count,
                     const void **buffers, ArrowArray **children,
                     ArrowArray *dictionary) {
  memset(a, 0, sizeof(*a));
  a->length = length;
  a->null_count = null_count;
  a->n_buffers = children != NULL ? 1 : 2;
  a->n_children = children != NULL ? 1 : 0;
  a->buffers = buffers;
  a->children = children;
  a->dictionary = dictionary;
}

static void fill_values(const struct column_case *c, uint8_t *data) {
  for (int64_t i = 0; i < NROWS; i++) {
    switch (c->kind) {
    case FLOAT64: {
      double v = (double) (i % 10007) + 0.25;
      memcpy(data + 8 * i, &v, 8);
      break;
    }
    case INT64:
    case TIMESTAMP_US: {
      int64_t v = 1000000 + i;
      memcpy(data + 8 * i, &v, 8);
      break;
    }
    case FLOAT32: {
      float v = (float) (i % 10007) + 0.25f;
      memcpy(data + 4 * i, &v, 4);
      break;
    }
    case DATE32: {
      int32_t v = 18000 + (int32_t) (i % 1000);
      memcpy(data + 4 * i, &v, 4);
      break;
    }
    case DICT_I32: {
      int32_t v = (int32_t) (i & 3);
      memcpy(data + 4 * i, &v, 4);
      break;
    }
    }
  }
}

static void fill_validity(enum null_kind kind, uint8_t *validity) {
  size_t bytes = (size_t) (NROWS + 7) / 8;
  switch (kind) {
  case NULL_1PCT:
    memset(validity, 0xFF, bytes);
    for (int64_t i = 0; i < NROWS; i += 100)
      validity[i >> 3] &= (uint8_t) ~(1u << (i & 7));
    break;
  case NULL_50PCT:
    memset(validity, 0x55, bytes);
    break;
  case ALL_NULL:
    memset(validity, 0, bytes);
    break;
  case NULL_FREE:
  case UNKNOWN_VALID:
    memset(validity, 0xFF, bytes);
    break;
  }
}

static double best_open(const ArrowSchema *root,
                        const ArrowArray *const *batches) {
  double best = 0;
  for (int run = 0; run <= NRUNS; run++) {
    mizu_arrow *a = NULL;
    double t0 = mizu_now();
    mizu_status st = mizu_arrow_open(&a, root, batches, 1, 0);
    double elapsed = (mizu_now() - t0) * 1e3;
    if (st != MIZU_OK) fail("mizu_arrow_open failed");
    mizu_arrow_close(a);
    if (run != 0 && (run == 1 || elapsed < best)) best = elapsed;
  }
  return best;
}

static double best_size(const ArrowSchema *root,
                        const ArrowArray *const *batches) {
  double best = 0;
  for (int run = 0; run <= NRUNS; run++) {
    mizu_arrow *a = NULL;
    if (mizu_arrow_open(&a, root, batches, 1, 0) != MIZU_OK)
      fail("mizu_arrow_open failed before size benchmark");
    double t0 = mizu_now();
    size_t n = mizu_arrow_size(a);
    double elapsed = (mizu_now() - t0) * 1e3;
    if (n == 0) fail("unexpected frame size");
    mizu_arrow_close(a);
    if (run != 0 && (run == 1 || elapsed < best)) best = elapsed;
  }
  return best;
}

static double best_body(mizu_arrow *a, uint8_t *out, size_t expected) {
  double best = 0;
  for (int run = 0; run <= NRUNS; run++) {
    mizu_arrow_warn warn;
    memset(&warn, 0, sizeof(warn));
    double t0 = mizu_now();
    size_t n = mizu_arrow_col_body(a, 0, out, &warn);
    double elapsed = (mizu_now() - t0) * 1e3;
    if (n != expected) fail("unexpected column body size");
    if (run != 0 && (run == 1 || elapsed < best)) best = elapsed;
  }
  return best;
}

static void bench_case(const struct column_case *c,
                       const struct null_case *nc, uint8_t *data,
                       uint8_t *validity, uint8_t *out) {
  ArrowSchema child, root, levels;
  ArrowSchema *schema_children[1] = { &child };
  mk_schema(&child, c->format, "x", NULL,
            c->kind == DICT_I32 ? &levels : NULL);
  if (c->kind == DICT_I32) mk_schema(&levels, "u", NULL, NULL, NULL);
  mk_schema(&root, "+s", NULL, schema_children, NULL);

  fill_values(c, data);
  fill_validity(nc->kind, validity);
  const void *buffers[2] = { nc->kind == NULL_FREE ? NULL : validity, data };
  ArrowArray array, batch, dictionary;
  ArrowArray *array_children[1] = { &array };
  mk_array(&array, NROWS, nc->count, buffers, NULL,
           c->kind == DICT_I32 ? &dictionary : NULL);
  if (c->kind == DICT_I32) {
    static const int32_t offsets[5] = { 0, 1, 3, 6, 10 };
    static const char bytes[10] = { 'a', 'b', 'b', 'c', 'c', 'c', 'd', 'd', 'd', 'd' };
    static const void *level_buffers[3] = { NULL, offsets, bytes };
    memset(&dictionary, 0, sizeof(dictionary));
    dictionary.length = 4;
    dictionary.n_buffers = 3;
    dictionary.buffers = level_buffers;
  }
  mk_array(&batch, NROWS, 0, NULL, array_children, NULL);
  const ArrowArray *batches[1] = { &batch };

  printf("arrow_open  %-14s %-17s %10.3f ms\n", c->name, nc->name,
         best_open(&root, batches));
  printf("arrow_size  %-14s %-17s %10.3f ms\n", c->name, nc->name,
         best_size(&root, batches));

  mizu_arrow *a = NULL;
  if (mizu_arrow_open(&a, &root, batches, 1, 0) != MIZU_OK)
    fail("mizu_arrow_open failed before body benchmark");
  printf("arrow_body  %-14s %-17s %10.3f ms\n", c->name, nc->name,
         best_body(a, out, (size_t) NROWS * c->out_width));
  mizu_arrow_close(a);
}

/* A whole-frame write of one float64 column split across two sliced
   batches: the shape whose validity tail is rebuilt rather than copied. */
static void bench_frame(const struct null_case *nc, uint8_t *data,
                        uint8_t *validity) {
  ArrowSchema child, root;
  ArrowSchema *schema_children[1] = { &child };
  mk_schema(&child, "g", "x", NULL, NULL);
  mk_schema(&root, "+s", NULL, schema_children, NULL);
  fill_values(&columns[0], data);
  fill_validity(nc->kind, validity);
  const int64_t len = NROWS / 2 - 3;
  const void *buffers[2] = { validity, data };
  ArrowArray array[2], batch[2];
  ArrowArray *array_children[2][1] = { { &array[0] }, { &array[1] } };
  for (int b = 0; b < 2; b++) {
    mk_array(&array[b], len, -1, buffers, NULL, NULL);
    array[b].offset = b == 0 ? 0 : 5;
    mk_array(&batch[b], len, 0, NULL, array_children[b], NULL);
  }
  const ArrowArray *batches[2] = { &batch[0], &batch[1] };
  mizu_arrow *a = NULL;
  if (mizu_arrow_open(&a, &root, batches, 2, 0) != MIZU_OK)
    fail("mizu_arrow_open failed before frame benchmark");
  uint8_t *out = xmalloc(mizu_arrow_size(a));
  double best = 0;
  for (int run = 0; run <= NRUNS; run++) {
    mizu_arrow_warn warn;
    memset(&warn, 0, sizeof(warn));
    double t0 = mizu_now();
    mizu_arrow_write(a, out, &warn);
    double elapsed = (mizu_now() - t0) * 1e3;
    if (run != 0 && (run == 1 || elapsed < best)) best = elapsed;
  }
  printf("arrow_frame float64_copy   two_batches %-9s %7.3f ms\n", nc->name,
         best);
  free(out);
  mizu_arrow_close(a);
}

int main(void) {
  uint8_t *data = xmalloc((size_t) NROWS * 8);
  uint8_t *validity = xmalloc((size_t) (NROWS + 7) / 8);
  uint8_t *out = xmalloc((size_t) NROWS * 8);
  printf("bench_arrow: %lld rows, one batch, warmup 1, best of %d; "
         "one 64 MiB source and one 64 MiB reusable output\n",
         (long long) NROWS, NRUNS);
  for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); i++)
    for (size_t j = 0; j < sizeof(nulls) / sizeof(nulls[0]); j++)
      bench_case(&columns[i], &nulls[j], data, validity, out);
  bench_frame(&nulls[2], data, validity);
  bench_frame(&nulls[3], data, validity);
  free(out);
  free(validity);
  free(data);
  return 0;
}
