/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/HashTable.h>
#include <AK/TemporaryChange.h>
#include <LibGC/RootHashMap.h>
#include <LibJS/AST.h>
#include <LibJS/Bytecode/BasicBlock.h>
#include <LibJS/Bytecode/Generator.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/Instructions.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Bytecode/Label.h>
#include <LibJS/Bytecode/Op.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Accessor.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/CompletionCell.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/FunctionEnvironment.h>
#include <LibJS/Runtime/GeneratorResult.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/MathObject.h>
#include <LibJS/Runtime/ModuleEnvironment.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/ObjectEnvironment.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Reference.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibJS/SourceTextModule.h>

namespace JS::Bytecode {

bool g_dump_bytecode = false;

Interpreter::Interpreter(VM& vm)
    : m_vm(vm)
{
}

Interpreter::~Interpreter()
{
}

// 16.1.6 ScriptEvaluation ( scriptRecord ), https://tc39.es/ecma262/#sec-runtime-semantics-scriptevaluation
ThrowCompletionOr<Value> Interpreter::run(Script& script_record, GC::Ptr<Environment> lexical_environment_override)
{
    auto& vm = this->vm();

    // 1. Let globalEnv be scriptRecord.[[Realm]].[[GlobalEnv]].
    auto& global_environment = script_record.realm().global_environment();

    // NOTE: Spec steps are rearranged in order to compute number of registers+constants+locals before construction of the execution context.

    // 11. Let script be scriptRecord.[[ECMAScriptCode]].
    auto& script = script_record.parse_node();

    // 12. Let result be Completion(GlobalDeclarationInstantiation(script, globalEnv)).
    auto instantiation_result = script.global_declaration_instantiation(vm, global_environment);
    Completion result = instantiation_result.is_throw_completion() ? instantiation_result.throw_completion() : normal_completion(js_undefined());

    GC::Ptr<Executable> executable;
    if (result.type() == Completion::Type::Normal) {
        auto executable_result = JS::Bytecode::Generator::generate_from_ast_node(vm, script, {});

        if (executable_result.is_error()) {
            if (auto error_string = executable_result.error().to_string(); error_string.is_error())
                result = vm.template throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory));
            else if (error_string = String::formatted("TODO({})", error_string.value()); error_string.is_error())
                result = vm.template throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory));
            else
                result = vm.template throw_completion<JS::InternalError>(error_string.release_value());
        } else {
            executable = executable_result.release_value();

            if (g_dump_bytecode)
                executable->dump();
        }
    }

    u32 registers_and_constants_and_locals_count = 0;
    if (executable) {
        registers_and_constants_and_locals_count = executable->number_of_registers + executable->constants.size() + executable->local_variable_names.size();
    }

    // 2. Let scriptContext be a new ECMAScript code execution context.
    ExecutionContext* script_context = nullptr;
    ALLOCATE_EXECUTION_CONTEXT_ON_NATIVE_STACK(script_context, registers_and_constants_and_locals_count, 0);

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

    // NOTE: This isn't in the spec, but we require it.
    script_context->is_strict_mode = script_record.parse_node().is_strict_mode();

    // 9. Suspend the currently running execution context.
    // 10. Push scriptContext onto the execution context stack; scriptContext is now the running execution context.
    TRY(vm.push_execution_context(*script_context, {}));

    // 13. If result.[[Type]] is normal, then
    if (executable) {
        // a. Set result to Completion(Evaluation of script).
        auto result_or_error = run_executable(*executable, {}, {});
        if (result_or_error.value.is_error())
            result = result_or_error.value.release_error();
        else {
            result = result_or_error.return_register_value.is_special_empty_value() ? normal_completion(js_undefined()) : result_or_error.return_register_value;
        }

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

ThrowCompletionOr<Value> Interpreter::run(SourceTextModule& module)
{
    // FIXME: This is not a entry point as defined in the spec, but is convenient.
    //        To avoid work we use link_and_eval_module however that can already be
    //        dangerous if the vm loaded other modules.
    auto& vm = this->vm();

    TRY(vm.link_and_eval_module(Badge<Bytecode::Interpreter> {}, module));

    vm.run_queued_promise_jobs();

    vm.run_queued_finalization_registry_cleanup_jobs();

    return js_undefined();
}

NEVER_INLINE Interpreter::HandleExceptionResponse Interpreter::handle_exception(size_t& program_counter, Value exception)
{
    reg(Register::exception()) = exception;
    m_scheduled_jump = {};
    auto handlers = current_executable().exception_handlers_for_offset(program_counter);
    if (!handlers.has_value()) {
        return HandleExceptionResponse::ExitFromExecutable;
    }
    auto& handler = handlers->handler_offset;
    auto& finalizer = handlers->finalizer_offset;

    VERIFY(!running_execution_context().unwind_contexts.is_empty());
    auto& unwind_context = running_execution_context().unwind_contexts.last();
    VERIFY(unwind_context.executable == m_current_executable);

    if (handler.has_value()) {
        program_counter = handler.value();
        return HandleExceptionResponse::ContinueInThisExecutable;
    }
    if (finalizer.has_value()) {
        program_counter = finalizer.value();
        return HandleExceptionResponse::ContinueInThisExecutable;
    }
    VERIFY_NOT_REACHED();
}

// FIXME: GCC takes a *long* time to compile with flattening, and it will time out our CI. :|
#if defined(AK_COMPILER_CLANG)
#    define FLATTEN_ON_CLANG FLATTEN
#else
#    define FLATTEN_ON_CLANG
#endif

FLATTEN_ON_CLANG void Interpreter::run_bytecode(size_t entry_point)
{
    if (vm().did_reach_stack_space_limit()) [[unlikely]] {
        reg(Register::exception()) = vm().throw_completion<InternalError>(ErrorType::CallStackSizeExceeded).value();
        return;
    }

    auto& running_execution_context = this->running_execution_context();
    auto& executable = current_executable();
    auto const* bytecode = executable.bytecode.data();

    size_t& program_counter = running_execution_context.program_counter;
    program_counter = entry_point;
    // FIXME: For some reason we can't do a tail call here
    auto const& instruction = *reinterpret_cast<Instruction const*>(&bytecode[program_counter]);
    auto fn = dispatch_instruction_table[static_cast<size_t>(instruction.type())];
    (this->*fn)(bytecode, program_counter);
}

Interpreter::ResultAndReturnRegister Interpreter::run_executable(Executable& executable, Optional<size_t> entry_point, Value initial_accumulator_value)
{
    dbgln_if(JS_BYTECODE_DEBUG, "Bytecode::Interpreter will run unit {:p}", &executable);

    TemporaryChange restore_executable { m_current_executable, GC::Ptr { executable } };
    TemporaryChange restore_saved_jump { m_scheduled_jump, Optional<size_t> {} };
    TemporaryChange restore_realm { m_realm, GC::Ptr { vm().current_realm() } };
    TemporaryChange restore_global_object { m_global_object, GC::Ptr { m_realm->global_object() } };
    TemporaryChange restore_global_declarative_environment { m_global_declarative_environment, GC::Ptr { m_realm->global_environment().declarative_record() } };

    auto& running_execution_context = vm().running_execution_context();
    u32 registers_and_constants_and_locals_count = executable.number_of_registers + executable.constants.size() + executable.local_variable_names.size();
    VERIFY(registers_and_constants_and_locals_count <= running_execution_context.registers_and_constants_and_locals_and_arguments_span().size());

    TemporaryChange restore_running_execution_context { m_running_execution_context, &running_execution_context };
    TemporaryChange restore_registers_and_constants_and_locals { m_registers_and_constants_and_locals_arguments, running_execution_context.registers_and_constants_and_locals_and_arguments_span() };

    reg(Register::accumulator()) = initial_accumulator_value;
    reg(Register::return_value()) = js_special_empty_value();

    // NOTE: We only copy the `this` value from ExecutionContext if it's not already set.
    //       If we are re-entering an async/generator context, the `this` value
    //       may have already been cached by a ResolveThisBinding instruction,
    //       and subsequent instructions expect this value to be set.
    if (reg(Register::this_value()).is_special_empty_value())
        reg(Register::this_value()) = running_execution_context.this_value.value_or(js_special_empty_value());

    running_execution_context.executable = &executable;

    auto* registers_and_constants_and_locals_and_arguments = running_execution_context.registers_and_constants_and_locals_and_arguments();
    for (size_t i = 0; i < executable.constants.size(); ++i) {
        registers_and_constants_and_locals_and_arguments[executable.number_of_registers + i] = executable.constants[i];
    }

    run_bytecode(entry_point.value_or(0));

    dbgln_if(JS_BYTECODE_DEBUG, "Bytecode::Interpreter did run unit {:p}", &executable);

    if constexpr (JS_BYTECODE_DEBUG) {
        for (size_t i = 0; i < executable.number_of_registers; ++i) {
            String value_string;
            if (registers_and_constants_and_locals_and_arguments[i].is_special_empty_value())
                value_string = "(empty)"_string;
            else
                value_string = registers_and_constants_and_locals_and_arguments[i].to_string_without_side_effects();
            dbgln("[{:3}] {}", i, value_string);
        }
    }

    auto return_value = js_undefined();
    if (!reg(Register::return_value()).is_special_empty_value())
        return_value = reg(Register::return_value());
    auto exception = reg(Register::exception());

    vm().run_queued_promise_jobs();
    vm().finish_execution_generation();

    if (!exception.is_special_empty_value())
        return { throw_completion(exception), registers_and_constants_and_locals_and_arguments[0] };
    return { return_value, registers_and_constants_and_locals_and_arguments[0] };
}

void Interpreter::enter_unwind_context()
{
    running_execution_context().unwind_contexts.empend(
        m_current_executable,
        running_execution_context().lexical_environment);
    running_execution_context().previously_scheduled_jumps.append(m_scheduled_jump);
    m_scheduled_jump = {};
}

void Interpreter::leave_unwind_context()
{
    running_execution_context().unwind_contexts.take_last();
}

void Interpreter::catch_exception(Operand dst)
{
    set(dst, reg(Register::exception()));
    reg(Register::exception()) = js_special_empty_value();
    auto& context = running_execution_context().unwind_contexts.last();
    VERIFY(!context.handler_called);
    VERIFY(context.executable == &current_executable());
    context.handler_called = true;
    running_execution_context().lexical_environment = context.lexical_environment;
}

void Interpreter::restore_scheduled_jump()
{
    m_scheduled_jump = running_execution_context().previously_scheduled_jumps.take_last();
}

void Interpreter::leave_finally()
{
    reg(Register::exception()) = js_special_empty_value();
    m_scheduled_jump = running_execution_context().previously_scheduled_jumps.take_last();
}

void Interpreter::enter_object_environment(Object& object)
{
    auto& old_environment = running_execution_context().lexical_environment;
    running_execution_context().saved_lexical_environments.append(old_environment);
    running_execution_context().lexical_environment = new_object_environment(object, true, old_environment);
}

ThrowCompletionOr<GC::Ref<Bytecode::Executable>> compile(VM& vm, ASTNode const& node, FunctionKind kind, FlyString const& name)
{
    auto executable_result = Bytecode::Generator::generate_from_ast_node(vm, node, kind);
    if (executable_result.is_error())
        return vm.throw_completion<InternalError>(ErrorType::NotImplemented, TRY_OR_THROW_OOM(vm, executable_result.error().to_string()));

    auto bytecode_executable = executable_result.release_value();
    bytecode_executable->name = name;

    if (Bytecode::g_dump_bytecode)
        bytecode_executable->dump();

    return bytecode_executable;
}

ThrowCompletionOr<GC::Ref<Bytecode::Executable>> compile(VM& vm, ECMAScriptFunctionObject const& function)
{
    auto const& name = function.name();

    auto executable_result = Bytecode::Generator::generate_from_function(vm, function);
    if (executable_result.is_error())
        return vm.throw_completion<InternalError>(ErrorType::NotImplemented, TRY_OR_THROW_OOM(vm, executable_result.error().to_string()));

    auto bytecode_executable = executable_result.release_value();
    bytecode_executable->name = name;

    if (Bytecode::g_dump_bytecode)
        bytecode_executable->dump();

    return bytecode_executable;
}

}
