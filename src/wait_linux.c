/* Linux kernel-wait primitive and death listener. The parker sleeps on
   the epoch word with FUTEX_WAIT without FUTEX_PRIVATE_FLAG (cross-process
   futexes key on the physical page). The death listener is one dormant
   per-process thread in epoll_wait over pidfd_open descriptors
   (kernel >= 5.3): no periodic work, asleep until a death event or
   teardown. */

/* _GNU_SOURCE: glibc hides O_CLOEXEC, MAP_POPULATE, pidfd_open, ...
   under strict -std=c11; define before any system header. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#include "internal.h"

#ifdef __linux__

#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

// Parker -----------------------------------------------------------------------

int rei_parker_attach(rei_parker *pk, _Atomic uint32_t *epoch,
                      const char *region_name, int entity, int create) {
  (void) region_name; (void) entity; (void) create;
  pk->epoch = epoch;
  return 0;
}

void rei_parker_detach(rei_parker *pk) {
  pk->epoch = NULL;
}

int rei_park(rei_parker *pk, uint32_t snapshot, long timeout_ms) {
  if (atomic_load_explicit(pk->epoch, memory_order_acquire) != snapshot)
    return REI_PARK_WOKEN;
  if (timeout_ms == 0) return REI_PARK_TIMEOUT;

  /* Always timed: an untimed FUTEX_WAIT returns ERESTARTSYS on signal and is
     silently restarted under SA_RESTART; only the timed wait's
     ERESTART_RESTARTBLOCK is converted to EINTR when a handler runs. */
  long ms = timeout_ms < 0 ? REI_PARK_NOMINAL_MS : timeout_ms;
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  long r = syscall(SYS_futex, (uint32_t *) pk->epoch, FUTEX_WAIT, snapshot,
                   &ts, NULL, 0);
  if (r == 0) return REI_PARK_WOKEN;
  switch (errno) {
  case ETIMEDOUT: return REI_PARK_TIMEOUT;
  case EINTR:     return REI_PARK_INTR;
  default:        return REI_PARK_WOKEN;    /* EAGAIN: epoch already moved */
  }
}

void rei_unpark(rei_parker *pk) {
  atomic_fetch_add_explicit(pk->epoch, 1, memory_order_release);
  syscall(SYS_futex, (uint32_t *) pk->epoch, FUTEX_WAKE, 1, NULL, NULL, 0);
}

// Death listener -----------------------------------------------------------------

enum { REI_DW_ACTIVE, REI_DW_FIRED, REI_DW_STOPPED };

struct rei_death_watch_s {
  int pidfd;
  int state;
  int listed;                       /* on the registry; freed by the thread */
  _Atomic int *flag;
  rei_parker pk;
  int has_pk;
  void (*cb)(void *);               /* pure-C death callback (may be NULL) */
  void *cb_arg;
  struct rei_death_watch_s *prev, *next;
};

static pthread_mutex_t rei_dl_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t rei_dl_thread;
static int rei_dl_running = 0;
static int rei_dl_shutdown = 0;
static int rei_dl_epfd = -1;
static int rei_dl_evfd = -1;
static pid_t rei_dl_pid = 0;
static int rei_dl_atexit_set = 0;
static struct rei_death_watch_s *rei_dl_watches = NULL;

/* A forked child inherits the parent's bookkeeping but not its thread
   (and possibly a mutex held at fork time): abandon everything and start
   fresh. Registered watches are the parent's — leaked in the child by
   design. Callers are single-threaded here (only the forking thread
   survives fork). */
static void rei_dl_fork_guard(void) {
  if (rei_dl_pid == 0 || rei_dl_pid == getpid()) return;
  if (rei_dl_epfd >= 0) close(rei_dl_epfd);
  if (rei_dl_evfd >= 0) close(rei_dl_evfd);
  rei_dl_epfd = rei_dl_evfd = -1;
  rei_dl_running = 0;
  rei_dl_shutdown = 0;
  rei_dl_watches = NULL;
  rei_dl_pid = 0;
  pthread_mutex_init(&rei_dl_mu, NULL);
}

static void rei_dw_fire(struct rei_death_watch_s *w) {
  atomic_store_explicit(w->flag, 1, memory_order_release);
  if (w->has_pk) rei_unpark(&w->pk);
  if (w->cb != NULL) w->cb(w->cb_arg);
}

static void *rei_dl_main(void *arg) {
  (void) arg;
  struct epoll_event evs[16];
  for (;;) {
    int n = epoll_wait(rei_dl_epfd, evs, 16, -1);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    pthread_mutex_lock(&rei_dl_mu);
    for (int i = 0; i < n; i++) {
      if (evs[i].data.ptr == NULL) {          /* eventfd tick */
        uint64_t v;
        ssize_t r = read(rei_dl_evfd, &v, sizeof(v));
        (void) r;
        continue;
      }
      struct rei_death_watch_s *w = evs[i].data.ptr;
      if (w->state == REI_DW_ACTIVE) {
        rei_dw_fire(w);
        epoll_ctl(rei_dl_epfd, EPOLL_CTL_DEL, w->pidfd, NULL);
        close(w->pidfd);
        w->pidfd = -1;
        w->state = REI_DW_FIRED;
      }
    }
    /* Reap stopped watches. Only this thread frees listed watches, so an
       event fetched in this batch can never reference an already-freed one. */
    for (struct rei_death_watch_s *w = rei_dl_watches, *next; w; w = next) {
      next = w->next;
      if (w->state == REI_DW_STOPPED) {
        if (w->pidfd >= 0) {
          epoll_ctl(rei_dl_epfd, EPOLL_CTL_DEL, w->pidfd, NULL);
          close(w->pidfd);
        }
        if (w->prev) w->prev->next = w->next; else rei_dl_watches = w->next;
        if (w->next) w->next->prev = w->prev;
        free(w);
      }
    }
    int shut = rei_dl_shutdown;
    pthread_mutex_unlock(&rei_dl_mu);
    if (shut) break;
  }
  return NULL;
}

/* Callers hold rei_dl_mu. */
static int rei_dl_ensure_started(void) {
  if (rei_dl_running) return 0;
  rei_dl_epfd = epoll_create1(EPOLL_CLOEXEC);
  if (rei_dl_epfd < 0) return -1;
  rei_dl_evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (rei_dl_evfd < 0) goto fail;
  struct epoll_event ev = {.events = EPOLLIN, .data = {.ptr = NULL}};
  if (epoll_ctl(rei_dl_epfd, EPOLL_CTL_ADD, rei_dl_evfd, &ev) != 0) goto fail;
  if (pthread_create(&rei_dl_thread, NULL, rei_dl_main, NULL) != 0) goto fail;
  rei_dl_running = 1;
  rei_dl_pid = getpid();
  /* Join at exit(): normal shutdown never unloads the package, and a thread
     still live at exit reports its TLS as possibly lost under valgrind.
     Registered once per DSO lifetime; glibc runs it early on dlclose. */
  if (!rei_dl_atexit_set) {
    rei_dl_atexit_set = 1;
    atexit(rei_death_listener_teardown);
  }
  return 0;

fail:
  if (rei_dl_epfd >= 0) close(rei_dl_epfd);
  if (rei_dl_evfd >= 0) close(rei_dl_evfd);
  rei_dl_epfd = rei_dl_evfd = -1;
  return -1;
}

static void rei_dl_poke(void) {
  uint64_t one = 1;
  ssize_t r = write(rei_dl_evfd, &one, sizeof(one));
  (void) r;
}

rei_death_watch *rei_death_watch_start2(long pid, _Atomic int *flag,
                                        const rei_parker *pk,
                                        void (*cb)(void *), void *cb_arg) {
  rei_dl_fork_guard();

  struct rei_death_watch_s *w = calloc(1, sizeof(*w));
  if (w == NULL) return NULL;
  w->flag = flag;
  if (pk != NULL) {
    w->pk = *pk;
    w->has_pk = 1;
  }
  w->cb = cb;
  w->cb_arg = cb_arg;

  int pidfd = (int) syscall(SYS_pidfd_open, (pid_t) pid, 0);
  if (pidfd < 0) {
    if (errno == ESRCH) {              /* already dead: fire immediately */
      w->pidfd = -1;
      w->state = REI_DW_FIRED;
      rei_dw_fire(w);
      return w;
    }
    free(w);
    return NULL;
  }

  pthread_mutex_lock(&rei_dl_mu);
  if (rei_dl_ensure_started() != 0) {
    pthread_mutex_unlock(&rei_dl_mu);
    close(pidfd);
    free(w);
    return NULL;
  }
  w->pidfd = pidfd;
  w->state = REI_DW_ACTIVE;
  w->listed = 1;
  w->next = rei_dl_watches;
  if (rei_dl_watches) rei_dl_watches->prev = w;
  rei_dl_watches = w;
  struct epoll_event ev = {.events = EPOLLIN, .data = {.ptr = w}};
  if (epoll_ctl(rei_dl_epfd, EPOLL_CTL_ADD, pidfd, &ev) != 0) {
    if (w->next) w->next->prev = NULL;
    rei_dl_watches = w->next;
    pthread_mutex_unlock(&rei_dl_mu);
    close(pidfd);
    free(w);
    return NULL;
  }
  pthread_mutex_unlock(&rei_dl_mu);
  return w;
}

void rei_death_watch_stop(rei_death_watch *w) {
  rei_dl_fork_guard();
  if (w->listed && rei_dl_running) {
    /* Taking the mutex synchronizes with a fire in flight: after it is
       released, the thread never touches this watch's flag/parker again. */
    pthread_mutex_lock(&rei_dl_mu);
    w->state = REI_DW_STOPPED;
    pthread_mutex_unlock(&rei_dl_mu);
    rei_dl_poke();
    return;
  }
  if (w->pidfd >= 0) close(w->pidfd);
  free(w);
}

void rei_death_listener_teardown(void) {
  rei_dl_fork_guard();
  if (!rei_dl_running) return;
  pthread_mutex_lock(&rei_dl_mu);
  rei_dl_shutdown = 1;
  pthread_mutex_unlock(&rei_dl_mu);
  rei_dl_poke();
  pthread_join(rei_dl_thread, NULL);
  /* Thread joined: single-threaded from here. Close per-watch fds but leave
     the structs for their handles' finalizers, which free unlisted watches. */
  for (struct rei_death_watch_s *w = rei_dl_watches, *next; w; w = next) {
    next = w->next;
    if (w->state == REI_DW_STOPPED) {
      if (w->pidfd >= 0) close(w->pidfd);
      free(w);
    } else {
      if (w->pidfd >= 0) close(w->pidfd);
      w->pidfd = -1;
      w->listed = 0;
    }
  }
  rei_dl_watches = NULL;
  close(rei_dl_epfd);
  close(rei_dl_evfd);
  rei_dl_epfd = rei_dl_evfd = -1;
  rei_dl_running = 0;
  rei_dl_shutdown = 0;
  rei_dl_pid = 0;
}

#endif /* __linux__ */
