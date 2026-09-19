/* Writable attach and populated create — what the plain region layer
   (shm.c) deliberately does not provide. The plain consumer open maps
   read-only, right for write-once shared objects and SHM_RAW payloads;
   channel/pool control regions are mutable shared state (both sides
   write ring indices, parker epochs, the control block), so peers attach
   through here. */

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

int mizu_shm_open_rw_stack(mizu_shm *shm, const char *name, int populate) {

  shm->addr = NULL;
  shm->size = 0;
  shm->handle = NULL;
  size_t nl = strlen(name);
  if (nl >= sizeof(shm->name)) nl = sizeof(shm->name) - 1;
  memcpy(shm->name, name, nl);
  shm->name[nl] = '\0';
  shm->name_len = (uint8_t) nl;

  HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
  if (h == NULL) return -1;

  void *addr = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  if (addr == NULL) {
    CloseHandle(h);
    return -1;
  }

  MEMORY_BASIC_INFORMATION mbi;
  VirtualQuery(addr, &mbi, sizeof(mbi));

  /* No mapping-time flag on Windows: read-touch to populate. Reads only —
     the region is live, creator-initialized state. */
  if (populate)
    for (size_t off = 0; off < mbi.RegionSize; off += 4096)
      (void) ((const volatile unsigned char *) addr)[off];

  shm->addr = addr;
  shm->size = mbi.RegionSize;
  shm->handle = h;
  return 0;
}

#else /* POSIX */

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

/* Same /dev/shm direct-open as shm.c on Linux (avoids -lrt); macOS has
   shm_open in libc. */
#ifdef __linux__
static int mizu_shm_os_open_rw(const char *name) {
  char path[64];
  snprintf(path, sizeof(path), "/dev/shm%s", name);
  return open(path, O_RDWR, 0);
}
#else
static int mizu_shm_os_open_rw(const char *name) {
  return shm_open(name, O_RDWR, 0);
}
#endif

int mizu_shm_open_rw_stack(mizu_shm *shm, const char *name, int populate) {

  shm->addr = NULL;
  shm->size = 0;
  size_t nl = strlen(name);
  if (nl >= sizeof(shm->name)) nl = sizeof(shm->name) - 1;
  memcpy(shm->name, name, nl);
  shm->name[nl] = '\0';
  shm->name_len = (uint8_t) nl;

  int fd = mizu_shm_os_open_rw(name);
  if (fd < 0) return -1;

  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return -1;
  }
  size_t size = (size_t) st.st_size;

  /* Pre-fault when asked, unlike the plain read-only consumer open: the
     whole ring is hot on the peer, so one populate beats faulting on the
     hot path. Lazy attaches (map contexts) opt out. */
  void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                    MAP_SHARED | (populate ? MAP_POPULATE : 0), fd, 0);
  if (addr == MAP_FAILED) {
    close(fd);
    return -1;
  }

  close(fd);

#ifndef __linux__
  /* No MAP_POPULATE here: read-touch is the pre-fault. Reads only — the
     creator's populated create already materialized the pages; each touch
     just fills this process's page table off the hot path. */
  if (populate)
    for (size_t off = 0; off < size; off += 4096)
      (void) ((const volatile unsigned char *) addr)[off];
#endif

#if defined(__linux__) && defined(MADV_COLLAPSE)
  /* Collapse works on shm under shmem_enabled=[never] (kernel >= 6.1)
     and installs PMD mappings for this process even when the creator
     already collapsed the folios. Populated attaches only: a lazy attach
     must keep demand paging. Failure is benign. */
  if (populate && size >= ((size_t) 2 << 20))
    (void) madvise(addr, size, MADV_COLLAPSE);
#endif

  shm->addr = addr;
  shm->size = size;
  return 0;
}

#ifdef __linux__
/* Read-only populated open for SHM_RAW payload reads. The plain consumer
   open skips MAP_POPULATE — right for write-once objects read lazily,
   wrong for a stream unserialized in full immediately: 2,048 read faults
   per 8 MB where one populate syscall does. Linux-only; macOS has no
   mapping-time populate flag (the fallback below uses the plain open).
   NULL on failure, so the caller's gone semantics are unchanged. */
mizu_shm *mizu_shm_open_ro_heap(const char *name) {
  mizu_shm *shm = malloc(sizeof(mizu_shm));
  if (shm == NULL) return NULL;
  shm->addr = NULL;
  shm->size = 0;
  size_t nl = strlen(name);
  if (nl >= sizeof(shm->name)) nl = sizeof(shm->name) - 1;
  memcpy(shm->name, name, nl);
  shm->name[nl] = '\0';
  shm->name_len = (uint8_t) nl;

  char path[64];
  snprintf(path, sizeof(path), "/dev/shm%s", name);
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    free(shm);
    return NULL;
  }
  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    free(shm);
    return NULL;
  }
  size_t size = (size_t) st.st_size;
  void *addr = mmap(NULL, size, PROT_READ, MAP_SHARED | MAP_POPULATE, fd, 0);
  if (addr == MAP_FAILED) {
    close(fd);
    free(shm);
    return NULL;
  }
  close(fd);
#ifdef MADV_HUGEPAGE
  if (size >= 2 * 1024 * 1024)
    madvise(addr, size, MADV_HUGEPAGE);
#endif
  shm->addr = addr;
  shm->size = size;
  return shm;
}
#endif /* __linux__ */

#endif /* _WIN32 */

#ifndef __linux__
mizu_shm *mizu_shm_open_ro_heap(const char *name) {
  return mizu_shm_open_heap(name);
}
#endif

mizu_shm *mizu_shm_open_rw_heap(const char *name, int populate) {
  mizu_shm *shm = malloc(sizeof(mizu_shm));
  if (shm == NULL) return NULL;
  if (mizu_shm_open_rw_stack(shm, name, populate) != 0) {
    free(shm);
    return NULL;
  }
  return shm;
}

/* Create + populate, for the pool/channel control regions: slot arrays
   walked incrementally, where a lazy first touch is a zero-fill fault in
   a µs-scale round trip. Linux's plain create already maps MAP_POPULATE;
   macOS and Windows have no mmap-time flag, so touch one byte per page —
   the creator pays the zero-fill once, and every later first access on
   either side is a soft fault on a resident page. Stride 4096 never
   skips a page (16 KiB pages on arm64 macOS take four stores). Payload
   regions keep the plain create: written in full immediately, prefault
   would be a redundant pass. */
int mizu_shm_create_populate(mizu_shm *shm, size_t size) {
  int rc = mizu_shm_create_stack(shm, size);
#ifndef __linux__
  if (rc == MIZU_ERRCAT_NONE) {
    volatile unsigned char *b = (volatile unsigned char *) shm->addr;
    for (size_t off = 0; off < size; off += 4096) b[off] = 0;
  }
#endif
#if defined(__linux__) && defined(MADV_COLLAPSE)
  /* The plain create's MADV_HUGEPAGE is inert under the stock
     shmem_enabled=[never]; a synchronous collapse works regardless, and
     the MAP_POPULATE pages already exist. Failure is benign. */
  if (rc == MIZU_ERRCAT_NONE && size >= ((size_t) 2 << 20))
    (void) madvise(shm->addr, size, MADV_COLLAPSE);
#endif
  return rc;
}

// Public writable / view opens and the zc protocol words (mizu.h) --------------

mizu_status mizu_shm_open_rw(mizu_shm **out, const char *name, int populate) {
  *out = mizu_shm_open_rw_heap(name, populate);
  if (*out != NULL) return MIZU_OK;
  mizu_err_record_tls(MIZU_ERRCAT_OTHER, "cannot open region '%s'", name);
  return MIZU_ERR;
}

/* The zc consumer open: page 0 read-write (the refcount word), the rest
   read-only. Lazy everywhere — a view is touched on demand, so eager PTE
   install would prefault never-read pages on the recv hot path. The
   default form performs the zc counted add itself: open implies counted,
   so no view mapping exists without the count. MIZU_OPEN_VIEW_NOCOUNT
   skips the add — the caller owns the mizu_zc_ref timing then (a binding
   whose wrap can fail between map and count) and must complete it before
   its consumer-done signal. */
mizu_status mizu_shm_open_view_flags(mizu_shm **out, const char *name,
                                   uint32_t flags) {
  mizu_shm *shm = mizu_shm_open_rw_heap(name, 0);
  if (shm == NULL) {
    mizu_err_record_tls(MIZU_ERRCAT_OTHER, "cannot open region '%s'", name);
    return MIZU_ERR;
  }
  size_t size = shm->size;
#ifdef _WIN32
  /* MapViewOfFile offsets must be 64 KiB allocation-granularity aligned,
     so a second view starting at page 1 fails: one RW view, then protect
     the tail. */
  static DWORD pagesz = 0;
  if (pagesz == 0) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    pagesz = si.dwPageSize;
  }
  if (size > (size_t) pagesz) {
    DWORD old;
    VirtualProtect((unsigned char *) shm->addr + pagesz, size - pagesz,
                   PAGE_READONLY, &old);
  }
#else
  static long pagesz = 0;
  if (pagesz == 0) pagesz = sysconf(_SC_PAGESIZE);
  /* Best-effort: if mprotect fails the mapping stays fully RW — the
     refcount still works, only the defense-in-depth degrades. */
  if (size > (size_t) pagesz)
    (void) mprotect((unsigned char *) shm->addr + pagesz, size - pagesz,
                    PROT_READ);
#endif
  if (!(flags & MIZU_OPEN_VIEW_NOCOUNT))
    atomic_fetch_add_explicit(mizu_zc_rc(shm->addr), 1, memory_order_acq_rel);
  *out = shm;
  return MIZU_OK;
}

mizu_status mizu_shm_open_view(mizu_shm **out, const char *name) {
  return mizu_shm_open_view_flags(out, name, 0);
}

void mizu_zc_ref(mizu_shm *shm) {
  atomic_fetch_add_explicit(mizu_zc_rc(shm->addr), 1, memory_order_acq_rel);
}

void mizu_zc_unref(mizu_shm *shm) {
  atomic_fetch_sub_explicit(mizu_zc_rc(shm->addr), 1, memory_order_acq_rel);
}

uint32_t mizu_zc_refcount(const mizu_shm *shm) {
  return atomic_load_explicit(mizu_zc_rc(shm->addr), memory_order_acquire);
}

uint32_t mizu_zc_flags(const mizu_shm *shm) {
  return atomic_load_explicit(mizu_zc_flags_(shm->addr), memory_order_acquire);
}

void mizu_zc_flag_refheld(mizu_shm *shm) {
  atomic_fetch_or_explicit(mizu_zc_flags_(shm->addr), MIZU_ZC_FLAG_REFHELD,
                           memory_order_acq_rel);
}
