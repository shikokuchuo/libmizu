/* The thread-local error slot: handle-free entry points (region
   create/open, failed channel/pool create/attach) record a portable
   category plus formatted message here, read back via
   rei_last_error_category / rei_last_error_message. Depends only on
   rei.h so the region layer vendors as a unit (shm.c + err_tls.c). */

#include "rei.h"

#include <stdarg.h>
#include <stdio.h>

static _Thread_local rei_errcat tls_cat = REI_ERRCAT_NONE;
static _Thread_local char tls_msg[256];

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
