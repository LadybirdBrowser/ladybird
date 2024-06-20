/*
 * Copyright (c) 2021-2024, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/HashTable.h>
#include <AK/TemporaryChange.h>
#include <LibJS/AST.h>
#include <LibJS/Bytecode/BasicBlock.h>
#include <LibJS/Bytecode/CommonImplementations.h>
#include <LibJS/Bytecode/Generator.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Bytecode/Label.h>
#include <LibJS/Bytecode/Op.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/FunctionEnvironment.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/MathObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/ObjectEnvironment.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Reference.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibJS/SourceTextModule.h>

namespace JS::Bytecode {

bool g_dump_bytecode = false;

static ByteString format_operand(StringView name, Operand operand, Bytecode::Executable const& executable)
{
    StringBuilder builder;
    if (!name.is_empty())
        builder.appendff("\033[32m{}\033[0m:", name);
    switch (operand.type()) {
    case Operand::Type::Register:
        if (operand.index() == Register::this_value().index()) {
            builder.appendff("\033[33mthis\033[0m");
        } else {
            builder.appendff("\033[33mreg{}\033[0m", operand.index());
        }
        break;
    case Operand::Type::Local:
        builder.appendff("\033[34m{}~{}\033[0m", executable.local_variable_names[operand.index() - executable.local_index_base], operand.index() - executable.local_index_base);
        break;
    case Operand::Type::Constant: {
        builder.append("\033[36m"sv);
        auto value = executable.constants[operand.index() - executable.number_of_registers];
        if (value.is_empty())
            builder.append("<Empty>"sv);
        else if (value.is_boolean())
            builder.appendff("Bool({})", value.as_bool() ? "true"sv : "false"sv);
        else if (value.is_int32())
            builder.appendff("Int32({})", value.as_i32());
        else if (value.is_double())
            builder.appendff("Double({})", value.as_double());
        else if (value.is_bigint())
            builder.appendff("BigInt({})", value.as_bigint().to_byte_string());
        else if (value.is_string())
            builder.appendff("String(\"{}\")", value.as_string().utf8_string_view());
        else if (value.is_undefined())
            builder.append("Undefined"sv);
        else if (value.is_null())
            builder.append("Null"sv);
        else
            builder.appendff("Value: {}", value);
        builder.append("\033[0m"sv);
        break;
    }
    default:
        VERIFY_NOT_REACHED();
    }
    return builder.to_byte_string();
}

static ByteString format_operand_list(StringView name, ReadonlySpan<Operand> operands, Bytecode::Executable const& executable)
{
    StringBuilder builder;
    if (!name.is_empty())
        builder.appendff("\033[32m{}\033[0m:[", name);
    for (size_t i = 0; i < operands.size(); ++i) {
        if (i != 0)
            builder.append(", "sv);
        builder.appendff("{}", format_operand(""sv, operands[i], executable));
    }
    builder.append("]"sv);
    return builder.to_byte_string();
}

static ByteString format_value_list(StringView name, ReadonlySpan<Value> values)
{
    StringBuilder builder;
    if (!name.is_empty())
        builder.appendff("\033[32m{}\033[0m:[", name);
    builder.join(", "sv, values);
    builder.append("]"sv);
    return builder.to_byte_string();
}

ALWAYS_INLINE static ThrowCompletionOr<Value> loosely_inequals(VM& vm, Value src1, Value src2)
{
    if (src1.tag() == src2.tag()) {
        if (src1.is_int32() || src1.is_object() || src1.is_boolean() || src1.is_nullish())
            return Value(src1.encoded() != src2.encoded());
    }
    return Value(!TRY(is_loosely_equal(vm, src1, src2)));
}

ALWAYS_INLINE static ThrowCompletionOr<Value> loosely_equals(VM& vm, Value src1, Value src2)
{
    if (src1.tag() == src2.tag()) {
        if (src1.is_int32() || src1.is_object() || src1.is_boolean() || src1.is_nullish())
            return Value(src1.encoded() == src2.encoded());
    }
    return Value(TRY(is_loosely_equal(vm, src1, src2)));
}

ALWAYS_INLINE static ThrowCompletionOr<Value> strict_inequals(VM&, Value src1, Value src2)
{
    if (src1.tag() == src2.tag()) {
        if (src1.is_int32() || src1.is_object() || src1.is_boolean() || src1.is_nullish())
            return Value(src1.encoded() != src2.encoded());
    }
    return Value(!is_strictly_equal(src1, src2));
}

ALWAYS_INLINE static ThrowCompletionOr<Value> strict_equals(VM&, Value src1, Value src2)
{
    if (src1.tag() == src2.tag()) {
        if (src1.is_int32() || src1.is_object() || src1.is_boolean() || src1.is_nullish())
            return Value(src1.encoded() == src2.encoded());
    }
    return Value(is_strictly_equal(src1, src2));
}

Interpreter::Interpreter(VM& vm)
    : m_vm(vm)
{
}

Interpreter::~Interpreter()
{
}

ALWAYS_INLINE Value Interpreter::get(Operand op) const
{
    return m_registers_and_constants_and_locals.data()[op.index()];
}

ALWAYS_INLINE void Interpreter::set(Operand op, Value value)
{
    m_registers_and_constants_and_locals.data()[op.index()] = value;
}

ALWAYS_INLINE Value Interpreter::do_yield(Value value, Optional<Label> continuation)
{
    auto object = Object::create(realm(), nullptr);
    object->define_direct_property("result", value, JS::default_attributes);

    if (continuation.has_value())
        // FIXME: If we get a pointer, which is not accurately representable as a double
        //        will cause this to explode
        object->define_direct_property("continuation", Value(continuation->address()), JS::default_attributes);
    else
        object->define_direct_property("continuation", js_null(), JS::default_attributes);

    object->define_direct_property("isAwait", Value(false), JS::default_attributes);
    return object;
}

// 16.1.6 ScriptEvaluation ( scriptRecord ), https://tc39.es/ecma262/#sec-runtime-semantics-scriptevaluation
ThrowCompletionOr<Value> Interpreter::run(Script& script_record, JS::GCPtr<Environment> lexical_environment_override)
{
    auto& vm = this->vm();

    // 1. Let globalEnv be scriptRecord.[[Realm]].[[GlobalEnv]].
    auto& global_environment = script_record.realm().global_environment();

    // 2. Let scriptContext be a new ECMAScript code execution context.
    auto script_context = ExecutionContext::create();

    // 3. Set the Function of scriptContext to null.
    // NOTE: This was done during execution context construction.

    // 4. Set the Realm of scriptContext to scriptRecord.[[Realm]].
    script_context->realm = &script_record.realm();

    // 5. Set the ScriptOrModule of scriptContext to scriptRecord.
    script_context->script_or_module = NonnullGCPtr<Script>(script_record);

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

    // FIXME: 9. Suspend the currently running execution context.

    // 10. Push scriptContext onto the execution context stack; scriptContext is now the running execution context.
    TRY(vm.push_execution_context(*script_context, {}));

    // 11. Let script be scriptRecord.[[ECMAScriptCode]].
    auto& script = script_record.parse_node();

    // 12. Let result be Completion(GlobalDeclarationInstantiation(script, globalEnv)).
    auto instantiation_result = script.global_declaration_instantiation(vm, global_environment);
    Completion result = instantiation_result.is_throw_completion() ? instantiation_result.throw_completion() : normal_completion({});

    // 13. If result.[[Type]] is normal, then
    if (result.type() == Completion::Type::Normal) {
        auto executable_result = JS::Bytecode::Generator::generate_from_ast_node(vm, script, {});

        if (executable_result.is_error()) {
            if (auto error_string = executable_result.error().to_string(); error_string.is_error())
                result = vm.template throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory));
            else if (error_string = String::formatted("TODO({})", error_string.value()); error_string.is_error())
                result = vm.template throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory));
            else
                result = JS::throw_completion(JS::InternalError::create(realm(), error_string.release_value()));
        } else {
            auto executable = executable_result.release_value();

            if (g_dump_bytecode)
                executable->dump();

            // a. Set result to the result of evaluating script.
            auto result_or_error = run_executable(*executable, {}, {});
            if (result_or_error.value.is_error())
                result = result_or_error.value.release_error();
            else
                result = result_or_error.return_register_value;
        }
    }

    // 14. If result.[[Type]] is normal and result.[[Value]] is empty, then
    if (result.type() == Completion::Type::Normal && !result.value().has_value()) {
        // a. Set result to NormalCompletion(undefined).
        result = normal_completion(js_undefined());
    }

    // FIXME: 15. Suspend scriptContext and remove it from the execution context stack.
    vm.pop_execution_context();

    // 16. Assert: The execution context stack is not empty.
    VERIFY(!vm.execution_context_stack().is_empty());

    // FIXME: 17. Resume the context that is now on the top of the execution context stack as the running execution context.

    // At this point we may have already run any queued promise jobs via on_call_stack_emptied,
    // in which case this is a no-op.
    // FIXME: These three should be moved out of Interpreter::run and give the host an option to run these, as it's up to the host when these get run.
    //        https://tc39.es/ecma262/#sec-jobs for jobs and https://tc39.es/ecma262/#_ref_3508 for ClearKeptObjects
    //        finish_execution_generation is particularly an issue for LibWeb, as the HTML spec wants to run it specifically after performing a microtask checkpoint.
    //        The promise and registry cleanup queues don't cause LibWeb an issue, as LibWeb overrides the hooks that push onto these queues.
    vm.run_queued_promise_jobs();

    vm.run_queued_finalization_registry_cleanup_jobs();

    vm.finish_execution_generation();

    // 18. Return ? result.
    if (result.is_abrupt()) {
        VERIFY(result.type() == Completion::Type::Throw);
        return result.release_error();
    }

    VERIFY(result.value().has_value());
    return *result.value();
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

Interpreter::HandleExceptionResponse Interpreter::handle_exception(size_t& program_counter, Value exception)
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
    if (vm().did_reach_stack_space_limit()) {
        reg(Register::exception()) = vm().throw_completion<InternalError>(ErrorType::CallStackSizeExceeded).release_value().value();
        return;
    }

    auto& running_execution_context = this->running_execution_context();
    auto* arguments = running_execution_context.arguments.data();
    auto& accumulator = this->accumulator();
    auto& executable = current_executable();
    auto const* bytecode = executable.bytecode.data();

    size_t program_counter = entry_point;

    TemporaryChange change(m_program_counter, Optional<size_t&>(program_counter));

    // Declare a lookup table for computed goto with each of the `handle_*` labels
    // to avoid the overhead of a switch statement.
    // This is a GCC extension, but it's also supported by Clang.

    static void* const bytecode_dispatch_table[] = {
#define SET_UP_LABEL(name) &&handle_##name,
        ENUMERATE_BYTECODE_OPS(SET_UP_LABEL)
    };
#undef SET_UP_LABEL

#define DISPATCH_NEXT(name)                                                                         \
    do {                                                                                            \
        if constexpr (Op::name::IsVariableLength)                                                   \
            program_counter += instruction.length();                                                \
        else                                                                                        \
            program_counter += sizeof(Op::name);                                                    \
        auto& next_instruction = *reinterpret_cast<Instruction const*>(&bytecode[program_counter]); \
        goto* bytecode_dispatch_table[static_cast<size_t>(next_instruction.type())];                \
    } while (0)

    for (;;) {
    start:
        for (;;) {
            goto* bytecode_dispatch_table[static_cast<size_t>((*reinterpret_cast<Instruction const*>(&bytecode[program_counter])).type())];

        handle_GetArgument: {
            auto const& instruction = *reinterpret_cast<Op::GetArgument const*>(&bytecode[program_counter]);
            set(instruction.dst(), arguments[instruction.index()]);
            DISPATCH_NEXT(GetArgument);
        }

        handle_SetArgument: {
            auto const& instruction = *reinterpret_cast<Op::SetArgument const*>(&bytecode[program_counter]);
            arguments[instruction.index()] = get(instruction.src());
            DISPATCH_NEXT(SetArgument);
        }

        handle_Mov: {
            auto& instruction = *reinterpret_cast<Op::Mov const*>(&bytecode[program_counter]);
            set(instruction.dst(), get(instruction.src()));
            DISPATCH_NEXT(Mov);
        }

        handle_End: {
            auto& instruction = *reinterpret_cast<Op::End const*>(&bytecode[program_counter]);
            accumulator = get(instruction.value());
            return;
        }

        handle_Jump: {
            auto& instruction = *reinterpret_cast<Op::Jump const*>(&bytecode[program_counter]);
            program_counter = instruction.target().address();
            goto start;
        }

        handle_JumpIf: {
            auto& instruction = *reinterpret_cast<Op::JumpIf const*>(&bytecode[program_counter]);
            if (get(instruction.condition()).to_boolean())
                program_counter = instruction.true_target().address();
            else
                program_counter = instruction.false_target().address();
            goto start;
        }

        handle_JumpTrue: {
            auto& instruction = *reinterpret_cast<Op::JumpTrue const*>(&bytecode[program_counter]);
            if (get(instruction.condition()).to_boolean()) {
                program_counter = instruction.target().address();
                goto start;
            }
            DISPATCH_NEXT(JumpTrue);
        }

        handle_JumpFalse: {
            auto& instruction = *reinterpret_cast<Op::JumpFalse const*>(&bytecode[program_counter]);
            if (!get(instruction.condition()).to_boolean()) {
                program_counter = instruction.target().address();
                goto start;
            }
            DISPATCH_NEXT(JumpFalse);
        }

        handle_JumpNullish: {
            auto& instruction = *reinterpret_cast<Op::JumpNullish const*>(&bytecode[program_counter]);
            if (get(instruction.condition()).is_nullish())
                program_counter = instruction.true_target().address();
            else
                program_counter = instruction.false_target().address();
            goto start;
        }

#define HANDLE_COMPARISON_OP(op_TitleCase, op_snake_case, numeric_operator)                                             \
    handle_Jump##op_TitleCase:                                                                                          \
    {                                                                                                                   \
        auto& instruction = *reinterpret_cast<Op::Jump##op_TitleCase const*>(&bytecode[program_counter]);               \
        auto lhs = get(instruction.lhs());                                                                              \
        auto rhs = get(instruction.rhs());                                                                              \
        if (lhs.is_number() && rhs.is_number()) {                                                                       \
            bool result;                                                                                                \
            if (lhs.is_int32() && rhs.is_int32()) {                                                                     \
                result = lhs.as_i32() numeric_operator rhs.as_i32();                                                    \
            } else {                                                                                                    \
                result = lhs.as_double() numeric_operator rhs.as_double();                                              \
            }                                                                                                           \
            program_counter = result ? instruction.true_target().address() : instruction.false_target().address();      \
            goto start;                                                                                                 \
        }                                                                                                               \
        auto result = op_snake_case(vm(), get(instruction.lhs()), get(instruction.rhs()));                              \
        if (result.is_error()) {                                                                                        \
            if (handle_exception(program_counter, result.error_value()) == HandleExceptionResponse::ExitFromExecutable) \
                return;                                                                                                 \
            goto start;                                                                                                 \
        }                                                                                                               \
        if (result.value().to_boolean())                                                                                \
            program_counter = instruction.true_target().address();                                                      \
        else                                                                                                            \
            program_counter = instruction.false_target().address();                                                     \
        goto start;                                                                                                     \
    }

            JS_ENUMERATE_COMPARISON_OPS(HANDLE_COMPARISON_OP)
#undef HANDLE_COMPARISON_OP

        handle_JumpUndefined: {
            auto& instruction = *reinterpret_cast<Op::JumpUndefined const*>(&bytecode[program_counter]);
            if (get(instruction.condition()).is_undefined())
                program_counter = instruction.true_target().address();
            else
                program_counter = instruction.false_target().address();
            goto start;
        }

        handle_EnterUnwindContext: {
            auto& instruction = *reinterpret_cast<Op::EnterUnwindContext const*>(&bytecode[program_counter]);
            enter_unwind_context();
            program_counter = instruction.entry_point().address();
            goto start;
        }

        handle_ContinuePendingUnwind: {
            auto& instruction = *reinterpret_cast<Op::ContinuePendingUnwind const*>(&bytecode[program_counter]);
            if (auto exception = reg(Register::exception()); !exception.is_empty()) {
                if (handle_exception(program_counter, exception) == HandleExceptionResponse::ExitFromExecutable)
                    return;
                goto start;
            }
            if (!saved_return_value().is_empty()) {
                do_return(saved_return_value());
                if (auto handlers = executable.exception_handlers_for_offset(program_counter); handlers.has_value()) {
                    if (auto finalizer = handlers.value().finalizer_offset; finalizer.has_value()) {
                        VERIFY(!running_execution_context.unwind_contexts.is_empty());
                        auto& unwind_context = running_execution_context.unwind_contexts.last();
                        VERIFY(unwind_context.executable == m_current_executable);
                        reg(Register::saved_return_value()) = reg(Register::return_value());
                        reg(Register::return_value()) = {};
                        program_counter = finalizer.value();
                        // the unwind_context will be pop'ed when entering the finally block
                        goto start;
                    }
                }
                return;
            }
            auto const old_scheduled_jump = running_execution_context.previously_scheduled_jumps.take_last();
            if (m_scheduled_jump.has_value()) {
                program_counter = m_scheduled_jump.value();
                m_scheduled_jump = {};
            } else {
                program_counter = instruction.resume_target().address();
                // set the scheduled jump to the old value if we continue
                // where we left it
                m_scheduled_jump = old_scheduled_jump;
            }
            goto start;
        }

        handle_ScheduleJump: {
            auto& instruction = *reinterpret_cast<Op::ScheduleJump const*>(&bytecode[program_counter]);
            m_scheduled_jump = instruction.target().address();
            auto finalizer = executable.exception_handlers_for_offset(program_counter).value().finalizer_offset;
            VERIFY(finalizer.has_value());
            program_counter = finalizer.value();
            goto start;
        }

#define HANDLE_INSTRUCTION(name)                                                                                            \
    handle_##name:                                                                                                          \
    {                                                                                                                       \
        auto& instruction = *reinterpret_cast<Op::name const*>(&bytecode[program_counter]);                                 \
        {                                                                                                                   \
            auto result = instruction.execute_impl(*this);                                                                  \
            if (result.is_error()) {                                                                                        \
                if (handle_exception(program_counter, result.error_value()) == HandleExceptionResponse::ExitFromExecutable) \
                    return;                                                                                                 \
                goto start;                                                                                                 \
            }                                                                                                               \
        }                                                                                                                   \
        DISPATCH_NEXT(name);                                                                                                \
    }

#define HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(name)                                    \
    handle_##name:                                                                          \
    {                                                                                       \
        auto& instruction = *reinterpret_cast<Op::name const*>(&bytecode[program_counter]); \
        instruction.execute_impl(*this);                                                    \
        DISPATCH_NEXT(name);                                                                \
    }

            HANDLE_INSTRUCTION(Add);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(AddPrivateName);
            HANDLE_INSTRUCTION(ArrayAppend);
            HANDLE_INSTRUCTION(AsyncIteratorClose);
            HANDLE_INSTRUCTION(BitwiseAnd);
            HANDLE_INSTRUCTION(BitwiseNot);
            HANDLE_INSTRUCTION(BitwiseOr);
            HANDLE_INSTRUCTION(BitwiseXor);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(BlockDeclarationInstantiation);
            HANDLE_INSTRUCTION(Call);
            HANDLE_INSTRUCTION(CallWithArgumentArray);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(Catch);
            HANDLE_INSTRUCTION(ConcatString);
            HANDLE_INSTRUCTION(CopyObjectExcludingProperties);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(CreateLexicalEnvironment);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(CreateVariableEnvironment);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(CreatePrivateEnvironment);
            HANDLE_INSTRUCTION(CreateVariable);
            HANDLE_INSTRUCTION(CreateRestParams);
            HANDLE_INSTRUCTION(CreateArguments);
            HANDLE_INSTRUCTION(Decrement);
            HANDLE_INSTRUCTION(DeleteById);
            HANDLE_INSTRUCTION(DeleteByIdWithThis);
            HANDLE_INSTRUCTION(DeleteByValue);
            HANDLE_INSTRUCTION(DeleteByValueWithThis);
            HANDLE_INSTRUCTION(DeleteVariable);
            HANDLE_INSTRUCTION(Div);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(Dump);
            HANDLE_INSTRUCTION(EnterObjectEnvironment);
            HANDLE_INSTRUCTION(Exp);
            HANDLE_INSTRUCTION(GetById);
            HANDLE_INSTRUCTION(GetByIdWithThis);
            HANDLE_INSTRUCTION(GetByValue);
            HANDLE_INSTRUCTION(GetByValueWithThis);
            HANDLE_INSTRUCTION(GetCalleeAndThisFromEnvironment);
            HANDLE_INSTRUCTION(GetGlobal);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(GetImportMeta);
            HANDLE_INSTRUCTION(GetIterator);
            HANDLE_INSTRUCTION(GetLength);
            HANDLE_INSTRUCTION(GetLengthWithThis);
            HANDLE_INSTRUCTION(GetMethod);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(GetNewTarget);
            HANDLE_INSTRUCTION(GetNextMethodFromIteratorRecord);
            HANDLE_INSTRUCTION(GetObjectFromIteratorRecord);
            HANDLE_INSTRUCTION(GetObjectPropertyIterator);
            HANDLE_INSTRUCTION(GetPrivateById);
            HANDLE_INSTRUCTION(GetBinding);
            HANDLE_INSTRUCTION(GreaterThan);
            HANDLE_INSTRUCTION(GreaterThanEquals);
            HANDLE_INSTRUCTION(HasPrivateId);
            HANDLE_INSTRUCTION(ImportCall);
            HANDLE_INSTRUCTION(In);
            HANDLE_INSTRUCTION(Increment);
            HANDLE_INSTRUCTION(InitializeLexicalBinding);
            HANDLE_INSTRUCTION(InitializeVariableBinding);
            HANDLE_INSTRUCTION(InstanceOf);
            HANDLE_INSTRUCTION(IteratorClose);
            HANDLE_INSTRUCTION(IteratorNext);
            HANDLE_INSTRUCTION(IteratorToArray);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(LeaveFinally);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(LeaveLexicalEnvironment);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(LeavePrivateEnvironment);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(LeaveUnwindContext);
            HANDLE_INSTRUCTION(LeftShift);
            HANDLE_INSTRUCTION(LessThan);
            HANDLE_INSTRUCTION(LessThanEquals);
            HANDLE_INSTRUCTION(LooselyEquals);
            HANDLE_INSTRUCTION(LooselyInequals);
            HANDLE_INSTRUCTION(Mod);
            HANDLE_INSTRUCTION(Mul);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(NewArray);
            HANDLE_INSTRUCTION(NewClass);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(NewFunction);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(NewObject);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(NewPrimitiveArray);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(NewRegExp);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(NewTypeError);
            HANDLE_INSTRUCTION(Not);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(PrepareYield);
            HANDLE_INSTRUCTION(PostfixDecrement);
            HANDLE_INSTRUCTION(PostfixIncrement);
            HANDLE_INSTRUCTION(PutById);
            HANDLE_INSTRUCTION(PutByIdWithThis);
            HANDLE_INSTRUCTION(PutByValue);
            HANDLE_INSTRUCTION(PutByValueWithThis);
            HANDLE_INSTRUCTION(PutPrivateById);
            HANDLE_INSTRUCTION(ResolveSuperBase);
            HANDLE_INSTRUCTION(ResolveThisBinding);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(RestoreScheduledJump);
            HANDLE_INSTRUCTION(RightShift);
            HANDLE_INSTRUCTION(SetLexicalBinding);
            HANDLE_INSTRUCTION(SetVariableBinding);
            HANDLE_INSTRUCTION(StrictlyEquals);
            HANDLE_INSTRUCTION(StrictlyInequals);
            HANDLE_INSTRUCTION(Sub);
            HANDLE_INSTRUCTION(SuperCallWithArgumentArray);
            HANDLE_INSTRUCTION(Throw);
            HANDLE_INSTRUCTION(ThrowIfNotObject);
            HANDLE_INSTRUCTION(ThrowIfNullish);
            HANDLE_INSTRUCTION(ThrowIfTDZ);
            HANDLE_INSTRUCTION(Typeof);
            HANDLE_INSTRUCTION(TypeofBinding);
            HANDLE_INSTRUCTION(UnaryMinus);
            HANDLE_INSTRUCTION(UnaryPlus);
            HANDLE_INSTRUCTION(UnsignedRightShift);

        handle_Await: {
            auto& instruction = *reinterpret_cast<Op::Await const*>(&bytecode[program_counter]);
            instruction.execute_impl(*this);
            return;
        }

        handle_Return: {
            auto& instruction = *reinterpret_cast<Op::Return const*>(&bytecode[program_counter]);
            instruction.execute_impl(*this);
            return;
        }

        handle_Yield: {
            auto& instruction = *reinterpret_cast<Op::Yield const*>(&bytecode[program_counter]);
            instruction.execute_impl(*this);
            // Note: A `yield` statement will not go through a finally statement,
            //       hence we need to set a flag to not do so,
            //       but we generate a Yield Operation in the case of returns in
            //       generators as well, so we need to check if it will actually
            //       continue or is a `return` in disguise
            return;
        }
        }
    }
}

Interpreter::ResultAndReturnRegister Interpreter::run_executable(Executable& executable, Optional<size_t> entry_point, Value initial_accumulator_value)
{
    dbgln_if(JS_BYTECODE_DEBUG, "Bytecode::Interpreter will run unit {:p}", &executable);

    TemporaryChange restore_executable { m_current_executable, GCPtr { executable } };
    TemporaryChange restore_saved_jump { m_scheduled_jump, Optional<size_t> {} };
    TemporaryChange restore_realm { m_realm, GCPtr { vm().current_realm() } };
    TemporaryChange restore_global_object { m_global_object, GCPtr { m_realm->global_object() } };
    TemporaryChange restore_global_declarative_environment { m_global_declarative_environment, GCPtr { m_realm->global_environment().declarative_record() } };

    VERIFY(!vm().execution_context_stack().is_empty());

    auto& running_execution_context = vm().running_execution_context();
    u32 registers_and_constants_and_locals_count = executable.number_of_registers + executable.constants.size() + executable.local_variable_names.size();
    if (running_execution_context.registers_and_constants_and_locals.size() < registers_and_constants_and_locals_count)
        running_execution_context.registers_and_constants_and_locals.resize(registers_and_constants_and_locals_count);

    TemporaryChange restore_running_execution_context { m_running_execution_context, &running_execution_context };
    TemporaryChange restore_arguments { m_arguments, running_execution_context.arguments.span() };
    TemporaryChange restore_registers_and_constants_and_locals { m_registers_and_constants_and_locals, running_execution_context.registers_and_constants_and_locals.span() };

    reg(Register::accumulator()) = initial_accumulator_value;
    reg(Register::return_value()) = {};

    // NOTE: We only copy the `this` value from ExecutionContext if it's not already set.
    //       If we are re-entering an async/generator context, the `this` value
    //       may have already been cached by a ResolveThisBinding instruction,
    //       and subsequent instructions expect this value to be set.
    if (reg(Register::this_value()).is_empty())
        reg(Register::this_value()) = running_execution_context.this_value;

    running_execution_context.executable = &executable;

    for (size_t i = 0; i < executable.constants.size(); ++i) {
        running_execution_context.registers_and_constants_and_locals[executable.number_of_registers + i] = executable.constants[i];
    }

    run_bytecode(entry_point.value_or(0));

    dbgln_if(JS_BYTECODE_DEBUG, "Bytecode::Interpreter did run unit {:p}", &executable);

    if constexpr (JS_BYTECODE_DEBUG) {
        auto const& registers_and_constants_and_locals = running_execution_context.registers_and_constants_and_locals;
        for (size_t i = 0; i < executable.number_of_registers; ++i) {
            String value_string;
            if (registers_and_constants_and_locals[i].is_empty())
                value_string = "(empty)"_string;
            else
                value_string = registers_and_constants_and_locals[i].to_string_without_side_effects();
            dbgln("[{:3}] {}", i, value_string);
        }
    }

    auto return_value = js_undefined();
    if (!reg(Register::return_value()).is_empty())
        return_value = reg(Register::return_value());
    else if (!reg(Register::saved_return_value()).is_empty())
        return_value = reg(Register::saved_return_value());
    auto exception = reg(Register::exception());

    // At this point we may have already run any queued promise jobs via on_call_stack_emptied,
    // in which case this is a no-op.
    vm().run_queued_promise_jobs();

    vm().finish_execution_generation();

    if (!exception.is_empty())
        return { throw_completion(exception), running_execution_context.registers_and_constants_and_locals[0] };
    return { return_value, running_execution_context.registers_and_constants_and_locals[0] };
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
    reg(Register::exception()) = {};
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
    reg(Register::exception()) = {};
    m_scheduled_jump = running_execution_context().previously_scheduled_jumps.take_last();
}

void Interpreter::enter_object_environment(Object& object)
{
    auto& old_environment = running_execution_context().lexical_environment;
    running_execution_context().saved_lexical_environments.append(old_environment);
    running_execution_context().lexical_environment = new_object_environment(object, true, old_environment);
}

ThrowCompletionOr<NonnullGCPtr<Bytecode::Executable>> compile(VM& vm, ASTNode const& node, FunctionKind kind, DeprecatedFlyString const& name)
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

ThrowCompletionOr<NonnullGCPtr<Bytecode::Executable>> compile(VM& vm, ECMAScriptFunctionObject const& function)
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

namespace JS::Bytecode {

ByteString Instruction::to_byte_string(Bytecode::Executable const& executable) const
{
#define __BYTECODE_OP(op)       \
    case Instruction::Type::op: \
        return static_cast<Bytecode::Op::op const&>(*this).to_byte_string_impl(executable);

    switch (type()) {
        ENUMERATE_BYTECODE_OPS(__BYTECODE_OP)
    default:
        VERIFY_NOT_REACHED();
    }

#undef __BYTECODE_OP
}

}

namespace JS::Bytecode::Op {

static void dump_object(Object& o, HashTable<Object const*>& seen, int indent = 0)
{
    if (seen.contains(&o))
        return;
    seen.set(&o);
    for (auto& it : o.shape().property_table()) {
        auto value = o.get_direct(it.value.offset);
        dbgln("{}  {} -> {}", String::repeated(' ', indent).release_value(), it.key.to_display_string(), value);
        if (value.is_object()) {
            dump_object(value.as_object(), seen, indent + 2);
        }
    }
}

void Dump::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto value = interpreter.get(m_value);
    dbgln("(DUMP) {}: {}", m_text, value);
    if (value.is_object()) {
        HashTable<Object const*> seen;
        dump_object(value.as_object(), seen);
    }
}

#define JS_DEFINE_EXECUTE_FOR_COMMON_BINARY_OP(OpTitleCase, op_snake_case)                      \
    ThrowCompletionOr<void> OpTitleCase::execute_impl(Bytecode::Interpreter& interpreter) const \
    {                                                                                           \
        auto& vm = interpreter.vm();                                                            \
        auto lhs = interpreter.get(m_lhs);                                                      \
        auto rhs = interpreter.get(m_rhs);                                                      \
        interpreter.set(m_dst, TRY(op_snake_case(vm, lhs, rhs)));                               \
        return {};                                                                              \
    }

#define JS_DEFINE_TO_BYTE_STRING_FOR_COMMON_BINARY_OP(OpTitleCase, op_snake_case)             \
    ByteString OpTitleCase::to_byte_string_impl(Bytecode::Executable const& executable) const \
    {                                                                                         \
        return ByteString::formatted(#OpTitleCase " {}, {}, {}",                              \
            format_operand("dst"sv, m_dst, executable),                                       \
            format_operand("lhs"sv, m_lhs, executable),                                       \
            format_operand("rhs"sv, m_rhs, executable));                                      \
    }

JS_ENUMERATE_COMMON_BINARY_OPS_WITHOUT_FAST_PATH(JS_DEFINE_EXECUTE_FOR_COMMON_BINARY_OP)
JS_ENUMERATE_COMMON_BINARY_OPS_WITHOUT_FAST_PATH(JS_DEFINE_TO_BYTE_STRING_FOR_COMMON_BINARY_OP)
JS_ENUMERATE_COMMON_BINARY_OPS_WITH_FAST_PATH(JS_DEFINE_TO_BYTE_STRING_FOR_COMMON_BINARY_OP)

ThrowCompletionOr<void> Add::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);

    if (lhs.is_number() && rhs.is_number()) {
        if (lhs.is_int32() && rhs.is_int32()) {
            if (!Checked<i32>::addition_would_overflow(lhs.as_i32(), rhs.as_i32())) {
                interpreter.set(m_dst, Value(lhs.as_i32() + rhs.as_i32()));
                return {};
            }
        }
        interpreter.set(m_dst, Value(lhs.as_double() + rhs.as_double()));
        return {};
    }

    interpreter.set(m_dst, TRY(add(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> Mul::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);

    if (lhs.is_number() && rhs.is_number()) {
        if (lhs.is_int32() && rhs.is_int32()) {
            if (!Checked<i32>::multiplication_would_overflow(lhs.as_i32(), rhs.as_i32())) {
                interpreter.set(m_dst, Value(lhs.as_i32() * rhs.as_i32()));
                return {};
            }
        }
        interpreter.set(m_dst, Value(lhs.as_double() * rhs.as_double()));
        return {};
    }

    interpreter.set(m_dst, TRY(mul(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> Sub::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);

    if (lhs.is_number() && rhs.is_number()) {
        if (lhs.is_int32() && rhs.is_int32()) {
            if (!Checked<i32>::subtraction_would_overflow(lhs.as_i32(), rhs.as_i32())) {
                interpreter.set(m_dst, Value(lhs.as_i32() - rhs.as_i32()));
                return {};
            }
        }
        interpreter.set(m_dst, Value(lhs.as_double() - rhs.as_double()));
        return {};
    }

    interpreter.set(m_dst, TRY(sub(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> BitwiseXor::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_int32() && rhs.is_int32()) {
        interpreter.set(m_dst, Value(lhs.as_i32() ^ rhs.as_i32()));
        return {};
    }
    interpreter.set(m_dst, TRY(bitwise_xor(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> BitwiseAnd::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_int32() && rhs.is_int32()) {
        interpreter.set(m_dst, Value(lhs.as_i32() & rhs.as_i32()));
        return {};
    }
    interpreter.set(m_dst, TRY(bitwise_and(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> BitwiseOr::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_int32() && rhs.is_int32()) {
        interpreter.set(m_dst, Value(lhs.as_i32() | rhs.as_i32()));
        return {};
    }
    interpreter.set(m_dst, TRY(bitwise_or(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> UnsignedRightShift::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_int32() && rhs.is_int32()) {
        auto const shift_count = static_cast<u32>(rhs.as_i32()) % 32;
        interpreter.set(m_dst, Value(static_cast<u32>(lhs.as_i32()) >> shift_count));
        return {};
    }
    interpreter.set(m_dst, TRY(unsigned_right_shift(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> RightShift::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_int32() && rhs.is_int32()) {
        auto const shift_count = static_cast<u32>(rhs.as_i32()) % 32;
        interpreter.set(m_dst, Value(lhs.as_i32() >> shift_count));
        return {};
    }
    interpreter.set(m_dst, TRY(right_shift(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> LeftShift::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_int32() && rhs.is_int32()) {
        auto const shift_count = static_cast<u32>(rhs.as_i32()) % 32;
        interpreter.set(m_dst, Value(lhs.as_i32() << shift_count));
        return {};
    }
    interpreter.set(m_dst, TRY(left_shift(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> LessThan::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_number() && rhs.is_number()) {
        if (lhs.is_int32() && rhs.is_int32()) {
            interpreter.set(m_dst, Value(lhs.as_i32() < rhs.as_i32()));
            return {};
        }
        interpreter.set(m_dst, Value(lhs.as_double() < rhs.as_double()));
        return {};
    }
    interpreter.set(m_dst, TRY(less_than(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> LessThanEquals::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_number() && rhs.is_number()) {
        if (lhs.is_int32() && rhs.is_int32()) {
            interpreter.set(m_dst, Value(lhs.as_i32() <= rhs.as_i32()));
            return {};
        }
        interpreter.set(m_dst, Value(lhs.as_double() <= rhs.as_double()));
        return {};
    }
    interpreter.set(m_dst, TRY(less_than_equals(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> GreaterThan::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_number() && rhs.is_number()) {
        if (lhs.is_int32() && rhs.is_int32()) {
            interpreter.set(m_dst, Value(lhs.as_i32() > rhs.as_i32()));
            return {};
        }
        interpreter.set(m_dst, Value(lhs.as_double() > rhs.as_double()));
        return {};
    }
    interpreter.set(m_dst, TRY(greater_than(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> GreaterThanEquals::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);
    if (lhs.is_number() && rhs.is_number()) {
        if (lhs.is_int32() && rhs.is_int32()) {
            interpreter.set(m_dst, Value(lhs.as_i32() >= rhs.as_i32()));
            return {};
        }
        interpreter.set(m_dst, Value(lhs.as_double() >= rhs.as_double()));
        return {};
    }
    interpreter.set(m_dst, TRY(greater_than_equals(vm, lhs, rhs)));
    return {};
}

static ThrowCompletionOr<Value> not_(VM&, Value value)
{
    return Value(!value.to_boolean());
}

static ThrowCompletionOr<Value> typeof_(VM& vm, Value value)
{
    return PrimitiveString::create(vm, value.typeof());
}

#define JS_DEFINE_COMMON_UNARY_OP(OpTitleCase, op_snake_case)                                   \
    ThrowCompletionOr<void> OpTitleCase::execute_impl(Bytecode::Interpreter& interpreter) const \
    {                                                                                           \
        auto& vm = interpreter.vm();                                                            \
        interpreter.set(dst(), TRY(op_snake_case(vm, interpreter.get(src()))));                 \
        return {};                                                                              \
    }                                                                                           \
    ByteString OpTitleCase::to_byte_string_impl(Bytecode::Executable const& executable) const   \
    {                                                                                           \
        return ByteString::formatted(#OpTitleCase " {}, {}",                                    \
            format_operand("dst"sv, dst(), executable),                                         \
            format_operand("src"sv, src(), executable));                                        \
    }

JS_ENUMERATE_COMMON_UNARY_OPS(JS_DEFINE_COMMON_UNARY_OP)

void NewArray::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto array = MUST(Array::create(interpreter.realm(), 0));
    for (size_t i = 0; i < m_element_count; i++) {
        array->indexed_properties().put(i, interpreter.get(m_elements[i]), default_attributes);
    }
    interpreter.set(dst(), array);
}

void NewPrimitiveArray::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto array = MUST(Array::create(interpreter.realm(), 0));
    for (size_t i = 0; i < m_element_count; i++)
        array->indexed_properties().put(i, m_elements[i], default_attributes);
    interpreter.set(dst(), array);
}

void AddPrivateName::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto const& name = interpreter.current_executable().get_identifier(m_name);
    interpreter.vm().running_execution_context().private_environment->add_private_name(name);
}

ThrowCompletionOr<void> ArrayAppend::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return append(interpreter.vm(), interpreter.get(dst()), interpreter.get(src()), m_is_spread);
}

ThrowCompletionOr<void> ImportCall::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto specifier = interpreter.get(m_specifier);
    auto options_value = interpreter.get(m_options);
    interpreter.set(dst(), TRY(perform_import_call(vm, specifier, options_value)));
    return {};
}

ThrowCompletionOr<void> IteratorToArray::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.set(dst(), TRY(iterator_to_array(interpreter.vm(), interpreter.get(iterator()))));
    return {};
}

void NewObject::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto& realm = *vm.current_realm();
    interpreter.set(dst(), Object::create(realm, realm.intrinsics().object_prototype()));
}

void NewRegExp::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.set(dst(),
        new_regexp(
            interpreter.vm(),
            interpreter.current_executable().regex_table->get(m_regex_index),
            interpreter.current_executable().get_string(m_source_index),
            interpreter.current_executable().get_string(m_flags_index)));
}

#define JS_DEFINE_NEW_BUILTIN_ERROR_OP(ErrorName)                                                                      \
    void New##ErrorName::execute_impl(Bytecode::Interpreter& interpreter) const                                        \
    {                                                                                                                  \
        auto& vm = interpreter.vm();                                                                                   \
        auto& realm = *vm.current_realm();                                                                             \
        interpreter.set(dst(), ErrorName::create(realm, interpreter.current_executable().get_string(m_error_string))); \
    }                                                                                                                  \
    ByteString New##ErrorName::to_byte_string_impl(Bytecode::Executable const& executable) const                       \
    {                                                                                                                  \
        return ByteString::formatted("New" #ErrorName " {}, {}",                                                       \
            format_operand("dst"sv, m_dst, executable),                                                                \
            executable.string_table->get(m_error_string));                                                             \
    }

JS_ENUMERATE_NEW_BUILTIN_ERROR_OPS(JS_DEFINE_NEW_BUILTIN_ERROR_OP)

ThrowCompletionOr<void> CopyObjectExcludingProperties::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto& realm = *vm.current_realm();

    auto from_object = interpreter.get(m_from_object);

    auto to_object = Object::create(realm, realm.intrinsics().object_prototype());

    HashTable<PropertyKey> excluded_names;
    for (size_t i = 0; i < m_excluded_names_count; ++i) {
        excluded_names.set(TRY(interpreter.get(m_excluded_names[i]).to_property_key(vm)));
    }

    TRY(to_object->copy_data_properties(vm, from_object, excluded_names));

    interpreter.set(dst(), to_object);
    return {};
}

ThrowCompletionOr<void> ConcatString::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto string = TRY(interpreter.get(src()).to_primitive_string(vm));
    interpreter.set(dst(), PrimitiveString::create(vm, interpreter.get(dst()).as_string(), string));
    return {};
}

ThrowCompletionOr<void> GetBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto& executable = interpreter.current_executable();

    if (m_cache.is_valid()) {
        auto const* environment = interpreter.running_execution_context().lexical_environment.ptr();
        for (size_t i = 0; i < m_cache.hops; ++i)
            environment = environment->outer_environment();
        if (!environment->is_permanently_screwed_by_eval()) {
            interpreter.set(dst(), TRY(static_cast<DeclarativeEnvironment const&>(*environment).get_binding_value_direct(vm, m_cache.index)));
            return {};
        }
        m_cache = {};
    }

    auto reference = TRY(vm.resolve_binding(executable.get_identifier(m_identifier)));
    if (reference.environment_coordinate().has_value())
        m_cache = reference.environment_coordinate().value();
    interpreter.set(dst(), TRY(reference.get_value(vm)));
    return {};
}

ThrowCompletionOr<void> GetCalleeAndThisFromEnvironment::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto callee_and_this = TRY(get_callee_and_this_from_environment(
        interpreter,
        interpreter.current_executable().get_identifier(m_identifier),
        m_cache));
    interpreter.set(m_callee, callee_and_this.callee);
    interpreter.set(m_this_value, callee_and_this.this_value);
    return {};
}

ThrowCompletionOr<void> GetGlobal::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.set(dst(), TRY(get_global(interpreter, m_identifier, interpreter.current_executable().global_variable_caches[m_cache_index])));
    return {};
}

ThrowCompletionOr<void> DeleteVariable::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const& string = interpreter.current_executable().get_identifier(m_identifier);
    auto reference = TRY(vm.resolve_binding(string));
    interpreter.set(dst(), Value(TRY(reference.delete_(vm))));
    return {};
}

void CreateLexicalEnvironment::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto make_and_swap_envs = [&](auto& old_environment) {
        auto declarative_environment = new_declarative_environment(*old_environment).ptr();
        declarative_environment->ensure_capacity(m_capacity);
        GCPtr<Environment> environment = declarative_environment;
        swap(old_environment, environment);
        return environment;
    };
    auto& running_execution_context = interpreter.running_execution_context();
    running_execution_context.saved_lexical_environments.append(make_and_swap_envs(running_execution_context.lexical_environment));
}

void CreatePrivateEnvironment::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& running_execution_context = interpreter.vm().running_execution_context();
    auto outer_private_environment = running_execution_context.private_environment;
    running_execution_context.private_environment = new_private_environment(interpreter.vm(), outer_private_environment);
}

void CreateVariableEnvironment::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& running_execution_context = interpreter.running_execution_context();
    auto var_environment = new_declarative_environment(*running_execution_context.lexical_environment);
    var_environment->ensure_capacity(m_capacity);
    running_execution_context.variable_environment = var_environment;
    running_execution_context.lexical_environment = var_environment;
}

ThrowCompletionOr<void> EnterObjectEnvironment::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto object = TRY(interpreter.get(m_object).to_object(interpreter.vm()));
    interpreter.enter_object_environment(*object);
    return {};
}

void Catch::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.catch_exception(dst());
}

void LeaveFinally::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.leave_finally();
}

void RestoreScheduledJump::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.restore_scheduled_jump();
}

ThrowCompletionOr<void> CreateVariable::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto const& name = interpreter.current_executable().get_identifier(m_identifier);
    return create_variable(interpreter.vm(), name, m_mode, m_is_global, m_is_immutable, m_is_strict);
}

ThrowCompletionOr<void> CreateRestParams::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto const& arguments = interpreter.running_execution_context().arguments;
    auto arguments_count = interpreter.running_execution_context().passed_argument_count;
    auto array = MUST(Array::create(interpreter.realm(), 0));
    for (size_t rest_index = m_rest_index; rest_index < arguments_count; ++rest_index)
        array->indexed_properties().append(arguments[rest_index]);
    interpreter.set(m_dst, array);
    return {};
}

ThrowCompletionOr<void> CreateArguments::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto const& function = interpreter.running_execution_context().function;
    auto const& arguments = interpreter.running_execution_context().arguments;
    auto const& environment = interpreter.running_execution_context().lexical_environment;

    auto passed_arguments = ReadonlySpan<Value> { arguments.data(), interpreter.running_execution_context().passed_argument_count };
    Object* arguments_object;
    if (m_kind == Kind::Mapped) {
        arguments_object = create_mapped_arguments_object(interpreter.vm(), *function, function->formal_parameters(), passed_arguments, *environment);
    } else {
        arguments_object = create_unmapped_arguments_object(interpreter.vm(), passed_arguments);
    }

    if (m_dst.has_value()) {
        interpreter.set(*m_dst, arguments_object);
        return {};
    }

    if (m_is_immutable) {
        MUST(environment->create_immutable_binding(interpreter.vm(), interpreter.vm().names.arguments.as_string(), false));
    } else {
        MUST(environment->create_mutable_binding(interpreter.vm(), interpreter.vm().names.arguments.as_string(), false));
    }
    MUST(environment->initialize_binding(interpreter.vm(), interpreter.vm().names.arguments.as_string(), arguments_object, Environment::InitializeBindingHint::Normal));

    return {};
}

template<EnvironmentMode environment_mode, BindingInitializationMode initialization_mode>
static ThrowCompletionOr<void> initialize_or_set_binding(Interpreter& interpreter, IdentifierTableIndex identifier_index, Value value, EnvironmentCoordinate& cache)
{
    auto& vm = interpreter.vm();

    auto* environment = environment_mode == EnvironmentMode::Lexical
        ? interpreter.running_execution_context().lexical_environment.ptr()
        : interpreter.running_execution_context().variable_environment.ptr();

    if (cache.is_valid()) {
        for (size_t i = 0; i < cache.hops; ++i)
            environment = environment->outer_environment();
        if (!environment->is_permanently_screwed_by_eval()) {
            if constexpr (initialization_mode == BindingInitializationMode::Initialize) {
                TRY(static_cast<DeclarativeEnvironment&>(*environment).initialize_binding_direct(vm, cache.index, value, Environment::InitializeBindingHint::Normal));
            } else {
                TRY(static_cast<DeclarativeEnvironment&>(*environment).set_mutable_binding_direct(vm, cache.index, value, vm.in_strict_mode()));
            }
            return {};
        }
        cache = {};
    }

    auto reference = TRY(vm.resolve_binding(interpreter.current_executable().get_identifier(identifier_index), environment));
    if (reference.environment_coordinate().has_value())
        cache = reference.environment_coordinate().value();
    if constexpr (initialization_mode == BindingInitializationMode::Initialize) {
        TRY(reference.initialize_referenced_binding(vm, value));
    } else if (initialization_mode == BindingInitializationMode::Set) {
        TRY(reference.put_value(vm, value));
    }
    return {};
}

ThrowCompletionOr<void> InitializeLexicalBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return initialize_or_set_binding<EnvironmentMode::Lexical, BindingInitializationMode::Initialize>(interpreter, m_identifier, interpreter.get(m_src), m_cache);
}

ThrowCompletionOr<void> InitializeVariableBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return initialize_or_set_binding<EnvironmentMode::Var, BindingInitializationMode::Initialize>(interpreter, m_identifier, interpreter.get(m_src), m_cache);
}

ThrowCompletionOr<void> SetLexicalBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return initialize_or_set_binding<EnvironmentMode::Lexical, BindingInitializationMode::Set>(interpreter, m_identifier, interpreter.get(m_src), m_cache);
}

ThrowCompletionOr<void> SetVariableBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return initialize_or_set_binding<EnvironmentMode::Var, BindingInitializationMode::Set>(interpreter, m_identifier, interpreter.get(m_src), m_cache);
}

ThrowCompletionOr<void> GetById::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_identifier = interpreter.current_executable().get_identifier(m_base_identifier);
    auto const& property_identifier = interpreter.current_executable().get_identifier(m_property);

    auto base_value = interpreter.get(base());
    auto& cache = interpreter.current_executable().property_lookup_caches[m_cache_index];

    interpreter.set(dst(), TRY(get_by_id(interpreter.vm(), base_identifier, property_identifier, base_value, base_value, cache)));
    return {};
}

ThrowCompletionOr<void> GetByIdWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_value = interpreter.get(m_base);
    auto this_value = interpreter.get(m_this_value);
    auto& cache = interpreter.current_executable().property_lookup_caches[m_cache_index];
    interpreter.set(dst(), TRY(get_by_id(interpreter.vm(), {}, interpreter.current_executable().get_identifier(m_property), base_value, this_value, cache)));
    return {};
}

ThrowCompletionOr<void> GetLength::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_identifier = interpreter.current_executable().get_identifier(m_base_identifier);

    auto base_value = interpreter.get(base());
    auto& cache = interpreter.current_executable().property_lookup_caches[m_cache_index];

    interpreter.set(dst(), TRY(get_by_id<GetByIdMode::Length>(interpreter.vm(), base_identifier, interpreter.vm().names.length.as_string(), base_value, base_value, cache)));
    return {};
}

ThrowCompletionOr<void> GetLengthWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_value = interpreter.get(m_base);
    auto this_value = interpreter.get(m_this_value);
    auto& cache = interpreter.current_executable().property_lookup_caches[m_cache_index];
    interpreter.set(dst(), TRY(get_by_id<GetByIdMode::Length>(interpreter.vm(), {}, interpreter.vm().names.length.as_string(), base_value, this_value, cache)));
    return {};
}

ThrowCompletionOr<void> GetPrivateById::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const& name = interpreter.current_executable().get_identifier(m_property);
    auto base_value = interpreter.get(m_base);
    auto private_reference = make_private_reference(vm, base_value, name);
    interpreter.set(dst(), TRY(private_reference.get_value(vm)));
    return {};
}

ThrowCompletionOr<void> HasPrivateId::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();

    auto base = interpreter.get(m_base);
    if (!base.is_object())
        return vm.throw_completion<TypeError>(ErrorType::InOperatorWithObject);

    auto private_environment = interpreter.running_execution_context().private_environment;
    VERIFY(private_environment);
    auto private_name = private_environment->resolve_private_identifier(interpreter.current_executable().get_identifier(m_property));
    interpreter.set(dst(), Value(base.as_object().private_element_find(private_name) != nullptr));
    return {};
}

ThrowCompletionOr<void> PutById::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto value = interpreter.get(m_src);
    auto base = interpreter.get(m_base);
    auto base_identifier = interpreter.current_executable().get_identifier(m_base_identifier);
    PropertyKey name = interpreter.current_executable().get_identifier(m_property);
    auto& cache = interpreter.current_executable().property_lookup_caches[m_cache_index];
    TRY(put_by_property_key(vm, base, base, value, base_identifier, name, m_kind, &cache));
    return {};
}

ThrowCompletionOr<void> PutByIdWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto value = interpreter.get(m_src);
    auto base = interpreter.get(m_base);
    PropertyKey name = interpreter.current_executable().get_identifier(m_property);
    auto& cache = interpreter.current_executable().property_lookup_caches[m_cache_index];
    TRY(put_by_property_key(vm, base, interpreter.get(m_this_value), value, {}, name, m_kind, &cache));
    return {};
}

ThrowCompletionOr<void> PutPrivateById::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto value = interpreter.get(m_src);
    auto object = TRY(interpreter.get(m_base).to_object(vm));
    auto name = interpreter.current_executable().get_identifier(m_property);
    auto private_reference = make_private_reference(vm, object, name);
    TRY(private_reference.put_value(vm, value));
    return {};
}

ThrowCompletionOr<void> DeleteById::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_value = interpreter.get(m_base);
    interpreter.set(dst(), TRY(Bytecode::delete_by_id(interpreter, base_value, m_property)));
    return {};
}

ThrowCompletionOr<void> DeleteByIdWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto base_value = interpreter.get(m_base);
    auto const& identifier = interpreter.current_executable().get_identifier(m_property);
    bool strict = vm.in_strict_mode();
    auto reference = Reference { base_value, identifier, interpreter.get(m_this_value), strict };
    interpreter.set(dst(), Value(TRY(reference.delete_(vm))));
    return {};
}

ThrowCompletionOr<void> ResolveThisBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& cached_this_value = interpreter.reg(Register::this_value());
    if (!cached_this_value.is_empty())
        return {};
    // OPTIMIZATION: Because the value of 'this' cannot be reassigned during a function execution, it's
    //               resolved once and then saved for subsequent use.
    auto& running_execution_context = interpreter.running_execution_context();
    if (auto function = running_execution_context.function; function && is<ECMAScriptFunctionObject>(*function) && !static_cast<ECMAScriptFunctionObject&>(*function).allocates_function_environment()) {
        cached_this_value = running_execution_context.this_value;
    } else {
        auto& vm = interpreter.vm();
        cached_this_value = TRY(vm.resolve_this_binding());
    }
    return {};
}

// https://tc39.es/ecma262/#sec-makesuperpropertyreference
ThrowCompletionOr<void> ResolveSuperBase::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();

    // 1. Let env be GetThisEnvironment().
    auto& env = verify_cast<FunctionEnvironment>(*get_this_environment(vm));

    // 2. Assert: env.HasSuperBinding() is true.
    VERIFY(env.has_super_binding());

    // 3. Let baseValue be ? env.GetSuperBase().
    interpreter.set(dst(), TRY(env.get_super_base()));

    return {};
}

void GetNewTarget::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.set(dst(), interpreter.vm().get_new_target());
}

void GetImportMeta::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.set(dst(), interpreter.vm().get_import_meta());
}

static ThrowCompletionOr<Value> dispatch_builtin_call(Bytecode::Interpreter& interpreter, Bytecode::Builtin builtin, ReadonlySpan<Operand> arguments)
{
    switch (builtin) {
    case Builtin::MathAbs:
        return TRY(MathObject::abs_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathLog:
        return TRY(MathObject::log_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathPow:
        return TRY(MathObject::pow_impl(interpreter.vm(), interpreter.get(arguments[0]), interpreter.get(arguments[1])));
    case Builtin::MathExp:
        return TRY(MathObject::exp_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathCeil:
        return TRY(MathObject::ceil_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathFloor:
        return TRY(MathObject::floor_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathRound:
        return TRY(MathObject::round_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathSqrt:
        return TRY(MathObject::sqrt_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Bytecode::Builtin::__Count:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

ThrowCompletionOr<void> Call::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto callee = interpreter.get(m_callee);

    TRY(throw_if_needed_for_call(interpreter, callee, call_type(), expression_string()));

    if (m_builtin.has_value()
        && m_argument_count == Bytecode::builtin_argument_count(m_builtin.value())
        && callee.is_object()
        && interpreter.realm().get_builtin_value(m_builtin.value()) == &callee.as_object()) {
        interpreter.set(dst(), TRY(dispatch_builtin_call(interpreter, m_builtin.value(), { m_arguments, m_argument_count })));
        return {};
    }

    Vector<Value> argument_values;
    argument_values.ensure_capacity(m_argument_count);
    for (size_t i = 0; i < m_argument_count; ++i)
        argument_values.unchecked_append(interpreter.get(m_arguments[i]));
    interpreter.set(dst(), TRY(perform_call(interpreter, interpreter.get(m_this_value), call_type(), callee, argument_values)));
    return {};
}

ThrowCompletionOr<void> CallWithArgumentArray::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto callee = interpreter.get(m_callee);
    TRY(throw_if_needed_for_call(interpreter, callee, call_type(), expression_string()));
    auto argument_values = argument_list_evaluation(interpreter.vm(), interpreter.get(arguments()));
    interpreter.set(dst(), TRY(perform_call(interpreter, interpreter.get(m_this_value), call_type(), callee, move(argument_values))));
    return {};
}

// 13.3.7.1 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
ThrowCompletionOr<void> SuperCallWithArgumentArray::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.set(dst(), TRY(super_call_with_argument_array(interpreter.vm(), interpreter.get(arguments()), m_is_synthetic)));
    return {};
}

void NewFunction::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    interpreter.set(dst(), new_function(vm, m_function_node, m_lhs_name, m_home_object));
}

void Return::execute_impl(Bytecode::Interpreter& interpreter) const
{
    if (m_value.has_value())
        interpreter.do_return(interpreter.get(*m_value));
    else
        interpreter.do_return(js_undefined());
}

ThrowCompletionOr<void> Increment::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto old_value = interpreter.get(dst());

    // OPTIMIZATION: Fast path for Int32 values.
    if (old_value.is_int32()) {
        auto integer_value = old_value.as_i32();
        if (integer_value != NumericLimits<i32>::max()) [[likely]] {
            interpreter.set(dst(), Value { integer_value + 1 });
            return {};
        }
    }

    old_value = TRY(old_value.to_numeric(vm));

    if (old_value.is_number())
        interpreter.set(dst(), Value(old_value.as_double() + 1));
    else
        interpreter.set(dst(), BigInt::create(vm, old_value.as_bigint().big_integer().plus(Crypto::SignedBigInteger { 1 })));
    return {};
}

ThrowCompletionOr<void> PostfixIncrement::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto old_value = interpreter.get(m_src);

    // OPTIMIZATION: Fast path for Int32 values.
    if (old_value.is_int32()) {
        auto integer_value = old_value.as_i32();
        if (integer_value != NumericLimits<i32>::max()) [[likely]] {
            interpreter.set(m_dst, old_value);
            interpreter.set(m_src, Value { integer_value + 1 });
            return {};
        }
    }

    old_value = TRY(old_value.to_numeric(vm));
    interpreter.set(m_dst, old_value);

    if (old_value.is_number())
        interpreter.set(m_src, Value(old_value.as_double() + 1));
    else
        interpreter.set(m_src, BigInt::create(vm, old_value.as_bigint().big_integer().plus(Crypto::SignedBigInteger { 1 })));
    return {};
}

ThrowCompletionOr<void> Decrement::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto old_value = interpreter.get(dst());

    old_value = TRY(old_value.to_numeric(vm));

    if (old_value.is_number())
        interpreter.set(dst(), Value(old_value.as_double() - 1));
    else
        interpreter.set(dst(), BigInt::create(vm, old_value.as_bigint().big_integer().minus(Crypto::SignedBigInteger { 1 })));
    return {};
}

ThrowCompletionOr<void> PostfixDecrement::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto old_value = interpreter.get(m_src);

    old_value = TRY(old_value.to_numeric(vm));
    interpreter.set(m_dst, old_value);

    if (old_value.is_number())
        interpreter.set(m_src, Value(old_value.as_double() - 1));
    else
        interpreter.set(m_src, BigInt::create(vm, old_value.as_bigint().big_integer().minus(Crypto::SignedBigInteger { 1 })));
    return {};
}

ThrowCompletionOr<void> Throw::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return throw_completion(interpreter.get(src()));
}

ThrowCompletionOr<void> ThrowIfNotObject::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto src = interpreter.get(m_src);
    if (!src.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, src.to_string_without_side_effects());
    return {};
}

ThrowCompletionOr<void> ThrowIfNullish::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto value = interpreter.get(m_src);
    if (value.is_nullish())
        return vm.throw_completion<TypeError>(ErrorType::NotObjectCoercible, value.to_string_without_side_effects());
    return {};
}

ThrowCompletionOr<void> ThrowIfTDZ::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto value = interpreter.get(m_src);
    if (value.is_empty())
        return vm.throw_completion<ReferenceError>(ErrorType::BindingNotInitialized, value.to_string_without_side_effects());
    return {};
}

void LeaveLexicalEnvironment::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& running_execution_context = interpreter.running_execution_context();
    running_execution_context.lexical_environment = running_execution_context.saved_lexical_environments.take_last();
}

void LeavePrivateEnvironment::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& running_execution_context = interpreter.vm().running_execution_context();
    running_execution_context.private_environment = running_execution_context.private_environment->outer_environment();
}

void LeaveUnwindContext::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.leave_unwind_context();
}

void Yield::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto yielded_value = interpreter.get(m_value).value_or(js_undefined());
    interpreter.do_return(
        interpreter.do_yield(yielded_value, m_continuation_label));
}

void PrepareYield::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto value = interpreter.get(m_value).value_or(js_undefined());
    interpreter.set(m_dest, interpreter.do_yield(value, {}));
}

void Await::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto yielded_value = interpreter.get(m_argument).value_or(js_undefined());
    auto object = Object::create(interpreter.realm(), nullptr);
    object->define_direct_property("result", yielded_value, JS::default_attributes);
    // FIXME: If we get a pointer, which is not accurately representable as a double
    //        will cause this to explode
    object->define_direct_property("continuation", Value(m_continuation_label.address()), JS::default_attributes);
    object->define_direct_property("isAwait", Value(true), JS::default_attributes);
    interpreter.do_return(object);
}

ThrowCompletionOr<void> GetByValue::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_identifier = interpreter.current_executable().get_identifier(m_base_identifier);

    interpreter.set(dst(), TRY(get_by_value(interpreter.vm(), base_identifier, interpreter.get(m_base), interpreter.get(m_property))));
    return {};
}

ThrowCompletionOr<void> GetByValueWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto property_key_value = interpreter.get(m_property);
    auto object = TRY(interpreter.get(m_base).to_object(vm));
    auto property_key = TRY(property_key_value.to_property_key(vm));
    interpreter.set(dst(), TRY(object->internal_get(property_key, interpreter.get(m_this_value))));
    return {};
}

ThrowCompletionOr<void> PutByValue::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto value = interpreter.get(m_src);
    auto base_identifier = interpreter.current_executable().get_identifier(m_base_identifier);
    TRY(put_by_value(vm, interpreter.get(m_base), base_identifier, interpreter.get(m_property), value, m_kind));
    return {};
}

ThrowCompletionOr<void> PutByValueWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto value = interpreter.get(m_src);
    auto base = interpreter.get(m_base);
    auto property_key = m_kind != PropertyKind::Spread ? TRY(interpreter.get(m_property).to_property_key(vm)) : PropertyKey {};
    TRY(put_by_property_key(vm, base, interpreter.get(m_this_value), value, {}, property_key, m_kind));
    return {};
}

ThrowCompletionOr<void> DeleteByValue::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_value = interpreter.get(m_base);
    auto property_key_value = interpreter.get(m_property);
    interpreter.set(dst(), TRY(delete_by_value(interpreter, base_value, property_key_value)));

    return {};
}

ThrowCompletionOr<void> DeleteByValueWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto property_key_value = interpreter.get(m_property);
    auto base_value = interpreter.get(m_base);
    auto this_value = interpreter.get(m_this_value);
    interpreter.set(dst(), TRY(delete_by_value_with_this(interpreter, base_value, property_key_value, this_value)));

    return {};
}

ThrowCompletionOr<void> GetIterator::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    interpreter.set(dst(), TRY(get_iterator(vm, interpreter.get(iterable()), m_hint)));
    return {};
}

ThrowCompletionOr<void> GetObjectFromIteratorRecord::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& iterator_record = verify_cast<IteratorRecord>(interpreter.get(m_iterator_record).as_object());
    interpreter.set(m_object, iterator_record.iterator);
    return {};
}

ThrowCompletionOr<void> GetNextMethodFromIteratorRecord::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& iterator_record = verify_cast<IteratorRecord>(interpreter.get(m_iterator_record).as_object());
    interpreter.set(m_next_method, iterator_record.next_method);
    return {};
}

ThrowCompletionOr<void> GetMethod::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto identifier = interpreter.current_executable().get_identifier(m_property);
    auto method = TRY(interpreter.get(m_object).get_method(vm, identifier));
    interpreter.set(dst(), method ?: js_undefined());
    return {};
}

ThrowCompletionOr<void> GetObjectPropertyIterator::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.set(dst(), TRY(get_object_property_iterator(interpreter.vm(), interpreter.get(object()))));
    return {};
}

ThrowCompletionOr<void> IteratorClose::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto& iterator = verify_cast<IteratorRecord>(interpreter.get(m_iterator_record).as_object());

    // FIXME: Return the value of the resulting completion. (Note that m_completion_value can be empty!)
    TRY(iterator_close(vm, iterator, Completion { m_completion_type, m_completion_value }));
    return {};
}

ThrowCompletionOr<void> AsyncIteratorClose::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto& iterator = verify_cast<IteratorRecord>(interpreter.get(m_iterator_record).as_object());

    // FIXME: Return the value of the resulting completion. (Note that m_completion_value can be empty!)
    TRY(async_iterator_close(vm, iterator, Completion { m_completion_type, m_completion_value }));
    return {};
}

ThrowCompletionOr<void> IteratorNext::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto& iterator_record = verify_cast<IteratorRecord>(interpreter.get(m_iterator_record).as_object());
    interpreter.set(dst(), TRY(iterator_next(vm, iterator_record)));
    return {};
}

ThrowCompletionOr<void> NewClass::execute_impl(Bytecode::Interpreter& interpreter) const
{
    Value super_class;
    if (m_super_class.has_value())
        super_class = interpreter.get(m_super_class.value());
    Vector<Value> element_keys;
    for (size_t i = 0; i < m_element_keys_count; ++i) {
        Value element_key;
        if (m_element_keys[i].has_value())
            element_key = interpreter.get(m_element_keys[i].value());
        element_keys.append(element_key);
    }
    interpreter.set(dst(), TRY(new_class(interpreter.vm(), super_class, m_class_expression, m_lhs_name, element_keys)));
    return {};
}

// 13.5.3.1 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-typeof-operator-runtime-semantics-evaluation
ThrowCompletionOr<void> TypeofBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();

    if (m_cache.is_valid()) {
        auto const* environment = interpreter.running_execution_context().lexical_environment.ptr();
        for (size_t i = 0; i < m_cache.hops; ++i)
            environment = environment->outer_environment();
        if (!environment->is_permanently_screwed_by_eval()) {
            auto value = TRY(static_cast<DeclarativeEnvironment const&>(*environment).get_binding_value_direct(vm, m_cache.index));
            interpreter.set(dst(), PrimitiveString::create(vm, value.typeof()));
            return {};
        }
        m_cache = {};
    }

    // 1. Let val be the result of evaluating UnaryExpression.
    auto reference = TRY(vm.resolve_binding(interpreter.current_executable().get_identifier(m_identifier)));

    // 2. If val is a Reference Record, then
    //    a. If IsUnresolvableReference(val) is true, return "undefined".
    if (reference.is_unresolvable()) {
        interpreter.set(dst(), PrimitiveString::create(vm, "undefined"_string));
        return {};
    }

    // 3. Set val to ? GetValue(val).
    auto value = TRY(reference.get_value(vm));

    if (reference.environment_coordinate().has_value())
        m_cache = reference.environment_coordinate().value();

    // 4. NOTE: This step is replaced in section B.3.6.3.
    // 5. Return a String according to Table 41.
    interpreter.set(dst(), PrimitiveString::create(vm, value.typeof()));
    return {};
}

void BlockDeclarationInstantiation::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto old_environment = interpreter.running_execution_context().lexical_environment;
    auto& running_execution_context = interpreter.running_execution_context();
    running_execution_context.saved_lexical_environments.append(old_environment);
    running_execution_context.lexical_environment = new_declarative_environment(*old_environment);
    m_scope_node.block_declaration_instantiation(vm, running_execution_context.lexical_environment);
}

ByteString Mov::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("Mov {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("src"sv, m_src, executable));
}

ByteString NewArray::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("NewArray {}", format_operand("dst"sv, dst(), executable));
    if (m_element_count != 0) {
        builder.appendff(", {}", format_operand_list("args"sv, { m_elements, m_element_count }, executable));
    }
    return builder.to_byte_string();
}

ByteString NewPrimitiveArray::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("NewPrimitiveArray {}, {}"sv,
        format_operand("dst"sv, dst(), executable),
        format_value_list("elements"sv, elements()));
}

ByteString AddPrivateName::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("AddPrivateName {}"sv, executable.identifier_table->get(m_name));
}

ByteString ArrayAppend::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("Append {}, {}{}",
        format_operand("dst"sv, dst(), executable),
        format_operand("src"sv, src(), executable),
        m_is_spread ? " **"sv : ""sv);
}

ByteString IteratorToArray::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("IteratorToArray {}, {}",
        format_operand("dst"sv, dst(), executable),
        format_operand("iterator"sv, iterator(), executable));
}

ByteString NewObject::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("NewObject {}", format_operand("dst"sv, dst(), executable));
}

ByteString NewRegExp::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("NewRegExp {}, source:{} (\"{}\") flags:{} (\"{}\")",
        format_operand("dst"sv, dst(), executable),
        m_source_index, executable.get_string(m_source_index), m_flags_index, executable.get_string(m_flags_index));
}

ByteString CopyObjectExcludingProperties::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("CopyObjectExcludingProperties {}, {}",
        format_operand("dst"sv, dst(), executable),
        format_operand("from"sv, m_from_object, executable));
    if (m_excluded_names_count != 0) {
        builder.append(" excluding:["sv);
        for (size_t i = 0; i < m_excluded_names_count; ++i) {
            if (i != 0)
                builder.append(", "sv);
            builder.append(format_operand("#"sv, m_excluded_names[i], executable));
        }
        builder.append(']');
    }
    return builder.to_byte_string();
}

ByteString ConcatString::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("ConcatString {}, {}",
        format_operand("dst"sv, dst(), executable),
        format_operand("src"sv, src(), executable));
}

ByteString GetCalleeAndThisFromEnvironment::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetCalleeAndThisFromEnvironment {}, {} <- {}",
        format_operand("callee"sv, m_callee, executable),
        format_operand("this"sv, m_this_value, executable),
        executable.identifier_table->get(m_identifier));
}

ByteString GetBinding::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetBinding {}, {}",
        format_operand("dst"sv, dst(), executable),
        executable.identifier_table->get(m_identifier));
}

ByteString GetGlobal::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetGlobal {}, {}", format_operand("dst"sv, dst(), executable),
        executable.identifier_table->get(m_identifier));
}

ByteString DeleteVariable::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("DeleteVariable {}", executable.identifier_table->get(m_identifier));
}

ByteString CreateLexicalEnvironment::to_byte_string_impl(Bytecode::Executable const&) const
{
    return "CreateLexicalEnvironment"sv;
}

ByteString CreatePrivateEnvironment::to_byte_string_impl(Bytecode::Executable const&) const
{
    return "CreatePrivateEnvironment"sv;
}

ByteString CreateVariableEnvironment::to_byte_string_impl(Bytecode::Executable const&) const
{
    return "CreateVariableEnvironment"sv;
}

ByteString CreateVariable::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    auto mode_string = m_mode == EnvironmentMode::Lexical ? "Lexical" : "Variable";
    return ByteString::formatted("CreateVariable env:{} immutable:{} global:{} {}", mode_string, m_is_immutable, m_is_global, executable.identifier_table->get(m_identifier));
}

ByteString CreateRestParams::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("CreateRestParams {}, rest_index:{}", format_operand("dst"sv, m_dst, executable), m_rest_index);
}

ByteString CreateArguments::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("CreateArguments");
    if (m_dst.has_value())
        builder.appendff(" {}", format_operand("dst"sv, *m_dst, executable));
    builder.appendff(" {} immutable:{}", m_kind == Kind::Mapped ? "mapped"sv : "unmapped"sv, m_is_immutable);
    return builder.to_byte_string();
}

ByteString EnterObjectEnvironment::to_byte_string_impl(Executable const& executable) const
{
    return ByteString::formatted("EnterObjectEnvironment {}",
        format_operand("object"sv, m_object, executable));
}

ByteString InitializeLexicalBinding::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("InitializeLexicalBinding {}, {}",
        executable.identifier_table->get(m_identifier),
        format_operand("src"sv, src(), executable));
}

ByteString InitializeVariableBinding::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("InitializeVariableBinding {}, {}",
        executable.identifier_table->get(m_identifier),
        format_operand("src"sv, src(), executable));
}

ByteString SetLexicalBinding::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("SetLexicalBinding {}, {}",
        executable.identifier_table->get(m_identifier),
        format_operand("src"sv, src(), executable));
}

ByteString SetVariableBinding::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("SetVariableBinding {}, {}",
        executable.identifier_table->get(m_identifier),
        format_operand("src"sv, src(), executable));
}

ByteString GetArgument::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetArgument {}, {}", index(), format_operand("dst"sv, dst(), executable));
}

ByteString SetArgument::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("SetArgument {}, {}", index(), format_operand("src"sv, src(), executable));
}

static StringView property_kind_to_string(PropertyKind kind)
{
    switch (kind) {
    case PropertyKind::Getter:
        return "getter"sv;
    case PropertyKind::Setter:
        return "setter"sv;
    case PropertyKind::KeyValue:
        return "key-value"sv;
    case PropertyKind::DirectKeyValue:
        return "direct-key-value"sv;
    case PropertyKind::Spread:
        return "spread"sv;
    case PropertyKind::ProtoSetter:
        return "proto-setter"sv;
    }
    VERIFY_NOT_REACHED();
}

ByteString PutById::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    auto kind = property_kind_to_string(m_kind);
    return ByteString::formatted("PutById {}, {}, {}, kind:{}",
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property),
        format_operand("src"sv, m_src, executable),
        kind);
}

ByteString PutByIdWithThis::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    auto kind = property_kind_to_string(m_kind);
    return ByteString::formatted("PutByIdWithThis {}, {}, {}, {}, kind:{}",
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property),
        format_operand("src"sv, m_src, executable),
        format_operand("this"sv, m_this_value, executable),
        kind);
}

ByteString PutPrivateById::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    auto kind = property_kind_to_string(m_kind);
    return ByteString::formatted(
        "PutPrivateById {}, {}, {}, kind:{} ",
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property),
        format_operand("src"sv, m_src, executable),
        kind);
}

ByteString GetById::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetById {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property));
}

ByteString GetByIdWithThis::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetByIdWithThis {}, {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property),
        format_operand("this"sv, m_this_value, executable));
}

ByteString GetLength::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetLength {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable));
}

ByteString GetLengthWithThis::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetLengthWithThis {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable),
        format_operand("this"sv, m_this_value, executable));
}

ByteString GetPrivateById::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetPrivateById {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property));
}

ByteString HasPrivateId::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("HasPrivateId {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property));
}

ByteString DeleteById::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("DeleteById {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property));
}

ByteString DeleteByIdWithThis::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("DeleteByIdWithThis {}, {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property),
        format_operand("this"sv, m_this_value, executable));
}

ByteString Jump::to_byte_string_impl(Bytecode::Executable const&) const
{
    return ByteString::formatted("Jump {}", m_target);
}

ByteString JumpIf::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("JumpIf {}, \033[32mtrue\033[0m:{} \033[32mfalse\033[0m:{}",
        format_operand("condition"sv, m_condition, executable),
        m_true_target,
        m_false_target);
}

ByteString JumpTrue::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("JumpTrue {}, {}",
        format_operand("condition"sv, m_condition, executable),
        m_target);
}

ByteString JumpFalse::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("JumpFalse {}, {}",
        format_operand("condition"sv, m_condition, executable),
        m_target);
}

ByteString JumpNullish::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("JumpNullish {}, null:{} nonnull:{}",
        format_operand("condition"sv, m_condition, executable),
        m_true_target,
        m_false_target);
}

#define HANDLE_COMPARISON_OP(op_TitleCase, op_snake_case, numeric_operator)                          \
    ByteString Jump##op_TitleCase::to_byte_string_impl(Bytecode::Executable const& executable) const \
    {                                                                                                \
        return ByteString::formatted("Jump" #op_TitleCase " {}, {}, true:{}, false:{}",              \
            format_operand("lhs"sv, m_lhs, executable),                                              \
            format_operand("rhs"sv, m_rhs, executable),                                              \
            m_true_target,                                                                           \
            m_false_target);                                                                         \
    }

JS_ENUMERATE_COMPARISON_OPS(HANDLE_COMPARISON_OP)

ByteString JumpUndefined::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("JumpUndefined {}, undefined:{} defined:{}",
        format_operand("condition"sv, m_condition, executable),
        m_true_target,
        m_false_target);
}

static StringView call_type_to_string(CallType type)
{
    switch (type) {
    case CallType::Call:
        return ""sv;
    case CallType::Construct:
        return " (Construct)"sv;
    case CallType::DirectEval:
        return " (DirectEval)"sv;
    }
    VERIFY_NOT_REACHED();
}

ByteString Call::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    auto type = call_type_to_string(m_type);

    StringBuilder builder;
    builder.appendff("Call{} {}, {}, {}, "sv,
        type,
        format_operand("dst"sv, m_dst, executable),
        format_operand("callee"sv, m_callee, executable),
        format_operand("this"sv, m_this_value, executable));

    builder.append(format_operand_list("args"sv, { m_arguments, m_argument_count }, executable));

    if (m_builtin.has_value()) {
        builder.appendff(", (builtin:{})", m_builtin.value());
    }

    if (m_expression_string.has_value()) {
        builder.appendff(", `{}`", executable.get_string(m_expression_string.value()));
    }

    return builder.to_byte_string();
}

ByteString CallWithArgumentArray::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    auto type = call_type_to_string(m_type);
    StringBuilder builder;
    builder.appendff("CallWithArgumentArray{} {}, {}, {}, {}",
        type,
        format_operand("dst"sv, m_dst, executable),
        format_operand("callee"sv, m_callee, executable),
        format_operand("this"sv, m_this_value, executable),
        format_operand("arguments"sv, m_arguments, executable));

    if (m_expression_string.has_value())
        builder.appendff(" ({})", executable.get_string(m_expression_string.value()));
    return builder.to_byte_string();
}

ByteString SuperCallWithArgumentArray::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("SuperCallWithArgumentArray {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("arguments"sv, m_arguments, executable));
}

ByteString NewFunction::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("NewFunction {}",
        format_operand("dst"sv, m_dst, executable));
    if (m_function_node.has_name())
        builder.appendff(" name:{}"sv, m_function_node.name());
    if (m_lhs_name.has_value())
        builder.appendff(" lhs_name:{}"sv, executable.get_identifier(m_lhs_name.value()));
    if (m_home_object.has_value())
        builder.appendff(", {}"sv, format_operand("home_object"sv, m_home_object.value(), executable));
    return builder.to_byte_string();
}

ByteString NewClass::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    auto name = m_class_expression.name();
    builder.appendff("NewClass {}",
        format_operand("dst"sv, m_dst, executable));
    if (m_super_class.has_value())
        builder.appendff(", {}", format_operand("super_class"sv, *m_super_class, executable));
    if (!name.is_empty())
        builder.appendff(", {}", name);
    if (m_lhs_name.has_value())
        builder.appendff(", lhs_name:{}"sv, executable.get_identifier(m_lhs_name.value()));
    return builder.to_byte_string();
}

ByteString Return::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    if (m_value.has_value())
        return ByteString::formatted("Return {}", format_operand("value"sv, m_value.value(), executable));
    return "Return";
}

ByteString Increment::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("Increment {}", format_operand("dst"sv, m_dst, executable));
}

ByteString PostfixIncrement::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("PostfixIncrement {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("src"sv, m_src, executable));
}

ByteString Decrement::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("Decrement {}", format_operand("dst"sv, m_dst, executable));
}

ByteString PostfixDecrement::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("PostfixDecrement {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("src"sv, m_src, executable));
}

ByteString Throw::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("Throw {}",
        format_operand("src"sv, m_src, executable));
}

ByteString ThrowIfNotObject::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("ThrowIfNotObject {}",
        format_operand("src"sv, m_src, executable));
}

ByteString ThrowIfNullish::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("ThrowIfNullish {}",
        format_operand("src"sv, m_src, executable));
}

ByteString ThrowIfTDZ::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("ThrowIfTDZ {}",
        format_operand("src"sv, m_src, executable));
}

ByteString EnterUnwindContext::to_byte_string_impl(Bytecode::Executable const&) const
{
    return ByteString::formatted("EnterUnwindContext entry:{}", m_entry_point);
}

ByteString ScheduleJump::to_byte_string_impl(Bytecode::Executable const&) const
{
    return ByteString::formatted("ScheduleJump {}", m_target);
}

ByteString LeaveLexicalEnvironment::to_byte_string_impl(Bytecode::Executable const&) const
{
    return "LeaveLexicalEnvironment"sv;
}

ByteString LeavePrivateEnvironment::to_byte_string_impl(Bytecode::Executable const&) const
{
    return "LeavePrivateEnvironment"sv;
}

ByteString LeaveUnwindContext::to_byte_string_impl(Bytecode::Executable const&) const
{
    return "LeaveUnwindContext";
}

ByteString ContinuePendingUnwind::to_byte_string_impl(Bytecode::Executable const&) const
{
    return ByteString::formatted("ContinuePendingUnwind resume:{}", m_resume_target);
}

ByteString Yield::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    if (m_continuation_label.has_value()) {
        return ByteString::formatted("Yield continuation:{}, {}",
            m_continuation_label.value(),
            format_operand("value"sv, m_value, executable));
    }
    return ByteString::formatted("Yield return {}",
        format_operand("value"sv, m_value, executable));
}

ByteString PrepareYield::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("PrepareYield {}, {}",
        format_operand("dst"sv, m_dest, executable),
        format_operand("value"sv, m_value, executable));
}

ByteString Await::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("Await {}, continuation:{}",
        format_operand("argument"sv, m_argument, executable),
        m_continuation_label);
}

ByteString GetByValue::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetByValue {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable),
        format_operand("property"sv, m_property, executable));
}

ByteString GetByValueWithThis::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetByValueWithThis {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("base"sv, m_base, executable),
        format_operand("property"sv, m_property, executable));
}

ByteString PutByValue::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    auto kind = property_kind_to_string(m_kind);
    return ByteString::formatted("PutByValue {}, {}, {}, kind:{}",
        format_operand("base"sv, m_base, executable),
        format_operand("property"sv, m_property, executable),
        format_operand("src"sv, m_src, executable),
        kind);
}

ByteString PutByValueWithThis::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    auto kind = property_kind_to_string(m_kind);
    return ByteString::formatted("PutByValueWithThis {}, {}, {}, {}, kind:{}",
        format_operand("base"sv, m_base, executable),
        format_operand("property"sv, m_property, executable),
        format_operand("src"sv, m_src, executable),
        format_operand("this"sv, m_this_value, executable),
        kind);
}

ByteString DeleteByValue::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("DeleteByValue {}, {}, {}",
        format_operand("dst"sv, dst(), executable),
        format_operand("base"sv, m_base, executable),
        format_operand("property"sv, m_property, executable));
}

ByteString DeleteByValueWithThis::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("DeleteByValueWithThis {}, {}, {}, {}",
        format_operand("dst"sv, dst(), executable),
        format_operand("base"sv, m_base, executable),
        format_operand("property"sv, m_property, executable),
        format_operand("this"sv, m_this_value, executable));
}

ByteString GetIterator::to_byte_string_impl(Executable const& executable) const
{
    auto hint = m_hint == IteratorHint::Sync ? "sync" : "async";
    return ByteString::formatted("GetIterator {}, {}, hint:{}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("iterable"sv, m_iterable, executable),
        hint);
}

ByteString GetMethod::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetMethod {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("object"sv, m_object, executable),
        executable.identifier_table->get(m_property));
}

ByteString GetObjectPropertyIterator::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetObjectPropertyIterator {}, {}",
        format_operand("dst"sv, dst(), executable),
        format_operand("object"sv, object(), executable));
}

ByteString IteratorClose::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    if (!m_completion_value.has_value())
        return ByteString::formatted("IteratorClose {}, completion_type={} completion_value=<empty>",
            format_operand("iterator_record"sv, m_iterator_record, executable),
            to_underlying(m_completion_type));

    auto completion_value_string = m_completion_value->to_string_without_side_effects();
    return ByteString::formatted("IteratorClose {}, completion_type={} completion_value={}",
        format_operand("iterator_record"sv, m_iterator_record, executable),
        to_underlying(m_completion_type), completion_value_string);
}

ByteString AsyncIteratorClose::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    if (!m_completion_value.has_value()) {
        return ByteString::formatted("AsyncIteratorClose {}, completion_type:{} completion_value:<empty>",
            format_operand("iterator_record"sv, m_iterator_record, executable),
            to_underlying(m_completion_type));
    }

    return ByteString::formatted("AsyncIteratorClose {}, completion_type:{}, completion_value:{}",
        format_operand("iterator_record"sv, m_iterator_record, executable),
        to_underlying(m_completion_type), m_completion_value);
}

ByteString IteratorNext::to_byte_string_impl(Executable const& executable) const
{
    return ByteString::formatted("IteratorNext {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("iterator_record"sv, m_iterator_record, executable));
}

ByteString ResolveThisBinding::to_byte_string_impl(Bytecode::Executable const&) const
{
    return "ResolveThisBinding"sv;
}

ByteString ResolveSuperBase::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("ResolveSuperBase {}",
        format_operand("dst"sv, m_dst, executable));
}

ByteString GetNewTarget::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetNewTarget {}", format_operand("dst"sv, m_dst, executable));
}

ByteString GetImportMeta::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetImportMeta {}", format_operand("dst"sv, m_dst, executable));
}

ByteString TypeofBinding::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("TypeofBinding {}, {}",
        format_operand("dst"sv, m_dst, executable),
        executable.identifier_table->get(m_identifier));
}

ByteString BlockDeclarationInstantiation::to_byte_string_impl(Bytecode::Executable const&) const
{
    return "BlockDeclarationInstantiation"sv;
}

ByteString ImportCall::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("ImportCall {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("specifier"sv, m_specifier, executable),
        format_operand("options"sv, m_options, executable));
}

ByteString Catch::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("Catch {}",
        format_operand("dst"sv, m_dst, executable));
}

ByteString LeaveFinally::to_byte_string_impl(Bytecode::Executable const&) const
{
    return ByteString::formatted("LeaveFinally");
}

ByteString RestoreScheduledJump::to_byte_string_impl(Bytecode::Executable const&) const
{
    return ByteString::formatted("RestoreScheduledJump");
}

ByteString GetObjectFromIteratorRecord::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetObjectFromIteratorRecord {}, {}",
        format_operand("object"sv, m_object, executable),
        format_operand("iterator_record"sv, m_iterator_record, executable));
}

ByteString GetNextMethodFromIteratorRecord::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetNextMethodFromIteratorRecord {}, {}",
        format_operand("next_method"sv, m_next_method, executable),
        format_operand("iterator_record"sv, m_iterator_record, executable));
}

ByteString End::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("End {}", format_operand("value"sv, m_value, executable));
}

ByteString Dump::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("Dump '{}', {}", m_text,
        format_operand("value"sv, m_value, executable));
}

}
