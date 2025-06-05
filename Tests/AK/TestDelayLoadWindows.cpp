/*
 * Copyright (c) 2025, Tomasz Strejczek
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Demangle.h>

#include <Windows.h>
#include <psapi.h>

static bool is_dll_loaded(wchar_t const* dll_name)
{
    HMODULE mods[1024];
    DWORD needed;
    HANDLE process = GetCurrentProcess();

    if (EnumProcessModules(process, mods, sizeof(mods), &needed)) {
        for (unsigned int i = 0; i < (needed / sizeof(HMODULE)); i++) {
            wchar_t mod_name[MAX_PATH];
            if (GetModuleFileNameExW(process, mods[i], mod_name,
                    sizeof(mod_name) / sizeof(wchar_t))) {
                wchar_t const* base = wcsrchr(mod_name, L'\\');
                if (base && _wcsicmp(base + 1, dll_name) == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

TEST_CASE(class_method)
{
    auto test_string = "?unicode_substring_view@Utf16View@AK@@QEBA?AV12@_K0@Z"sv;
    auto expected_result = "public: class AK::Utf16View __cdecl AK::Utf16View::unicode_substring_view(unsigned __int64,unsigned __int64)const __ptr64"sv;

    EXPECT_EQ(false, is_dll_loaded(L"dbghelp.dll"));
    EXPECT_EQ(expected_result, demangle(test_string));
    EXPECT_EQ(true, is_dll_loaded(L"dbghelp.dll"));
}
