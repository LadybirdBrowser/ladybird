/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/Macros.h>

namespace Test {

static jmp_buf g_assert_jmp_buf = {};

static bool g_assert_jmp_buf_valid = false;

jmp_buf& assertion_jump_buffer() { return g_assert_jmp_buf; }

void set_assertion_jump_validity(bool validity)
{
    g_assert_jmp_buf_valid = validity;
}

static bool is_assertion_jump_valid()
{
    return g_assert_jmp_buf_valid;
}

static void assertion_handler_impl(char const*)
{
    if (is_assertion_jump_valid()) {
        set_assertion_jump_validity(false);
        LIBTEST_LONGJMP(assertion_jump_buffer(), 1); /* NOLINT(cert-err52-cpp, bugprone-setjmp-longjmp) Isolated to test infrastructure and allows us to not depend on spawning child processes for death tests */
    }
    // Fall through to default assertion handler
}

}

#if defined(AK_OS_WINDOWS)
#    define EXPORT __declspec(dllexport)
#else
#    define EXPORT __attribute__((visibility("default")))
#endif

extern "C" EXPORT void ak_assertion_handler(char const* message);
void ak_assertion_handler(char const* message)
{
    ::Test::assertion_handler_impl(message);
}
