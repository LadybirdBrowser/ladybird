/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <wchar.h>

TEST_CASE(swprint_no_format)
{
    wchar_t buffer[256];
    size_t len = swprintf(buffer, 64, L"Well, hello friends!");

    VERIFY(wcscmp(buffer, L"Well, hello friends!") == 0);
    VERIFY(wcscmp(buffer, L"Well, hello friends") != 0);
    VERIFY(wcslen(buffer) == len);
}

TEST_CASE(swprint_single_wchar_argument)
{
    wchar_t buffer[256];
    size_t len = swprintf(buffer, 64, L"Well, %ls friends!", L"hello");

    VERIFY(wcscmp(buffer, L"Well, hello friends!") == 0);
    VERIFY(wcscmp(buffer, L"Well, hello friends") != 0);
    VERIFY(wcslen(buffer) == len);
}

TEST_CASE(swprint_single_char_argument)
{
    wchar_t buffer[256];
#ifdef AK_OS_WINDOWS
    size_t len = swprintf(buffer, 32, L"Well, %s friends!", L"hello");
#else
    size_t len = swprintf(buffer, 32, L"Well, %s friends!", "hello");
#endif

    VERIFY(wcscmp(buffer, L"Well, hello friends!") == 0);
    VERIFY(wcscmp(buffer, L"Well, hello friends") != 0);
    VERIFY(wcslen(buffer) == len);
}

TEST_CASE(swprint_single_narrow_char_argument)
{
    wchar_t buffer[256];
    size_t len = swprintf(buffer, 64, L"Well, %hs friends!", "hello");

    VERIFY(wcscmp(buffer, L"Well, hello friends!") == 0);
    VERIFY(wcscmp(buffer, L"Well, hello friends") != 0);
    VERIFY(wcslen(buffer) == len);
}

TEST_CASE(swprint_mixed_arguments)
{
    wchar_t buffer[256];
#ifdef AK_OS_WINDOWS
    size_t len = swprintf(buffer, 64, L"Well, %s friends! %hs is less then %s.", L"hello", "10", L"20");
#else
    size_t len = swprintf(buffer, 64, L"Well, %ls friends! %hs is less then %s.", L"hello", "10", "20");
#endif

    VERIFY(wcscmp(buffer, L"Well, hello friends! 10 is less then 20.") == 0);
    VERIFY(wcscmp(buffer, L"Well, hello friends! 10 is less then 2.") != 0);
    VERIFY(wcslen(buffer) == len);
}
