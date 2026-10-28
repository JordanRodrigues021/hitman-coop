#include <chrono>
#include <cstdio>
#include <cstring>

#include "TestFramework.h"

int main(int argc, char** argv) {
    const char* s_Filter = argc > 1 ? argv[1] : nullptr;
    int s_Ran = 0;
    int s_FailedCases = 0;
    const auto s_Start = std::chrono::steady_clock::now();

    for (const auto& s_Case : test::Registry()) {
        if (s_Filter && !std::strstr(s_Case.name, s_Filter))
            continue;

        const int s_Before = test::Failures();
        std::printf("[ RUN  ] %s\n", s_Case.name);
        s_Case.fn();
        ++s_Ran;

        if (test::Failures() != s_Before) {
            ++s_FailedCases;
            std::printf("[ FAIL ] %s\n", s_Case.name);
        }
        else {
            std::printf("[  OK  ] %s\n", s_Case.name);
        }
    }

    const double s_Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_Start).count();
    std::printf("\n%d tests, %d failed, %d failed checks (%.1f s)\n", s_Ran, s_FailedCases, test::Failures(), s_Seconds);
    return s_FailedCases == 0 ? 0 : 1;
}
