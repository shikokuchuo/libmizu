/* Public surface: version, size-stamping initializers, wire-type sizes. */

#include "internal.h"

#define MIZU_STR_(x) #x
#define MIZU_STR(x) MIZU_STR_(x)

const char *mizu_version(void) {
  return MIZU_STR(MIZU_VERSION_MAJOR) "." MIZU_STR(MIZU_VERSION_MINOR) "."
    MIZU_STR(MIZU_VERSION_PATCH);
}

void mizu_binding_init(mizu_binding *b) {
  memset(b, 0, sizeof(*b));
  b->size = (uint32_t) sizeof(*b);
}

/* The opts defaults; max_workers has none (0 = the caller must set
   it). */
void mizu_channel_opts_init(mizu_channel_opts *opts) {
  memset(opts, 0, sizeof(*opts));
  opts->size = (uint32_t) sizeof(*opts);
  opts->capacity = 16384;
  opts->slot_size = 256;
  opts->arena_size = 4u << 20;
}

void mizu_pool_opts_init(mizu_pool_opts *opts) {
  memset(opts, 0, sizeof(*opts));
  opts->size = (uint32_t) sizeof(*opts);
  opts->max_submitters = 8;
  opts->injection_cap = 1024;
  opts->per_worker_cap = 1024;
  opts->result_slots = 4096;
  opts->slot_size = 512;
}

size_t mizu_type_elt_size(int type) {
  switch (type) {
  case MIZU_TYPE_REAL: return sizeof(double);
  case MIZU_TYPE_INT:
  case MIZU_TYPE_LGL: return sizeof(int32_t);
  case MIZU_TYPE_RAW: return 1;
  case MIZU_TYPE_INT64: return sizeof(int64_t);
  case MIZU_TYPE_CPLX: return 2 * sizeof(double);
  default:           return 0;
  }
}
