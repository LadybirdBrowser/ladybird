/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibGC/WeakHashSet.h>
#include <LibJS/Breakpoint.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/SourceRange.h>

namespace JS {

class JS_API Debugger {
    AK_MAKE_NONCOPYABLE(Debugger);
    AK_MAKE_NONMOVABLE(Debugger);

public:
    enum class PauseReason : u8 {
        Entry,
        Breakpoint,
        DebuggerStatement,
    };

    struct PauseInfo {
        GC::Ref<Bytecode::Executable> executable;
        u32 bytecode_offset { 0 };
        Optional<SourceRange> source_range;
        PauseReason reason { PauseReason::Breakpoint };
    };

    Debugger();
    ~Debugger();

    // The callback must call continue_execution() before it returns. It may evaluate JavaScript,
    // in which case any pause triggered by that evaluation is ignored.
    void set_pause_callback(Function<void(PauseInfo const&)> callback) { m_pause_callback = move(callback); }

    bool pause_execution(Bytecode::Executable&, u32 bytecode_offset, PauseReason);
    void continue_execution();
    bool is_paused() const { return m_is_paused; }

    // Set before each instruction is executed, so that a `debugger` statement doesn't pause a
    // second time when we've already paused at a breakpoint on that same instruction.
    void set_did_pause_before_current_instruction(bool value) { m_did_pause_before_current_instruction = value; }
    bool did_pause_before_current_instruction() const { return m_did_pause_before_current_instruction; }

    void request_pause_on_next_bytecode_execution() { m_pause_on_next_bytecode_execution = true; }
    bool should_pause_on_next_bytecode_execution(Bytecode::Executable const&, u32 bytecode_offset);

    ErrorOr<BreakpointID> add_breakpoint(Utf16View filename, u32 line, Optional<u32> column = {});
    bool remove_breakpoint(BreakpointID);
    Vector<Breakpoint> const& breakpoints() const { return m_breakpoints; }
    bool is_breakpoint_resolved(BreakpointID) const;

    void register_executable(Bytecode::Executable&);

private:
    void resolve_breakpoint_in_executable(Breakpoint const&, Bytecode::Executable&);
    void clear_executable_breakpoints();

    Function<void(PauseInfo const&)> m_pause_callback;
    GC::WeakHashSet<Bytecode::Executable> m_executables;
    Vector<Breakpoint> m_breakpoints;
    BreakpointID m_next_breakpoint_id { 1 };
    bool m_is_paused { false };
    bool m_pause_on_next_bytecode_execution { false };
    bool m_did_pause_before_current_instruction { false };
};

}
