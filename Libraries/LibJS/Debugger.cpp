/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibGC/Heap.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Debugger.h>

namespace JS {

Debugger::Debugger() = default;

Debugger::~Debugger()
{
    clear_executable_breakpoints();
}

bool Debugger::pause_execution(Bytecode::Executable& executable, u32 bytecode_offset, PauseReason reason)
{
    // The pause callback is free to evaluate JavaScript, which may pause again. Only the outermost
    // pause is reported; nested ones are ignored so that the callback can't deadlock against itself.
    if (m_is_paused)
        return false;

    if (!m_pause_callback)
        return false;

    if (reason == PauseReason::Entry)
        m_pause_on_next_bytecode_execution = false;
    m_is_paused = true;
    m_pause_callback({
        .executable = executable,
        .bytecode_offset = bytecode_offset,
        .source_range = executable.source_range_at(bytecode_offset),
        .reason = reason,
    });
    VERIFY(!m_is_paused);
    return true;
}

void Debugger::continue_execution()
{
    VERIFY(m_is_paused);
    m_is_paused = false;
}

bool Debugger::should_pause_on_next_bytecode_execution(Bytecode::Executable const& executable, u32 bytecode_offset)
{
    if (!m_pause_on_next_bytecode_execution)
        return false;

    if (auto source_range = executable.source_range_at(bytecode_offset); source_range.has_value() && source_range->start.line > 0)
        return true;

    for (auto const& entry : executable.source_map) {
        if (entry.bytecode_offset > bytecode_offset && entry.line > 0)
            return false;
    }

    return true;
}

ErrorOr<BreakpointID> Debugger::add_breakpoint(Utf16View filename, u32 line, Optional<u32> column)
{
    if (line == 0)
        return AK::Error::from_string_literal("Breakpoint line must be greater than zero");

    for (auto const& breakpoint : m_breakpoints) {
        if (breakpoint.filename == filename && breakpoint.line == line && breakpoint.column == column)
            return breakpoint.id;
    }

    if (m_next_breakpoint_id == NumericLimits<BreakpointID>::max())
        return AK::Error::from_string_literal("Too many breakpoints");

    m_breakpoints.empend(m_next_breakpoint_id++, Utf16String::from_utf16(filename), line, column);
    auto const& breakpoint = m_breakpoints.last();
    for (auto& executable : m_executables)
        resolve_breakpoint_in_executable(breakpoint, executable);
    return breakpoint.id;
}

bool Debugger::remove_breakpoint(BreakpointID breakpoint_id)
{
    auto did_remove = m_breakpoints.remove_first_matching([&](auto const& breakpoint) {
        return breakpoint.id == breakpoint_id;
    });
    if (!did_remove)
        return false;

    for (auto& executable : m_executables)
        executable.remove_debugger_breakpoint(breakpoint_id);
    return true;
}

bool Debugger::is_breakpoint_resolved(BreakpointID breakpoint_id) const
{
    for (auto& executable : m_executables) {
        if (executable.has_debugger_breakpoint(breakpoint_id))
            return true;
    }
    return false;
}

void Debugger::register_executable(Bytecode::Executable& executable)
{
    if (m_executables.contains(executable))
        return;

    m_executables.set(executable);
    for (auto const& breakpoint : m_breakpoints)
        resolve_breakpoint_in_executable(breakpoint, executable);
}

void Debugger::resolve_breakpoint_in_executable(Breakpoint const& breakpoint, Bytecode::Executable& executable)
{
    if (executable.source_code->filename() != breakpoint.filename)
        return;

    Bytecode::SourceMapEntry const* matching_entry = nullptr;
    for (auto const& entry : executable.source_map) {
        if (entry.line == 0 || entry.line < breakpoint.line)
            continue;
        if (entry.line == breakpoint.line && breakpoint.column.has_value() && entry.column < *breakpoint.column)
            continue;
        if (!matching_entry || entry.line < matching_entry->line || (entry.line == matching_entry->line && entry.column < matching_entry->column))
            matching_entry = &entry;
    }

    if (matching_entry)
        executable.add_debugger_breakpoint(matching_entry->bytecode_offset, breakpoint.id);
}

void Debugger::clear_executable_breakpoints()
{
    for (auto& executable : m_executables)
        executable.clear_debugger_breakpoints();
}

}
