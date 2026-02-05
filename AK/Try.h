/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Diagnostics.h>
#include <AK/StdLibExtras.h>

// This macro is redefined in `AK/Format.h` to give a nicer error message.
#define AK_HANDLE_UNEXPECTED_ERROR(result) VERIFY(!result.is_error());

// NOTE: This macro works with any result type that has the expected APIs.
//       It's designed with AK::Result and AK::Error in mind.
//
//       It depends on a non-standard C++ extension, specifically
//       on statement expressions [1]. This is known to be implemented
//       by at least clang and gcc.
//       [1] https://gcc.gnu.org/onlinedocs/gcc/Statement-Exprs.html
//
//       If the static_assert below is triggered, it means you tried to return a reference
//       from a fallible expression. This will not do what you want; the statement expression
//       will create a copy regardless, so it is explicitly disallowed.

#define TRY(...)                                                                                     \
    ({                                                                                               \
        /* Ignore -Wshadow to allow nesting the macro. */                                            \
        AK_IGNORE_DIAGNOSTIC("-Wshadow",                                                             \
            auto&& _temporary_result = (__VA_ARGS__));                                               \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_temporary_result.release_value())>, \
            "Do not return a reference from a fallible expression");                                 \
        if (_temporary_result.is_error()) [[unlikely]]                                               \
            return _temporary_result.release_error();                                                \
        _temporary_result.release_value();                                                           \
    })

#define MUST(...)                                                                                    \
    ({                                                                                               \
        /* Ignore -Wshadow to allow nesting the macro. */                                            \
        AK_IGNORE_DIAGNOSTIC("-Wshadow",                                                             \
            auto&& _temporary_result = (__VA_ARGS__));                                               \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_temporary_result.release_value())>, \
            "Do not return a reference from a fallible expression");                                 \
        AK_HANDLE_UNEXPECTED_ERROR(_temporary_result)                                                \
        _temporary_result.release_value();                                                           \
    })
