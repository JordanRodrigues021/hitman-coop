#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& Registry() {
    static std::vector<Case> s_Cases;
    return s_Cases;
}

inline int& Failures() {
    static int s_Failures = 0;
    return s_Failures;
}

struct Registrar {
    Registrar(const char* p_Name, std::function<void()> p_Fn) { Registry().push_back({ p_Name, std::move(p_Fn) }); }
};

} // namespace test

#define TEST_CONCAT2(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT2(a, b)

#define TEST(name)                                                                       \
    static void name();                                                                  \
    static test::Registrar TEST_CONCAT(s_Registrar_, name)(#name, &name);                \
    static void name()

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            ++test::Failures();                                                          \
        }                                                                                \
    } while (0)

#define CHECK_MSG(cond, ...)                                                             \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::printf("    FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);            \
            std::printf(__VA_ARGS__);                                                    \
            std::printf("\n");                                                           \
            ++test::Failures();                                                          \
        }                                                                                \
    } while (0)

#define REQUIRE(cond)                                                                    \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::printf("    FAIL %s:%d: %s (stopping this test)\n", __FILE__, __LINE__, #cond); \
            ++test::Failures();                                                          \
            return;                                                                      \
        }                                                                                \
    } while (0)
