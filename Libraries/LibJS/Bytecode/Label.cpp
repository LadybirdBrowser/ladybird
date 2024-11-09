/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Bytecode/BasicBlock.h>
#include <LibJS/Bytecode/Label.h>

namespace JS::Bytecode {

Label::Label(Bytecode::BasicBlock const& basic_block)
    : m_address_or_basic_block_index(basic_block.index())
{
}

}
