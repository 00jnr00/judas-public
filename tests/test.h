// Minimal test helpers: CHECK keeps going, REQUIRE stops the test. Each test file has its own main().
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>

inline int judas_test_failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            judas_test_failures++;                                                    \
        }                                                                             \
    } while (0)
#define REQUIRE(cond)                                                                 \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "%s:%d: REQUIRE failed: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                             \
        }                                                                             \
    } while (0)
#define TEST_MAIN_END()                                                               \
    do {                                                                              \
        if (judas_test_failures) std::fprintf(stderr, "%d check(s) failed\n", judas_test_failures); \
        return judas_test_failures ? 1 : 0;                                           \
    } while (0)
