/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Bytecode/Generator.h>
#include <LibJS/Bytecode/ScopedOperand.h>

namespace JS::Bytecode {

ScopedOperandImpl::~ScopedOperandImpl()
{
    if (!m_generator.is_finished() && m_operand.is_register() && m_operand.as_register().index() >= Register::reserved_register_count)
        m_generator.free_register(m_operand.as_register());
}

Register Operand::as_register() const
{
    return Register { m_index };
}

}
