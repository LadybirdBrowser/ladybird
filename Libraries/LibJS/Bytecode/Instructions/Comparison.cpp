/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../Instructions.h"

#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Runtime/VM.h>

namespace JS::Bytecode {

ALWAYS_INLINE static ThrowCompletionOr<bool> loosely_inequals(VM& vm, Value src1, Value src2)
{
    if (src1.tag() == src2.tag()) {
        if (src1.is_int32() || src1.is_object() || src1.is_boolean() || src1.is_nullish())
            return src1.encoded() != src2.encoded();
    }
    return !TRY(is_loosely_equal(vm, src1, src2));
}

ALWAYS_INLINE static ThrowCompletionOr<bool> loosely_equals(VM& vm, Value src1, Value src2)
{
    if (src1.tag() == src2.tag()) {
        if (src1.is_int32() || src1.is_object() || src1.is_boolean() || src1.is_nullish())
            return src1.encoded() == src2.encoded();
    }
    return TRY(is_loosely_equal(vm, src1, src2));
}

ALWAYS_INLINE static ThrowCompletionOr<bool> strict_inequals(VM&, Value src1, Value src2)
{
    if (src1.tag() == src2.tag()) {
        if (src1.is_int32() || src1.is_object() || src1.is_boolean() || src1.is_nullish())
            return src1.encoded() != src2.encoded();
    }
    return !is_strictly_equal(src1, src2);
}

ALWAYS_INLINE static ThrowCompletionOr<bool> strict_equals(VM&, Value src1, Value src2)
{
    if (src1.tag() == src2.tag()) {
        if (src1.is_int32() || src1.is_object() || src1.is_boolean() || src1.is_nullish())
            return src1.encoded() == src2.encoded();
    }
    return is_strictly_equal(src1, src2);
}

template<typename OP, ThrowCompletionOr<bool> (*op)(VM&, Value, Value), bool (*numeric_operator)(Value, Value)>
ALWAYS_INLINE FLATTEN void Interpreter::handle_comparison(u8 const* bytecode, size_t& program_counter)
{
    auto& instruction = *reinterpret_cast<OP const*>(&bytecode[program_counter]);
    auto lhs = get(instruction.lhs());
    auto rhs = get(instruction.rhs());
    if (lhs.is_number() && rhs.is_number()) {
        bool result = numeric_operator(lhs, rhs);

        program_counter = result ? instruction.true_target().address() : instruction.false_target().address();
        DISPATCH_NEXT();
    }
    auto result = op(vm(), lhs, rhs);
    if (result.is_error()) [[unlikely]] {
        if (handle_exception(program_counter, result.error_value()) == HandleExceptionResponse::ExitFromExecutable)
            return;
        DISPATCH_NEXT();
    }
    if (result.value())
        program_counter = instruction.true_target().address();
    else
        program_counter = instruction.false_target().address();
    DISPATCH_NEXT();
}

#define HANDLE_COMPARISON_OP(op_TitleCase, op_snake_case, numeric_operator)                  \
    void Interpreter::handle_Jump##op_TitleCase(u8 const* bytecode, size_t& program_counter) \
    {                                                                                        \
        return handle_comparison<Op::Jump##op_TitleCase, op_snake_case,                      \
            [] [[gnu::always_inline]] (Value lhs, Value rhs) static -> bool {                \
                if (lhs.is_int32() && rhs.is_int32())                                        \
                    return lhs.as_i32() numeric_operator rhs.as_i32();                       \
                return lhs.as_double() numeric_operator rhs.as_double();                     \
            }>(bytecode, program_counter);                                                   \
    }

JS_ENUMERATE_COMPARISON_OPS(HANDLE_COMPARISON_OP)
#undef HANDLE_COMPARISON_OP

}
