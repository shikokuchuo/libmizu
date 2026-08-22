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
#include <mach/mach_time.h>
#include <unistd.h>
#else
#include <time.h>
#include <unistd.h>
#endif

double rei_now(void) {
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

long rei_self_pid(void) {
#ifdef _WIN32
  return (long) GetCurrentProcessId();
#else
  return (long) getpid();
#endif
}

rei_death_watch *rei_death_watch_start(long pid, _Atomic int *flag,
                                       const rei_parker *pk) {
  return rei_death_watch_start2(pid, flag, pk, NULL, NULL);
}
