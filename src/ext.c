/* ext.c — the exported (extern) half of rei_ext.h's dual-form accessors.

   C TUs (core and binding alike) inline their own copy from the header;
   FFI consumers bind these symbols. REI_EXT_NO_INLINES keeps the
   header's static inlines out of this TU: internal and external linkage
   of one name cannot coexist in a TU. The bodies are the same offset
   math over the same macros — the macros, not the bodies, are the one
   definition.

   The amalgamation excludes this file: folding it into the single rei.c
   TU would redefine the inlines it already carries. FFI consumers of
   the shared library get these symbols; C consumers of the amalgamation
   get the inlines from rei_ext.h. */

#define REI_EXT_NO_INLINES
#include "rei_ext.h"

uint32_t rei_parker_snapshot(const rei_parker *pk) {
  return atomic_load_explicit(pk->epoch, memory_order_acquire);
}

REI_ATOMIC(uint32_t) *rei_zc_rc(void *base) {
  return (REI_ATOMIC(uint32_t) *) ((unsigned char *) base +
                                   REI_ZC_REFCOUNT_OFF);
}

REI_ATOMIC(uint32_t) *rei_zc_flags_(void *base) {
  return (REI_ATOMIC(uint32_t) *) ((unsigned char *) base +
                                   REI_ZC_FLAGS_OFF);
}
