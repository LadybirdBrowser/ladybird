/*
 * Copyright (c) 2025, Tomasz Strejczek
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Demangle.h>

TEST_CASE(class_method)
{
#ifndef AK_OS_WINDOWS
    auto test_string = "_ZNK2AK9Utf16View22unicode_substring_viewEmm"sv;
    auto expected_result = "AK::Utf16View::unicode_substring_view(unsigned long, unsigned long) const"sv;
#else
    auto test_string = "?unicode_substring_view@Utf16View@AK@@QEBA?AV12@_K0@Z"sv;
    auto expected_result = "public: class AK::Utf16View __cdecl AK::Utf16View::unicode_substring_view(unsigned __int64,unsigned __int64)const __ptr64"sv;
#endif

    EXPECT_EQ(expected_result, demangle(test_string));
}

#ifndef AK_OS_WINDOWS
TEST_CASE(backtrace_symbols_lines)
{
    EXPECT_EQ(demangle_backtrace_symbols_line("./TestDemangle(_ZNK2AK9Utf16View22unicode_substring_viewEmm+0x1a) [0x55d0c1a2]"sv),
        "./TestDemangle(AK::Utf16View::unicode_substring_view(unsigned long, unsigned long) const+0x1a) [0x55d0c1a2]"sv);
    EXPECT_EQ(demangle_backtrace_symbols_line("3   liblagom-ak.dylib   0x00000001045e8c3c _ZNK2AK9Utf16View22unicode_substring_viewEmm + 60"sv),
        "3   liblagom-ak.dylib   0x00000001045e8c3c AK::Utf16View::unicode_substring_view(unsigned long, unsigned long) const + 60"sv);
    EXPECT_EQ(demangle_backtrace_symbols_line("./TestDemangle(main+0x10) [0x55d0c1a2]"sv), "./TestDemangle(main+0x10) [0x55d0c1a2]"sv);
    EXPECT_EQ(demangle_backtrace_symbols_line("/build/lib_Zstd/TestDemangle(+0x1a) [0x55d0c1a2]"sv), "/build/lib_Zstd/TestDemangle(+0x1a) [0x55d0c1a2]"sv);
}
#endif
