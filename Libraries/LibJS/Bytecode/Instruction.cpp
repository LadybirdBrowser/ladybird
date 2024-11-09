/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/Op.h>

namespace JS::Bytecode {

void Instruction::destroy(Instruction& instruction)
{
#define __BYTECODE_OP(op)                        \
    case Type::op:                               \
        static_cast<Op::op&>(instruction).~op(); \
        return;

    switch (instruction.type()) {
        ENUMERATE_BYTECODE_OPS(__BYTECODE_OP)
    default:
        VERIFY_NOT_REACHED();
    }

#undef __BYTECODE_OP
}

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

UnrealizedSourceRange InstructionStreamIterator::source_range() const
{
    VERIFY(m_executable);
    auto record = m_executable->source_map.get(offset()).value();
    return {
        .source_code = m_executable->source_code,
        .start_offset = record.source_start_offset,
        .end_offset = record.source_end_offset,
    };
}

RefPtr<SourceCode> InstructionStreamIterator::source_code() const
{
    return m_executable ? m_executable->source_code.ptr() : nullptr;
}

Operand::Operand(Register reg)
    : m_type(Type::Register)
    , m_index(reg.index())
{
}

}
