/* Unit tier seed: the region layer, preamble validator, parker, and
   liveness lock, exercised in-process. Grows into the full unit tier
   (plan step 3). Assert-based; run via `make test`. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

int main(void) {
  assert(mizu_version() != NULL);
  assert(mizu_type_elt_size(MIZU_TYPE_REAL) == 8);
  assert(mizu_type_elt_size(MIZU_TYPE_INT) == 4);
  assert(mizu_type_elt_size(MIZU_TYPE_INT64) == 8);
  assert(mizu_type_elt_size(MIZU_TYPE_STR) == 0);

  /* region create / open / round-trip / close */
  mizu_shm *shm = NULL;
  assert(mizu_shm_create(&shm, 4096) == MIZU_OK);
  assert(mizu_shm_size(shm) == 4096);
  memset(mizu_shm_addr(shm), 0x5A, 4096);

  mizu_shm *ro = NULL;
  assert(mizu_shm_open(&ro, mizu_shm_name(shm)) == MIZU_OK);
  assert(((unsigned char *) mizu_shm_addr(ro))[0] == 0x5A);
  mizu_shm_close(ro, 0);

  mizu_shm *rw = NULL;
  assert(mizu_shm_open_rw(&rw, mizu_shm_name(shm), 1) == MIZU_OK);
  assert(((unsigned char *) mizu_shm_addr(rw))[4095] == 0x5A);
  mizu_shm_close(rw, 0);

  /* a vanished name opens NULL with the error slot set (copy the name
     first — close frees the handle it points into) */
  mizu_shm *gone = NULL;
  char gone_name[MIZU_NAME_MAX];
  snprintf(gone_name, sizeof(gone_name), "%s", mizu_shm_name(shm));
  mizu_shm_close(shm, 1);
  assert(mizu_shm_open(&gone, gone_name) == MIZU_ERR);
  assert(mizu_last_error_category() != MIZU_ERRCAT_NONE);
  assert(mizu_last_error_message()[0] != '\0');

  /* preamble write + validate, incl. a corrupt-magic rejection (the
     region must hold the fixed layout plus both rings: 512 + 2*8*64) */
  mizu_shm *ctl = NULL;
  assert(mizu_shm_create(&ctl, 4096) == MIZU_OK);
  mizu_preamble p;
  memset(&p, 0, sizeof(p));
  p.magic = MIZU_MAGIC;
  p.version = MIZU_ABI_VERSION;
  p.cap = 8;
  p.slot = 64;
  mizu_preamble_write(mizu_shm_addr(ctl), &p);
  mizu_preamble out;
  assert(mizu_preamble_validate(mizu_shm_addr(ctl), mizu_shm_size(ctl),
                               &out) == NULL);
  assert(out.cap == 8 && out.slot == 64);
  memset(mizu_shm_addr(ctl), 0, 4);
  assert(mizu_preamble_validate(mizu_shm_addr(ctl), mizu_shm_size(ctl),
                               &out) != NULL);

  /* parker: an unpark between snapshot and park makes the park return
     WOKEN immediately (the lost-wakeup handshake) */
  mizu_parker pk;
  _Atomic uint32_t *epoch =
    (_Atomic uint32_t *) ((char *) mizu_shm_addr(ctl) +
                          MIZU_ENTITY_OFFSET(MIZU_ENTITY_HOST));
  assert(mizu_parker_attach(&pk, epoch, mizu_shm_name(ctl), MIZU_ENTITY_HOST,
                           1) == 0);
  uint32_t snap = mizu_parker_snapshot(&pk);
  mizu_unpark(&pk);
  assert(mizu_park(&pk, snap, 0) == MIZU_PARK_WOKEN);
  assert(mizu_park(&pk, mizu_parker_snapshot(&pk), 0) == MIZU_PARK_TIMEOUT);
  mizu_parker_detach(&pk);
  mizu_shm_close(ctl, 1);

  /* liveness: a held lock probes HELD on a second handle, ACQUIRED after
     the holder closes */
  const char *dir = mizu_live_dir();
  assert(dir != NULL);
  char path[1024];
  int pn = snprintf(path, sizeof(path), "%s/mizu_test_%ld.live", dir,
                    mizu_self_pid());
  assert(pn > 0 && (size_t) pn < sizeof(path));
  intptr_t h1, h2;
  assert(mizu_live_open(path, &h1) == 0);
  assert(mizu_live_try(h1) == MIZU_LIVE_ACQUIRED);
  assert(mizu_live_open_existing(path, &h2) == 0);
  assert(mizu_live_try(h2) == MIZU_LIVE_HELD);
  mizu_live_close(h1);
  assert(mizu_live_try(h2) == MIZU_LIVE_ACQUIRED);
  mizu_live_unlock(h2);
  mizu_live_close(h2);
  remove(path);

  /* rng jump: deterministic, and changes the state */
  int seed[6] = { 1, 2, 3, 4, 5, 6 };
  int copy[6];
  memcpy(copy, seed, sizeof(seed));
  mizu_rng_jump(seed);
  assert(memcmp(copy, seed, sizeof(seed)) != 0);

  puts("test_region: ok");
  return 0;
}
