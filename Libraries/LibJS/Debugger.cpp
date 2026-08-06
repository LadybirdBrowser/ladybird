/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <AK/HashMap.h>
#include <AK/NumericLimits.h>
#include <LibGC/Heap.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Debugger.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

Debugger::Debugger() = default;

Debugger::~Debugger()
{
    clear_executable_breakpoints();
}

bool Debugger::pause_execution(Bytecode::Executable& executable, u32 bytecode_offset, PauseReason reason, Optional<Value> exception, bool exception_will_be_caught)
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

    auto stack_trace = executable.vm().stack_trace();
    VERIFY(!stack_trace.is_empty());
    auto source_range = executable.source_range_at(bytecode_offset);
    stack_trace.first().source_range = source_range;
    m_paused_execution_context = stack_trace.first().execution_context;
    m_paused_source_range = source_range;
    m_pause_callback({
        .executable = executable,
        .bytecode_offset = bytecode_offset,
        .source_range = move(source_range),
        .stack_trace = move(stack_trace),
        .breakpoint_ids = Vector<BreakpointID> { executable.debugger_breakpoints_at(bytecode_offset) },
        .exception = exception,
        .exception_will_be_caught = exception_will_be_caught,
        .reason = reason,
    });
    VERIFY(!m_is_paused);
    m_paused_execution_context = nullptr;
    m_paused_source_range.clear();
    return true;
}

void Debugger::continue_execution_preserving_step_state()
{
    VERIFY(m_is_paused);
    m_is_paused = false;
}

void Debugger::continue_execution(ResumeMode mode)
{
    VERIFY(m_is_paused);
    if (mode == ResumeMode::Continue) {
        m_step_state.clear();
    } else {
        VERIFY(m_paused_execution_context);
        m_step_state = StepState {
            .mode = mode,
            .frame_id = m_paused_execution_context->frame_id,
            .source_range = m_paused_source_range,
        };
    }
    m_is_paused = false;
}

ThrowCompletionOr<Value> Debugger::evaluate_in_frame(ExecutionContext& execution_context, Utf16View source_text)
{
    VERIFY(m_is_paused);
    VERIFY(execution_context.executable);

    auto& vm = execution_context.executable->vm();
    auto context = execution_context.copy();
    auto local_environment = new_declarative_environment(*context->lexical_environment);

    auto binding_locations = binding_locations_for_frame(execution_context);

    for (auto const& [name, location] : binding_locations) {
        if (location.is_mutable)
            TRY(local_environment->create_mutable_binding(vm, name, false));
        else
            TRY(local_environment->create_immutable_binding(vm, name, true));
        auto value = location.storage == BindingStorage::Local
            ? context->local_variables()[location.index]
            : context->argument(location.index);
        if (!value.is_special_empty_value())
            TRY(local_environment->initialize_binding(vm, name, value, Environment::InitializeBindingHint::Normal));
    }
    context->lexical_environment = local_environment;
    context->variable_environment = local_environment;
    TRY(vm.push_execution_context(*context, {}));
    ScopeGuard pop_context = [&] {
        vm.pop_execution_context();
    };

    auto strict_caller = execution_context.executable->is_strict_mode ? CallerMode::Strict : CallerMode::NonStrict;
    auto result = perform_eval(vm, PrimitiveString::create(vm, source_text), strict_caller, EvalMode::Direct);

    for (auto const& [name, location] : binding_locations) {
        if (!location.is_mutable)
            continue;
        auto value = local_environment->get_binding_value(vm, name, false);
        if (value.is_error())
            continue;
        if (location.storage == BindingStorage::Local)
            execution_context.local_variables()[location.index] = value.release_value();
        else
            execution_context.arguments_span()[location.index] = value.release_value();
    }
    return result;
}

HashMap<Utf16FlyString, Debugger::BindingLocation> Debugger::binding_locations_for_frame(ExecutionContext const& context) const
{
    VERIFY(context.executable);

    HashMap<Utf16FlyString, BindingLocation> binding_locations;
    for (auto const& [index, name] : enumerate(context.executable->argument_variable_names)) {
        if (!name.is_empty())
            binding_locations.set(name, { BindingStorage::Argument, index, true, {} });
    }

    auto paused_source_range = m_paused_execution_context == &context
        ? m_paused_source_range
        : context.executable->source_range_at(context.program_counter);
    auto scope_is_active = [&](auto const& metadata) {
        if (!metadata.scope_range.has_value() || !paused_source_range.has_value())
            return true;
        auto const& position = paused_source_range->start;
        return metadata.scope_range->start <= position && position < metadata.scope_range->end;
    };

    for (auto const& [index, name] : enumerate(context.executable->local_variable_names)) {
        if (name.is_empty())
            continue;
        auto const& metadata = context.executable->local_variable_metadata[index];
        if (!scope_is_active(metadata))
            continue;

        auto existing = binding_locations.find(name);
        if (existing != binding_locations.end() && existing->value.storage == BindingStorage::Local) {
            if (!metadata.scope_range.has_value())
                continue;
            if (existing->value.scope_start.has_value()
                && *existing->value.scope_start >= metadata.scope_range->start) {
                continue;
            }
        }
        binding_locations.set(name, {
                                        BindingStorage::Local,
                                        index,
                                        metadata.is_mutable,
                                        metadata.scope_range.map([](auto const& range) { return range.start; }),
                                    });
    }

    return binding_locations;
}

Vector<Debugger::FrameBinding> Debugger::bindings_for_frame(ExecutionContext const& context) const
{
    auto binding_locations = binding_locations_for_frame(context);
    Vector<FrameBinding> bindings;
    bindings.ensure_capacity(binding_locations.size());
    for (auto const& [name, location] : binding_locations) {
        auto value = location.storage == BindingStorage::Local
            ? context.local_variables()[location.index]
            : context.argument(location.index);
        bindings.append({ name, value, location.is_mutable });
    }
    return bindings;
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

bool Debugger::should_pause_for_step(Bytecode::Executable const& executable, u32 bytecode_offset) const
{
    if (!m_step_state.has_value())
        return false;

    auto source_range = executable.source_range_at(bytecode_offset);
    if (!source_range.has_value() || source_range->start.line == 0)
        return false;

    auto const& state = *m_step_state;
    auto& vm = executable.vm();
    auto* current_context = &vm.running_execution_context();

    bool start_context_is_active = false;
    vm.for_each_execution_context_top_to_bottom([&](ExecutionContext const& context) {
        if (context.frame_id == state.frame_id) {
            start_context_is_active = true;
            return false;
        }
        return true;
    });

    auto has_moved = [&] {
        if (!state.source_range.has_value())
            return true;
        return source_range->code != state.source_range->code
            || source_range->start.line != state.source_range->start.line;
    };

    switch (state.mode) {
    case ResumeMode::Continue:
        VERIFY_NOT_REACHED();
    case ResumeMode::StepInto:
        return current_context->frame_id != state.frame_id || has_moved();
    case ResumeMode::StepOver:
        if (current_context->frame_id == state.frame_id)
            return has_moved();
        return !start_context_is_active;
    case ResumeMode::StepOut:
        return !start_context_is_active;
    }
    VERIFY_NOT_REACHED();
}

bool Debugger::pause_on_exception_if_needed(Bytecode::Executable& executable, u32 bytecode_offset, Value exception, bool exception_will_be_caught)
{
    if (m_pause_on_exceptions == PauseOnExceptions::None)
        return false;
    if (m_pause_on_exceptions == PauseOnExceptions::Uncaught && exception_will_be_caught)
        return false;
    if (m_last_paused_exception == exception)
        return false;

    if (!pause_execution(executable, bytecode_offset, PauseReason::Exception, exception, exception_will_be_caught))
        return false;

    m_last_paused_exception = GC::make_root(exception);
    return true;
}

void Debugger::did_finish_exception_propagation(Value exception)
{
    if (m_last_paused_exception == exception)
        m_last_paused_exception = {};
}

void Debugger::did_finish_bytecode_execution()
{
    m_step_state.clear();
    m_last_paused_exception = {};
}

ErrorOr<BreakpointID> Debugger::add_breakpoint(Utf16View filename, u32 line, Optional<u32> column)
{
    return add_breakpoint(nullptr, filename, line, column);
}

ErrorOr<BreakpointID> Debugger::add_breakpoint(NonnullRefPtr<SourceCode const> source_code, u32 line, Optional<u32> column)
{
    auto filename = source_code->filename();
    return add_breakpoint(move(source_code), filename, line, column);
}

ErrorOr<BreakpointID> Debugger::add_breakpoint(RefPtr<SourceCode const> source_code, Utf16View filename, u32 line, Optional<u32> column)
{
    if (line == 0)
        return AK::Error::from_string_literal("Breakpoint line must be greater than zero");

    for (auto const& breakpoint : m_breakpoints) {
        if (breakpoint.source_code == source_code && breakpoint.filename == filename && breakpoint.line == line && breakpoint.column == column)
            return breakpoint.id;
    }

    if (m_next_breakpoint_id == NumericLimits<BreakpointID>::max())
        return AK::Error::from_string_literal("Too many breakpoints");

    m_breakpoints.append({
        .id = m_next_breakpoint_id++,
        .source_code = move(source_code),
        .filename = Utf16String::from_utf16(filename),
        .line = line,
        .column = column,
    });
    auto const& breakpoint = m_breakpoints.last();
    resolve_breakpoint(breakpoint);
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

static Bytecode::SourceMapEntry const* breakpoint_candidate_for_executable(Breakpoint const& breakpoint, Bytecode::Executable const& executable)
{
    if (breakpoint.source_code) {
        if (executable.source_code.ptr() != breakpoint.source_code.ptr())
            return nullptr;
    } else if (executable.source_code->filename() != breakpoint.filename) {
        return nullptr;
    }

    Bytecode::SourceMapEntry const* matching_entry = nullptr;
    for (auto const& entry : executable.source_map) {
        if (entry.line == 0 || entry.line < breakpoint.line)
            continue;
        if (entry.line == breakpoint.line && breakpoint.column.has_value() && entry.column < *breakpoint.column)
            continue;
        if (!matching_entry || entry.line < matching_entry->line || (entry.line == matching_entry->line && entry.column < matching_entry->column))
            matching_entry = &entry;
    }
    return matching_entry;
}

void Debugger::resolve_breakpoint(Breakpoint const& breakpoint)
{
    struct Candidate {
        GC::Ptr<Bytecode::Executable> executable;
        Bytecode::SourceMapEntry const* source_map_entry { nullptr };
    };

    Vector<Candidate> candidates;
    HashMap<SourceCode const*, Position> resolved_positions;

    for (auto& executable : m_executables) {
        executable.remove_debugger_breakpoint(breakpoint.id);
        auto const* candidate = breakpoint_candidate_for_executable(breakpoint, executable);
        if (!candidate)
            continue;
        candidates.append({ &executable, candidate });

        Position candidate_position { candidate->line, candidate->column };
        auto resolved_position = resolved_positions.find(executable.source_code.ptr());
        if (resolved_position == resolved_positions.end()
            || candidate_position.line < resolved_position->value.line
            || (candidate_position.line == resolved_position->value.line && candidate_position.column < resolved_position->value.column)) {
            resolved_positions.set(executable.source_code.ptr(), candidate_position);
        }
    }

    for (auto const& candidate : candidates) {
        auto resolved_position = resolved_positions.find(candidate.executable->source_code.ptr());
        if (resolved_position == resolved_positions.end())
            continue;

        if (candidate.source_map_entry->line == resolved_position->value.line && candidate.source_map_entry->column == resolved_position->value.column)
            candidate.executable->add_debugger_breakpoint(candidate.source_map_entry->bytecode_offset, breakpoint.id);
    }
}

void Debugger::resolve_breakpoint_in_executable(Breakpoint const& breakpoint, Bytecode::Executable& executable)
{
    auto const* candidate = breakpoint_candidate_for_executable(breakpoint, executable);
    if (!candidate)
        return;

    Bytecode::SourceMapEntry const* resolved_candidate = nullptr;
    for (auto& existing_executable : m_executables) {
        if (existing_executable.source_code.ptr() != executable.source_code.ptr())
            continue;
        if (existing_executable.has_debugger_breakpoint(breakpoint.id)) {
            resolved_candidate = breakpoint_candidate_for_executable(breakpoint, existing_executable);
            break;
        }
    }

    auto position_is_before = [](Bytecode::SourceMapEntry const& a, Bytecode::SourceMapEntry const& b) {
        return a.line < b.line || (a.line == b.line && a.column < b.column);
    };
    if (resolved_candidate && position_is_before(*resolved_candidate, *candidate))
        return;

    if (resolved_candidate && position_is_before(*candidate, *resolved_candidate)) {
        for (auto& existing_executable : m_executables) {
            if (existing_executable.source_code.ptr() == executable.source_code.ptr())
                existing_executable.remove_debugger_breakpoint(breakpoint.id);
        }
    }

    executable.add_debugger_breakpoint(candidate->bytecode_offset, breakpoint.id);
}

void Debugger::clear_executable_breakpoints()
{
    for (auto& executable : m_executables)
        executable.clear_debugger_breakpoints();
}

}
