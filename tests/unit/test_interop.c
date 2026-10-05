/* Unit tier: the interop cursor and emit helpers — the golden corpus
   (tests/interop/, spec-authored fixtures) driven through the cursor and
   re-emitted byte-for-byte, plus hand-built checks for the pieces the
   corpus has no rows for yet (the err and task items join the corpus
   with Phases 2 and 4). The corpus rows assert: every rt/dec/enc stream
   parses to completion and re-emits to the same bytes; a "cursor:"
   read-err fails at the cursor; a "builder:" read-err parses clean (the
   rejection is each binding builder's assertion). Assert-based; run via
   `make test` from the repo root (the corpus paths are relative). */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mizu_ext.h"

#define MAX_STREAM 8192
#define MAX_LINE 16384

struct corpus_row {
  char id[128];
  unsigned char bytes[MAX_STREAM];
  size_t len;
};

/* Re-emit a parsed stream through the put helpers: the flat walk needs
   no nesting logic (container begins are just emitted; ends are
   implicit), the one state being a bare string's position (dict key or
   strv element), which the item's key flag carries. */
static size_t reemit(const unsigned char *in, size_t n, unsigned char *out,
                     int *fail) {
  mizu_ix cur;
  mizu_ix_item it;
  size_t off;
  *fail = 0;
  if (mizu_ix_open(&cur, in, n) != MIZU_OK) {
    *fail = 1;
    return 0;
  }
  off = mizu_ix_put_header(out);
  for (;;) {
    if (mizu_ix_next(&cur, &it) != MIZU_OK) {
      *fail = 1;
      return 0;
    }
    switch (it.kind) {
    case MIZU_IX_NIL:
      off += mizu_ix_put_nil(out + off);
      break;
    case MIZU_IX_LGL:
      off += mizu_ix_put_lgl(out + off, (int) it.u64[0]);
      break;
    case MIZU_IX_INT: {
      int64_t v;
      memcpy(&v, &it.u64[0], 8);
      off += mizu_ix_put_int(out + off, v);
      break;
    }
    case MIZU_IX_REAL: {
      double v;
      memcpy(&v, &it.u64[0], 8);
      off += mizu_ix_put_real(out + off, v);
      break;
    }
    case MIZU_IX_CPLX: {
      double re, im;
      memcpy(&re, &it.u64[0], 8);
      memcpy(&im, &it.u64[1], 8);
      off += mizu_ix_put_cplx(out + off, re, im);
      break;
    }
    case MIZU_IX_STR1:
      off += mizu_ix_put_str(out + off, it.ptr,
                             it.na ? -1 : (int32_t) it.len);
      break;
    case MIZU_IX_STR:
      if (it.key) {
        off += mizu_ix_put_key(out + off, it.ptr, (uint32_t) it.len);
      } else {
        off += mizu_ix_put_strelt(out + off, it.ptr,
                                  it.na ? -1 : (int32_t) it.len);
      }
      break;
    case MIZU_IX_BYTES:
      off += mizu_ix_put_bytes(out + off, it.ptr, it.count);
      break;
    case MIZU_IX_VEC:
      off += mizu_ix_put_vec(out + off, (int) it.type, it.ptr, it.count);
      break;
    case MIZU_IX_STRV:
      off += mizu_ix_put_strv_begin(out + off, it.count);
      break;
    case MIZU_IX_LIST:
      off += mizu_ix_put_list_begin(out + off, it.count);
      break;
    case MIZU_IX_DICT:
      off += mizu_ix_put_dict_begin(out + off, it.count);
      break;
    case MIZU_IX_ATTR:
      off += mizu_ix_put_attr(out + off);
      break;
    case MIZU_IX_ERR:
      off += mizu_ix_put_err(out + off, (int) (it.err_flags & 1u),
                             it.err_index,
                             it.err_str[0].ptr, (uint32_t) it.err_str[0].len,
                             it.err_str[1].ptr, (uint32_t) it.err_str[1].len,
                             it.err_str[2].ptr, (uint32_t) it.err_str[2].len);
      break;
    case MIZU_IX_TASK:
      off += mizu_ix_put_task(out + off, (int) it.target,
                              (int) it.task_kind, it.u64[0]);
      break;
    case MIZU_IX_REF:
      off += mizu_ix_put_ref(out + off, it.ptr, (uint32_t) it.len);
      break;
    default:
      *fail = 1;
      return 0;
    }
    if (cur.done) break;
  }
  if (mizu_ix_end(&cur) != MIZU_OK) {
    *fail = 1;
    return 0;
  }
  return off;
}

static int unhex1(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static size_t unhex(const char *s, unsigned char *out) {
  size_t n = 0;
  while (s[0] != '\0' && s[1] != '\0' && s[0] != '\n' && s[0] != ' ') {
    int hi = unhex1((unsigned char) s[0]);
    int lo = unhex1((unsigned char) s[1]);
    if (hi < 0 || lo < 0) break;
    out[n++] = (unsigned char) ((hi << 4) | lo);
    s += 2;
  }
  return n;
}

static void corpus_tests(void) {
  /* corpus.txt: id | hex */
  FILE *fc = fopen("tests/interop/corpus.txt", "r");
  assert(fc != NULL);
  /* static: the 4 MB array overflows Windows' 1 MB stack reserve */
  static struct corpus_row rows[512];
  size_t nrows = 0;
  char line[MAX_LINE];
  while (fgets(line, sizeof line, fc) != NULL) {
    if (line[0] == '#' || line[0] == '\n') continue;
    char *bar = strchr(line, '|');
    assert(bar != NULL);
    *bar = '\0';
    assert(nrows < sizeof rows / sizeof rows[0]);
    size_t idlen = strlen(line);
    assert(idlen < sizeof rows[nrows].id);
    memcpy(rows[nrows].id, line, idlen + 1);
    while (idlen > 0 && rows[nrows].id[idlen - 1] == ' ')
      rows[nrows].id[--idlen] = '\0';
    char *hex = bar + 1;
    while (*hex == ' ') hex++;
    rows[nrows].len = unhex(hex, rows[nrows].bytes);
    assert(rows[nrows].len > 0);
    nrows++;
  }
  fclose(fc);
  assert(nrows > 100);

  /* cases.txt: id | kind | langs | value | note — the test needs the
     kind and the cursor:/builder: level of read-err rows. */
  FILE *ff = fopen("tests/interop/cases.txt", "r");
  assert(ff != NULL);
  size_t n_rt = 0, n_task = 0, n_cursor_err = 0, n_builder_err = 0, n_wd = 0;
  unsigned char out[MAX_STREAM];
  while (fgets(line, sizeof line, ff) != NULL) {
    if (line[0] == '#' || line[0] == '\n') continue;
    char *fields[5] = { NULL, NULL, NULL, NULL, NULL };
    size_t nf = 0;
    char *s = line;
    while (nf < 5) {
      char *bar = strchr(s, '|');
      if (bar == NULL || nf == 4) {
        fields[nf++] = s;
        break;
      }
      *bar = '\0';
      fields[nf++] = s;
      s = bar + 1;
    }
    for (size_t i = 0; i < nf; i++) {
      char *f = fields[i];
      while (*f == ' ') f++;
      char *end = f + strlen(f);
      while (end > f && (end[-1] == ' ' || end[-1] == '\n')) *--end = '\0';
      fields[i] = f;
    }
    assert(nf >= 4);
    const char *note = nf == 5 ? fields[4] : "";

    const char *kind = fields[1];
    if (strcmp(kind, "write-decline") == 0) {
      n_wd++;
      continue;
    }
    /* find the corpus row */
    size_t r;
    for (r = 0; r < nrows; r++)
      if (strcmp(rows[r].id, fields[0]) == 0) break;
    assert(r < nrows);

    if (strcmp(kind, "read-err") == 0) {
      int fail;
      reemit(rows[r].bytes, rows[r].len, out, &fail);
      if (strncmp(note, "cursor:", 7) == 0) {
        if (!fail) fprintf(stderr, "cursor row accepted: %s\n", fields[0]);
        assert(fail);                    /* the cursor rejects */
        n_cursor_err++;
      } else {
        assert(strncmp(note, "builder:", 8) == 0);
        assert(!fail);                   /* the cursor parses clean */
        n_builder_err++;
      }
    } else {
      /* rt / dec / enc / task: parse to completion and re-emit to the
         same bytes — the corpus streams are the canonical form */
      int fail;
      size_t off = reemit(rows[r].bytes, rows[r].len, out, &fail);
      assert(!fail);
      assert(off == rows[r].len);
      assert(memcmp(out, rows[r].bytes, off) == 0);
      if (strcmp(kind, "task") == 0) n_task++; else n_rt++;
    }
  }
  fclose(ff);
  assert(n_rt > 80 && n_task >= 5 && n_cursor_err >= 15 && n_builder_err >= 4 && n_wd >= 5);
  printf("interop corpus: %zu re-emitted (%zu tasks), %zu cursor errors, %zu builder rows, %zu declines\n",
         n_rt, n_task, n_cursor_err, n_builder_err, n_wd);
}

/* The cursor rejects a bad magic, a short stream, and an unknown version
   with the informative texts. */
static void open_tests(void) {
  mizu_ix cur;
  const unsigned char good[] = { 'I', 0x01, 0x00 };
  assert(mizu_ix_open(&cur, good, 3) == MIZU_OK);
  assert(mizu_ix_open(&cur, good, 1) == MIZU_ERR);
  const unsigned char bad[] = { 'X', 0x01, 0x00 };
  assert(mizu_ix_open(&cur, bad, 3) == MIZU_ERR);
  const unsigned char ver[] = { 'I', 0x02, 0x00 };
  assert(mizu_ix_open(&cur, ver, 3) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "newer format") != NULL);
  assert(mizu_ix_next(&cur, &(mizu_ix_item) {0}) == MIZU_ERR);  /* latched */
}

/* The err and task items: parsed from day one, ahead of their builders
   (Phases 2 and 4). */
static void err_task_tests(void) {
  unsigned char buf[256];
  size_t off = mizu_ix_put_header(buf);
  off += mizu_ix_put_err(buf + off, 1, 42, "type", 4, "msg", 3, "dt", 2);
  mizu_ix cur;
  assert(mizu_ix_open(&cur, buf, off) == MIZU_OK);
  mizu_ix_item it;
  assert(mizu_ix_next(&cur, &it) == MIZU_OK);
  assert(it.kind == MIZU_IX_ERR && it.err_flags == 1 && it.err_index == 42);
  assert(it.err_str[0].len == 4 && memcmp(it.err_str[0].ptr, "type", 4) == 0);
  assert(it.err_str[1].len == 3 && memcmp(it.err_str[1].ptr, "msg", 3) == 0);
  assert(it.err_str[2].len == 2 && memcmp(it.err_str[2].ptr, "dt", 2) == 0);
  assert(mizu_ix_end(&cur) == MIZU_OK);

  /* no index: flags 0, three strings only */
  off = mizu_ix_put_header(buf);
  off += mizu_ix_put_err(buf + off, 0, 0, "t", 1, "m", 1, "d", 1);
  assert(mizu_ix_open(&cur, buf, off) == MIZU_OK);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK);
  assert(it.kind == MIZU_IX_ERR && it.err_flags == 0);
  assert(mizu_ix_end(&cur) == MIZU_OK);

  /* list[task, x]: the task nests as one element of kind-determined
     arity, so the list's count is 2 */
  off = mizu_ix_put_header(buf);
  off += mizu_ix_put_list_begin(buf + off, 2);
  off += mizu_ix_put_task(buf + off, MIZU_LANG_PYTHON, 0,
                          MIZU_IDENT(MIZU_LANG_R, 0));
  off += mizu_ix_put_str(buf + off, "pkg::fn", 7);
  off += mizu_ix_put_list_begin(buf + off, 1);
  off += mizu_ix_put_int(buf + off, 5);
  off += mizu_ix_put_dict_begin(buf + off, 0);
  off += mizu_ix_put_nil(buf + off);
  assert(mizu_ix_open(&cur, buf, off) == MIZU_OK);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_LIST &&
         it.count == 2);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_TASK &&
         it.target == MIZU_LANG_PYTHON && it.task_kind == 0 &&
         it.count == 3 && it.u64[0] == MIZU_IDENT(MIZU_LANG_R, 0));
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_STR1 &&
         it.len == 7);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_LIST &&
         it.count == 1);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_INT);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_DICT &&
         it.count == 0);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_NIL);
  assert(mizu_ix_end(&cur) == MIZU_OK);

  /* the kind-2 (runner) header */
  off = mizu_ix_put_header(buf);
  off += mizu_ix_put_task(buf + off, MIZU_LANG_R, 2, 0);
  off += mizu_ix_put_str(buf + off, "region", 6);
  off += mizu_ix_put_int(buf + off, 7);
  off += mizu_ix_put_nil(buf + off);
  assert(mizu_ix_open(&cur, buf, off) == MIZU_OK);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_TASK &&
         it.task_kind == 2 && it.count == 3);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_STR1);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_INT);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_NIL);
  assert(mizu_ix_end(&cur) == MIZU_OK);

  /* an unknown task kind declines with the newer-format text */
  unsigned char unk[] = { 'I', 0x01, 0x12, 0x02, 0x09, 0x00, 0x00,
                          0, 0, 0, 0, 0, 0, 0, 0 };
  assert(mizu_ix_open(&cur, unk, sizeof unk) == MIZU_OK);
  assert(mizu_ix_next(&cur, &it) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "unknown task kind 0x09") != NULL);
  assert(strstr(mizu_last_error_message(), "newer format") != NULL);
}

/* The ref leaf (0x13): the identifier span is opaque to the cursor —
   length and bounds only. */
static void ref_tests(void) {
  unsigned char buf[64];
  size_t off = mizu_ix_put_header(buf);
  const char id[] = "mizu_a1b2c3d4e5f6";
  off += mizu_ix_put_ref(buf + off, id, (uint32_t) (sizeof id - 1));
  mizu_ix cur;
  mizu_ix_item it;
  assert(mizu_ix_open(&cur, buf, off) == MIZU_OK);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK);
  assert(it.kind == MIZU_IX_REF && it.len == sizeof id - 1);
  assert(memcmp(it.ptr, id, sizeof id - 1) == 0);
  assert(mizu_ix_end(&cur) == MIZU_OK);

  /* the emitter refuses an empty or over-long identifier (0, nothing
     written) in both passes */
  assert(mizu_ix_put_ref(buf, id, 0) == 0);
  assert(mizu_ix_put_ref(NULL, id, 0) == 0);
  assert(mizu_ix_put_ref(buf, id, 256) == 0);
  assert(mizu_ix_put_ref(NULL, id, 256) == 0);

  /* the cursor rejects an empty identifier and a truncated span (the
     corpus pins both) */
  unsigned char empty[] = { 'I', 0x01, 0x13, 0x00 };
  assert(mizu_ix_open(&cur, empty, sizeof empty) == MIZU_OK);
  assert(mizu_ix_next(&cur, &it) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "ref identifier is empty") != NULL);
  unsigned char trunc[] = { 'I', 0x01, 0x13, 0x05, 'm', 'i', 'z', 'u' };
  assert(mizu_ix_open(&cur, trunc, sizeof trunc) == MIZU_OK);
  assert(mizu_ix_next(&cur, &it) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "truncated") != NULL);

  /* no UTF-8 rule: arbitrary bytes pass */
  unsigned char bin[] = { 'I', 0x01, 0x13, 0x02, 0xff, 0x00 };
  assert(mizu_ix_open(&cur, bin, sizeof bin) == MIZU_OK);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_REF &&
         it.len == 2 && it.ptr[0] == 0xff && it.ptr[1] == 0x00);
  assert(mizu_ix_end(&cur) == MIZU_OK);
}

/* done semantics: one value per stream, then next/end report the rest. */
static void done_tests(void) {
  unsigned char buf[16];
  size_t off = mizu_ix_put_header(buf);
  off += mizu_ix_put_nil(buf + off);
  mizu_ix cur;
  mizu_ix_item it;
  assert(mizu_ix_open(&cur, buf, off) == MIZU_OK);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_NIL);
  assert(mizu_ix_end(&cur) == MIZU_OK);
  /* with a trailing byte present, a second pull past the root value
     reports the same malformed shape as the finishing check */
  off += mizu_ix_put_nil(buf + off);
  assert(mizu_ix_open(&cur, buf, off) == MIZU_OK);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_NIL);
  assert(mizu_ix_next(&cur, &it) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "bytes past the one value") != NULL);

  /* an unterminated container truncates at end (the begin passes the
     pre-allocation bound; the element's payload is missing) */
  unsigned char buf2[32];
  off = mizu_ix_put_header(buf2);
  off += mizu_ix_put_list_begin(buf2 + off, 1);
  buf2[off++] = MIZU_IX_TAG_INT;   /* a tag byte with no payload */
  assert(mizu_ix_open(&cur, buf2, off) == MIZU_OK);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_LIST);
  assert(mizu_ix_next(&cur, &it) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "truncated") != NULL);
}

/* The emit/cursor round trip across every helper, strv/dict-key bare
   strings and the NA forms included. */
static void roundtrip_tests(void) {
  unsigned char buf[512];
  size_t off = mizu_ix_put_header(buf);
  off += mizu_ix_put_dict_begin(buf + off, 2);
  off += mizu_ix_put_key(buf + off, "a", 1);
  off += mizu_ix_put_strv_begin(buf + off, 2);
  off += mizu_ix_put_strelt(buf + off, "x", 1);
  off += mizu_ix_put_strelt(buf + off, NULL, -1);
  off += mizu_ix_put_key(buf + off, "", 0);
  off += mizu_ix_put_attr(buf + off);
  const int32_t codes[3] = { 1, 2, 1 };
  off += mizu_ix_put_vec(buf + off, MIZU_TYPE_INT, codes, 3);
  off += mizu_ix_put_dict_begin(buf + off, 2);
  off += mizu_ix_put_key(buf + off, "levels", 6);
  off += mizu_ix_put_strv_begin(buf + off, 2);
  off += mizu_ix_put_strelt(buf + off, "u", 1);
  off += mizu_ix_put_strelt(buf + off, "v", 1);
  off += mizu_ix_put_key(buf + off, "class", 5);
  off += mizu_ix_put_strv_begin(buf + off, 1);
  off += mizu_ix_put_strelt(buf + off, "factor", 6);

  unsigned char out[512];
  int fail;
  size_t n = reemit(buf, off, out, &fail);
  assert(!fail && n == off && memcmp(out, buf, n) == 0);

  mizu_ix cur;
  mizu_ix_item it;
  assert(mizu_ix_open(&cur, buf, off) == MIZU_OK);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_DICT &&
         it.count == 2);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_STR &&
         it.key == 1 && it.len == 1 && it.ptr[0] == 'a');
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_STRV &&
         it.count == 2);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_STR &&
         it.key == 0 && !it.na && it.len == 1);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_STR &&
         it.na == 1);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_STR &&
         it.key == 1 && it.len == 0);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_ATTR);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_VEC &&
         it.type == MIZU_TYPE_INT && it.count == 3);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_DICT &&
         it.count == 2);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_STR &&
         it.key == 1 && it.len == 6);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_STRV &&
         it.count == 2);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_STR &&
         it.key == 0 && !it.na);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_STR);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_STR &&
         it.key == 1 && it.len == 5);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_STRV &&
         it.count == 1);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_STR);
  assert(mizu_ix_end(&cur) == MIZU_OK);
}

/* The byte-shape helper registry (DESIGN.md): the validator's edge
   matrix, the framer's budget matrix, the shim's accept/reject rows with
   the exact TLS texts, and the full tag table. */

static void utf8_tests(void) {
  /* the empty span and ASCII */
  assert(mizu_ix_utf8_valid("", 0) == 1);
  assert(mizu_ix_utf8_valid("abc", 3) == 1);
  assert(mizu_ix_utf8_valid("\0\0", 2) == 1);        /* NUL is valid */
  /* 2-byte: the boundaries, then overlong / stray / truncated */
  assert(mizu_ix_utf8_valid("\xc2\x80", 2) == 1);    /* U+0080 */
  assert(mizu_ix_utf8_valid("\xdf\xbf", 2) == 1);    /* U+07FF */
  assert(mizu_ix_utf8_valid("\xc0\x80", 2) == 0);    /* overlong NUL */
  assert(mizu_ix_utf8_valid("\xc1\xbf", 2) == 0);    /* overlong */
  assert(mizu_ix_utf8_valid("\x80", 1) == 0);        /* stray continuation */
  assert(mizu_ix_utf8_valid("\xbf\x80", 2) == 0);
  assert(mizu_ix_utf8_valid("\xc3", 1) == 0);        /* truncated */
  assert(mizu_ix_utf8_valid("\xc3\x28", 2) == 0);    /* bad continuation */
  /* 3-byte: boundaries, overlong, surrogates, truncated */
  assert(mizu_ix_utf8_valid("\xe0\xa0\x80", 3) == 1);  /* U+0800 */
  assert(mizu_ix_utf8_valid("\xe2\x82\xac", 3) == 1);  /* U+20AC */
  assert(mizu_ix_utf8_valid("\xef\xbf\xbf", 3) == 1);  /* U+FFFF */
  assert(mizu_ix_utf8_valid("\xe0\x9f\xbf", 3) == 0);  /* overlong */
  assert(mizu_ix_utf8_valid("\xed\xa0\x80", 3) == 0);  /* U+D800 */
  assert(mizu_ix_utf8_valid("\xed\xbf\xbf", 3) == 0);  /* U+DFFF */
  assert(mizu_ix_utf8_valid("\xe2\x82", 2) == 0);      /* truncated */
  assert(mizu_ix_utf8_valid("\xe2\x82\x28", 3) == 0);  /* bad continuation */
  /* 4-byte: boundaries, overlong, past U+10FFFF, the F5-FF range */
  assert(mizu_ix_utf8_valid("\xf0\x90\x80\x80", 4) == 1);  /* U+10000 */
  assert(mizu_ix_utf8_valid("\xf4\x8f\xbf\xbf", 4) == 1);  /* U+10FFFF */
  assert(mizu_ix_utf8_valid("\xf0\x8f\xbf\xbf", 4) == 0);  /* overlong */
  assert(mizu_ix_utf8_valid("\xf4\x90\x80\x80", 4) == 0);  /* U+110000 */
  assert(mizu_ix_utf8_valid("\xf5\x80\x80\x80", 4) == 0);  /* F5+ */
  assert(mizu_ix_utf8_valid("\xf8\x88\x80\x80\x80", 5) == 0);
  assert(mizu_ix_utf8_valid("\xf0\x90\x80", 3) == 0);      /* truncated */
  /* a valid sequence followed by garbage fails whole */
  assert(mizu_ix_utf8_valid("a\xc3\xa9z", 4) == 1);
  assert(mizu_ix_utf8_valid("a\xc3\xa9\x80", 4) == 0);
}

/* Frame an err stream and parse it back through the cursor, asserting
   the emitted lengths. */
static void err_check(uint32_t inline_max,
                      const char *type, size_t type_n,
                      const char *msg, size_t msg_n,
                      const char *detail, size_t detail_n,
                      int has_index, uint64_t index,
                      size_t tn, size_t mn, size_t dn) {
  unsigned char buf[1024];
  size_t n = mizu_ix_write_err(buf, inline_max, type, type_n, msg, msg_n,
                               detail, detail_n, has_index, index);
  assert(n <= inline_max);
  assert(n == 2 + 3 + (has_index ? 8 : 0) + 12 + tn + mn + dn);
  assert(mizu_ix_write_err(NULL, inline_max, type, type_n, msg, msg_n,
                           detail, detail_n, has_index, index) == n);
  mizu_ix cur;
  mizu_ix_item it;
  assert(mizu_ix_open(&cur, buf, n) == MIZU_OK);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_ERR);
  assert(it.err_flags == (uint32_t) (has_index != 0));
  if (has_index) assert(it.err_index == index);
  assert(it.err_str[0].len == tn &&
         (tn == 0 || memcmp(it.err_str[0].ptr, type, tn) == 0));
  assert(it.err_str[1].len == mn &&
         (mn == 0 || memcmp(it.err_str[1].ptr, msg, mn) == 0));
  assert(it.err_str[2].len == dn &&
         (dn == 0 || memcmp(it.err_str[2].ptr, detail, dn) == 0));
  assert(mizu_ix_end(&cur) == MIZU_OK);
}

static void err_framer_tests(void) {
  static char big[300];
  memset(big, 'x', sizeof big);

  /* inline_max at the 25-byte minimum: every span floors to zero; with
     the index the stream is exactly 25 */
  err_check(25, "t", 1, "m", 1, "d", 1, 0, 0, 0, 0, 0);
  err_check(25, "t", 1, "m", 1, "d", 1, 1, 7, 0, 0, 0);
  /* short spans survive whole */
  err_check(64, "type", 4, "msg", 3, "detail", 6, 1, 42, 4, 3, 6);
  /* the type share cap: avail = 487 > 128, so type truncates at 128 */
  err_check(512, big, 300, "m", 1, "d", 1, 0, 0, 128, 1, 1);
  /* a smaller budget binds the type below its share cap */
  err_check(64, big, 300, "m", 1, "d", 1, 0, 0, 39, 0, 0);
  /* the message share: min(inline_max / 2, the remainder) */
  err_check(64, "t", 1, big, 300, big, 300, 0, 0, 1, 32, 6);
  err_check(300, "", 0, big, 300, big, 300, 0, 0, 0, 150, 125);
  /* detail takes what remains */
  err_check(128, "tttt", 4, big, 300, big, 300, 0, 0, 4, 64, 35);
  /* the UTF-8-boundary step-back: 127 ASCII + one 2-byte sequence, the
     128 share cuts inside it and steps back over the whole sequence */
  {
    static char s[200];
    memset(s, 'a', 127);
    memcpy(s + 127, "\xc3\xa9", 2);              /* U+00E9 at 127-128 */
    memset(s + 129, 'b', 71);
    err_check(512, s, 200, "m", 1, "d", 1, 0, 0, 127, 1, 1);
    /* exact-boundary cuts do not step back: 126 ASCII + the sequence,
       ending exactly at 128 */
    static char s2[200];
    memset(s2, 'a', 126);
    memcpy(s2 + 126, "\xc3\xa9", 2);             /* U+00E9 at 126-127 */
    memset(s2 + 128, 'b', 72);
    err_check(512, s2, 128, "m", 1, "d", 1, 0, 0, 128, 1, 1);
    err_check(512, s2, 129, "m", 1, "d", 1, 0, 0, 128, 1, 1);
  }
  /* zero-length spans (NULL pointers) frame three empty strings */
  err_check(64, NULL, 0, NULL, 0, NULL, 0, 0, 0, 0, 0, 0);

  /* the fits-by-construction invariant across the budget range */
  unsigned char buf[1024];
  for (uint32_t cap = 25; cap <= 400; cap += 7) {
    const int hi = (int) (cap & 1u);
    size_t n = mizu_ix_write_err(buf, cap, big, 300, big, 300, big, 300,
                                 hi, 12345);
    assert(n <= cap);
    assert(mizu_ix_write_err(NULL, cap, big, 300, big, 300, big, 300,
                             hi, 12345) == n);
    mizu_ix cur;
    mizu_ix_item it;
    assert(mizu_ix_open(&cur, buf, n) == MIZU_OK);
    assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_ERR);
    assert(mizu_ix_end(&cur) == MIZU_OK);
  }
}

static void shim_tests(void) {
  unsigned char buf[128];
  mizu_ix cur;
  mizu_ix_item it;

  /* the accept row: the header, then code / positional / named fields */
  size_t off = mizu_ix_put_header(buf);
  off += mizu_ix_put_task(buf + off, MIZU_LANG_PYTHON, 0,
                          MIZU_IDENT(MIZU_LANG_R, 0));
  off += mizu_ix_put_str(buf + off, "pkg::fn", 7);
  off += mizu_ix_put_list_begin(buf + off, 0);
  off += mizu_ix_put_dict_begin(buf + off, 0);
  assert(mizu_ixt_open(&cur, buf, off, &it, 1) == MIZU_OK);
  assert(it.kind == MIZU_IX_TASK &&
         it.u64[0] == MIZU_IDENT(MIZU_LANG_R, 0));
  assert(mizu_ixt_want_code(&cur, &it) == MIZU_OK &&
         it.kind == MIZU_IX_STR1 && !it.na && it.len == 7);
  assert(mizu_ixt_want_list(&cur, &it) == MIZU_OK &&
         it.kind == MIZU_IX_LIST);
  assert(mizu_ixt_want_dict(&cur, &it) == MIZU_OK &&
         it.kind == MIZU_IX_DICT);
  assert(mizu_ix_end(&cur) == MIZU_OK);

  /* a runner (kind 2) opens at ceiling 2, declines at ceiling 1 with the
     shim's text; a kind past the registry keeps the cursor's text */
  off = mizu_ix_put_header(buf);
  off += mizu_ix_put_task(buf + off, MIZU_LANG_R, 2, 0);
  off += mizu_ix_put_str(buf + off, "r", 1);
  off += mizu_ix_put_int(buf + off, 0);
  off += mizu_ix_put_nil(buf + off);
  assert(mizu_ixt_open(&cur, buf, off, &it, 2) == MIZU_OK &&
         it.task_kind == 2);
  assert(mizu_ixt_open(&cur, buf, off, &it, 1) == MIZU_ERR);
  assert(strcmp(mizu_last_error_message(),
                "unsupported task kind 0x02") == 0);
  unsigned char unk[] = { 'I', 0x01, 0x12, 0x02, 0x09, 0x00, 0x00,
                          0, 0, 0, 0, 0, 0, 0, 0 };
  assert(mizu_ixt_open(&cur, unk, sizeof unk, &it, 2) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "unknown task kind 0x09") != NULL);

  /* a non-task first item */
  off = mizu_ix_put_header(buf);
  off += mizu_ix_put_nil(buf + off);
  assert(mizu_ixt_open(&cur, buf, off, &it, 1) == MIZU_ERR);
  assert(strcmp(mizu_last_error_message(),
                "malformed task stream: no task tag") == 0);
  /* the latch: later calls fail */
  assert(mizu_ixt_want_code(&cur, &it) == MIZU_ERR);

  /* the want_* rejections, each with its normative text */
  off = mizu_ix_put_header(buf);
  off += mizu_ix_put_task(buf + off, MIZU_LANG_R, 0, 0);
  off += mizu_ix_put_list_begin(buf + off, 0);   /* a list where code goes */
  assert(mizu_ixt_open(&cur, buf, off, &it, 1) == MIZU_OK);
  assert(mizu_ixt_want_code(&cur, &it) == MIZU_ERR);
  assert(strcmp(mizu_last_error_message(),
                "malformed task stream: the code field is not a string") == 0);

  off = mizu_ix_put_header(buf);
  off += mizu_ix_put_task(buf + off, MIZU_LANG_R, 0, 0);
  off += mizu_ix_put_str(buf + off, NULL, -1);   /* an NA code string */
  assert(mizu_ixt_open(&cur, buf, off, &it, 1) == MIZU_OK);
  assert(mizu_ixt_want_code(&cur, &it) == MIZU_ERR);
  assert(strcmp(mizu_last_error_message(),
                "malformed task stream: the code field is not a string") == 0);

  off = mizu_ix_put_header(buf);
  off += mizu_ix_put_task(buf + off, MIZU_LANG_R, 0, 0);
  off += mizu_ix_put_str(buf + off, "f", 1);
  off += mizu_ix_put_str(buf + off, "x", 1);     /* a string for the list */
  assert(mizu_ixt_open(&cur, buf, off, &it, 1) == MIZU_OK);
  assert(mizu_ixt_want_code(&cur, &it) == MIZU_OK);
  assert(mizu_ixt_want_list(&cur, &it) == MIZU_ERR);
  assert(strcmp(mizu_last_error_message(),
                "malformed task stream: the positional field is not a list") == 0);

  off = mizu_ix_put_header(buf);
  off += mizu_ix_put_task(buf + off, MIZU_LANG_R, 0, 0);
  off += mizu_ix_put_str(buf + off, "f", 1);
  off += mizu_ix_put_list_begin(buf + off, 0);
  off += mizu_ix_put_list_begin(buf + off, 0);   /* a list for the dict */
  assert(mizu_ixt_open(&cur, buf, off, &it, 1) == MIZU_OK);
  assert(mizu_ixt_want_code(&cur, &it) == MIZU_OK);
  assert(mizu_ixt_want_list(&cur, &it) == MIZU_OK);
  assert(mizu_ixt_want_dict(&cur, &it) == MIZU_ERR);
  assert(strcmp(mizu_last_error_message(),
                "malformed task stream: the named field is not a dict") == 0);

  /* a cursor failure inside a want_* carries the cursor's own text */
  off = mizu_ix_put_header(buf);
  off += mizu_ix_put_task(buf + off, MIZU_LANG_R, 0, 0);
  assert(mizu_ixt_open(&cur, buf, off, &it, 1) == MIZU_OK);
  assert(mizu_ixt_want_code(&cur, &it) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "truncated") != NULL);
}

static void tag_table_tests(void) {
  assert(mizu_ix_tag_of(MIZU_TYPE_LGL) == MIZU_IX_TAG_LGLV);
  assert(mizu_ix_tag_of(MIZU_TYPE_INT) == MIZU_IX_TAG_INTV);
  assert(mizu_ix_tag_of(MIZU_TYPE_REAL) == MIZU_IX_TAG_REALV);
  assert(mizu_ix_tag_of(MIZU_TYPE_CPLX) == MIZU_IX_TAG_CPLXV);
  assert(mizu_ix_tag_of(MIZU_TYPE_RAW) == MIZU_IX_TAG_RAWV);
  assert(mizu_ix_tag_of(MIZU_TYPE_INT64) == MIZU_IX_TAG_I64V);
  /* any other input: 0 — MIZU_IX_TAG_NIL, never a vector tag */
  assert(mizu_ix_tag_of(0) == MIZU_IX_TAG_NIL);
  assert(mizu_ix_tag_of(1) == MIZU_IX_TAG_NIL);
  assert(mizu_ix_tag_of(99) == MIZU_IX_TAG_NIL);
  assert(mizu_ix_tag_of(-1) == MIZU_IX_TAG_NIL);
  /* put_vec's refusal tracks the table exactly */
  unsigned char buf[16];
  const int32_t v[1] = { 1 };
  assert(mizu_ix_put_vec(buf, 0, v, 1) == 0);
  assert(mizu_ix_put_vec(NULL, 99, v, 1) == 0);
  assert(mizu_ix_put_vec(buf, MIZU_TYPE_INT, v, 1) == 13);
  assert(buf[0] == MIZU_IX_TAG_INTV);
}

int main(void) {
  open_tests();
  err_task_tests();
  ref_tests();
  done_tests();
  roundtrip_tests();
  utf8_tests();
  err_framer_tests();
  shim_tests();
  tag_table_tests();
  corpus_tests();
  printf("test_interop: OK\n");
  return 0;
}
