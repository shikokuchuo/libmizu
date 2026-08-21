/* Error records: every REI_ERR carries a portable category plus a
   formatted message. Handle verbs record on the handle (valid until the
   next call on it); handle-free entry points use a thread-local slot.
   Bindings map the category onto their own error hierarchy and may
   re-compose the message with their own context. */

#include "internal.h"

#include <stdarg.h>
#include <stdio.h>

static _Thread_local rei_errcat tls_cat = REI_ERRCAT_NONE;
static _Thread_local char tls_msg[256];

void rei_err_record(rei_handle *h, rei_errcat cat, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(h->errmsg, sizeof(h->errmsg), fmt, ap);
  va_end(ap);
  h->errcat = cat;
}

void rei_err_record_tls(rei_errcat cat, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tls_msg, sizeof(tls_msg), fmt, ap);
  va_end(ap);
  tls_cat = cat;
}

rei_errcat rei_last_error_category(void) {
  return tls_cat;
}

const char *rei_last_error_message(void) {
  return tls_msg;
}
