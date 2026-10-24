//test_util.h
// 测试公共工具：断言宏（失败不 abort，继续执行并计数）+ 临时目录辅助
// 用法：
//   每个测试函数中使用 CHECK(...) / CHECK_EQ(...)
//   main 用 RUN_TEST(TestXxx) 注册，结尾打印汇总
#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <type_traits>
#include <unistd.h>

namespace testutil {

// 断言失败时的值打印：支持算术类型、枚举（含 enum class）、bool
template <class T>
inline std::string ToStr(const T& v) {
    if constexpr (std::is_same_v<T, bool>) {
        return v ? "true" : "false";
    } else if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<long long>(v));
    } else if constexpr (std::is_arithmetic_v<T>) {
        return std::to_string(v);
    } else {
        return std::string("<non-arithmetic>");
    }
}

inline int& FailedCount() {
    static int failed = 0;
    return failed;
}

inline int& CheckCount() {
    static int checked = 0;
    return checked;
}

inline void OnCheckFail(const char* file, int line, const std::string& expr) {
    ++FailedCount();
    std::printf("    [FAIL] %s:%d: %s\n", file, line, expr.c_str());
}

// 独立临时目录（每个用例一个，避免互相污染）
inline int& DirCounter() {
    static int c = 0;
    return c;
}

inline std::string MakeTempDir(const std::string& tag) {
    std::string dir = "test_data/" + tag + "_"
                    + std::to_string(getpid()) + "_"
                    + std::to_string(DirCounter()++);
    std::filesystem::create_directories(dir);
    return dir;
}

inline void CleanupDir(const std::string& dir) {
    std::filesystem::remove_all(dir);
}

} // namespace testutil

// ==================== 断言宏 ====================

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++testutil::CheckCount();                                           \
        if (!(cond)) {                                                       \
            testutil::OnCheckFail(__FILE__, __LINE__, "CHECK(" #cond ")");   \
        }                                                                    \
    } while (0)

// 前置条件断言：失败后立即 return 结束当前测试函数（测试函数必须返回 void）
// 用于保护后续依赖该条件的解引用（如 optional::value()）
#define REQUIRE(cond)                                                        \
    do {                                                                     \
        ++testutil::CheckCount();                                           \
        if (!(cond)) {                                                       \
            testutil::OnCheckFail(__FILE__, __LINE__, "REQUIRE(" #cond ")"); \
            return;                                                          \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        ++testutil::CheckCount();                                           \
        auto _va = (a);                                                      \
        auto _vb = (b);                                                      \
        if (!(_va == _vb)) {                                                 \
            std::string _msg = "CHECK_EQ(" #a ", " #b ")  => actual: "       \
                + testutil::ToStr(_va) + " vs expect: " + testutil::ToStr(_vb); \
            testutil::OnCheckFail(__FILE__, __LINE__, _msg);                 \
        }                                                                    \
    } while (0)

// 字符串专用：to_string 对字符串不好用
#define CHECK_STR_EQ(a, b)                                                   \
    do {                                                                     \
        ++testutil::CheckCount();                                           \
        std::string _va = (a);                                               \
        std::string _vb = (b);                                               \
        if (_va != _vb) {                                                    \
            testutil::OnCheckFail(__FILE__, __LINE__,                        \
                ("CHECK_STR_EQ(" #a ", " #b ")  => actual: \"" + _va +       \
                 "\" vs expect: \"" + _vb + "\"").c_str());                  \
        }                                                                    \
    } while (0)

// ==================== 用例注册 ====================

#define RUN_TEST(fn)                                                         \
    do {                                                                     \
        int _before = testutil::FailedCount();                              \
        std::printf("  -> %s ... ", #fn);                                    \
        std::fflush(stdout);                                                 \
        fn();                                                                \
        if (testutil::FailedCount() == _before) {                            \
            std::printf("[PASS]\n");                                         \
        } else {                                                             \
            std::printf("  !! %s has failures\n", #fn);                      \
        }                                                                    \
    } while (0)

#define TEST_SUMMARY_RETURN()                                                \
    do {                                                                     \
        std::printf("\n%d checks, %d failures\n",                            \
                    testutil::CheckCount(), testutil::FailedCount());         \
        if (testutil::FailedCount() != 0) {                                  \
            std::printf("TEST FAILED\n");                                    \
            return 1;                                                        \
        }                                                                    \
        std::printf("ALL TESTS PASSED\n");                                   \
        return 0;                                                            \
    } while (0)
