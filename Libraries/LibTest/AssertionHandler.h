/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <LibTest/Export.h>
#include <setjmp.h>

#ifndef AK_OS_WINDOWS
#    define LIBTEST_SETJMP(env) sigsetjmp(env, 1)
#    define LIBTEST_LONGJMP siglongjmp
#else
#    define LIBTEST_SETJMP(env) setjmp(env)
#    define LIBTEST_LONGJMP longjmp
#endif

namespace Test {

jmp_buf& assertion_jump_buffer();
void set_assertion_jump_validity(bool);
bool assertion_jump_validity();

}
