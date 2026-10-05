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
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/SourceRange.h>

namespace JS {

// The debugger of a VM while debugging is enabled. The runtime keeps its breakpoints, stepping state and pause callback,
// so a Debugger is the VM itself seen through this interface, which VM::debugger() hands out.
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

    struct PauseInfo {
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

    // The callback must call continue_execution() before it returns. It may evaluate JavaScript,
    // in which case any pause triggered by that evaluation is ignored.
    void set_pause_callback(Function<void(PauseInfo const&)> callback);

    void continue_execution(ResumeMode = ResumeMode::Continue);
    // Continue after the host filters out a pause without cancelling an active step operation.
    void continue_execution_preserving_step_state();
    bool is_paused() const;
    Vector<FrameBinding> bindings_for_frame(ExecutionContext const&) const;
    ThrowCompletionOr<Value> evaluate_in_frame(ExecutionContext&, Utf16View source_text);

    void request_pause_on_next_bytecode_execution();
    void did_finish_exception_propagation(Value);
    void set_pause_on_exceptions(PauseOnExceptions mode);

    ErrorOr<BreakpointID> add_breakpoint(Utf16View filename, u32 line, Optional<u32> column = {});
    ErrorOr<BreakpointID> add_breakpoint(NonnullRefPtr<SourceCode const>, u32 line, Optional<u32> column = {});
    bool remove_breakpoint(BreakpointID);

private:
    Debugger() = delete;
    ~Debugger() = delete;

    VM& vm() const;
};

}
