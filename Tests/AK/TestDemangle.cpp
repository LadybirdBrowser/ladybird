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
