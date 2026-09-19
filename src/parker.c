/* Monotonic clock and current-pid reads for the spin/wait machinery.
   The park/unpark primitives live in wait_{linux,macos,win32}.c. */

/* _GNU_SOURCE: glibc hides O_CLOEXEC, MAP_POPULATE, pidfd_open, ...
   under strict -std=c11; define before any system header. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#include "internal.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#  include <mach/mach_time.h>
#  include <pthread.h>
#  include <unistd.h>
#else
#  include <pthread.h>
#  include <time.h>
#  include <unistd.h>
#endif

double mizu_now(void) {
#ifdef _WIN32
  /* QueryPerformanceCounter, not GetTickCount64: µs-scale batch timing
     is blind at ~15.6 ms tick granularity. QPF is constant after boot;
     the atomic makes a racing double-init defined (identical values). */
  static _Atomic int64_t freq;
  int64_t f = atomic_load_explicit(&freq, memory_order_relaxed);
  if (f == 0) {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    f = li.QuadPart;
    atomic_store_explicit(&freq, f, memory_order_relaxed);
  }
  LARGE_INTEGER count;
  QueryPerformanceCounter(&count);
  return (double) count.QuadPart / (double) f;
#elif defined(__APPLE__)
  /* Call the commpage export directly: CLOCK_MONOTONIC wraps
     mach_absolute_time in several libsystem frames, and in profiles the
     wrapper is the cost. The timebase is constant after boot; the atomic
     makes a racing double-init defined (identical values). */
  static _Atomic double tick_ns;
  double t = atomic_load_explicit(&tick_ns, memory_order_relaxed);
  if (t == 0) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    t = (double) tb.numer / (double) tb.denom;
    atomic_store_explicit(&tick_ns, t, memory_order_relaxed);
  }
  return (double) mach_absolute_time() * t / 1e9;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
#endif
}

#ifndef _WIN32
static _Atomic long mizu_pid_cache;

static void mizu_pid_child(void) {
  atomic_store_explicit(&mizu_pid_cache, 0, memory_order_relaxed);
}

static void mizu_pid_atefork(void) {
  pthread_atfork(NULL, NULL, mizu_pid_child);
}
#endif

long mizu_self_pid(void) {
#ifdef _WIN32
  return (long) GetCurrentProcessId();
#else
  /* getpid is a real syscall (glibc dropped its cache in 2.25) and the
     fork guards call it per verb — several us per pool round trip on
     Linux. Cache it; a pthread_atfork child handler zeroes the cache so
     a forked child re-reads. fork() through the libc wrapper runs the
     handlers (R's fork does); a raw-clone child is out of scope, as it
     is for every atfork guard in the process. */
  static pthread_once_t pid_once = PTHREAD_ONCE_INIT;
  long pid = atomic_load_explicit(&mizu_pid_cache, memory_order_relaxed);
  if (pid == 0) {
    pthread_once(&pid_once, mizu_pid_atefork);
    pid = (long) getpid();
    atomic_store_explicit(&mizu_pid_cache, pid, memory_order_relaxed);
  }
  return pid;
#endif
}

mizu_death_watch *mizu_death_watch_start(long pid, _Atomic int *flag,
                                       const mizu_parker *pk) {
  return mizu_death_watch_start2(pid, flag, pk, NULL, NULL);
}
