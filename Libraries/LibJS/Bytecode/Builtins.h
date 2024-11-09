/*
 * Copyright (c) 2023, Simon Wanner <simon@skyrising.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <LibJS/Forward.h>

namespace JS::Bytecode {

// TitleCaseName, snake_case_name, base, property, argument_count
#define JS_ENUMERATE_BUILTINS(O)             \
    O(MathAbs, math_abs, Math, abs, 1)       \
    O(MathLog, math_log, Math, log, 1)       \
    O(MathPow, math_pow, Math, pow, 2)       \
    O(MathExp, math_exp, Math, exp, 1)       \
    O(MathCeil, math_ceil, Math, ceil, 1)    \
    O(MathFloor, math_floor, Math, floor, 1) \
    O(MathRound, math_round, Math, round, 1) \
    O(MathSqrt, math_sqrt, Math, sqrt, 1)

enum class Builtin : u8 {
#define DEFINE_BUILTIN_ENUM(name, ...) name,
    JS_ENUMERATE_BUILTINS(DEFINE_BUILTIN_ENUM)
#undef DEFINE_BUILTIN_ENUM
        __Count,
};

static StringView builtin_name(Builtin value)
{
    switch (value) {
#define DEFINE_BUILTIN_CASE(name, snake_case_name, base, property, ...) \
    case Builtin::name:                                                 \
        return #base "." #property##sv;
        JS_ENUMERATE_BUILTINS(DEFINE_BUILTIN_CASE)
#undef DEFINE_BUILTIN_CASE
    case Builtin::__Count:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

inline size_t builtin_argument_count(Builtin value)
{
    switch (value) {
#define DEFINE_BUILTIN_CASE(name, snake_case_name, base, property, arg_count, ...) \
    case Builtin::name:                                                            \
        return arg_count;
        JS_ENUMERATE_BUILTINS(DEFINE_BUILTIN_CASE)
#undef DEFINE_BUILTIN_CASE
    case Builtin::__Count:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

Optional<Builtin> get_builtin(MemberExpression const& expression);

}

namespace AK {

template<>
struct Formatter<JS::Bytecode::Builtin> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, JS::Bytecode::Builtin value)
    {
        return Formatter<StringView>::format(builder, builtin_name(value));
    }
};

}
