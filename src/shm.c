/* The shared-memory region layer: create/map/unlink over POSIX shm or
   Win32 file mappings. */

/* _GNU_SOURCE: glibc hides O_CLOEXEC, MAP_POPULATE, pidfd_open, ...
   under strict -std=c11; define before any system header. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"

/* splitmix64 finalizer seeding the region-name counter. From a fixed
   origin, a process reusing a dead creator's PID would regenerate its
   names and collide with its orphans (REI_ERRCAT_EXISTS). Spread
   suffices — not cryptographic. Fork needs no guard: names embed the
   live PID. */
static unsigned int rei_counter_seed(uint64_t x) {
  x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27; x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return (unsigned int) x;
}

// Platform-specific SHM implementations --------------------------------------

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define REI_HINT_NOSPACE \
  "Free disk space backing the system paging file."

static int rei_err_classify(long code) {
  switch ((DWORD) code) {
  case ERROR_DISK_FULL:
    return REI_ERRCAT_NOSPACE;
  case ERROR_NOT_ENOUGH_MEMORY:
  case ERROR_OUTOFMEMORY:
  case ERROR_COMMITMENT_LIMIT:
  case ERROR_NO_SYSTEM_RESOURCES:
    return REI_ERRCAT_NOMEMORY;
  default:
    return REI_ERRCAT_OTHER;
  }
}

static size_t rei_region_name(char *name, size_t size, unsigned int pid) {
  static _Atomic unsigned int counter;   /* 0 = unseeded; seeded once */
  if (atomic_load_explicit(&counter, memory_order_relaxed) == 0) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    unsigned int seed = rei_counter_seed((uint64_t) t.QuadPart ^
                                          ((uint64_t) pid << 40) ^
                                          (uint64_t) (uintptr_t) &counter);
    if (seed == 0) seed = 1;             /* 0 is the unseeded sentinel */
    /* a losing CAS just means another thread's seed won */
    unsigned int expect = 0;
    (void) atomic_compare_exchange_strong_explicit(&counter, &expect, seed,
                                                   memory_order_relaxed,
                                                   memory_order_relaxed);
  }
  unsigned int c = atomic_fetch_add_explicit(&counter, 1,
                                             memory_order_relaxed);
  int n = snprintf(name, size, REI_PREFIX_LITERAL "%lx_%x",
                   (unsigned long) pid, c);
  return (n > 0 && (size_t) n < size) ? (size_t) n : 0;
}

int rei_shm_create_stack(rei_shm *shm, size_t size) {

  shm->addr = NULL;
  shm->size = 0;
  shm->handle = NULL;
  shm->pid = (unsigned int) GetCurrentProcessId();

  DWORD hi = (DWORD) ((uint64_t) size >> 32);
  DWORD lo = (DWORD) (size & 0xFFFFFFFF);

  shm->name_len = (uint8_t) rei_region_name(shm->name, sizeof(shm->name), shm->pid);
  HANDLE h = CreateFileMappingA(
    INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, hi, lo, shm->name
  );
  if (h == NULL) return rei_err_classify((long) GetLastError());
  if (GetLastError() == ERROR_ALREADY_EXISTS) {  /* name already taken */
    CloseHandle(h);                              /* opened a pre-existing region */
    return REI_ERRCAT_EXISTS;
  }

  void *addr = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (addr == NULL) {
    long code = (long) GetLastError();       /* CloseHandle would clobber it */
    CloseHandle(h);
    return rei_err_classify(code);
  }

  shm->addr = addr;
  shm->size = size;
  shm->handle = h;
  return REI_ERRCAT_NONE;
}

int rei_shm_open_stack(rei_shm *shm, const char *name) {

  shm->addr = NULL;
  shm->size = 0;
  shm->handle = NULL;
  size_t nl = strlen(name);
  if (nl >= sizeof(shm->name)) nl = sizeof(shm->name) - 1;
  memcpy(shm->name, name, nl);
  shm->name[nl] = '\0';
  shm->name_len = (uint8_t) nl;
  shm->pid = 0;                      /* consumer: never the creator */

  HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
  if (h == NULL) return -1;

  void *addr = MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
  if (addr == NULL) {
    CloseHandle(h);
    return -1;
  }

  MEMORY_BASIC_INFORMATION mbi;
  VirtualQuery(addr, &mbi, sizeof(mbi));

  shm->addr = addr;
  shm->size = mbi.RegionSize;
  shm->handle = h;
  return 0;
}

void rei_shm_close_stack(rei_shm *shm, int unlink) {
  (void) unlink;
  if (shm->addr != NULL) UnmapViewOfFile(shm->addr);
  if (shm->handle != NULL) CloseHandle(shm->handle);
  shm->addr = NULL;
  shm->handle = NULL;
}

/* A Win32 file mapping lives only while a handle to it is open, so it cannot
   outlive its creator: there are no persistent names and no orphans to reap. */
char **rei_shm_reap(int *n) {
  *n = 0;
  return NULL;
}

#else /* POSIX */

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <time.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define REI_HINT_NOSPACE \
  "Shared memory is provisioned at the OS or container level; in containers, " \
  "raise it at start (e.g. `docker run --shm-size=2g ...`)."

static int rei_err_classify(long code) {
  switch ((int) code) {
  case ENOSPC:
    return REI_ERRCAT_NOSPACE;
  case ENOMEM:
    return REI_ERRCAT_NOMEMORY;
  default:
    return REI_ERRCAT_OTHER;
  }
}

/* Linux: go through /dev/shm directly to avoid the -lrt link dependency
   that shm_open/shm_unlink would introduce. macOS has them in libc. */

#ifdef __linux__

static int rei_shm_os_open(const char *name, int flags, mode_t mode) {
  char path[64];
  snprintf(path, sizeof(path), "/dev/shm%s", name);
  return open(path, flags, mode);
}

static int rei_shm_os_unlink(const char *name) {
  char path[64];
  snprintf(path, sizeof(path), "/dev/shm%s", name);
  return unlink(path);
}

#else /* macOS / other POSIX */

static int rei_shm_os_open(const char *name, int flags, mode_t mode) {
  return shm_open(name, flags, mode);
}

#ifdef __APPLE__

/* macOS has no enumerable SHM namespace (no /dev/shm), so rei keeps its
   own registry for reaping: one append-only log per process, "rei_<pid>"
   under a per-user dir, holding the counter of every region the process
   creates as a 4-byte record (the pid comes from the filename). The
   reaper reads a dead PID's log, unlinks each reconstructed name, then
   removes the log. One write() per region on a cached O_APPEND fd — the
   kernel serializes concurrent appends per record, so no locking. A
   live-region counter truncates the log in place at zero crossings past
   a small floor, so the file stays near peak concurrency rather than
   lifetime creates. All log ops are best-effort — failure forfeits only
   reapability. */

/* Per-user registry dir "<temp>/rei", resolved fresh each call (read-
   through: tests point TMPDIR at a scratch dir). Builds the path but
   never creates it (rei_log_append's job), so resolving for a reap
   leaves no empty dir behind. $TMPDIR first; -1 if unresolvable. */
static int rei_log_dir(char *out, size_t size) {
  char base[PATH_MAX];
  const char *tmp = getenv("TMPDIR");
  if (tmp != NULL && tmp[0] != '\0') {
    int bn = snprintf(base, sizeof(base), "%s", tmp);
    if (bn <= 0 || (size_t) bn >= sizeof(base)) return -1;
  } else {
    size_t len = confstr(_CS_DARWIN_USER_TEMP_DIR, base, sizeof(base));
    if (len == 0 || len > sizeof(base)) {
      int bn = snprintf(base, sizeof(base), "%s", "/tmp");
      if (bn <= 0 || (size_t) bn >= sizeof(base)) return -1;
    }
  }

  size_t bl = strlen(base);
  while (bl > 1 && base[bl - 1] == '/') base[--bl] = '\0';   /* avoid "//rei" */

  int n = snprintf(out, size, "%s/rei", base);
  return (n > 0 && (size_t) n < size) ? 0 : -1;
}

/* This process's log fd, opened on first append and cached for the
   process's lifetime — never closed, so a thread using a loaded value
   can never write to a reused fd. A forked child sees the pid mismatch
   and opens its own log, leaking the inherited fd (closing it could
   race a sibling thread's write). The release/acquire pairing on
   rei_log_pid orders the fd reset before the pid change becomes
   visible. */
static _Atomic int   rei_log_fd  = -1;
static _Atomic pid_t rei_log_pid = 0;        /* pid that opened rei_log_fd */

/* Truncate the log at a zero crossing only once it holds this many
   records: below the floor the file is at most 1 KB and the ftruncate
   costs more than the hygiene is worth (a create+destroy loop would
   otherwise pay it every cycle). */
#define REI_LOG_TRUNC_MIN 256

/* Created-minus-torn-down region count, driving the zero-crossing
   truncate, and the records written since the last truncate. Neither is
   reset after a fork: the inherited values are a conservative floor
   (teardowns of the parent's regions are pid-guarded out), so a child
   can only ever truncate its own log. */
static _Atomic long rei_log_live  = 0;
static _Atomic long rei_log_dirty = 0;

/* Record a created region: its name's counter as one 4-byte record
   (native endianness — the log is read on the same host), opening the
   log on first use. The count increments unconditionally, staying
   balanced against rei_log_release even when logging itself fails. */
static void rei_log_append(const char *name) {
  atomic_fetch_add_explicit(&rei_log_live, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&rei_log_dirty, 1, memory_order_relaxed);

  const char *us = strrchr(name, '_');
  if (us == NULL) return;
  uint32_t rec = (uint32_t) strtoul(us + 1, NULL, 16);

  pid_t pid = getpid();
  int fd = -1;
  if (atomic_load_explicit(&rei_log_pid, memory_order_acquire) == pid)
    fd = atomic_load_explicit(&rei_log_fd, memory_order_relaxed);
  else {                                     /* first append, or post-fork */
    atomic_store_explicit(&rei_log_fd, -1, memory_order_relaxed);
    atomic_store_explicit(&rei_log_pid, pid, memory_order_release);
  }
  if (fd < 0) {
    char dir[PATH_MAX], path[PATH_MAX];
    if (rei_log_dir(dir, sizeof(dir)) != 0) return;
    int n = snprintf(path, sizeof(path), "%s/%s%x", dir,
                     &REI_PREFIX_LITERAL[1], (unsigned) pid);
    if (n <= 0 || (size_t) n >= sizeof(path)) return;
    int nfd = open(path, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0600);
    if (nfd < 0 && errno == ENOENT) {   /* dir absent: create it and retry */
      mkdir(dir, 0700);
      nfd = open(path, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0600);
    }
    if (nfd < 0) return;
    int expect = -1;
    if (atomic_compare_exchange_strong_explicit(&rei_log_fd, &expect, nfd,
                                                memory_order_relaxed,
                                                memory_order_relaxed)) {
      fd = nfd;
    } else {
      close(nfd);                          /* another thread's open won */
      fd = expect;
    }
  }
  ssize_t w = write(fd, &rec, sizeof(rec));   /* O_APPEND: whole records */
  (void) w;
}

/* One of our regions was torn down. At a zero crossing the log holds
   only stale records, so truncate it in place once past the floor — the
   fd is never closed, and O_APPEND continues from the new end. A create
   racing the truncate can lose its record: a microseconds-wide
   best-effort forfeiture. */
static void rei_log_release(void) {
  if (atomic_fetch_sub_explicit(&rei_log_live, 1, memory_order_relaxed) != 1)
    return;
  if (atomic_load_explicit(&rei_log_pid, memory_order_relaxed) != getpid())
    return;                                  /* pre-fork state: not our log */
  int fd = atomic_load_explicit(&rei_log_fd, memory_order_relaxed);
  if (fd < 0) return;
  if (atomic_load_explicit(&rei_log_dirty, memory_order_relaxed) <
      REI_LOG_TRUNC_MIN)
    return;
  ftruncate(fd, 0);
  /* Appends racing the reset leave dirty low: the next truncate is
     merely delayed — the conservative direction. */
  atomic_store_explicit(&rei_log_dirty, 0, memory_order_relaxed);
}

/* Exit/unload hook, registered as a library destructor: with every
   created region torn down the log holds only stale records, so unlink
   it and prune the dir — a clean process leaves no registry residue and
   the reaper's job shrinks to crashed processes. A region that outlives
   its creator keeps the count nonzero and the log in place for the
   reaper; the pid guard keeps a forked child that never opened its own
   log from removing the parent's. The fd is never closed, only
   forgotten: an append racing exit writes to the unlinked inode — the
   same microseconds-wide forfeiture as a create racing the truncate. */
__attribute__((destructor)) void rei_log_teardown(void) {
  if (atomic_load_explicit(&rei_log_live, memory_order_relaxed) != 0)
    return;
  if (atomic_load_explicit(&rei_log_pid, memory_order_relaxed) != getpid())
    return;
  char dir[PATH_MAX], path[PATH_MAX];
  if (rei_log_dir(dir, sizeof(dir)) != 0) return;
  int n = snprintf(path, sizeof(path), "%s/%s%x", dir,
                   &REI_PREFIX_LITERAL[1], (unsigned) getpid());
  if (n <= 0 || (size_t) n >= sizeof(path)) return;
  unlink(path);
  rmdir(dir);            /* succeeds once the last process's log is gone */
  atomic_store_explicit(&rei_log_fd, -1, memory_order_relaxed);
}

#endif /* __APPLE__ */

static int rei_shm_os_unlink(const char *name) {
  return shm_unlink(name);             /* region only; the log is per-process */
}

#endif

#if defined(__linux__) || defined(__APPLE__)

#include <signal.h>
#include <dirent.h>

static int rei_pid_alive(pid_t pid) {
  if (pid <= 0) return 1;             /* never treat as reapable */
  if (kill(pid, 0) == 0) return 1;    /* exists and signalable */
  return errno == EPERM;              /* exists but owned by another user */
}

/* Unlink region `name`; on success append a malloc'd copy to the growable
   (*list,*cap,*count) result. A name already gone (lost a race with
   another reap/unlink) is skipped, not reported. -1 on OOM so the caller
   stops, else 0. */
static int rei_reap_unlink(const char *name,
                            char ***list, size_t *cap, size_t *count) {
  if (rei_shm_os_unlink(name) != 0) return 0;
  if (*count == *cap) {
    size_t ncap = *cap ? *cap * 2 : 8;
    char **grown = realloc(*list, ncap * sizeof(**list));
    if (grown == NULL) return -1;
    *list = grown;
    *cap = ncap;
  }
  size_t len = strlen(name) + 1;
  char *copy = malloc(len);
  if (copy == NULL) return -1;
  memcpy(copy, name, len);
  (*list)[(*count)++] = copy;
  return 0;
}

#ifdef __APPLE__
/* Read a dead process's log and unlink every region it names: 4-byte
   counter records, each reconstructed with the pid from the log's
   filename. A tail short of a record is a crash-torn write: fread
   declines it. */
static int rei_reap_log(const char *path, unsigned long pid,
                        char ***list, size_t *cap, size_t *count) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) return 0;
  uint32_t rec;
  int rc = 0;
  while (fread(&rec, sizeof(rec), 1, f) == 1) {
    char name[REI_NAME_MAX];
    int n = snprintf(name, sizeof(name), "%s%lx_%x",
                     REI_PREFIX_LITERAL, pid, (unsigned int) rec);
    if (n <= 0 || (size_t) n >= sizeof(name)) continue;
    if (rei_reap_unlink(name, list, cap, count) != 0) {
      rc = -1;                                   /* OOM */
      break;
    }
  }
  fclose(f);
  return rc;
}
#endif

/* Reap orphans of dead creators. Linux scans /dev/shm, where each entry
   is a region name; macOS scans the registry dir, where each entry is a
   per-process log (rei_<pid>) whose records name that process's regions.
   Either way the PID embedded in "rei_<pid>..." drives the liveness test.
   Returns the removed names as a malloc'd array of *n malloc'd strings
   (caller frees each, then the array); NULL / *n == 0 if none. */
char **rei_shm_reap(int *n) {
  *n = 0;

  const char *prefix = &REI_PREFIX_LITERAL[1];  /* skip the leading '/' */
  const size_t prefix_len = strlen(prefix);
  char **list = NULL;
  size_t cap = 0, count = 0;

#ifdef __linux__
  DIR *dir = opendir("/dev/shm");                /* the kernel's own registry */
  if (dir == NULL) return NULL;

  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    const char *fname = ent->d_name;
    if (strncmp(fname, prefix, prefix_len) != 0) continue;

    const char *pid_str = fname + prefix_len;
    char *end;
    long pid = strtol(pid_str, &end, 16);
    if (end == pid_str || *end != '_') continue;    /* not <pid>_<counter> */
    if (rei_pid_alive((pid_t) pid)) continue;       /* creator still alive */

    char shm_name[REI_NAME_MAX];                    /* shm name = "/" + entry */
    int wn = snprintf(shm_name, sizeof(shm_name), "/%s", fname);
    if (wn <= 0 || (size_t) wn >= sizeof(shm_name)) continue;
    if (rei_reap_unlink(shm_name, &list, &cap, &count) != 0) break;
  }
  closedir(dir);
#else /* __APPLE__ */
  char scan[PATH_MAX];
  if (rei_log_dir(scan, sizeof(scan)) != 0) return NULL;
  DIR *dir = opendir(scan);                      /* rei's per-process logs */
  if (dir == NULL) return NULL;                  /* no logs: nothing to reap */

  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    const char *fname = ent->d_name;
    if (strncmp(fname, prefix, prefix_len) != 0) continue;

    const char *pid_str = fname + prefix_len;
    char *end;
    long pid = strtol(pid_str, &end, 16);
    if (end == pid_str || *end != '\0') continue;   /* log is "rei_<pid>" */
    if (rei_pid_alive((pid_t) pid)) continue;       /* live owner: leave its log */

    char path[PATH_MAX];
    int pn = snprintf(path, sizeof(path), "%s/%s", scan, fname);
    if (pn <= 0 || (size_t) pn >= sizeof(path)) continue;
    if (rei_reap_log(path, (unsigned long) pid, &list, &cap, &count) != 0)
      break;                          /* OOM: leave the log for a later retry */
    unlink(path);                                /* drop the dead process's log */
  }
  closedir(dir);
  rmdir(scan);                                   /* prune the dir once empty */
#endif

  *n = (int) count;
  return list;
}

#else /* other POSIX: the SHM namespace cannot be enumerated */

char **rei_shm_reap(int *n) {
  *n = 0;
  return NULL;
}

#endif /* __linux__ || __APPLE__ */

static size_t rei_region_name(char *name, size_t size, unsigned int pid) {
  static _Atomic unsigned int counter;   /* 0 = unseeded; seeded once */
  if (atomic_load_explicit(&counter, memory_order_relaxed) == 0) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned int seed = rei_counter_seed(((uint64_t) ts.tv_sec << 32) ^
                                          (uint64_t) ts.tv_nsec ^
                                          ((uint64_t) pid << 40) ^
                                          (uint64_t) (uintptr_t) &counter);
    if (seed == 0) seed = 1;             /* 0 is the unseeded sentinel */
    /* a losing CAS just means another thread's seed won */
    unsigned int expect = 0;
    (void) atomic_compare_exchange_strong_explicit(&counter, &expect, seed,
                                                   memory_order_relaxed,
                                                   memory_order_relaxed);
  }
  unsigned int c = atomic_fetch_add_explicit(&counter, 1,
                                             memory_order_relaxed);
  int n = snprintf(name, size, REI_PREFIX_LITERAL "%x_%x", pid, c);
  return (n > 0 && (size_t) n < size) ? (size_t) n : 0;
}

/* Tear down a partially-created region and return the failure category.
   Callers pass errno (or posix_fallocate's return) as an argument: close
   and unlink would clobber errno. */
static int rei_create_fail(int fd, const char *name, int code) {
  close(fd);
  rei_shm_os_unlink(name);
#ifdef __APPLE__
  rei_log_release();  /* balance the append's count; the record goes stale */
#endif
  return rei_err_classify(code);
}

/* Create a new region under a fresh name. EEXIST means the name is held
   by an orphan from a crashed process that reused this PID — surfaced as
   an error rather than worked around: rei_shm_reap() reclaims such
   orphans. */
int rei_shm_create_stack(rei_shm *shm, size_t size) {

  shm->addr = NULL;
  shm->size = 0;

  shm->pid = (unsigned int) getpid();
  shm->name_len = (uint8_t) rei_region_name(shm->name, sizeof(shm->name), shm->pid);
  int fd = rei_shm_os_open(shm->name, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0)
    return errno == EEXIST ? REI_ERRCAT_EXISTS : rei_err_classify(errno);

#ifdef __APPLE__
  rei_log_append(shm->name);   /* register before the region escapes */
#endif

  if (ftruncate(fd, (off_t) size) != 0)
    return rei_create_fail(fd, shm->name, errno);

#ifdef __linux__
  /* Reserve tmpfs pages now: ftruncate leaves the file sparse and tmpfs
     allocates only on write fault — SIGBUS if /dev/shm is full.
     MAP_POPULATE alone won't help: read prefault of a hole resolves to
     the shared zero page without allocating. posix_fallocate returns
     errno directly. */
  int ferr = posix_fallocate(fd, 0, (off_t) size);
  if (ferr != 0)
    return rei_create_fail(fd, shm->name, ferr);
#endif

  void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE, fd, 0);
  if (addr == MAP_FAILED)
    return rei_create_fail(fd, shm->name, errno);

  close(fd);

#ifdef MADV_HUGEPAGE
  if (size >= 2 * 1024 * 1024)
    madvise(addr, size, MADV_HUGEPAGE);
#endif
  /* No MADV_WILLNEED on macOS: the region is written in full before use, so its
     pages fault in on demand — prefaulting would be a redundant pass + syscall. */

  shm->addr = addr;
  shm->size = size;
  return REI_ERRCAT_NONE;
}

int rei_shm_open_stack(rei_shm *shm, const char *name) {

  shm->addr = NULL;
  shm->size = 0;
  size_t nl = strlen(name);
  if (nl >= sizeof(shm->name)) nl = sizeof(shm->name) - 1;
  memcpy(shm->name, name, nl);
  shm->name[nl] = '\0';
  shm->name_len = (uint8_t) nl;
  shm->pid = 0;                      /* consumer: never the creator */

  int fd = rei_shm_os_open(name, O_RDONLY, 0);
  if (fd < 0) return -1;

  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return -1;
  }
  size_t size = (size_t) st.st_size;

  /* No MAP_POPULATE on the consumer: pages already exist (host wrote
     them), so populating only installs PTEs eagerly and defeats lazy
     access (reading 1 of 10 list elements would prefault the unread 9).
     Pages fault in on first touch. */
  void *addr = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    close(fd);
    return -1;
  }

  close(fd);

#ifdef MADV_HUGEPAGE
  if (size >= 2 * 1024 * 1024)
    madvise(addr, size, MADV_HUGEPAGE);
#endif
  /* No MADV_WILLNEED on macOS: the same eager PTE install MAP_POPULATE
     is omitted to avoid. */

  shm->addr = addr;
  shm->size = size;
  return 0;
}

void rei_shm_close_stack(rei_shm *shm, int unlink) {
  if (shm->addr != NULL) munmap(shm->addr, shm->size);
  if (unlink) {
    rei_shm_os_unlink(shm->name);
#ifdef __APPLE__
    /* Creator-only: opened regions carry pid 0 and were never logged
       here. */
    if (shm->pid == (unsigned int) getpid()) rei_log_release();
#endif
  }
  shm->addr = NULL;
}

#endif /* _WIN32 */

// Platform-independent error rendering ---------------------------------------

/* Map a failure category to a summary plus an actionable remediation
   hint ("" where the summary suffices); the caller composes its error
   message from these. */
REI_COLD void rei_err_describe(rei_errcat category, const char **summary,
                      const char **hint) {
  *hint = "";
  switch (category) {
  case REI_ERRCAT_NOSPACE:
    *summary = "out of space";
    *hint = REI_HINT_NOSPACE;
    break;
  case REI_ERRCAT_NOMEMORY:
    *summary = "not enough memory";
    break;
  case REI_ERRCAT_EXISTS:
    /* Preventative, not curative: the colliding orphans carry this PID, so the
       erroring process cannot reap them itself (it reads its own PID as alive)
       — rei_shm_reap() must run while the PID is free, before reuse. */
    *summary = "the region name is already in use";
    *hint = "Clear orphans of crashed processes with rei_shm_reap() before a "
            "PID is reused.";
    break;
  default:
    *summary = "an unexpected error occurred";
    break;
  }
}

// Platform-independent heap-allocating variants ------------------------------

/* Malloc a rei_shm and create the region into it. Success: *out set,
   REI_ERRCAT_NONE. Failure: *out NULL, nothing leaked, returns the
   failure category. */
int rei_shm_create_heap(rei_shm **out, size_t size) {
  *out = NULL;
  rei_shm *shm = malloc(sizeof(rei_shm));
  if (shm == NULL) return REI_ERRCAT_NOMEMORY;
  int rc = rei_shm_create_stack(shm, size);
  if (rc != REI_ERRCAT_NONE) {
    free(shm);
    return rc;
  }
  *out = shm;
  return REI_ERRCAT_NONE;
}

/* Malloc a rei_shm and open an existing SHM region into it. */
rei_shm *rei_shm_open_heap(const char *name) {
  rei_shm *shm = malloc(sizeof(rei_shm));
  if (shm == NULL) return NULL;
  if (rei_shm_open_stack(shm, name) != 0) {
    free(shm);
    return NULL;
  }
  return shm;
}

// Platform-independent host teardown -----------------------------------------

/* Release the host side of a created region — the name (POSIX: unlink) /
   creator handle (Windows) — without touching the mapping, which
   rei_shm_close_stack releases. Unlinks only in the creating process: a
   forked child inherits this teardown for the parent's regions and must
   not destroy their names. */
void rei_shm_host_release(rei_shm *shm) {
#ifdef _WIN32
  if (shm->handle != NULL) CloseHandle(shm->handle);
#else
  if (shm->name[0] != '\0' && shm->pid == (unsigned int) getpid()) {
    rei_shm_os_unlink(shm->name);
#ifdef __APPLE__
    rei_log_release();
#endif
  }
#endif
}

// Public heap API (rei.h) ----------------------------------------------------

/* The public region verbs are the heap form plus the thread-local error
   record on failure. */

rei_status rei_shm_create(rei_shm **out, size_t size) {
  int rc = rei_shm_create_heap(out, size);
  if (rc == REI_ERRCAT_NONE) return REI_OK;
  const char *summary, *hint;
  rei_err_describe((rei_errcat) rc, &summary, &hint);
  rei_err_record_tls((rei_errcat) rc,
                     "cannot create region (%llu bytes): %s%s%s",
                     (unsigned long long) size, summary,
                     hint[0] != '\0' ? ". " : "", hint);
  return REI_ERR;
}

rei_status rei_shm_open(rei_shm **out, const char *name) {
  *out = rei_shm_open_heap(name);
  if (*out != NULL) return REI_OK;
  rei_err_record_tls(REI_ERRCAT_OTHER, "cannot open region '%s'", name);
  return REI_ERR;
}

void rei_shm_close(rei_shm *shm, int unlink) {
  if (shm == NULL) return;
  rei_shm_close_stack(shm, unlink);
  free(shm);
}

void *rei_shm_addr(rei_shm *shm) {
  return shm->addr;
}

size_t rei_shm_size(const rei_shm *shm) {
  return shm->size;
}

const char *rei_shm_name(const rei_shm *shm) {
  return shm->name;
}
