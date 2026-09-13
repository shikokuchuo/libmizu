/* Unit tier: the zc consumer opens (rei_shm_open_view and its flags
   form). The default open performs the counted add fused; NOCOUNT skips
   it, leaving the rei_zc_ref timing to the caller (a binding whose wrap
   can fail between map and count). Assert-based; run via `make test`. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

int main(void) {
  rei_shm *shm = NULL;
  assert(rei_shm_create(&shm, 8192) == REI_OK);
  /* a fresh region is zero-filled: the refcount word starts at 0 */
  assert(rei_zc_refcount(shm) == 0);

  /* NOCOUNT: the open leaves the count untouched until the explicit
     ref, and the split protection still applies (page 0 writable). */
  rei_shm *v1 = NULL;
  assert(rei_shm_open_view_flags(&v1, rei_shm_name(shm),
                                 REI_OPEN_VIEW_NOCOUNT) == REI_OK);
  assert(rei_zc_refcount(shm) == 0);
  rei_zc_ref(v1);
  assert(rei_zc_refcount(shm) == 1);

  /* the default form keeps the fused counted add */
  rei_shm *v2 = NULL;
  assert(rei_shm_open_view(&v2, rei_shm_name(shm)) == REI_OK);
  assert(rei_zc_refcount(shm) == 2);

  /* unbalanced flags reject nothing (0 is the default); a vanished name
     fails with the thread-local slot set, either form */
  rei_shm *gone = NULL;
  assert(rei_shm_open_view_flags(&gone, "/rei_no_such_region",
                                 REI_OPEN_VIEW_NOCOUNT) == REI_ERR);
  assert(rei_last_error_category() != REI_ERRCAT_NONE);

  rei_zc_unref(v1);
  rei_zc_unref(v2);
  rei_shm_close(v1, 0);
  rei_shm_close(v2, 0);
  rei_shm_close(shm, 1);

  printf("test_view_open: OK\n");
  return 0;
}
