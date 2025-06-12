/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibJS/Bytecode/BasicBlock.h>
#include <LibJS/Bytecode/Op.h>

namespace JS::Bytecode {

NonnullOwnPtr<BasicBlock> BasicBlock::create(u32 index, String name)
{
    return adopt_own(*new BasicBlock(index, move(name)));
}

BasicBlock::BasicBlock(u32 index, String name)
    : m_index(index)
    , m_name(move(name))
{
}

BasicBlock::~BasicBlock()
{
    Bytecode::InstructionStreamIterator it(instruction_stream());
    while (!it.at_end()) {
        auto& to_destroy = (*it);
        ++it;
        Instruction::destroy(const_cast<Instruction&>(to_destroy));
    }
}

void BasicBlock::grow(size_t additional_size)
{
    m_buffer.grow_capacity(m_buffer.size() + additional_size);
    m_buffer.resize(m_buffer.size() + additional_size);
}

}
