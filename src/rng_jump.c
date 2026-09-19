/* L'Ecuyer-CMRG RNG stream jumping -----------------------------------------------
 *
 * Pure-C implementation of MRG32k3a stream jumping. Moduli and jump matrix
 * constants below are from the RngStreams package by Pierre L'Ecuyer,
 * University of Montreal
 * (https://github.com/umontreal-simul/RngStreams), licensed under the Apache
 * License, Version 2.0. The original copyright notice requests citation of:
 *
 *   P. L'Ecuyer, "Good Parameter Sets for Combined Multiple Recursive Random
 *     Number Generators", Operations Research, 47, 1 (1999), 159-164.
 *   P. L'Ecuyer, R. Simard, E. J. Chen, and W. D. Kelton, "An Objected-
 *     Oriented Random-Number Package with Many Long Streams and Substreams",
 *     Operations Research, 50, 6 (2002), 1073-1075.
 *
 * The kernel ships in the core so a future mizu_map can derive per-element
 * streams. */

#include "internal.h"

#define MIZU_RNG_M1 4294967087ULL
#define MIZU_RNG_M2 4294944443ULL

/* Jump matrices A1^(2^127) mod m1 and A2^(2^127) mod m2 */
static const unsigned long long A1p127[3][3] = {
  { 2427906178ULL, 3580155704ULL,  949770784ULL },
  {  226153695ULL, 1230515664ULL, 3580155704ULL },
  { 1988835001ULL,  986791581ULL, 1230515664ULL }
};
static const unsigned long long A2p127[3][3] = {
  { 1464411153ULL,  277697599ULL, 1610723613ULL },
  {   32183930ULL, 1464411153ULL, 1022607788ULL },
  { 2824425944ULL,   32183930ULL, 2093834863ULL }
};

static void mat_vec_mod(const unsigned long long A[3][3],
                        const unsigned long long *v,
                        unsigned long long *out, unsigned long long m) {
  for (int i = 0; i < 3; i++) {
    unsigned long long s = 0;
    for (int j = 0; j < 3; j++) {
      s = (s + (A[i][j] * v[j]) % m) % m;
    }
    out[i] = s;
  }
}

/* One 2^127-step stream jump in place over a 6-word CMRG state, held as
   signed ints with values in [0, m). */
void mizu_rng_jump(int *seed) {
  unsigned long long v1[3] = { (unsigned int) seed[0], (unsigned int) seed[1],
                               (unsigned int) seed[2] };
  unsigned long long v2[3] = { (unsigned int) seed[3], (unsigned int) seed[4],
                               (unsigned int) seed[5] };
  unsigned long long out1[3], out2[3];
  mat_vec_mod(A1p127, v1, out1, MIZU_RNG_M1);
  mat_vec_mod(A2p127, v2, out2, MIZU_RNG_M2);
  for (int i = 0; i < 3; i++) {
    seed[i]     = (int) out1[i];
    seed[i + 3] = (int) out2[i];
  }
}
