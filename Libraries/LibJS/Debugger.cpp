/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Function.h>
#include <LibJS/Debugger.h>
#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

#define JS_ASSERT_ENUM_VALUE_MATCHES_ABI(enum_name, value_name, abi_prefix, abi_value_name) \
    static_assert(to_underlying(Debugger::enum_name::value_name) == abi_prefix##abi_value_name);
JS_ASSERT_ENUM_VALUE_MATCHES_ABI(PauseReason, Entry, JS_PAUSE_REASON_, ENTRY)
JS_ASSERT_ENUM_VALUE_MATCHES_ABI(PauseReason, Breakpoint, JS_PAUSE_REASON_, BREAKPOINT)
JS_ASSERT_ENUM_VALUE_MATCHES_ABI(PauseReason, DebuggerStatement, JS_PAUSE_REASON_, DEBUGGER_STATEMENT)
JS_ASSERT_ENUM_VALUE_MATCHES_ABI(PauseReason, Exception, JS_PAUSE_REASON_, EXCEPTION)
JS_ASSERT_ENUM_VALUE_MATCHES_ABI(PauseReason, Step, JS_PAUSE_REASON_, STEP)
JS_ASSERT_ENUM_VALUE_MATCHES_ABI(PauseOnExceptions, None, JS_PAUSE_ON_EXCEPTIONS_, NONE)
JS_ASSERT_ENUM_VALUE_MATCHES_ABI(PauseOnExceptions, All, JS_PAUSE_ON_EXCEPTIONS_, ALL)
JS_ASSERT_ENUM_VALUE_MATCHES_ABI(PauseOnExceptions, Uncaught, JS_PAUSE_ON_EXCEPTIONS_, UNCAUGHT)
JS_ASSERT_ENUM_VALUE_MATCHES_ABI(ResumeMode, Continue, JS_RESUME_MODE_, CONTINUE)
JS_ASSERT_ENUM_VALUE_MATCHES_ABI(ResumeMode, StepInto, JS_RESUME_MODE_, STEP_INTO)
JS_ASSERT_ENUM_VALUE_MATCHES_ABI(ResumeMode, StepOut, JS_RESUME_MODE_, STEP_OUT)
JS_ASSERT_ENUM_VALUE_MATCHES_ABI(ResumeMode, StepOver, JS_RESUME_MODE_, STEP_OVER)
#undef JS_ASSERT_ENUM_VALUE_MATCHES_ABI

using PauseCallback = GC::Function<void(Debugger::PauseInfo const&)>;

VM& Debugger::vm() const
{
    return *reinterpret_cast<VM*>(const_cast<Debugger*>(this));
}

// A SourceCode is the runtime's own source code, so the pointers are the same.
static JSSourceCode const* source_code_to_abi(SourceCode const& source_code)
{
    return reinterpret_cast<JSSourceCode const*>(&source_code);
}

static NonnullRefPtr<SourceCode const> source_code_from_abi(JSSourceCode const* source_code)
{
    VERIFY(source_code);
    return *reinterpret_cast<SourceCode const*>(source_code);
}

static ExecutionContext* execution_context_from_abi(JSExecutionContext* execution_context)
{
    return reinterpret_cast<ExecutionContext*>(execution_context);
}

static JSExecutionContext const* execution_context_to_abi(ExecutionContext const& execution_context)
{
    return reinterpret_cast<JSExecutionContext const*>(&execution_context);
}

static Optional<SourceRange> source_range_from_abi(JSDebuggerSourceRange const& source_range)
{
    if (!source_range.source_code)
        return {};
    return SourceRange {
        .code = source_code_from_abi(source_range.source_code),
        .start = { .line = source_range.line, .column = source_range.column },
    };
}

static Debugger::PauseInfo pause_info_from_abi(JSDebuggerPauseInfo const& pause_info)
{
    Debugger::PauseInfo result {
        .bytecode_offset = pause_info.bytecode_offset,
        .source_range = source_range_from_abi(pause_info.source_range),
        .stack_trace = {},
        .breakpoint_ids = {},
        .exception = {},
        .exception_will_be_caught = pause_info.exception_will_be_caught,
        .reason = static_cast<Debugger::PauseReason>(pause_info.reason),
    };

    result.stack_trace.ensure_capacity(pause_info.stack_frame_count);
    for (size_t index = 0; index < pause_info.stack_frame_count; ++index) {
        auto const& stack_frame = pause_info.stack_frames[index];
        result.stack_trace.unchecked_append({
            .execution_context = execution_context_from_abi(stack_frame.execution_context),
            .source_range = source_range_from_abi(stack_frame.source_range),
        });
    }

    if (pause_info.breakpoint_id_count > 0)
        result.breakpoint_ids.append(pause_info.breakpoint_ids, pause_info.breakpoint_id_count);

    if (pause_info.has_exception)
        result.exception = value_from_abi(pause_info.exception);

    return result;
}

// The runtime calls this at each pause, with the callback as its context, which the runtime keeps alive.
static void call_pause_callback(void* context, JSVM*, JSDebuggerPauseInfo const* pause_info)
{
    VERIFY(pause_info);
    auto& pause_callback = static_cast<PauseCallback&>(*static_cast<GC::Cell*>(context));
    pause_callback.function()(pause_info_from_abi(*pause_info));
}

void Debugger::set_pause_callback(Function<void(PauseInfo const&)> callback)
{
    if (!callback) {
        js_debugger_set_pause_callback(vm_to_abi(vm()), nullptr, nullptr);
        return;
    }
    auto pause_callback = PauseCallback::create(vm().heap(), move(callback));
    js_debugger_set_pause_callback(vm_to_abi(vm()), call_pause_callback, static_cast<GC::Cell*>(pause_callback.ptr()));
}

void Debugger::continue_execution(ResumeMode resume_mode)
{
    js_debugger_continue_execution(vm_to_abi(vm()), to_underlying(resume_mode));
}

void Debugger::continue_execution_preserving_step_state()
{
    js_debugger_continue_execution_preserving_step_state(vm_to_abi(vm()));
}

bool Debugger::is_paused() const
{
    return js_debugger_is_paused(vm_to_abi(vm()));
}

static void append_frame_binding(void* context, JSDebuggerFrameBinding const* binding)
{
    VERIFY(binding);
    auto& bindings = *static_cast<Vector<Debugger::FrameBinding>*>(context);
    bindings.append({
        .name = Utf16FlyString::from_utf16(utf16_view_from_abi(binding->name)),
        .value = value_from_abi(binding->value),
        .is_mutable = binding->is_mutable,
    });
}

// The values are the frame's own, which the paused frame keeps alive.
Vector<Debugger::FrameBinding> Debugger::bindings_for_frame(ExecutionContext const& execution_context) const
{
    Vector<FrameBinding> bindings;
    JSDebuggerFrameBindingSink sink { .context = &bindings, .append = append_frame_binding };
    js_debugger_bindings_for_frame(vm_to_abi(vm()), execution_context_to_abi(execution_context), &sink);
    return bindings;
}

ThrowCompletionOr<Value> Debugger::evaluate_in_frame(ExecutionContext& execution_context, Utf16View source_text)
{
    auto* context = const_cast<JSExecutionContext*>(execution_context_to_abi(execution_context));
    return completion_from_abi<Value>(js_debugger_evaluate_in_frame(vm_to_abi(vm()), context, utf16_view_to_abi(source_text)));
}

void Debugger::request_pause_on_next_bytecode_execution()
{
    js_debugger_request_pause_on_next_bytecode_execution(vm_to_abi(vm()));
}

void Debugger::did_finish_exception_propagation(Value exception)
{
    js_debugger_did_finish_exception_propagation(vm_to_abi(vm()), value_to_abi(exception));
}

void Debugger::set_pause_on_exceptions(PauseOnExceptions mode)
{
    js_debugger_set_pause_on_exceptions(vm_to_abi(vm()), to_underlying(mode));
}

static ErrorOr<BreakpointID> breakpoint_id_from_abi(JSDebuggerAddBreakpointResult result)
{
    if (result.error_message)
        return AK::Error::from_string_view(StringView { result.error_message, result.error_message_length });
    return result.breakpoint_id;
}

ErrorOr<BreakpointID> Debugger::add_breakpoint(Utf16View filename, u32 line, Optional<u32> column)
{
    return breakpoint_id_from_abi(js_debugger_add_breakpoint(vm_to_abi(vm()), utf16_view_to_abi(filename), line, column.has_value(), column.value_or(0)));
}

ErrorOr<BreakpointID> Debugger::add_breakpoint(NonnullRefPtr<SourceCode const> source_code, u32 line, Optional<u32> column)
{
    return breakpoint_id_from_abi(js_debugger_add_breakpoint_for_source_code(vm_to_abi(vm()), source_code_to_abi(*source_code), line, column.has_value(), column.value_or(0)));
}

bool Debugger::remove_breakpoint(BreakpointID breakpoint_id)
{
    return js_debugger_remove_breakpoint(vm_to_abi(vm()), breakpoint_id);
}

}
