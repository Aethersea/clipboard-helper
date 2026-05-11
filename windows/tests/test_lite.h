#pragma once
//
// test_lite — minimal in-process unit test framework.
//
// Design choices that match the helper's "no external runtime deps" stance
// (see CMakeLists.txt rationale on libprotobuf / vcpkg):
//   - Header-only, no link-time dependencies, no third-party fetch.
//   - Auto-registration via static initializers — `TEST(suite, name)` adds
//     to a global registry that `RunAll` walks.
//   - Assertions record failures into a thread-local `Current()` state so
//     ASSERT_* can early-return without exceptions.
//
// Usage:
//   TEST(Suite, NameOfCase) {
//       EXPECT_EQ(1 + 1, 2);
//       ASSERT_TRUE(some_invariant());
//   }
//
//   int main(int argc, char** argv) {
//       return ::test_lite::RunAll(argc, argv);
//   }

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <ios>
#include <sstream>
#include <string>
#include <vector>

namespace test_lite {

struct TestCase {
    const char*           suite;
    const char*           name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> r;
    return r;
}

struct State {
    bool failed{false};
};

inline State& Current() {
    static thread_local State s;
    return s;
}

inline void FailAt(const char* file, int line, const std::string& message) {
    Current().failed = true;
    std::fprintf(stderr, "  FAIL %s:%d  %s\n", file, line, message.c_str());
}

// Streamable → string. Falls back to a generic stub when T isn't streamable
// (vector<uint8_t> etc. get a custom overload below).
template <typename T, typename = void>
struct Stringify {
    static std::string Apply(const T&) { return "<unprintable>"; }
};

template <typename T>
struct Stringify<T, decltype(void(std::declval<std::ostream&>() << std::declval<T>()))> {
    static std::string Apply(const T& v) {
        std::ostringstream os;
        os << v;
        return os.str();
    }
};

template <>
struct Stringify<bool, void> {
    static std::string Apply(bool v) { return v ? "true" : "false"; }
};

template <>
struct Stringify<std::vector<std::uint8_t>, void> {
    static std::string Apply(const std::vector<std::uint8_t>& v) {
        std::ostringstream os;
        os << "[" << v.size() << " bytes:";
        const std::size_t cap = v.size() < 32 ? v.size() : 32;
        for (std::size_t i = 0; i < cap; ++i) {
            char hex[5];
            std::snprintf(hex, sizeof(hex), " %02x", v[i]);
            os << hex;
        }
        if (v.size() > cap) os << " ...";
        os << "]";
        return os.str();
    }
};

template <typename T>
std::string ToStr(const T& v) { return Stringify<T>::Apply(v); }

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> fn) {
        Registry().push_back({suite, name, std::move(fn)});
    }
};

inline int RunAll(int argc, char** argv) {
    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--filter=", 9) == 0) {
            filter = argv[i] + 9;
        }
    }

    int passed = 0;
    int failed = 0;
    int skipped = 0;
    for (const auto& tc : Registry()) {
        std::string full = std::string(tc.suite) + "." + tc.name;
        if (filter && full.find(filter) == std::string::npos) {
            ++skipped;
            continue;
        }
        std::printf("[ RUN  ] %s\n", full.c_str());
        std::fflush(stdout);
        Current() = State{};
        try {
            tc.fn();
        } catch (const std::exception& e) {
            FailAt(__FILE__, __LINE__,
                   std::string("uncaught std::exception: ") + e.what());
        } catch (...) {
            FailAt(__FILE__, __LINE__, "uncaught exception of unknown type");
        }
        if (Current().failed) {
            std::printf("[ FAIL ] %s\n\n", full.c_str());
            ++failed;
        } else {
            std::printf("[  OK  ] %s\n", full.c_str());
            ++passed;
        }
        std::fflush(stdout);
    }
    std::printf("\n[ SUM  ] %d passed, %d failed, %d skipped\n",
                passed, failed, skipped);
    return failed == 0 ? 0 : 1;
}

}  // namespace test_lite

// ─── Macros ───────────────────────────────────────────────────────────────

#define TL_CONCAT_INNER(a, b) a##b
#define TL_CONCAT(a, b) TL_CONCAT_INNER(a, b)

#define TEST(suite, name)                                                  \
    static void TL_CONCAT(TestFn_##suite##_##name##_, __LINE__)();          \
    static ::test_lite::Registrar                                           \
        TL_CONCAT(TestReg_##suite##_##name##_, __LINE__)(                   \
            #suite, #name,                                                  \
            TL_CONCAT(TestFn_##suite##_##name##_, __LINE__));               \
    static void TL_CONCAT(TestFn_##suite##_##name##_, __LINE__)()

#define EXPECT_TRUE(expr)                                                  \
    do {                                                                   \
        if (!(expr)) {                                                     \
            ::test_lite::FailAt(__FILE__, __LINE__,                        \
                std::string("EXPECT_TRUE(" #expr ") failed"));              \
        }                                                                  \
    } while (0)

#define EXPECT_FALSE(expr)                                                 \
    do {                                                                   \
        if (expr) {                                                        \
            ::test_lite::FailAt(__FILE__, __LINE__,                        \
                std::string("EXPECT_FALSE(" #expr ") failed"));             \
        }                                                                  \
    } while (0)

#define TL_BIN_OP(op_name, op, a_expr, b_expr)                             \
    do {                                                                   \
        const auto& _tl_a = (a_expr);                                      \
        const auto& _tl_b = (b_expr);                                      \
        if (!(_tl_a op _tl_b)) {                                           \
            ::test_lite::FailAt(__FILE__, __LINE__,                        \
                std::string(op_name "(" #a_expr ", " #b_expr ") failed: ")  \
                  + ::test_lite::ToStr(_tl_a)                              \
                  + " " #op " "                                            \
                  + ::test_lite::ToStr(_tl_b));                            \
        }                                                                  \
    } while (0)

#define EXPECT_EQ(a, b) TL_BIN_OP("EXPECT_EQ", ==, a, b)
#define EXPECT_NE(a, b) TL_BIN_OP("EXPECT_NE", !=, a, b)
#define EXPECT_LT(a, b) TL_BIN_OP("EXPECT_LT", <,  a, b)
#define EXPECT_LE(a, b) TL_BIN_OP("EXPECT_LE", <=, a, b)
#define EXPECT_GT(a, b) TL_BIN_OP("EXPECT_GT", >,  a, b)
#define EXPECT_GE(a, b) TL_BIN_OP("EXPECT_GE", >=, a, b)

#define ASSERT_TRUE(expr)                                                  \
    do {                                                                   \
        if (!(expr)) {                                                     \
            ::test_lite::FailAt(__FILE__, __LINE__,                        \
                std::string("ASSERT_TRUE(" #expr ") failed"));              \
            return;                                                        \
        }                                                                  \
    } while (0)

#define ASSERT_EQ(a, b)                                                    \
    do {                                                                   \
        EXPECT_EQ(a, b);                                                   \
        if (::test_lite::Current().failed) return;                          \
    } while (0)
