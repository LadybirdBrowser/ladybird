/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Debugger.h>

namespace JS {

Debugger::Debugger() = default;

Debugger::~Debugger() = default;

void Debugger::pause_execution(Bytecode::Executable& executable, u32 bytecode_offset, PauseReason reason)
{
    // The pause callback is free to evaluate JavaScript, which may pause again. Only the outermost
    // pause is reported; nested ones are ignored so that the callback can't deadlock against itself.
    if (m_is_paused)
        return;

    if (!m_pause_callback)
        return;

    m_is_paused = true;
    m_pause_callback({
        .executable = executable,
        .bytecode_offset = bytecode_offset,
        .source_range = executable.source_range_at(bytecode_offset),
        .reason = reason,
    });
    VERIFY(!m_is_paused);
}

void Debugger::continue_execution()
{
    VERIFY(m_is_paused);
    m_is_paused = false;
}

}
