/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Bytecode/IdentifierTable.h>
#include <LibJS/Bytecode/Label.h>
#include <LibJS/Bytecode/OpCodes.h>
#include <LibJS/Bytecode/Operand.h>
#include <LibJS/Bytecode/PropertyKeyTable.h>
#include <LibJS/Bytecode/StringTable.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/EnvironmentCoordinate.h>

namespace JS::Bytecode::Op {

#define JS_ENUMERATE_COMMON_BINARY_OPS_WITHOUT_FAST_PATH(O) \
    O(LooselyInequals, loosely_inequals)                    \
    O(LooselyEquals, loosely_equals)                        \
    O(StrictlyInequals, strict_inequals)                    \
    O(StrictlyEquals, strict_equals)

#define JS_ENUMERATE_COMMON_UNARY_OPS(O) \
    O(BitwiseNot, bitwise_not)           \
    O(UnaryPlus, unary_plus)             \
    O(UnaryMinus, unary_minus)

#define JS_ENUMERATE_COMPARISON_OPS(X)            \
    X(LessThan, less_than, <)                     \
    X(LessThanEquals, less_than_equals, <=)       \
    X(GreaterThan, greater_than, >)               \
    X(GreaterThanEquals, greater_than_equals, >=) \
    X(LooselyEquals, loosely_equals, ==)          \
    X(LooselyInequals, loosely_inequals, !=)      \
    X(StrictlyEquals, strict_equals, ==)          \
    X(StrictlyInequals, strict_inequals, !=)

enum class EnvironmentMode {
    Lexical,
    Var,
};

enum class BindingInitializationMode {
    Initialize,
    Set,
};

enum class CallType {
    Call,
    Construct,
    DirectEval,
};

enum class ArgumentsKind {
    Mapped,
    Unmapped,
};

enum class FunctionNamePrefix {
    None,
    Get,
    Set,
};

}

namespace JS::Bytecode {

class alignas(void*) Instruction {
public:
    constexpr static bool IsTerminator = false;
    static constexpr bool IsVariableLength = false;

    enum class Type : u8 {
#define __BYTECODE_OP(op) \
    op,
        ENUMERATE_BYTECODE_OPS(__BYTECODE_OP)
#undef __BYTECODE_OP
    };

    Type type() const { return m_type; }
    size_t length() const;

    Strict strict() const { return m_strict; }
    void set_strict(Strict strict) { m_strict = strict; }

protected:
    explicit Instruction(Type type)
        : m_type(type)
    {
    }

private:
    Type m_type {};
    Strict m_strict {};
};

}
