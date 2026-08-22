/* Public surface: version, size-stamping initializers, wire-type sizes. */

#include "internal.h"

#define REI_STR_(x) #x
#define REI_STR(x) REI_STR_(x)

const char *rei_version(void) {
  return REI_STR(REI_VERSION_MAJOR) "." REI_STR(REI_VERSION_MINOR) "."
    REI_STR(REI_VERSION_PATCH);
}

void rei_binding_init(rei_binding *b) {
  memset(b, 0, sizeof(*b));
  b->size = (uint32_t) sizeof(*b);
}

/* The opts defaults; max_workers has none (0 = the caller must set
   it). */
void rei_channel_opts_init(rei_channel_opts *opts) {
  memset(opts, 0, sizeof(*opts));
  opts->size = (uint32_t) sizeof(*opts);
  opts->capacity = 16384;
  opts->slot_size = 256;
  opts->arena_size = 4u << 20;
}

void rei_pool_opts_init(rei_pool_opts *opts) {
  memset(opts, 0, sizeof(*opts));
  opts->size = (uint32_t) sizeof(*opts);
  opts->max_submitters = 8;
  opts->injection_cap = 1024;
  opts->per_worker_cap = 1024;
  opts->result_slots = 4096;
  opts->slot_size = 512;
}

size_t rei_type_elt_size(int type) {
  switch (type) {
  case REI_TYPE_REAL: return sizeof(double);
  case REI_TYPE_INT:
  case REI_TYPE_LGL: return sizeof(int32_t);
  case REI_TYPE_RAW: return 1;
  case REI_TYPE_CPLX: return 2 * sizeof(double);
  default:           return 0;
  }
}
