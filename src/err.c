/* Error records: every MIZU_ERR carries a portable category plus a
   formatted message. Handle verbs record on the handle (valid until the
   next call on it); handle-free entry points use the thread-local slot
   in err_tls.c. Bindings map the category onto their own error
   hierarchy and may re-compose the message with their own context. */

#include "internal.h"

#include <stdarg.h>
#include <stdio.h>

MIZU_COLD void mizu_err_record(mizu_handle *h, mizu_errcat cat, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(h->errmsg, sizeof(h->errmsg), fmt, ap);
  va_end(ap);
  h->errcat = cat;
  mizu_err_shadow(h, h->id, cat, h->errmsg);   /* this thread's own copy */
}

/* Handle ids: a process-wide counter, so the error shadow's tag never
   aliases across a handle's reallocation. */
uint64_t mizu_handle_next_id(void) {
  static _Atomic uint64_t next = 0;
  return atomic_fetch_add_explicit(&next, 1, memory_order_relaxed) + 1;
}

/* The region-create failure record (the channel/pool creates, the public
   region create, the spill layer): the describe + "cannot create ..."
   composition that used to be open-coded at each site. */
MIZU_COLD void mizu_err_record_create(mizu_errcat cat, const char *what,
                                     uint64_t size) {
  const char *summary, *hint;
  mizu_err_describe(cat, &summary, &hint);
  mizu_err_record_tls(cat, "cannot create %s (%llu bytes): %s%s%s", what,
                     (unsigned long long) size, summary,
                     hint[0] != '\0' ? ". " : "", hint);
}
