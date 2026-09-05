/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibGC/WeakHashSet.h>
#include <LibJS/Breakpoint.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/Value.h>
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
        Exception,
        Step,
    };

    enum class PauseOnExceptions : u8 {
        None,
        All,
        Uncaught,
    };

    enum class ResumeMode : u8 {
        Continue,
        StepInto,
        StepOut,
        StepOver,
    };

    enum class UpdateOriginalBindings : u8 {
        No,
        Yes,
    };

    struct PauseInfo {
        GC::Ref<Bytecode::Executable> executable;
        u32 bytecode_offset { 0 };
        Optional<SourceRange> source_range;
        Vector<StackTraceElement> stack_trace;
        Vector<BreakpointID> breakpoint_ids;
        Optional<Value> exception;
        bool exception_will_be_caught { false };
        PauseReason reason { PauseReason::Breakpoint };
    };

    struct FrameBinding {
        Utf16FlyString name;
        Value value;
        bool is_mutable { true };
    };

    Debugger();
    ~Debugger();

    // The callback must call continue_execution() before it returns. It may evaluate JavaScript,
    // in which case any pause triggered by that evaluation is ignored.
    void set_pause_callback(Function<void(PauseInfo const&)> callback) { m_pause_callback = move(callback); }

    bool pause_execution(Bytecode::Executable&, u32 bytecode_offset, PauseReason, Optional<Value> exception = {}, bool exception_will_be_caught = false);
    void continue_execution(ResumeMode = ResumeMode::Continue);
    // Continue after the host filters out a pause without cancelling an active step operation.
    void continue_execution_preserving_step_state();
    bool is_paused() const { return m_is_paused; }
    Vector<FrameBinding> bindings_for_frame(ExecutionContext const&) const;
    ThrowCompletionOr<Value> evaluate_in_frame(ExecutionContext&, Utf16View source_text, UpdateOriginalBindings = UpdateOriginalBindings::Yes);

    // Set before each instruction is executed, so that a `debugger` statement doesn't pause a
    // second time when we've already paused at a breakpoint on that same instruction.
    void set_did_pause_before_current_instruction(bool value) { m_did_pause_before_current_instruction = value; }
    bool did_pause_before_current_instruction() const { return m_did_pause_before_current_instruction; }

    void request_pause_on_next_bytecode_execution() { m_pause_on_next_bytecode_execution = true; }
    bool should_pause_on_next_bytecode_execution(Bytecode::Executable const&, u32 bytecode_offset);
    bool should_pause_for_step(Bytecode::Executable const&, u32 bytecode_offset) const;
    void did_finish_bytecode_execution();
    bool pause_on_exception_if_needed(Bytecode::Executable&, u32 bytecode_offset, Value exception, bool exception_will_be_caught);
    void did_finish_exception_propagation(Value);
    void set_pause_on_exceptions(PauseOnExceptions mode) { m_pause_on_exceptions = mode; }

    ErrorOr<BreakpointID> add_breakpoint(Utf16View filename, u32 line, Optional<u32> column = {});
    ErrorOr<BreakpointID> add_breakpoint(NonnullRefPtr<SourceCode const>, u32 line, Optional<u32> column = {});
    bool remove_breakpoint(BreakpointID);
    Vector<Breakpoint> const& breakpoints() const { return m_breakpoints; }
    bool is_breakpoint_resolved(BreakpointID) const;

    void register_executable(Bytecode::Executable&);

private:
    enum class BindingStorage {
        Argument,
        Local,
    };
    struct BindingLocation {
        BindingStorage storage;
        size_t index;
        bool is_mutable;
        Optional<Position> scope_start;
    };
    HashMap<Utf16FlyString, BindingLocation> binding_locations_for_frame(ExecutionContext const&) const;

    ErrorOr<BreakpointID> add_breakpoint(RefPtr<SourceCode const>, Utf16View filename, u32 line, Optional<u32> column);
    void resolve_breakpoint(Breakpoint const&);
    void resolve_breakpoint_in_executable(Breakpoint const&, Bytecode::Executable&);
    void clear_executable_breakpoints();

    Function<void(PauseInfo const&)> m_pause_callback;
    GC::WeakHashSet<Bytecode::Executable> m_executables;
    Vector<Breakpoint> m_breakpoints;
    BreakpointID m_next_breakpoint_id { 1 };
    struct StepState {
        ResumeMode mode { ResumeMode::Continue };
        u64 frame_id { 0 };
        Optional<SourceRange> source_range;
    };
    Optional<StepState> m_step_state;
    PauseOnExceptions m_pause_on_exceptions { PauseOnExceptions::None };
    GC::Root<Value> m_last_paused_exception;
    ExecutionContext* m_paused_execution_context { nullptr };
    Optional<SourceRange> m_paused_source_range;
    bool m_is_paused { false };
    bool m_pause_on_next_bytecode_execution { false };
    bool m_did_pause_before_current_instruction { false };
};

}
