/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// Needed to turn the 'name' token and the preceding 'GCC diagnostic ignored'
// into a single string literal, it won't accept "foo"#bar concatenation.
#define _AK_PRAGMA(x) _Pragma(#x)
#define AK_PRAGMA(x) _AK_PRAGMA(x)

// Helper macro to temporarily disable a diagnostic for the given statement.
// Using _Pragma() makes it possible to use this in other macros as well (and
// allows us to define it as a macro in the first place).
// NOTE: 'GCC' is also recognized by clang.
#define AK_IGNORE_DIAGNOSTIC(name, statement) \
    AK_PRAGMA(GCC diagnostic push);           \
    AK_PRAGMA(GCC diagnostic ignored name);   \
    statement;                                \
    AK_PRAGMA(GCC diagnostic pop);
