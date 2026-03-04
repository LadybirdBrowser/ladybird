/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/InterpreterStack.h>
#include <sys/mman.h>

namespace JS {

InterpreterStack::InterpreterStack()
{
    m_base = mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    VERIFY(m_base != MAP_FAILED);
    m_top = m_base;
    m_limit = static_cast<u8*>(m_base) + stack_size;
}

InterpreterStack::~InterpreterStack()
{
    munmap(m_base, stack_size);
}

}
