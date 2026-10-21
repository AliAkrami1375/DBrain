/* Minimal check macros shared by the unit tests. One executable per file. */
#ifndef DBRAIN_TESTS_UNIT_H
#define DBRAIN_TESTS_UNIT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int unit_failures = 0;
static int unit_checks = 0;

#define CHECK(cond) do { \
    unit_checks++; \
    if (!(cond)) { unit_failures++; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_EQ(a, b) do { \
    long long ua_ = (long long)(a), ub_ = (long long)(b); \
    unit_checks++; \
    if (ua_ != ub_) { unit_failures++; \
        fprintf(stderr, "%s:%d: %s = %lld, expected %s = %lld\n", __FILE__, __LINE__, #a, ua_, #b, ub_); } \
} while (0)

#define UNIT_END() do { \
    printf("%s: %d checks, %d failures\n", __FILE__, unit_checks, unit_failures); \
    return unit_failures ? 1 : 0; \
} while (0)

#endif
