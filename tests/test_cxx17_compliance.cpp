// 语言标准守卫：本文件在编译期断言项目处于 C++17，且 C++20 设施不可用。
// 若有人把标准提到 20+，编译会直接失败，而不是悄悄引入不兼容写法。

#include <gtest/gtest.h>

#if defined(__has_include)
#if __has_include(<version>)
#include <version>
#endif
#endif

#if __cplusplus != 201703L
#error "lowlat-video 必须使用 -std=c++17 编译"
#endif

#if defined(__cpp_lib_span)
#error "检测到 std::span 可用：项目禁止使用 C++20 特性"
#endif

#if defined(__cpp_lib_jthread)
#error "检测到 std::jthread 可用：项目禁止使用 C++20 特性"
#endif

#if defined(__cpp_lib_ranges)
#error "检测到 std::ranges 可用：项目禁止使用 C++20 特性"
#endif

TEST(Cxx17Compliance, CompiledAsCxx17)
{
    EXPECT_EQ(__cplusplus, 201703L);
}
