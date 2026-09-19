/* Unit tier: the zc consumer opens (mizu_shm_open_view and its flags
   form). The default open performs the counted add fused; NOCOUNT skips
   it, leaving the mizu_zc_ref timing to the caller (a binding whose wrap
   can fail between map and count). Assert-based; run via `make test`. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

int main(void) {
  mizu_shm *shm = NULL;
  assert(mizu_shm_create(&shm, 8192) == MIZU_OK);
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

  /* the default form keeps the fused counted add */
  mizu_shm *v2 = NULL;
  assert(mizu_shm_open_view(&v2, mizu_shm_name(shm)) == MIZU_OK);
  assert(mizu_zc_refcount(shm) == 2);

  /* unbalanced flags reject nothing (0 is the default); a vanished name
     fails with the thread-local slot set, either form */
  mizu_shm *gone = NULL;
  assert(mizu_shm_open_view_flags(&gone, "/mizu_no_such_region",
                                 MIZU_OPEN_VIEW_NOCOUNT) == MIZU_ERR);
  assert(mizu_last_error_category() != MIZU_ERRCAT_NONE);

  mizu_zc_unref(v1);
  mizu_zc_unref(v2);
  mizu_shm_close(v1, 0);
  mizu_shm_close(v2, 0);
  mizu_shm_close(shm, 1);

  printf("test_view_open: OK\n");
  return 0;
}
