/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/Op.h>

namespace JS::Bytecode {

void Instruction::visit_labels(Function<void(JS::Bytecode::Label&)> visitor)
{
#define __BYTECODE_OP(op)                                             \
    case Type::op:                                                    \
        static_cast<Op::op&>(*this).visit_labels_impl(move(visitor)); \
        return;

    switch (type()) {
        ENUMERATE_BYTECODE_OPS(__BYTECODE_OP)
    default:
        VERIFY_NOT_REACHED();
    }

#undef __BYTECODE_OP
}

void Instruction::visit_operands(Function<void(JS::Bytecode::Operand&)> visitor)
{
#define __BYTECODE_OP(op)                                               \
    case Type::op:                                                      \
        static_cast<Op::op&>(*this).visit_operands_impl(move(visitor)); \
        return;

    switch (type()) {
        ENUMERATE_BYTECODE_OPS(__BYTECODE_OP)
    default:
        VERIFY_NOT_REACHED();
    }

#undef __BYTECODE_OP
}

template<typename Op>
concept HasVariableLength = Op::IsVariableLength;

template<typename Op>
concept HasFixedLength = !Op::IsVariableLength;

template<HasVariableLength Op>
size_t get_length_impl(Op const& op)
{
    return op.length_impl();
}

// Function template for types without a length_impl method
template<HasFixedLength Op>
size_t get_length_impl(Op const&)
{
    return sizeof(Op);
}

size_t Instruction::length() const
{
#define __BYTECODE_OP(op)                                   \
    case Type::op: {                                        \
        auto& typed_op = static_cast<Op::op const&>(*this); \
        return get_length_impl(typed_op);                   \
    }

    switch (type()) {
        ENUMERATE_BYTECODE_OPS(__BYTECODE_OP)
    default:
        VERIFY_NOT_REACHED();
    }

#undef __BYTECODE_OP
}

Operand::Operand(Register reg)
    : Operand(Type::Register, reg.index())
{
}

}
