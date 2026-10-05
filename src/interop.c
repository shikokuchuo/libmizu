/* interop.c — the validating pull cursor for the interchange stream (the
   'I' wire format; DESIGN.md's Interchange codec section is the byte
   authority). The morsel.c / stage_raw.c precedent: the protocol half
   every binding would otherwise duplicate lives in the core. This is a
   library module bindings call, not a transport path — the transport
   still never interprets a slot payload on its own.

   The cursor owns: the magic/version checks (an unknown version declines
   as "the peer uses a newer format"), bounds, the depth cap, UTF-8
   validity, the container arity accounting (a task is one element of
   kind-determined arity, so list[task, x] has count 2), the attr-frame
   position rules (no attr wrapping an attr; the attributes are one 0x0d
   dict item), the top-level-only err rule, the reserved-flag rejections
   (format words reject what they do not know), and the informative
   unknown-tag / unknown-kind declines. Errors record in the thread-local
   slot (mizu_last_error_message) and latch: every later call fails.

   What it never does: allocate (a dict's key uniqueness and every shape
   check stay with the builder), interpret item values, or hold a key
   set. The emit half is the mizu_ix_put_* dual-form helpers in
   mizu_ext.h. */

#include <string.h>

#include "internal.h"

// Small reads -------------------------------------------------------------------

static int ix_has(const mizu_ix *cur, size_t n) {
  return (size_t) (cur->end - cur->p) >= n;
}

#define IX_FAIL(cur, ...)                                     \
  do {                                                        \
    if (!(cur)->err) {                                        \
      mizu_err_record_tls(MIZU_ERRCAT_OTHER, __VA_ARGS__);    \
      (cur)->err = 1;                                         \
    }                                                         \
    return MIZU_ERR;                                          \
  } while (0)

/* The UTF-8 rule is the registry's: mizu_ext.h's mizu_ix_utf8_valid
   (dual-form there), so the stream's check and the bindings' string rules
   have one implementation. */

/* One item completed at the innermost level: the innermost frame's count
   drops, and a frame that empties pops — which itself completes one item
   of the parent frame, so the completion cascades. With no frame open,
   the root value has completed. A container begin completes nothing until
   its own frame pops, so the frame stack is exactly the reader's nesting
   depth (what the depth cap bounds). */
static void ix_complete(mizu_ix *cur) {
  while (cur->depth != 0) {
    if (--cur->stack[cur->depth - 1].remaining != 0) return;
    cur->depth--;
  }
  cur->done = 1;
}

/* A bare string (an strv element, or a dict key when key is set): i32
   length, -1 = NA where the position allows it, then UTF-8 bytes. A
   complete item at the innermost level. */
static mizu_status ix_string(mizu_ix *cur, mizu_ix_item *it, int key) {
  if (!ix_has(cur, 4))
    IX_FAIL(cur, "truncated interop stream");
  int32_t len;
  memcpy(&len, cur->p, 4);
  cur->p += 4;
  if (len < -1)
    IX_FAIL(cur, "malformed interop stream: a string length below -1");
  if (len == -1) {
    if (key)
      IX_FAIL(cur, "malformed interop stream: a dict key has length -1");
    it->na = 1;
  } else {
    if ((uint64_t) (uint32_t) len > (uint64_t) (cur->end - cur->p))
      IX_FAIL(cur, "truncated interop stream");
    if (!mizu_ix_utf8_valid(cur->p, (size_t) len))
      IX_FAIL(cur, "malformed interop stream: invalid UTF-8");
    it->ptr = cur->p;
    it->len = (uint64_t) len;
    cur->p += len;
  }
  it->kind = MIZU_IX_STR;
  it->key = key;
  ix_complete(cur);
  return MIZU_OK;
}

static mizu_status ix_push(mizu_ix *cur, uint32_t kind, uint64_t n) {
  if (cur->depth == MIZU_IX_DEPTH_MAX)
    IX_FAIL(cur, "interop stream exceeds the depth cap (64)");
  cur->stack[cur->depth].kind = kind;
  cur->stack[cur->depth].remaining = n;
  cur->depth++;
  return MIZU_OK;
}

/* The task kind registry's arity column (DESIGN.md): kinds 0 and 1
   (qualified name, source) and 2 (runner) all carry three fields. Kind
   values are append-only. */
static const uint64_t ix_task_arity[] = { 3, 3, 3 };

// The cursor ---------------------------------------------------------------------

mizu_status mizu_ix_open(mizu_ix *cur, const void *buf, size_t len) {
  memset(cur, 0, sizeof *cur);
  const unsigned char *b = (const unsigned char *) buf;
  cur->p = b;
  cur->end = b + len;
  if (len < 2 || b[0] != MIZU_INTEROP_MAGIC)
    IX_FAIL(cur, "malformed interop stream: bad magic");
  if (b[1] != MIZU_IX_VERSION)
    IX_FAIL(cur, "interop format version 0x%02X — the peer uses a newer format",
            (unsigned) b[1]);
  cur->p = b + 2;
  return MIZU_OK;
}

mizu_status mizu_ix_next(mizu_ix *cur, mizu_ix_item *it) {
  if (cur->err) return MIZU_ERR;
  memset(it, 0, sizeof *it);

  /* bare-string positions: an strv element, or a dict key (a dict's
     remaining counts keys and values, so keys sit at even counts) */
  if (cur->depth != 0) {
    const uint32_t fk = cur->stack[cur->depth - 1].kind;
    const uint64_t rem = cur->stack[cur->depth - 1].remaining;
    if (fk == MIZU_IX_STRV) return ix_string(cur, it, 0);
    if (fk == MIZU_IX_DICT && (rem & 1) == 0) return ix_string(cur, it, 1);
  }

  if (!ix_has(cur, 1))
    IX_FAIL(cur, "truncated interop stream");
  if (cur->depth == 0 && cur->done)
    IX_FAIL(cur, "malformed interop stream: bytes past the one value");
  const uint32_t tag = cur->p[0];

  /* the attr-frame position rules: the value is any tag but attr (R has
     one attribute set per object), the attributes exactly one dict. The
     frame's remaining is 2 until the value completes, then 1. */
  if (cur->depth != 0 && cur->stack[cur->depth - 1].kind == MIZU_IX_ATTR) {
    const uint64_t rem = cur->stack[cur->depth - 1].remaining;
    if (rem == 2 && tag == MIZU_IX_TAG_ATTR)
      IX_FAIL(cur, "malformed interop stream: an attr wraps an attr");
    if (rem == 1 && tag != MIZU_IX_TAG_DICT)
      IX_FAIL(cur,
              "malformed interop stream: the attr attributes are not a dict");
  }
  /* the err tag is a value, legal at a stream's top level only */
  if (tag == MIZU_IX_TAG_ERR && cur->depth != 0)
    IX_FAIL(cur, "malformed interop stream: a nested err tag");

  cur->p++;

  switch (tag) {
  case MIZU_IX_TAG_NIL:
    it->kind = MIZU_IX_NIL;
    ix_complete(cur);
    break;
  case MIZU_IX_TAG_LGL1: {
    if (!ix_has(cur, 1))
      IX_FAIL(cur, "truncated interop stream");
    const uint32_t v = cur->p[0];
    if (v > 2)
      IX_FAIL(cur, "malformed interop stream: an lgl1 value past 2");
    cur->p++;
    it->kind = MIZU_IX_LGL;
    it->u64[0] = v;
    ix_complete(cur);
    break;
  }
  case MIZU_IX_TAG_INT:
    if (!ix_has(cur, 8))
      IX_FAIL(cur, "truncated interop stream");
    it->kind = MIZU_IX_INT;
    memcpy(&it->u64[0], cur->p, 8);
    cur->p += 8;
    ix_complete(cur);
    break;
  case MIZU_IX_TAG_REAL:
    if (!ix_has(cur, 8))
      IX_FAIL(cur, "truncated interop stream");
    it->kind = MIZU_IX_REAL;
    memcpy(&it->u64[0], cur->p, 8);
    cur->p += 8;
    ix_complete(cur);
    break;
  case MIZU_IX_TAG_CPLX:
    if (!ix_has(cur, 16))
      IX_FAIL(cur, "truncated interop stream");
    it->kind = MIZU_IX_CPLX;
    memcpy(&it->u64[0], cur->p, 8);
    memcpy(&it->u64[1], cur->p + 8, 8);
    cur->p += 16;
    ix_complete(cur);
    break;
  case MIZU_IX_TAG_STR: {
    if (!ix_has(cur, 4))
      IX_FAIL(cur, "truncated interop stream");
    int32_t len;
    memcpy(&len, cur->p, 4);
    cur->p += 4;
    if (len < -1)
      IX_FAIL(cur, "malformed interop stream: a string length below -1");
    if (len == -1) {
      it->na = 1;
    } else {
      if ((uint64_t) (uint32_t) len > (uint64_t) (cur->end - cur->p))
        IX_FAIL(cur, "truncated interop stream");
      if (!mizu_ix_utf8_valid(cur->p, (size_t) len))
        IX_FAIL(cur, "malformed interop stream: invalid UTF-8");
      it->ptr = cur->p;
      it->len = (uint64_t) len;
      cur->p += len;
    }
    it->kind = MIZU_IX_STR1;
    ix_complete(cur);
    break;
  }
  case MIZU_IX_TAG_BYTES: {
    if (!ix_has(cur, 8))
      IX_FAIL(cur, "truncated interop stream");
    uint64_t count;
    memcpy(&count, cur->p, 8);
    cur->p += 8;
    if (count > (uint64_t) (cur->end - cur->p))
      IX_FAIL(cur, "truncated interop stream");
    it->kind = MIZU_IX_BYTES;
    it->type = MIZU_TYPE_RAW;
    it->ptr = cur->p;
    it->count = count;
    cur->p += count;
    ix_complete(cur);
    break;
  }
  case MIZU_IX_TAG_LGLV:
  case MIZU_IX_TAG_INTV:
  case MIZU_IX_TAG_REALV:
  case MIZU_IX_TAG_CPLXV:
  case MIZU_IX_TAG_RAWV:
  case MIZU_IX_TAG_I64V: {
    uint32_t type;
    switch (tag) {
    case MIZU_IX_TAG_LGLV:  type = MIZU_TYPE_LGL;   break;
    case MIZU_IX_TAG_INTV:  type = MIZU_TYPE_INT;   break;
    case MIZU_IX_TAG_REALV: type = MIZU_TYPE_REAL;  break;
    case MIZU_IX_TAG_CPLXV: type = MIZU_TYPE_CPLX;  break;
    case MIZU_IX_TAG_RAWV:  type = MIZU_TYPE_RAW;   break;
    default:                type = MIZU_TYPE_INT64; break;
    }
    if (!ix_has(cur, 8))
      IX_FAIL(cur, "truncated interop stream");
    uint64_t count;
    memcpy(&count, cur->p, 8);
    cur->p += 8;
    const size_t elt = mizu_type_elt_size((int) type);
    if (count > (uint64_t) (cur->end - cur->p) / elt)
      IX_FAIL(cur, "truncated interop stream");
    it->kind = MIZU_IX_VEC;
    it->type = type;
    it->ptr = cur->p;
    it->count = count;
    cur->p += (size_t) count * elt;
    ix_complete(cur);
    break;
  }
  case MIZU_IX_TAG_STRV:
  case MIZU_IX_TAG_LIST:
  case MIZU_IX_TAG_DICT: {
    if (!ix_has(cur, 8))
      IX_FAIL(cur, "truncated interop stream");
    uint64_t count;
    memcpy(&count, cur->p, 8);
    cur->p += 8;
    /* the pre-allocation bound: every element costs at least one byte
       (a tag, or an i32 length), so count <= remaining */
    if (count > (uint64_t) (cur->end - cur->p))
      IX_FAIL(cur, "truncated interop stream");
    it->kind = tag == MIZU_IX_TAG_STRV ? MIZU_IX_STRV :
               tag == MIZU_IX_TAG_LIST ? MIZU_IX_LIST : MIZU_IX_DICT;
    it->count = count;
    if (count == 0) {
      ix_complete(cur);
    } else if (ix_push(cur, it->kind,
                       tag == MIZU_IX_TAG_DICT ? 2 * count : count) !=
               MIZU_OK) {
      return MIZU_ERR;
    }
    break;
  }
  case MIZU_IX_TAG_ATTR:
    it->kind = MIZU_IX_ATTR;
    if (ix_push(cur, MIZU_IX_ATTR, 2) != MIZU_OK) return MIZU_ERR;
    break;
  case MIZU_IX_TAG_ERR: {
    if (!ix_has(cur, 2))
      IX_FAIL(cur, "truncated interop stream");
    uint16_t flags;
    memcpy(&flags, cur->p, 2);
    cur->p += 2;
    if ((flags & ~(uint16_t) 1u) != 0)
      IX_FAIL(cur, "malformed interop stream: unknown err flag bits set");
    it->kind = MIZU_IX_ERR;
    it->err_flags = flags;
    if ((flags & 1u) != 0) {
      if (!ix_has(cur, 8))
        IX_FAIL(cur, "truncated interop stream");
      memcpy(&it->err_index, cur->p, 8);
      cur->p += 8;
    }
    for (int i = 0; i < 3; i++) {
      if (!ix_has(cur, 4))
        IX_FAIL(cur, "truncated interop stream");
      int32_t len;
      memcpy(&len, cur->p, 4);
      cur->p += 4;
      if (len < 0)
        IX_FAIL(cur, "malformed interop stream: an err string is NA");
      if ((uint64_t) (uint32_t) len > (uint64_t) (cur->end - cur->p))
        IX_FAIL(cur, "truncated interop stream");
      if (!mizu_ix_utf8_valid(cur->p, (size_t) len))
        IX_FAIL(cur, "malformed interop stream: invalid UTF-8");
      it->err_str[i].ptr = cur->p;
      it->err_str[i].len = (uint64_t) len;
      cur->p += len;
    }
    ix_complete(cur);
    break;
  }
  case MIZU_IX_TAG_TASK: {
    if (!ix_has(cur, 12))
      IX_FAIL(cur, "truncated interop stream");
    const uint32_t target = cur->p[0];
    const uint32_t kind = cur->p[1];
    uint16_t flags;
    memcpy(&flags, cur->p + 2, 2);
    memcpy(&it->u64[0], cur->p + 4, 8);
    cur->p += 12;
    if (flags != 0)
      IX_FAIL(cur, "malformed interop stream: unknown task flag bits set");
    if (kind >= sizeof ix_task_arity / sizeof ix_task_arity[0])
      IX_FAIL(cur, "unknown task kind 0x%02X — the peer uses a newer format",
              kind);
    it->kind = MIZU_IX_TASK;
    it->target = target;
    it->task_kind = kind;
    it->count = ix_task_arity[kind];
    if (ix_push(cur, MIZU_IX_TASK, it->count) != MIZU_OK) return MIZU_ERR;
    break;
  }
  case MIZU_IX_TAG_REF: {
    /* u8 length (1..255) + identifier bytes; the content rules stay with
       the resolver — the cursor checks the length and bounds only */
    if (!ix_has(cur, 1))
      IX_FAIL(cur, "truncated interop stream");
    const uint32_t nlen = cur->p[0];
    cur->p++;
    if (nlen == 0)
      IX_FAIL(cur, "malformed interop stream: a ref identifier is empty");
    if (!ix_has(cur, nlen))
      IX_FAIL(cur, "truncated interop stream");
    it->kind = MIZU_IX_REF;
    it->ptr = cur->p;
    it->len = nlen;
    cur->p += nlen;
    ix_complete(cur);
    break;
  }
  default:
    IX_FAIL(cur, "unsupported interop tag 0x%02X — the peer uses a newer format",
            tag);
  }

  return MIZU_OK;
}

mizu_status mizu_ix_end(mizu_ix *cur) {
  if (cur->err) return MIZU_ERR;
  if (cur->depth != 0 || !cur->done)
    IX_FAIL(cur, "truncated interop stream");
  if (cur->p != cur->end)
    IX_FAIL(cur, "malformed interop stream: bytes past the one value");
  return MIZU_OK;
}

// The task-stream decode shim -------------------------------------------------------

/* The per-field shape checks every task-stream decode shares (DESIGN.md's
   byte-shape helper registry): the field tags are the builder's check,
   not the cursor's, and the shim is that check once — exported only, as
   it wraps the cursor and records through the TLS slot, so the texts are
   byte-identical across bindings by construction. The item pull itself
   stays mizu_ix_next; the binding-side pieces (the target-byte misroute
   guard, name resolution, the key set, the per-kind call construction)
   never come here. */

mizu_status mizu_ixt_open(mizu_ix *cur, const void *buf, size_t len,
                          mizu_ix_item *item, int max_kind) {
  if (mizu_ix_open(cur, buf, len) != MIZU_OK) return MIZU_ERR;
  if (mizu_ix_next(cur, item) != MIZU_OK) return MIZU_ERR;
  if (item->kind != MIZU_IX_TASK)
    IX_FAIL(cur, "malformed task stream: no task tag");
  if (item->task_kind > (uint32_t) max_kind)
    IX_FAIL(cur, "unsupported task kind 0x%02X", item->task_kind);
  return MIZU_OK;
}

mizu_status mizu_ixt_want_code(mizu_ix *cur, mizu_ix_item *item) {
  if (mizu_ix_next(cur, item) != MIZU_OK) return MIZU_ERR;
  if (item->kind != MIZU_IX_STR1 || item->na)
    IX_FAIL(cur, "malformed task stream: the code field is not a string");
  return MIZU_OK;
}

mizu_status mizu_ixt_want_list(mizu_ix *cur, mizu_ix_item *item) {
  if (mizu_ix_next(cur, item) != MIZU_OK) return MIZU_ERR;
  if (item->kind != MIZU_IX_LIST)
    IX_FAIL(cur, "malformed task stream: the positional field is not a list");
  return MIZU_OK;
}

mizu_status mizu_ixt_want_dict(mizu_ix *cur, mizu_ix_item *item) {
  if (mizu_ix_next(cur, item) != MIZU_OK) return MIZU_ERR;
  if (item->kind != MIZU_IX_DICT)
    IX_FAIL(cur, "malformed task stream: the named field is not a dict");
  return MIZU_OK;
}
