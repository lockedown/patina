// TestFramework.h
//
// A deliberately tiny, dependency-free test harness -- no GoogleTest, no
// Catch2, matching this project's zero-dependency stance. A test file
// registers cases with AKZ_TEST(name) { ... } and asserts with
// AKZ_CHECK(expr) / AKZ_CHECK_NEAR(a, b, tol). main() (in main.cpp) runs
// every registered case and reports a pass/fail summary.

#ifndef AKAIZER_TEST_FRAMEWORK_H
#define AKAIZER_TEST_FRAMEWORK_H

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace akztest {

struct TestCase {
    std::string name;
    std::function<void()> run;
};

// Registry is a function-local static rather than a global so registration
// order is well-defined regardless of translation unit link order.
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

inline int& failureCount() {
    static int count = 0;
    return count;
}

inline const char*& currentTestName() {
    static const char* name = "";
    return name;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline void reportFailure(const char* file, int line, const std::string& message) {
    ++failureCount();
    std::fprintf(stderr, "  FAIL [%s] %s:%d: %s\n", currentTestName(), file, line, message.c_str());
}

inline int runAll() {
    int passed = 0;
    for (auto& test : registry()) {
        currentTestName() = test.name.c_str();
        int before = failureCount();
        std::printf("RUN  %s\n", test.name.c_str());
        test.run();
        if (failureCount() == before) {
            ++passed;
            std::printf("PASS %s\n", test.name.c_str());
        } else {
            std::printf("FAIL %s\n", test.name.c_str());
        }
    }
    std::printf("\n%d/%zu tests passed\n", passed, registry().size());
    return failureCount() == 0 ? 0 : 1;
}

} // namespace akztest

#define AKZ_TEST(name)                                                                 \
    static void akz_test_##name();                                                     \
    static akztest::Registrar akz_registrar_##name(#name, akz_test_##name);            \
    static void akz_test_##name()

#define AKZ_CHECK(expr)                                                                 \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            akztest::reportFailure(__FILE__, __LINE__, "AKZ_CHECK failed: " #expr);     \
        }                                                                               \
    } while (0)

#define AKZ_CHECK_EQ(a, b)                                                              \
    do {                                                                               \
        if (!((a) == (b))) {                                                           \
            akztest::reportFailure(__FILE__, __LINE__,                                 \
                std::string("AKZ_CHECK_EQ failed: " #a " == " #b " (")                 \
                    + std::to_string(a) + " vs " + std::to_string(b) + ")");           \
        }                                                                               \
    } while (0)

#define AKZ_CHECK_NEAR(a, b, tol)                                                       \
    do {                                                                               \
        double _akz_diff = std::fabs(static_cast<double>(a) - static_cast<double>(b)); \
        if (_akz_diff > (tol)) {                                                       \
            akztest::reportFailure(__FILE__, __LINE__,                                 \
                std::string("AKZ_CHECK_NEAR failed: |" #a " - " #b "| = ")             \
                    + std::to_string(_akz_diff) + " > " + std::to_string((double)(tol))); \
        }                                                                               \
    } while (0)

#endif // AKAIZER_TEST_FRAMEWORK_H
