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
}
