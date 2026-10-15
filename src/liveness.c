/* Liveness lock: the death verdict (listeners are wake triggers only).
   Each process holds an exclusive lock on a per-channel file for its whole
   lifetime; the kernel releases it on any exit path. Probes use the fd
   kept from open, never a fresh open of the path, so an unlinked path
   changes nothing — both fds still reference the same inode. */

/* _GNU_SOURCE: glibc hides O_CLOEXEC, MAP_POPULATE, pidfd_open, ...
   under strict -std=c11; define before any system header. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <stdio.h>
#include "internal.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int mizu_live_open(const char *path, intptr_t *out) {
  /* Handles are not inheritable (no SECURITY_ATTRIBUTES), so spawned
     children cannot keep a dead process's lock alive. */
  HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return -1;
  *out = (intptr_t) h;
  return 0;
}

int mizu_live_open_existing(const char *path, intptr_t *out) {
  HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return -1;
  *out = (intptr_t) h;
  return 0;
}

int mizu_live_try(intptr_t h) {
  OVERLAPPED ov;
  memset(&ov, 0, sizeof(ov));
  if (LockFileEx((HANDLE) h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                 0, 1, 0, &ov))
    return MIZU_LIVE_ACQUIRED;
  return GetLastError() == ERROR_LOCK_VIOLATION ? MIZU_LIVE_HELD : -1;
}

void mizu_live_unlock(intptr_t h) {
  OVERLAPPED ov;
  memset(&ov, 0, sizeof(ov));
  UnlockFileEx((HANDLE) h, 0, 1, 0, &ov);
}

void mizu_live_close(intptr_t h) {
  CloseHandle((HANDLE) h);
}

int mizu_live_ident(intptr_t h, uint64_t *dev, uint64_t *ino) {
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle((HANDLE) h, &info)) return -1;
  *dev = (uint64_t) info.dwVolumeSerialNumber;
  *ino = ((uint64_t) info.nFileIndexHigh << 32) | info.nFileIndexLow;
  return 0;
}

/* Platform default: GetTempPathA — per-user local by default, and a TMP
   redirected to SMB keeps first-class LockFileEx semantics, degrading in
   latency only. */
static int mizu_live_dir_default(char *buf, size_t size) {
  DWORD n = GetTempPathA((DWORD) size, buf);
  return (n > 0 && (size_t) n < size) ? 0 : -1;
}

#else /* POSIX */

#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

/* Consumers keep these in zero-initialized structs with 0 as the
   not-open sentinel, so a lock fd must never be 0: a process launched
   with stdin closed would otherwise leak the fd (and its flock) and
   probe every peer as permanently alive. fds 1/2 bounce too: launched
   with stdout/stderr closed the lock lands there, and a later dup2 /
   freopen onto it would release the flock — a manufactured false DEAD. */
static int mizu_live_fd_bounce(int fd) {
  if (fd > 2) return fd;
  int nfd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
  close(fd);
  return nfd;
}

/* Fork-inheritance guard: O_CLOEXEC covers exec'd children only — a
   fork-without-exec child (an mclapply worker, a multiprocessing child)
   inherits every liveness fd's open file description, so a dead holder
   reads ALIVE while any forked child lives. Track every fd handed out;
   an atfork child handler closes the child's copies — closing one fd of
   a duplicated pair does not release the parent's flock, so the lock
   then dies with the parent. The prepare handler holds the registry
   mutex across the fork, so the child walks a consistent list. */
typedef struct mizu_live_fd_s {
  intptr_t fd;
  struct mizu_live_fd_s *next;
} mizu_live_fd;

static pthread_mutex_t mizu_live_mu = PTHREAD_MUTEX_INITIALIZER;
static mizu_live_fd *mizu_live_fds;

static void mizu_live_atfork_prepare(void) {
  pthread_mutex_lock(&mizu_live_mu);
}

static void mizu_live_atfork_parent(void) {
  pthread_mutex_unlock(&mizu_live_mu);
}

static void mizu_live_atfork_child(void) {
  for (mizu_live_fd *n = mizu_live_fds; n != NULL; n = n->next)
    close((int) n->fd);
  /* the nodes leak in the child (no free: the heap may be mid-mutation
     from another thread at fork); its handles are dead by contract */
  mizu_live_fds = NULL;
  pthread_mutex_unlock(&mizu_live_mu);
}

static pthread_once_t mizu_live_once = PTHREAD_ONCE_INIT;

static void mizu_live_atfork_install(void) {
  (void) pthread_atfork(mizu_live_atfork_prepare, mizu_live_atfork_parent,
                        mizu_live_atfork_child);
}

/* Serialize every open/close against the atfork prepare handler: the
   child then sees exactly the tracked set — every tracked fd open, every
   open liveness fd tracked. */
static void mizu_live_registry_lock(void) {
  (void) pthread_once(&mizu_live_once, mizu_live_atfork_install);
  pthread_mutex_lock(&mizu_live_mu);
}

static int mizu_live_registry_add(intptr_t fd) {
  mizu_live_fd *n = malloc(sizeof(*n));
  if (n == NULL) return -1;
  n->fd = fd;
  n->next = mizu_live_fds;
  mizu_live_fds = n;
  return 0;
}

int mizu_live_open(const char *path, intptr_t *out) {
  /* O_CLOEXEC is required: flock is scoped to the open file
     description, so an inherited fd would keep the lock alive past the
     holder's death — a manufactured false ALIVE. */
  mizu_live_registry_lock();
  int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (fd >= 0) {
    /* Lock-file names are derivable and the default dir can be
       world-writable (/dev/shm), so refuse a file another user pre-created:
       a squatter taking the flock after the holder dies would feed the
       reaper a permanent false ALIVE. */
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_uid != geteuid()) {
      close(fd);
      fd = -1;
    } else {
      fd = mizu_live_fd_bounce(fd);
      if (fd >= 0 && mizu_live_registry_add((intptr_t) fd) != 0) {
        close(fd);
        fd = -1;
      }
    }
  }
  pthread_mutex_unlock(&mizu_live_mu);
  if (fd < 0) return -1;
  *out = (intptr_t) fd;
  return 0;
}

int mizu_live_open_existing(const char *path, intptr_t *out) {
  mizu_live_registry_lock();
  int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd >= 0) {
    fd = mizu_live_fd_bounce(fd);
    if (fd >= 0 && mizu_live_registry_add((intptr_t) fd) != 0) {
      close(fd);
      fd = -1;
    }
  }
  pthread_mutex_unlock(&mizu_live_mu);
  if (fd < 0) return -1;
  *out = (intptr_t) fd;
  return 0;
}

int mizu_live_try(intptr_t h) {
  if (flock((int) h, LOCK_EX | LOCK_NB) == 0) return MIZU_LIVE_ACQUIRED;
  return (errno == EWOULDBLOCK || errno == EAGAIN) ? MIZU_LIVE_HELD : -1;
}

void mizu_live_unlock(intptr_t h) {
  flock((int) h, LOCK_UN);
}

void mizu_live_close(intptr_t h) {
  /* Registry-gated: an untracked fd is a forked child's stale handle copy
     (the atfork handler already closed the real descriptor) or a double
     close — closing here could kill a descriptor since reused. */
  mizu_live_registry_lock();
  mizu_live_fd **pp = &mizu_live_fds;
  while (*pp != NULL && (*pp)->fd != h) pp = &(*pp)->next;
  if (*pp != NULL) {
    mizu_live_fd *dead = *pp;
    *pp = dead->next;
    close((int) h);
    free(dead);
  }
  pthread_mutex_unlock(&mizu_live_mu);
}

int mizu_live_ident(intptr_t h, uint64_t *dev, uint64_t *ino) {
  struct stat st;
  if (fstat((int) h, &st) != 0) return -1;
  *dev = (uint64_t) st.st_dev;
  *ino = (uint64_t) st.st_ino;
  return 0;
}

/* Platform default: /dev/shm on Linux — guaranteed local tmpfs, so the
   NFS-degraded flock failure mode cannot arise. Elsewhere the per-user
   temp dir, duplicating shm.c's mizu_log_dir() resolution (it is static). */
static int mizu_live_dir_default(char *buf, size_t size) {
#ifdef __linux__
  int n = snprintf(buf, size, "/dev/shm");
  return (n > 0 && (size_t) n < size) ? 0 : -1;
#else
  const char *tmp = getenv("TMPDIR");
  if (tmp != NULL && tmp[0] != '\0') {
    int n = snprintf(buf, size, "%s", tmp);
    return (n > 0 && (size_t) n < size) ? 0 : -1;
  }
#ifdef __APPLE__
  size_t len = confstr(_CS_DARWIN_USER_TEMP_DIR, buf, size);
  if (len > 0 && len <= size) return 0;
#endif
  int n = snprintf(buf, size, "/tmp");
  return (n > 0 && (size_t) n < size) ? 0 : -1;
#endif
}

#endif /* _WIN32 */

// Lock directory ------------------------------------------------------------------

static size_t mizu_live_dir_trim(char *buf, size_t n) {
  while (n > 1 && (buf[n - 1] == '/'
#ifdef _WIN32
                   || buf[n - 1] == '\\'
#endif
                   )) n--;
  buf[n] = '\0';
  return n;
}

const char *mizu_live_dir(void) {
  /* The override is read-through (tests set it per-call) and copied out:
     a later setenv can invalidate the getenv pointer. Oversized values
     return truncated for the callers' length guard to reject. Buffers
     are thread-local: threaded consumers create control regions
     concurrently, and the returned pointer must stay valid until the
     caller copies it out. */
  static _Thread_local char ovr[1024];
  static _Thread_local char def[1024];
  static _Thread_local int resolved = 0;  /* 0 = untried, 1 = valid, -1 = failed */
  const char *out;

  const char *env = getenv(MIZU_LIVENESS_DIR_ENV);
  if (env != NULL && env[0] != '\0') {
    size_t n = strlen(env);
    if (n >= sizeof(ovr)) n = sizeof(ovr) - 1;
    memcpy(ovr, env, n);
    mizu_live_dir_trim(ovr, n);
    out = ovr;
  } else {
    if (resolved == 0) {
      resolved = mizu_live_dir_default(def, sizeof(def)) == 0 ? 1 : -1;
      if (resolved > 0) mizu_live_dir_trim(def, strlen(def));
    }
    out = resolved > 0 ? def : NULL;
  }
  return out;
}
