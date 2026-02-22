/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Function.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/OpCodes.h>
#include <LibJS/Forward.h>

namespace JS::Bytecode::Op {

#define JS_ENUMERATE_COMMON_BINARY_OPS_WITHOUT_FAST_PATH(O) \
    O(Exp, exp)                                             \
    O(In, in)                                               \
    O(InstanceOf, instance_of)                              \
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
    ByteString to_byte_string(Bytecode::Executable const&) const;
    void visit_labels(Function<void(Label&)> visitor);
    void visit_operands(Function<void(Operand&)> visitor);

    Strict strict() const { return m_strict; }
    void set_strict(Strict strict) { m_strict = strict; }

protected:
    explicit Instruction(Type type)
        : m_type(type)
    {
    }

    void visit_labels_impl(Function<void(Label&)>) { }
    void visit_operands_impl(Function<void(Operand&)>) { }

private:
    Type m_type {};
    Strict m_strict {};
};

class InstructionStreamIterator {
public:
    InstructionStreamIterator(ReadonlyBytes bytes, Executable const* executable = nullptr, size_t offset = 0)
        : m_begin(bytes.data())
        , m_end(bytes.data() + bytes.size())
        , m_ptr(bytes.data() + offset)
        , m_executable(executable)
    {
    }

    size_t offset() const { return m_ptr - m_begin; }
    bool at_end() const { return m_ptr >= m_end; }

    Instruction const& operator*() const { return dereference(); }

    ALWAYS_INLINE void operator++()
    {
        m_ptr += dereference().length();
    }

    Executable const* executable() const { return m_executable; }

private:
    Instruction const& dereference() const { return *reinterpret_cast<Instruction const*>(m_ptr); }

    u8 const* m_begin { nullptr };
    u8 const* m_end { nullptr };
    u8 const* m_ptr { nullptr };
    GC::Ptr<Executable const> m_executable;
};

}
