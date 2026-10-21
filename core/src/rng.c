#include "dbrain/rng.h"

uint32_t rng_mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    x *= 0xC2B2AE35u;
    x ^= x >> 16;
    return x;
}

void rng_seed(rng_t *r, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t s = rng_mix(a ^ 0x9E3779B9u);
    s = rng_mix(s ^ b ^ 0x7F4A7C15u);
    s = rng_mix(s ^ c ^ 0x6A09E667u);
    r->s = s ? s : 0x2545F491u;
}

uint32_t rng_next(rng_t *r)
{
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}

uint32_t rng_below(rng_t *r, uint32_t n)
{
    return (uint32_t)(((uint64_t)rng_next(r) * (uint64_t)n) >> 32);
}

int32_t rng_unit(rng_t *r)
{
    return (int32_t)(rng_next(r) >> 17);
}
