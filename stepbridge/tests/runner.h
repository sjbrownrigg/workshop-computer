#pragma once
#include <cstdio>
#include <cstring>

// Minimal single-header test runner. Zero dependencies.
static int s_pass = 0, s_fail = 0;

#define CHECK(cond, msg) \
    do { if (cond) { ++s_pass; } \
         else { ++s_fail; printf("FAIL [%s:%d] %s\n", __FILE__, __LINE__, (msg)); } \
    } while(0)

#define CHECK_EQ(a, b) \
    do { auto _a=(a); auto _b=(b); \
         if (_a == _b) { ++s_pass; } \
         else { ++s_fail; printf("FAIL [%s:%d] %s == %s  got %lld != %lld\n", \
                __FILE__, __LINE__, #a, #b, (long long)_a, (long long)_b); } \
    } while(0)

inline int report()
{
    printf("\n%d passed, %d failed\n", s_pass, s_fail);
    return s_fail > 0 ? 1 : 0;
}
