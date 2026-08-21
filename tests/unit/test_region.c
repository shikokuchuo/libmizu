/* Unit tier seed: the region layer, preamble validator, parker, and
   liveness lock, exercised in-process. Grows into the full unit tier
   (plan step 3). Assert-based; run via `make test`. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

int main(void) {
  assert(rei_version() != NULL);
  assert(rei_type_elt_size(REI_TYPE_REAL) == 8);
  assert(rei_type_elt_size(REI_TYPE_INT) == 4);
  assert(rei_type_elt_size(REI_TYPE_STR) == 0);

  /* region create / open / round-trip / close */
  rei_shm *shm = NULL;
  assert(rei_shm_create(&shm, 4096) == REI_OK);
  assert(rei_shm_size(shm) == 4096);
  memset(rei_shm_addr(shm), 0x5A, 4096);

  rei_shm *ro = NULL;
  assert(rei_shm_open(&ro, rei_shm_name(shm)) == REI_OK);
  assert(((unsigned char *) rei_shm_addr(ro))[0] == 0x5A);
  rei_shm_close(ro, 0);

  rei_shm *rw = NULL;
  assert(rei_shm_open_rw(&rw, rei_shm_name(shm), 1) == REI_OK);
  assert(((unsigned char *) rei_shm_addr(rw))[4095] == 0x5A);
  rei_shm_close(rw, 0);

  /* a vanished name opens NULL with the error slot set (copy the name
     first — close frees the handle it points into) */
  rei_shm *gone = NULL;
  char gone_name[REI_NAME_MAX];
  snprintf(gone_name, sizeof(gone_name), "%s", rei_shm_name(shm));
  rei_shm_close(shm, 1);
  assert(rei_shm_open(&gone, gone_name) == REI_ERR);
  assert(rei_last_error_category() != REI_ERRCAT_NONE);
  assert(rei_last_error_message()[0] != '\0');

  /* preamble write + validate, incl. a corrupt-magic rejection (the
     region must hold the fixed layout plus both rings: 512 + 2*8*64) */
  rei_shm *ctl = NULL;
  assert(rei_shm_create(&ctl, 4096) == REI_OK);
  rei_preamble p;
  memset(&p, 0, sizeof(p));
  p.magic = REI_MAGIC;
  p.version = REI_ABI_VERSION;
  p.cap = 8;
  p.slot = 64;
  rei_preamble_write(rei_shm_addr(ctl), &p);
  rei_preamble out;
  assert(rei_preamble_validate(rei_shm_addr(ctl), rei_shm_size(ctl),
                               &out) == NULL);
  assert(out.cap == 8 && out.slot == 64);
  memset(rei_shm_addr(ctl), 0, 4);
  assert(rei_preamble_validate(rei_shm_addr(ctl), rei_shm_size(ctl),
                               &out) != NULL);

  /* parker: an unpark between snapshot and park makes the park return
     WOKEN immediately (the lost-wakeup handshake) */
  rei_parker pk;
  _Atomic uint32_t *epoch =
    (_Atomic uint32_t *) ((char *) rei_shm_addr(ctl) +
                          REI_ENTITY_OFFSET(REI_ENTITY_HOST));
  assert(rei_parker_attach(&pk, epoch, rei_shm_name(ctl), REI_ENTITY_HOST,
                           1) == 0);
  uint32_t snap = rei_parker_snapshot(&pk);
  rei_unpark(&pk);
  assert(rei_park(&pk, snap, 0) == REI_PARK_WOKEN);
  assert(rei_park(&pk, rei_parker_snapshot(&pk), 0) == REI_PARK_TIMEOUT);
  rei_parker_detach(&pk);
  rei_shm_close(ctl, 1);

  /* liveness: a held lock probes HELD on a second handle, ACQUIRED after
     the holder closes */
  const char *dir = rei_live_dir();
  assert(dir != NULL);
  char path[1024];
  int pn = snprintf(path, sizeof(path), "%s/rei_test_%ld.live", dir,
                    rei_self_pid());
  assert(pn > 0 && (size_t) pn < sizeof(path));
  intptr_t h1, h2;
  assert(rei_live_open(path, &h1) == 0);
  assert(rei_live_try(h1) == REI_LIVE_ACQUIRED);
  assert(rei_live_open_existing(path, &h2) == 0);
  assert(rei_live_try(h2) == REI_LIVE_HELD);
  rei_live_close(h1);
  assert(rei_live_try(h2) == REI_LIVE_ACQUIRED);
  rei_live_unlock(h2);
  rei_live_close(h2);
  remove(path);

  /* rng jump: deterministic, and changes the state */
  int seed[6] = { 1, 2, 3, 4, 5, 6 };
  int copy[6];
  memcpy(copy, seed, sizeof(seed));
  rei_rng_jump(seed);
  assert(memcmp(copy, seed, sizeof(seed)) != 0);

  puts("test_region: ok");
  return 0;
}
