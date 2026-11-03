/* The thread-local error slot: handle-free entry points (region
   create/open, failed channel/pool create/attach) record a portable
   category plus formatted message here, read back via
   mizu_last_error_category / mizu_last_error_message. A second slot
   shadows every handle record (mizu_err_shadow, from mizu_err_record),
   tagged with the handle: a thread reading a handle's error gets its own
   last record on that handle, not another thread's, so two threads
   failing verbs on one handle (a split waiter beside a submitter) never
   read each other's text. The two slots are independent — a handle
   record never disturbs a handle-free message read after it, nor the
   reverse. Depends only on mizu.h so the region layer vendors as a unit
   (shm.c + err_tls.c). */

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

/* The handle shadow: the last handle record this thread made, and the
   handle it was for (address + id — a handle reallocated at a dead one's
   address carries a new id, so no stale record outlives its handle). */
static _Thread_local const void *shadow_handle;
static _Thread_local uint64_t shadow_id;
static _Thread_local mizu_errcat shadow_cat = MIZU_ERRCAT_NONE;
static _Thread_local char shadow_msg[256];

/* Shadow a handle record (msg is the handle's own formatted buffer; the
   copy is bounded like the slot). */
MIZU_COLD void mizu_err_shadow(const void *h, uint64_t id, mizu_errcat cat,
                               const char *msg) {
  snprintf(shadow_msg, sizeof(shadow_msg), "%s", msg);
  shadow_cat = cat;
  shadow_handle = h;
  shadow_id = id;
}

/* This thread's shadow of handle h: the message, with *cat_out filled,
   when this thread's last handle record was h's; NULL otherwise (the
   caller falls back to the handle's own slot). */
const char *mizu_err_shadow_get(const void *h, uint64_t id,
                                mizu_errcat *cat_out) {
  if (h == NULL || shadow_handle != h || shadow_id != id) return NULL;
  *cat_out = shadow_cat;
  return shadow_msg;
}

mizu_errcat mizu_last_error_category(void) {
  return tls_cat;
}

const char *mizu_last_error_message(void) {
  return tls_msg;
}
