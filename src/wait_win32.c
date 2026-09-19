/* Windows kernel-wait primitive and death listener. WaitOnAddress is
   process-local, so the parker is a named auto-reset kernel event per
   waiting entity: the epoch compare is not atomic with the sleep, but
   auto-reset stickiness substitutes — a SetEvent with no waiter stays
   signalled and the next wait consumes it. Events are created by the
   region's host before any peer attaches, named from the region name,
   kernel-refcounted (no unlink). The death listener is threadless:
   RegisterWaitForSingleObject on the OS thread pool. */

#include "internal.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>

// Parker -----------------------------------------------------------------------

int mizu_parker_attach(mizu_parker *pk, _Atomic uint32_t *epoch,
                      const char *region_name, int entity, int create) {
  char name[MIZU_NAME_MAX + 16];
  int n = snprintf(name, sizeof(name), "%s.pk.%d", region_name, entity);
  if (n <= 0 || (size_t) n >= sizeof(name)) return -1;
  pk->epoch = epoch;
  pk->event = create ?
    CreateEventA(NULL, FALSE, FALSE, name) :
    OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
  return pk->event != NULL ? 0 : -1;
}

void mizu_parker_detach(mizu_parker *pk) {
  if (pk->event != NULL) CloseHandle((HANDLE) pk->event);
  pk->event = NULL;
  pk->epoch = NULL;
}

int mizu_park(mizu_parker *pk, uint32_t snapshot, long timeout_ms) {
  if (atomic_load_explicit(pk->epoch, memory_order_acquire) != snapshot)
    return MIZU_PARK_WOKEN;
  if (timeout_ms == 0) return MIZU_PARK_TIMEOUT;

  DWORD ms = timeout_ms < 0 ? INFINITE : (DWORD) timeout_ms;
  DWORD r = WaitForSingleObject((HANDLE) pk->event, ms);
  return r == WAIT_TIMEOUT ? MIZU_PARK_TIMEOUT : MIZU_PARK_WOKEN;
}

void mizu_unpark(mizu_parker *pk) {
  atomic_fetch_add_explicit(pk->epoch, 1, memory_order_release);
  SetEvent((HANDLE) pk->event);
}

// Death listener -----------------------------------------------------------------

struct mizu_death_watch_s {
  HANDLE process;
  HANDLE wait;
  _Atomic int *flag;
  mizu_parker pk;
  int has_pk;
  void (*cb)(void *);               /* pure-C death callback (may be NULL) */
  void *cb_arg;
};

static void mizu_dw_fire(struct mizu_death_watch_s *w) {
  atomic_store_explicit(w->flag, 1, memory_order_release);
  if (w->has_pk) mizu_unpark(&w->pk);
  if (w->cb != NULL) w->cb(w->cb_arg);
}

static VOID CALLBACK mizu_dw_cb(PVOID ctx, BOOLEAN timed_out) {
  (void) timed_out;
  mizu_dw_fire((struct mizu_death_watch_s *) ctx);
}

mizu_death_watch *mizu_death_watch_start2(long pid, _Atomic int *flag,
                                        const mizu_parker *pk,
                                        void (*cb)(void *), void *cb_arg) {
  struct mizu_death_watch_s *w = calloc(1, sizeof(*w));
  if (w == NULL) return NULL;
  w->flag = flag;
  if (pk != NULL) {
    w->pk = *pk;
    w->has_pk = 1;
  }
  w->cb = cb;
  w->cb_arg = cb_arg;

  w->process = OpenProcess(SYNCHRONIZE, FALSE, (DWORD) pid);
  if (w->process == NULL) {
    /* pid gone (reaped): fire immediately rather than error — matches the
       POSIX already-dead path. Access denial is a genuine failure. */
    if (GetLastError() == ERROR_INVALID_PARAMETER) {
      mizu_dw_fire(w);
      return w;
    }
    free(w);
    return NULL;
  }

  /* An already-signalled handle (process exited between OpenProcess and
     here) fires the callback immediately. */
  if (!RegisterWaitForSingleObject(&w->wait, w->process, mizu_dw_cb, w,
                                   INFINITE, WT_EXECUTEONLYONCE)) {
    CloseHandle(w->process);
    free(w);
    return NULL;
  }
  return w;
}

void mizu_death_watch_stop(mizu_death_watch *w) {
  /* Blocking unregister: returns only after any in-flight callback has
     completed, so freeing w (and the caller's flag/parker targets) is safe. */
  if (w->wait != NULL) UnregisterWaitEx(w->wait, INVALID_HANDLE_VALUE);
  if (w->process != NULL) CloseHandle(w->process);
  free(w);
}

void mizu_death_listener_teardown(void) {
}

#endif /* _WIN32 */
