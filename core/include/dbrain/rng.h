/*
 * Deterministic pseudo-random numbers for recipe mode.
 *
 * xorshift32 (Marsaglia) seeded through a 32-bit finaliser mix. Every node
 * and the host compiler must draw identical sequences from identical seeds,
 * so this generator is part of the model version, not an implementation
 * detail. rng_below() uses the multiply-shift reduction, which is exactly
 * reproducible with 64-bit integers in any language.
 */
#ifndef DBRAIN_RNG_H
#define DBRAIN_RNG_H

#include <stdint.h>

typedef struct {
    uint32_t s;
} rng_t;

/* Murmur3 finaliser. */
uint32_t rng_mix(uint32_t x);

/* Seeds from up to three words; the state is never zero. */
void rng_seed(rng_t *r, uint32_t a, uint32_t b, uint32_t c);

uint32_t rng_next(rng_t *r);

/* Uniform in [0, n); n must be > 0. */
uint32_t rng_below(rng_t *r, uint32_t n);

/* Uniform in [0, 1) as s16.15. */
int32_t rng_unit(rng_t *r);

#endif /* DBRAIN_RNG_H */
