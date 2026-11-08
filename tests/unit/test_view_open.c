/* Unit tier: the zc consumer opens (mizu_shm_open_view and its flags
   form). The default open performs the counted add fused; NOCOUNT skips
   it, leaving the mizu_zc_ref timing to the caller (a binding whose wrap
   can fail between map and count). Assert-based; run via `make test`. */

/* _GNU_SOURCE: glibc hides mincore behind __USE_BSD under strict -std=c11. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#  include <unistd.h>
#endif

#include "internal.h"

int main(void) {
  mizu_shm *shm = NULL;
  assert(mizu_shm_create(&shm, 65536) == MIZU_OK);   /* > any page size:
     the view open mprotects the tail */
  /* a fresh region is zero-filled: the refcount word starts at 0 */
  assert(mizu_zc_refcount(shm) == 0);

  /* NOCOUNT: the open leaves the count untouched until the explicit
     ref, and the split protection still applies (page 0 writable). */
  mizu_shm *v1 = NULL;
  assert(mizu_shm_open_view_flags(&v1, mizu_shm_name(shm),
                                 MIZU_OPEN_VIEW_NOCOUNT) == MIZU_OK);
  assert(mizu_zc_refcount(shm) == 0);
  mizu_zc_ref(v1);
  assert(mizu_zc_refcount(shm) == 1);

  /* the default form keeps the fused counted add — behind a layout
     gate: a region without a layout magic (a control region named by a
     torn REF would be one) takes no add on its offset 24 */
  mizu_shm *v2 = NULL;
  assert(mizu_shm_open_view(&v2, mizu_shm_name(shm)) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "not a layout region") != NULL);
  assert(mizu_zc_refcount(shm) == 1);
  mizu_mizh_write(mizu_shm_addr(shm), MIZU_TYPE_RAW, 0);
  atomic_store(mizu_zc_rc(mizu_shm_addr(shm)), 1);   /* the write zeroed it */
  assert(mizu_shm_open_view(&v2, mizu_shm_name(shm)) == MIZU_OK);
  assert(mizu_zc_refcount(shm) == 2);

  /* unbalanced flags reject nothing (0 is the default); a vanished name
     fails with the thread-local slot set, either form. PID-keyed: no
     stray region from another run ever matches. */
  char gname[64];
  int gn = snprintf(gname, sizeof(gname), "/mizu_0_%ld", mizu_self_pid());
  assert(gn > 0 && (size_t) gn < sizeof(gname));
  mizu_shm *gone = NULL;
  assert(mizu_shm_open_view_flags(&gone, gname,
                                 MIZU_OPEN_VIEW_NOCOUNT) == MIZU_ERR);
  assert(mizu_last_error_category() != MIZU_ERRCAT_NONE);

  /* the wire-name grammar gate: a traversal or malformed name is
     refused before the open (whose counted add writes offset 24) */
  const char *bad_names[] = {
    "/mizu_../x", "/mizu_1", "/mizu_1_2x", "/mizu__1", "/etc/mizu_1_2"
  };
  for (size_t i = 0; i < sizeof bad_names / sizeof *bad_names; i++) {
    mizu_shm *bad = NULL;
    assert(mizu_shm_open_view_flags(&bad, bad_names[i], 0) == MIZU_ERR);
    assert(bad == NULL);
  }

  /* Past the prefault gate (MIZU_VIEW_PREFAULT_MIN, 1 MiB): the open's
     MADV_POPULATE_READ leaves the read-only tail resident where the
     kernel has it (>= 5.14), and the view reads exactly what the creator
     wrote. */
  mizu_shm *big = NULL;
  assert(mizu_shm_create(&big, 2 << 20) == MIZU_OK);
  unsigned char *baddr = mizu_shm_addr(big);
  mizu_mizh_write(baddr, MIZU_TYPE_RAW, 0);
  atomic_store(mizu_zc_rc(baddr), 1);        /* the write zeroed it */
  const size_t bsize = mizu_shm_size(big);
  baddr[bsize / 2] = 0x5A;
  baddr[bsize - 1] = 0xA5;
  mizu_shm *bv = NULL;
  assert(mizu_shm_open_view(&bv, mizu_shm_name(big)) == MIZU_OK);
  const unsigned char *vaddr = mizu_shm_addr(bv);
  assert(vaddr[bsize / 2] == 0x5A && vaddr[bsize - 1] == 0xA5);
  assert(mizu_zc_refcount(big) == 2);
#if defined(__linux__)
  /* Residency is asserted only where POPULATE_READ works (the pre-5.14
     fallback leaves the mapping lazy by design). */
  const long ps = sysconf(_SC_PAGESIZE);
  void *probe = mmap(NULL, (size_t) ps, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(probe != MAP_FAILED);
  const int has_populate = madvise(probe, (size_t) ps, MADV_POPULATE_READ) == 0;
  munmap(probe, (size_t) ps);
  if (has_populate) {
    const size_t npages = (bsize + (size_t) ps - 1) / (size_t) ps;
    unsigned char *res = malloc(npages);
    assert(res != NULL);
    assert(mincore(mizu_shm_addr(bv), bsize, res) == 0);
    assert((res[npages / 2] & 1) && (res[npages - 1] & 1));
    free(res);
  }
#endif
  mizu_zc_unref(bv);
  mizu_shm_close(bv, 0);
  mizu_shm_close(big, 1);

  mizu_zc_unref(v1);
  mizu_zc_unref(v2);
  mizu_shm_close(v1, 0);
  mizu_shm_close(v2, 0);
  mizu_shm_close(shm, 1);

  printf("test_view_open: OK\n");
  return 0;
}
