/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <LibJS/Bytecode/Debug.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/Label.h>
#include <LibJS/Bytecode/Op.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/ClassConstruction.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/FunctionEnvironment.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibJS/SourceTextModule.h>

namespace JS {

using namespace Bytecode;

// Defined in generated assembly (asmint_x86_64.S or asmint_aarch64.S)
extern "C" void asm_interpreter_entry(u8 const* bytecode, u32 entry_point, Value* values, VM* vm);

bool Bytecode::g_dump_bytecode = false;

// 16.1.6 ScriptEvaluation ( scriptRecord ), https://tc39.es/ecma262/#sec-runtime-semantics-scriptevaluation
ThrowCompletionOr<Value> VM::run(Script& script_record, GC::Ptr<Environment> lexical_environment_override)
{
    auto& vm = this->vm();

    // 1. Let globalEnv be scriptRecord.[[Realm]].[[GlobalEnv]].
    auto& global_environment = script_record.realm().global_environment();

    // NOTE: Spec steps are rearranged in order to compute number of registers+constants+locals before construction of the execution context.

    // 12. Let result be Completion(GlobalDeclarationInstantiation(script, globalEnv)).
    auto instantiation_result = script_record.global_declaration_instantiation(vm, global_environment);
    Completion result = instantiation_result.is_throw_completion() ? instantiation_result.throw_completion() : normal_completion(js_undefined());

    // 11. Let script be scriptRecord.[[ECMAScriptCode]].
    GC::Ptr<Executable> executable = script_record.cached_executable();
    if (executable && g_dump_bytecode)
        executable->dump();

    u32 registers_and_locals_count = 0;
    ReadonlySpan<Value> constants;
    if (executable) {
        registers_and_locals_count = executable->registers_and_locals_count;
        constants = executable->constants;
    }

    // 2. Let scriptContext be a new ECMAScript code execution context.
    auto& stack = vm.interpreter_stack();
    auto* stack_mark = stack.top();
    auto* script_context = stack.allocate(registers_and_locals_count, constants, 0);
    if (!script_context) [[unlikely]]
        return vm.throw_completion<InternalError>(ErrorType::CallStackSizeExceeded);
    ScopeGuard deallocate_guard = [&stack, stack_mark] { stack.deallocate(stack_mark); };

    // 3. Set the Function of scriptContext to null.
    // NOTE: This was done during execution context construction.

    // 4. Set the Realm of scriptContext to scriptRecord.[[Realm]].
    script_context->realm = &script_record.realm();

    // 5. Set the ScriptOrModule of scriptContext to scriptRecord.
    script_context->script_or_module = GC::Ref<Script>(script_record);

    // 6. Set the VariableEnvironment of scriptContext to globalEnv.
    script_context->variable_environment = &global_environment;

    // 7. Set the LexicalEnvironment of scriptContext to globalEnv.
    script_context->lexical_environment = &global_environment;

    // Non-standard: Override the lexical environment if requested.
    if (lexical_environment_override)
        script_context->lexical_environment = lexical_environment_override;

    // 8. Set the PrivateEnvironment of scriptContext to null.

    // 9. Suspend the currently running execution context.
    // 10. Push scriptContext onto the execution context stack; scriptContext is now the running execution context.
    TRY(vm.push_execution_context(*script_context, {}));

    // 13. If result.[[Type]] is normal, then
    if (executable && result.type() == Completion::Type::Normal) {
        // a. Set result to Completion(Evaluation of script).
        result = run_executable(*script_context, *executable, 0, {});

        // b. If result is a normal completion and result.[[Value]] is empty, then
        if (result.type() == Completion::Type::Normal && result.value().is_special_empty_value()) {
            // i. Set result to NormalCompletion(undefined).
            result = normal_completion(js_undefined());
        }
    }

    // 14. Suspend scriptContext and remove it from the execution context stack.
    vm.pop_execution_context();

    // 15. Assert: The execution context stack is not empty.
    VERIFY(!vm.execution_context_stack().is_empty());

    // FIXME: 16. Resume the context that is now on the top of the execution context stack as the running execution context.

    vm.finish_execution_generation();

    // 17. Return ? result.
    if (result.is_abrupt()) {
        VERIFY(result.type() == Completion::Type::Throw);
        return result.release_error();
    }

    return result.value();
}

ThrowCompletionOr<Value> VM::run(SourceTextModule& module)
{
    // FIXME: This is not a entry point as defined in the spec, but is convenient.
    //        To avoid work we use link_and_eval_module however that can already be
    //        dangerous if the vm loaded other modules.
    auto& vm = this->vm();

    TRY(vm.link_and_eval_module(module));

    vm.run_queued_promise_jobs();

    vm.run_queued_finalization_registry_cleanup_jobs();

    return js_undefined();
}

VM::HandleExceptionResponse VM::handle_exception(u32 program_counter, Value exception)
{
    for (;;) {
        auto handlers = current_executable().exception_handlers_for_offset(program_counter);
        if (handlers.has_value()) {
            reg(Register::exception()) = exception;
            m_running_execution_context->program_counter = handlers->handler_offset;
            return HandleExceptionResponse::ContinueInThisExecutable;
        }

        // If we're in an inline frame, unwind to the caller and try its handlers.
        if (m_running_execution_context->caller_frame) {
            auto* callee_frame = m_running_execution_context;
            auto* caller_frame = callee_frame->caller_frame;
            auto caller_pc = callee_frame->caller_return_pc;

            vm().interpreter_stack().deallocate(callee_frame);

            m_running_execution_context = caller_frame;

            // NB: caller_pc is the return address (one past the Call instruction).
            //     For handler lookup we need a PC inside the Call instruction,
            //     since the exception occurred during that call, not after it.
            //     Exception handler ranges use an exclusive end offset, so using
            //     caller_pc directly would miss a handler ending right at that address.
            program_counter = caller_pc - 1;
            continue;
        }

        reg(Register::exception()) = exception;
        return HandleExceptionResponse::ExitFromExecutable;
    }
}

ExecutionContext* VM::push_inline_frame(
    ECMAScriptFunctionObject& callee_function,
    Executable& callee_executable,
    ReadonlySpan<Operand> arguments,
    u32 return_pc,
    u32 dst_raw,
    Value this_value,
    Object* new_target,
    bool is_construct)
{
    auto& stack = vm().interpreter_stack();

    u32 insn_argument_count = arguments.size();
    size_t registers_and_locals_count = callee_executable.registers_and_locals_count;
    size_t argument_count = max(insn_argument_count, static_cast<u32>(callee_function.formal_parameter_count()));

    auto* callee_context = stack.allocate(registers_and_locals_count, callee_executable.constants, argument_count);
    if (!callee_context) [[unlikely]]
        return nullptr;

    // Copy arguments from caller's registers into callee's argument slots.
    auto* callee_argument_values = callee_context->arguments_data();
    for (u32 i = 0; i < insn_argument_count; ++i)
        callee_argument_values[i] = get(arguments[i]);
    for (size_t i = insn_argument_count; i < argument_count; ++i)
        callee_argument_values[i] = js_undefined();
    callee_context->passed_argument_count = insn_argument_count;

    // Set up caller linkage so Return can restore the caller frame.
    callee_context->caller_frame = m_running_execution_context;
    callee_context->caller_dst_raw = dst_raw;
    callee_context->caller_return_pc = return_pc;
    callee_context->caller_is_construct = is_construct;

    // Inlined PrepareForOrdinaryCall (avoids function call overhead on hot path).
    callee_context->function = &callee_function;
    callee_context->realm = callee_function.realm();
    callee_context->script_or_module = callee_function.m_script_or_module;
    if (callee_function.function_environment_needed()) {
        auto local_environment = new_function_environment(callee_function, new_target);
        auto function_environment_bindings_count = callee_function.shared_data().m_function_environment_bindings_count;
        local_environment->set_environment_shape_cache(callee_function.shared_data().m_function_environment_shape, function_environment_bindings_count);
        local_environment->ensure_capacity(function_environment_bindings_count);
        callee_context->lexical_environment = local_environment;
        callee_context->variable_environment = local_environment;
    } else {
        callee_context->lexical_environment = callee_function.environment();
        callee_context->variable_environment = callee_function.environment();
    }
    callee_context->private_environment = callee_function.m_private_environment;

    // Inline JS-to-JS frames stay out of the VM execution context stack and
    // are tracked through caller_frame instead.
    m_running_execution_context = callee_context;

    // Bind this if the function uses it.
    if (callee_function.uses_this())
        callee_function.ordinary_call_bind_this(vm(), *callee_context, this_value);

    // Set up execution context fields that run_executable normally does.
    // NB: We must use the callee's realm (not the caller's) for global_object
    //     and global_declarative_environment, since the caller's realm may differ
    //     in cross-realm calls (e.g. iframe <-> parent).
    callee_context->executable = callee_executable;

    // Set this value register.
    auto* values = callee_context->registers_and_constants_and_locals_and_arguments();
    values[Register::this_value().index()] = callee_context->this_value.value_or(js_special_empty_value());

    return callee_context;
}

NEVER_INLINE void VM::unwind_inline_frame_for_exception()
{
    auto* callee_frame = m_running_execution_context;
    VERIFY(callee_frame);
    VERIFY(callee_frame->caller_frame);

    auto* caller_frame = callee_frame->caller_frame;
    vm().interpreter_stack().deallocate(callee_frame);
    m_running_execution_context = caller_frame;
}

Utf16FlyString const& VM::get_identifier(IdentifierTableIndex index) const
{
    return m_running_execution_context->executable->get_identifier(index);
}

PropertyKey const& VM::get_property_key(PropertyKeyTableIndex index) const
{
    return m_running_execution_context->executable->get_property_key(index);
}

DeclarativeEnvironment& VM::global_declarative_environment()
{
    return realm().global_declarative_environment();
}

ThrowCompletionOr<Value> VM::run_executable(ExecutionContext& context, Executable& executable, u32 entry_point)
{
    auto const is_outermost_bytecode_execution = m_run_executable_depth == 0;
    TemporaryChange restore_run_executable_depth { m_run_executable_depth, m_run_executable_depth + 1 };

    // NOTE: This is how we "push" a new execution context onto the VM's
    //       execution context stack.
    TemporaryChange restore_running_execution_context { m_running_execution_context, &context };

    context.executable = executable;

    VERIFY(executable.registers_and_locals_count + executable.constants.size() == executable.registers_and_locals_and_constants_count);
    VERIFY(executable.registers_and_locals_and_constants_count <= context.registers_and_constants_and_locals_and_arguments_span().size());

    // NOTE: We only copy the `this` value from ExecutionContext if it's not already set.
    //       If we are re-entering an async/generator context, the `this` value
    //       may have already been cached by a ResolveThisBinding instruction,
    //       and subsequent instructions expect this value to be set.
    if (reg(Register::this_value()).is_special_empty_value())
        reg(Register::this_value()) = context.this_value.value_or(js_special_empty_value());

    if (vm().interpreter_stack().is_exhausted() || vm().did_reach_stack_space_limit()) [[unlikely]] {
        reg(Register::exception()) = vm().throw_completion<InternalError>(ErrorType::CallStackSizeExceeded).value();
    } else {
        auto* bytecode = executable.bytecode.data();
        auto* values = context.registers_and_constants_and_locals_and_arguments_span().data();

        asm_interpreter_entry(bytecode, entry_point, values, this);
    }

    if (is_outermost_bytecode_execution && !vm().is_executing_module())
        vm().run_queued_promise_jobs();
    vm().finish_execution_generation();

    auto exception = reg(Register::exception());
    if (!exception.is_special_empty_value()) [[unlikely]]
        return JS::throw_completion(exception);

    return reg(Register::return_value());
}

}
