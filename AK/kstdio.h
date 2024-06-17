/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

#ifdef AK_OS_SERENITY
#    include <AK/Types.h>
#    include <stdarg.h>
extern "C" {
void dbgputstr(char const*, size_t);
int sprintf(char* buf, char const* fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char* buffer, size_t, char const* fmt, ...) __attribute__((format(printf, 3, 4)));
}
#else
#    include <stdio.h>
inline void dbgputstr(char const* characters, size_t length)
{
    fwrite(characters, 1, length, stderr);
}
#endif
template<size_t N>
inline void dbgputstr(char const (&array)[N])
{
    return ::dbgputstr(array, N);
}
