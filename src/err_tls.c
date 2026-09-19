/* The thread-local error slot: handle-free entry points (region
   create/open, failed channel/pool create/attach) record a portable
   category plus formatted message here, read back via
   mizu_last_error_category / mizu_last_error_message. Depends only on
   mizu.h so the region layer vendors as a unit (shm.c + err_tls.c). */

#include "mizu.h"

#include <stdarg.h>
#include <stdio.h>

/* Local copy of internal.h's cold annotation (this TU vendors with the
   region layer and cannot include internal.h). */
#if defined(_MSC_VER)
#  define MIZU_COLD
#else
#  define MIZU_COLD __attribute__((cold))
#endif

static _Thread_local mizu_errcat tls_cat = MIZU_ERRCAT_NONE;
static _Thread_local char tls_msg[256];

MIZU_COLD void mizu_err_record_tls(mizu_errcat cat, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tls_msg, sizeof(tls_msg), fmt, ap);
  va_end(ap);
  tls_cat = cat;
}

mizu_errcat mizu_last_error_category(void) {
  return tls_cat;
}

const char *mizu_last_error_message(void) {
  return tls_msg;
}
