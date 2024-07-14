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
#include <LibJS/Runtime/TypedArray.h>
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

// NOTE: This function assumes that the index is valid within the TypedArray,
//       and that the TypedArray is not detached.
template<typename T>
inline Value fast_typed_array_get_element(TypedArrayBase& typed_array, u32 index)
{
    Checked<u32> offset_into_array_buffer = index;
    offset_into_array_buffer *= sizeof(T);
    offset_into_array_buffer += typed_array.byte_offset();

    if (offset_into_array_buffer.has_overflow()) [[unlikely]] {
        return js_undefined();
    }

    auto const& array_buffer = *typed_array.viewed_array_buffer();
    auto const* slot = reinterpret_cast<T const*>(array_buffer.buffer().offset_pointer(offset_into_array_buffer.value()));
    return Value { *slot };
}

// NOTE: This function assumes that the index is valid within the TypedArray,
//       and that the TypedArray is not detached.
template<typename T>
inline void fast_typed_array_set_element(TypedArrayBase& typed_array, u32 index, T value)
{
    Checked<u32> offset_into_array_buffer = index;
    offset_into_array_buffer *= sizeof(T);
    offset_into_array_buffer += typed_array.byte_offset();

    if (offset_into_array_buffer.has_overflow()) [[unlikely]] {
        return;
    }

    auto& array_buffer = *typed_array.viewed_array_buffer();
    auto* slot = reinterpret_cast<T*>(array_buffer.buffer().offset_pointer(offset_into_array_buffer.value()));
    *slot = value;
}

static Completion throw_null_or_undefined_property_get(VM& vm, Value base_value, Optional<IdentifierTableIndex> base_identifier, IdentifierTableIndex property_identifier, Executable const& executable)
{
    VERIFY(base_value.is_nullish());

    if (base_identifier.has_value())
        return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithPropertyAndName, executable.get_identifier(property_identifier), base_value, executable.get_identifier(base_identifier.value()));
    return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithProperty, executable.get_identifier(property_identifier), base_value);
}

static Completion throw_null_or_undefined_property_get(VM& vm, Value base_value, Optional<IdentifierTableIndex> base_identifier, Value property, Executable const& executable)
{
    VERIFY(base_value.is_nullish());

    if (base_identifier.has_value())
        return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithPropertyAndName, property, base_value, executable.get_identifier(base_identifier.value()));
    return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithProperty, property, base_value);
}

template<typename BaseType, typename PropertyType>
ALWAYS_INLINE Completion throw_null_or_undefined_property_access(VM& vm, Value base_value, BaseType const& base_identifier, PropertyType const& property_identifier)
{
    VERIFY(base_value.is_nullish());

    bool has_base_identifier = true;
    bool has_property_identifier = true;

    if constexpr (requires { base_identifier.has_value(); })
        has_base_identifier = base_identifier.has_value();
    if constexpr (requires { property_identifier.has_value(); })
        has_property_identifier = property_identifier.has_value();

    if (has_base_identifier && has_property_identifier)
        return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithPropertyAndName, property_identifier, base_value, base_identifier);
    if (has_property_identifier)
        return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithProperty, property_identifier, base_value);
    if (has_base_identifier)
        return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithName, base_identifier, base_value);
    return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefined);
}

ALWAYS_INLINE GCPtr<Object> base_object_for_get_impl(VM& vm, Value base_value)
{
    if (base_value.is_object()) [[likely]]
        return base_value.as_object();

    // OPTIMIZATION: For various primitives we can avoid actually creating a new object for them.
    auto& realm = *vm.current_realm();
    if (base_value.is_string())
        return realm.intrinsics().string_prototype();
    if (base_value.is_number())
        return realm.intrinsics().number_prototype();
    if (base_value.is_boolean())
        return realm.intrinsics().boolean_prototype();
    if (base_value.is_bigint())
        return realm.intrinsics().bigint_prototype();
    if (base_value.is_symbol())
        return realm.intrinsics().symbol_prototype();

    return nullptr;
}

ALWAYS_INLINE ThrowCompletionOr<NonnullGCPtr<Object>> base_object_for_get(VM& vm, Value base_value, Optional<IdentifierTableIndex> base_identifier, IdentifierTableIndex property_identifier, Executable const& executable)
{
    if (auto base_object = base_object_for_get_impl(vm, base_value))
        return NonnullGCPtr { *base_object };

    // NOTE: At this point this is guaranteed to throw (null or undefined).
    return throw_null_or_undefined_property_get(vm, base_value, base_identifier, property_identifier, executable);
}

ALWAYS_INLINE ThrowCompletionOr<NonnullGCPtr<Object>> base_object_for_get(VM& vm, Value base_value, Optional<IdentifierTableIndex> base_identifier, Value property, Executable const& executable)
{
    if (auto base_object = base_object_for_get_impl(vm, base_value))
        return NonnullGCPtr { *base_object };

    // NOTE: At this point this is guaranteed to throw (null or undefined).
    return throw_null_or_undefined_property_get(vm, base_value, base_identifier, property, executable);
}

enum class GetByIdMode {
    Normal,
    Length,
};

template<GetByIdMode mode = GetByIdMode::Normal>
inline ThrowCompletionOr<Value> get_by_id(VM& vm, Optional<IdentifierTableIndex> base_identifier, IdentifierTableIndex property, Value base_value, Value this_value, PropertyLookupCache& cache, Executable const& executable)
{
    if constexpr (mode == GetByIdMode::Length) {
        if (base_value.is_string()) {
            return Value(base_value.as_string().utf16_string().length_in_code_units());
        }
    }

    auto base_obj = TRY(base_object_for_get(vm, base_value, base_identifier, property, executable));

    if constexpr (mode == GetByIdMode::Length) {
        // OPTIMIZATION: Fast path for the magical "length" property on Array objects.
        if (base_obj->has_magical_length_property()) {
            return Value { base_obj->indexed_properties().array_like_size() };
        }
    }

    auto& shape = base_obj->shape();

    if (cache.prototype) {
        // OPTIMIZATION: If the prototype chain hasn't been mutated in a way that would invalidate the cache, we can use it.
        bool can_use_cache = [&]() -> bool {
            if (&shape != cache.shape)
                return false;
            if (!cache.prototype_chain_validity)
                return false;
            if (!cache.prototype_chain_validity->is_valid())
                return false;
            return true;
        }();
        if (can_use_cache)
            return cache.prototype->get_direct(cache.property_offset.value());
    } else if (&shape == cache.shape) {
        // OPTIMIZATION: If the shape of the object hasn't changed, we can use the cached property offset.
        return base_obj->get_direct(cache.property_offset.value());
    }

    CacheablePropertyMetadata cacheable_metadata;
    auto value = TRY(base_obj->internal_get(executable.get_identifier(property), this_value, &cacheable_metadata));

    if (cacheable_metadata.type == CacheablePropertyMetadata::Type::OwnProperty) {
        cache = {};
        cache.shape = shape;
        cache.property_offset = cacheable_metadata.property_offset.value();
    } else if (cacheable_metadata.type == CacheablePropertyMetadata::Type::InPrototypeChain) {
        cache = {};
        cache.shape = &base_obj->shape();
        cache.property_offset = cacheable_metadata.property_offset.value();
        cache.prototype = *cacheable_metadata.prototype;
        cache.prototype_chain_validity = *cacheable_metadata.prototype->shape().prototype_chain_validity();
    }

    return value;
}

inline ThrowCompletionOr<Value> get_by_value(VM& vm, Optional<IdentifierTableIndex> base_identifier, Value base_value, Value property_key_value, Executable const& executable)
{
    // OPTIMIZATION: Fast path for simple Int32 indexes in array-like objects.
    if (base_value.is_object() && property_key_value.is_int32() && property_key_value.as_i32() >= 0) {
        auto& object = base_value.as_object();
        auto index = static_cast<u32>(property_key_value.as_i32());

        auto const* object_storage = object.indexed_properties().storage();

        // For "non-typed arrays":
        if (!object.may_interfere_with_indexed_property_access()
            && object_storage) {
            auto maybe_value = [&] {
                if (object_storage->is_simple_storage())
                    return static_cast<SimpleIndexedPropertyStorage const*>(object_storage)->inline_get(index);
                else
                    return static_cast<GenericIndexedPropertyStorage const*>(object_storage)->get(index);
            }();
            if (maybe_value.has_value()) {
                auto value = maybe_value->value;
                if (!value.is_accessor())
                    return value;
            }
        }

        // For typed arrays:
        if (object.is_typed_array()) {
            auto& typed_array = static_cast<TypedArrayBase&>(object);
            auto canonical_index = CanonicalIndex { CanonicalIndex::Type::Index, index };

            if (is_valid_integer_index(typed_array, canonical_index)) {
                switch (typed_array.kind()) {
                case TypedArrayBase::Kind::Uint8Array:
                    return fast_typed_array_get_element<u8>(typed_array, index);
                case TypedArrayBase::Kind::Uint16Array:
                    return fast_typed_array_get_element<u16>(typed_array, index);
                case TypedArrayBase::Kind::Uint32Array:
                    return fast_typed_array_get_element<u32>(typed_array, index);
                case TypedArrayBase::Kind::Int8Array:
                    return fast_typed_array_get_element<i8>(typed_array, index);
                case TypedArrayBase::Kind::Int16Array:
                    return fast_typed_array_get_element<i16>(typed_array, index);
                case TypedArrayBase::Kind::Int32Array:
                    return fast_typed_array_get_element<i32>(typed_array, index);
                case TypedArrayBase::Kind::Uint8ClampedArray:
                    return fast_typed_array_get_element<u8>(typed_array, index);
                default:
                    // FIXME: Support more TypedArray kinds.
                    break;
                }
            }

            switch (typed_array.kind()) {
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    case TypedArrayBase::Kind::ClassName:                                           \
        return typed_array_get_element<Type>(typed_array, canonical_index);
                JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE
            }
        }
    }

    auto object = TRY(base_object_for_get(vm, base_value, base_identifier, property_key_value, executable));

    auto property_key = TRY(property_key_value.to_property_key(vm));

    if (base_value.is_string()) {
        auto string_value = TRY(base_value.as_string().get(vm, property_key));
        if (string_value.has_value())
            return *string_value;
    }

    return TRY(object->internal_get(property_key, base_value));
}

inline ThrowCompletionOr<Value> get_global(Interpreter& interpreter, IdentifierTableIndex identifier_index, GlobalVariableCache& cache)
{
    auto& vm = interpreter.vm();
    auto& binding_object = interpreter.global_object();
    auto& declarative_record = interpreter.global_declarative_environment();

    auto& shape = binding_object.shape();
    if (cache.environment_serial_number == declarative_record.environment_serial_number()) {

        // OPTIMIZATION: For global var bindings, if the shape of the global object hasn't changed,
        //               we can use the cached property offset.
        if (&shape == cache.shape) {
            return binding_object.get_direct(cache.property_offset.value());
        }

        // OPTIMIZATION: For global lexical bindings, if the global declarative environment hasn't changed,
        //               we can use the cached environment binding index.
        if (cache.environment_binding_index.has_value())
            return declarative_record.get_binding_value_direct(vm, cache.environment_binding_index.value());
    }

    cache.environment_serial_number = declarative_record.environment_serial_number();

    auto& identifier = interpreter.current_executable().get_identifier(identifier_index);

    if (vm.running_execution_context().script_or_module.has<NonnullGCPtr<Module>>()) {
        // NOTE: GetGlobal is used to access variables stored in the module environment and global environment.
        //       The module environment is checked first since it precedes the global environment in the environment chain.
        auto& module_environment = *vm.running_execution_context().script_or_module.get<NonnullGCPtr<Module>>()->environment();
        if (TRY(module_environment.has_binding(identifier))) {
            // TODO: Cache offset of binding value
            return TRY(module_environment.get_binding_value(vm, identifier, vm.in_strict_mode()));
        }
    }

    Optional<size_t> offset;
    if (TRY(declarative_record.has_binding(identifier, &offset))) {
        cache.environment_binding_index = static_cast<u32>(offset.value());
        return TRY(declarative_record.get_binding_value(vm, identifier, vm.in_strict_mode()));
    }

    if (TRY(binding_object.has_property(identifier))) {
        CacheablePropertyMetadata cacheable_metadata;
        auto value = TRY(binding_object.internal_get(identifier, js_undefined(), &cacheable_metadata));
        if (cacheable_metadata.type == CacheablePropertyMetadata::Type::OwnProperty) {
            cache.shape = shape;
            cache.property_offset = cacheable_metadata.property_offset.value();
        }
        return value;
    }

    return vm.throw_completion<ReferenceError>(ErrorType::UnknownIdentifier, identifier);
}

inline ThrowCompletionOr<void> put_by_property_key(VM& vm, Value base, Value this_value, Value value, Optional<DeprecatedFlyString const&> const& base_identifier, PropertyKey name, Op::PropertyKind kind, PropertyLookupCache* cache = nullptr)
{
    // Better error message than to_object would give
    if (vm.in_strict_mode() && base.is_nullish())
        return vm.throw_completion<TypeError>(ErrorType::ReferenceNullishSetProperty, name, base.to_string_without_side_effects());

    // a. Let baseObj be ? ToObject(V.[[Base]]).
    auto maybe_object = base.to_object(vm);
    if (maybe_object.is_error())
        return throw_null_or_undefined_property_access(vm, base, base_identifier, name);
    auto object = maybe_object.release_value();

    if (kind == Op::PropertyKind::Getter || kind == Op::PropertyKind::Setter) {
        // The generator should only pass us functions for getters and setters.
        VERIFY(value.is_function());
    }
    switch (kind) {
    case Op::PropertyKind::Getter: {
        auto& function = value.as_function();
        if (function.name().is_empty() && is<ECMAScriptFunctionObject>(function))
            static_cast<ECMAScriptFunctionObject*>(&function)->set_name(ByteString::formatted("get {}", name));
        object->define_direct_accessor(name, &function, nullptr, Attribute::Configurable | Attribute::Enumerable);
        break;
    }
    case Op::PropertyKind::Setter: {
        auto& function = value.as_function();
        if (function.name().is_empty() && is<ECMAScriptFunctionObject>(function))
            static_cast<ECMAScriptFunctionObject*>(&function)->set_name(ByteString::formatted("set {}", name));
        object->define_direct_accessor(name, nullptr, &function, Attribute::Configurable | Attribute::Enumerable);
        break;
    }
    case Op::PropertyKind::KeyValue: {
        if (cache && cache->shape == &object->shape()) {
            object->put_direct(*cache->property_offset, value);
            return {};
        }

        CacheablePropertyMetadata cacheable_metadata;
        bool succeeded = TRY(object->internal_set(name, value, this_value, &cacheable_metadata));

        if (succeeded && cache && cacheable_metadata.type == CacheablePropertyMetadata::Type::OwnProperty) {
            cache->shape = object->shape();
            cache->property_offset = cacheable_metadata.property_offset.value();
        }

        if (!succeeded && vm.in_strict_mode()) {
            if (base.is_object())
                return vm.throw_completion<TypeError>(ErrorType::ReferenceNullishSetProperty, name, base.to_string_without_side_effects());
            return vm.throw_completion<TypeError>(ErrorType::ReferencePrimitiveSetProperty, name, base.typeof(), base.to_string_without_side_effects());
        }
        break;
    }
    case Op::PropertyKind::DirectKeyValue:
        object->define_direct_property(name, value, Attribute::Enumerable | Attribute::Writable | Attribute::Configurable);
        break;
    case Op::PropertyKind::Spread:
        TRY(object->copy_data_properties(vm, value, {}));
        break;
    case Op::PropertyKind::ProtoSetter:
        if (value.is_object() || value.is_null())
            MUST(object->internal_set_prototype_of(value.is_object() ? &value.as_object() : nullptr));
        break;
    }

    return {};
}

inline ThrowCompletionOr<Value> perform_call(Interpreter& interpreter, Value this_value, Op::CallType call_type, Value callee, ReadonlySpan<Value> argument_values)
{
    auto& vm = interpreter.vm();
    auto& function = callee.as_function();
    Value return_value;
    if (call_type == Op::CallType::DirectEval) {
        if (callee == interpreter.realm().intrinsics().eval_function())
            return_value = TRY(perform_eval(vm, !argument_values.is_empty() ? argument_values[0].value_or(JS::js_undefined()) : js_undefined(), vm.in_strict_mode() ? CallerMode::Strict : CallerMode::NonStrict, EvalMode::Direct));
        else
            return_value = TRY(JS::call(vm, function, this_value, argument_values));
    } else if (call_type == Op::CallType::Call)
        return_value = TRY(JS::call(vm, function, this_value, argument_values));
    else
        return_value = TRY(construct(vm, function, argument_values));

    return return_value;
}

static inline Completion throw_type_error_for_callee(Bytecode::Interpreter& interpreter, Value callee, StringView callee_type, Optional<StringTableIndex> const& expression_string)
{
    auto& vm = interpreter.vm();

    if (expression_string.has_value())
        return vm.throw_completion<TypeError>(ErrorType::IsNotAEvaluatedFrom, callee.to_string_without_side_effects(), callee_type, interpreter.current_executable().get_string(expression_string->value()));

    return vm.throw_completion<TypeError>(ErrorType::IsNotA, callee.to_string_without_side_effects(), callee_type);
}

inline ThrowCompletionOr<void> throw_if_needed_for_call(Interpreter& interpreter, Value callee, Op::CallType call_type, Optional<StringTableIndex> const& expression_string)
{
    if ((call_type == Op::CallType::Call || call_type == Op::CallType::DirectEval)
        && !callee.is_function())
        return throw_type_error_for_callee(interpreter, callee, "function"sv, expression_string);
    if (call_type == Op::CallType::Construct && !callee.is_constructor())
        return throw_type_error_for_callee(interpreter, callee, "constructor"sv, expression_string);
    return {};
}

inline Value new_function(VM& vm, FunctionNode const& function_node, Optional<IdentifierTableIndex> const& lhs_name, Optional<Operand> const& home_object)
{
    Value value;

    if (!function_node.has_name()) {
        DeprecatedFlyString name = {};
        if (lhs_name.has_value())
            name = vm.bytecode_interpreter().current_executable().get_identifier(lhs_name.value());
        value = function_node.instantiate_ordinary_function_expression(vm, name);
    } else {
        value = ECMAScriptFunctionObject::create(*vm.current_realm(), function_node.name(), function_node.source_text(), function_node.body(), function_node.parameters(), function_node.function_length(), function_node.local_variables_names(), vm.lexical_environment(), vm.running_execution_context().private_environment, function_node.kind(), function_node.is_strict_mode(),
            function_node.parsing_insights(), function_node.is_arrow_function());
    }

    if (home_object.has_value()) {
        auto home_object_value = vm.bytecode_interpreter().get(home_object.value());
        static_cast<ECMAScriptFunctionObject&>(value.as_function()).set_home_object(&home_object_value.as_object());
    }

    return value;
}

inline ThrowCompletionOr<void> put_by_value(VM& vm, Value base, Optional<DeprecatedFlyString const&> const& base_identifier, Value property_key_value, Value value, Op::PropertyKind kind)
{
    // OPTIMIZATION: Fast path for simple Int32 indexes in array-like objects.
    if ((kind == Op::PropertyKind::KeyValue || kind == Op::PropertyKind::DirectKeyValue)
        && base.is_object() && property_key_value.is_int32() && property_key_value.as_i32() >= 0) {
        auto& object = base.as_object();
        auto* storage = object.indexed_properties().storage();
        auto index = static_cast<u32>(property_key_value.as_i32());

        // For "non-typed arrays":
        if (storage
            && storage->is_simple_storage()
            && !object.may_interfere_with_indexed_property_access()) {
            auto maybe_value = storage->get(index);
            if (maybe_value.has_value()) {
                auto existing_value = maybe_value->value;
                if (!existing_value.is_accessor()) {
                    storage->put(index, value);
                    return {};
                }
            }
        }

        // For typed arrays:
        if (object.is_typed_array()) {
            auto& typed_array = static_cast<TypedArrayBase&>(object);
            auto canonical_index = CanonicalIndex { CanonicalIndex::Type::Index, index };

            if (value.is_int32() && is_valid_integer_index(typed_array, canonical_index)) {
                switch (typed_array.kind()) {
                case TypedArrayBase::Kind::Uint8Array:
                    fast_typed_array_set_element<u8>(typed_array, index, static_cast<u8>(value.as_i32()));
                    return {};
                case TypedArrayBase::Kind::Uint16Array:
                    fast_typed_array_set_element<u16>(typed_array, index, static_cast<u16>(value.as_i32()));
                    return {};
                case TypedArrayBase::Kind::Uint32Array:
                    fast_typed_array_set_element<u32>(typed_array, index, static_cast<u32>(value.as_i32()));
                    return {};
                case TypedArrayBase::Kind::Int8Array:
                    fast_typed_array_set_element<i8>(typed_array, index, static_cast<i8>(value.as_i32()));
                    return {};
                case TypedArrayBase::Kind::Int16Array:
                    fast_typed_array_set_element<i16>(typed_array, index, static_cast<i16>(value.as_i32()));
                    return {};
                case TypedArrayBase::Kind::Int32Array:
                    fast_typed_array_set_element<i32>(typed_array, index, value.as_i32());
                    return {};
                case TypedArrayBase::Kind::Uint8ClampedArray:
                    fast_typed_array_set_element<u8>(typed_array, index, clamp(value.as_i32(), 0, 255));
                    return {};
                default:
                    // FIXME: Support more TypedArray kinds.
                    break;
                }
            }

            if (typed_array.kind() == TypedArrayBase::Kind::Uint32Array && value.is_integral_number()) {
                auto integer = value.as_double();

                if (AK::is_within_range<u32>(integer) && is_valid_integer_index(typed_array, canonical_index)) {
                    fast_typed_array_set_element<u32>(typed_array, index, static_cast<u32>(integer));
                    return {};
                }
            }

            switch (typed_array.kind()) {
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    case TypedArrayBase::Kind::ClassName:                                           \
        return typed_array_set_element<Type>(typed_array, canonical_index, value);
                JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE
            }
            return {};
        }
    }

    auto property_key = kind != Op::PropertyKind::Spread ? TRY(property_key_value.to_property_key(vm)) : PropertyKey {};
    TRY(put_by_property_key(vm, base, base, value, base_identifier, property_key, kind));
    return {};
}

struct CalleeAndThis {
    Value callee;
    Value this_value;
};

inline ThrowCompletionOr<CalleeAndThis> get_callee_and_this_from_environment(Bytecode::Interpreter& interpreter, DeprecatedFlyString const& name, EnvironmentCoordinate& cache)
{
    auto& vm = interpreter.vm();

    Value callee = js_undefined();
    Value this_value = js_undefined();

    if (cache.is_valid()) {
        auto const* environment = interpreter.running_execution_context().lexical_environment.ptr();
        for (size_t i = 0; i < cache.hops; ++i)
            environment = environment->outer_environment();
        if (!environment->is_permanently_screwed_by_eval()) {
            callee = TRY(static_cast<DeclarativeEnvironment const&>(*environment).get_binding_value_direct(vm, cache.index));
            this_value = js_undefined();
            if (auto base_object = environment->with_base_object())
                this_value = base_object;
            return CalleeAndThis {
                .callee = callee,
                .this_value = this_value,
            };
        }
        cache = {};
    }

    auto reference = TRY(vm.resolve_binding(name));
    if (reference.environment_coordinate().has_value())
        cache = reference.environment_coordinate().value();

    callee = TRY(reference.get_value(vm));

    if (reference.is_property_reference()) {
        this_value = reference.get_this_value();
    } else {
        if (reference.is_environment_reference()) {
            if (auto base_object = reference.base_environment().with_base_object(); base_object != nullptr)
                this_value = base_object;
        }
    }

    return CalleeAndThis {
        .callee = callee,
        .this_value = this_value,
    };
}

// 13.2.7.3 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-regular-expression-literals-runtime-semantics-evaluation
inline Value new_regexp(VM& vm, ParsedRegex const& parsed_regex, ByteString const& pattern, ByteString const& flags)
{
    // 1. Let pattern be CodePointsToString(BodyText of RegularExpressionLiteral).
    // 2. Let flags be CodePointsToString(FlagText of RegularExpressionLiteral).

    // 3. Return ! RegExpCreate(pattern, flags).
    auto& realm = *vm.current_realm();
    Regex<ECMA262> regex(parsed_regex.regex, parsed_regex.pattern, parsed_regex.flags);
    // NOTE: We bypass RegExpCreate and subsequently RegExpAlloc as an optimization to use the already parsed values.
    auto regexp_object = RegExpObject::create(realm, move(regex), pattern, flags);
    // RegExpAlloc has these two steps from the 'Legacy RegExp features' proposal.
    regexp_object->set_realm(realm);
    // We don't need to check 'If SameValue(newTarget, thisRealm.[[Intrinsics]].[[%RegExp%]]) is true'
    // here as we know RegExpCreate calls RegExpAlloc with %RegExp% for newTarget.
    regexp_object->set_legacy_features_enabled(true);
    return regexp_object;
}

// 13.3.8.1 https://tc39.es/ecma262/#sec-runtime-semantics-argumentlistevaluation
inline MarkedVector<Value> argument_list_evaluation(VM& vm, Value arguments)
{
    // Note: Any spreading and actual evaluation is handled in preceding opcodes
    // Note: The spec uses the concept of a list, while we create a temporary array
    //       in the preceding opcodes, so we have to convert in a manner that is not
    //       visible to the user
    MarkedVector<Value> argument_values { vm.heap() };

    auto& argument_array = arguments.as_array();
    auto array_length = argument_array.indexed_properties().array_like_size();

    argument_values.ensure_capacity(array_length);

    for (size_t i = 0; i < array_length; ++i) {
        if (auto maybe_value = argument_array.indexed_properties().get(i); maybe_value.has_value())
            argument_values.append(maybe_value.release_value().value);
        else
            argument_values.append(js_undefined());
    }

    return argument_values;
}

inline ThrowCompletionOr<void> create_variable(VM& vm, DeprecatedFlyString const& name, Op::EnvironmentMode mode, bool is_global, bool is_immutable, bool is_strict)
{
    if (mode == Op::EnvironmentMode::Lexical) {
        VERIFY(!is_global);

        // Note: This is papering over an issue where "FunctionDeclarationInstantiation" creates these bindings for us.
        //       Instead of crashing in there, we'll just raise an exception here.
        if (TRY(vm.lexical_environment()->has_binding(name)))
            return vm.throw_completion<InternalError>(TRY_OR_THROW_OOM(vm, String::formatted("Lexical environment already has binding '{}'", name)));

        if (is_immutable)
            return vm.lexical_environment()->create_immutable_binding(vm, name, is_strict);
        return vm.lexical_environment()->create_mutable_binding(vm, name, is_strict);
    }

    if (!is_global) {
        if (is_immutable)
            return vm.variable_environment()->create_immutable_binding(vm, name, is_strict);
        return vm.variable_environment()->create_mutable_binding(vm, name, is_strict);
    }

    // NOTE: CreateVariable with m_is_global set to true is expected to only be used in GlobalDeclarationInstantiation currently, which only uses "false" for "can_be_deleted".
    //       The only area that sets "can_be_deleted" to true is EvalDeclarationInstantiation, which is currently fully implemented in C++ and not in Bytecode.
    return verify_cast<GlobalEnvironment>(vm.variable_environment())->create_global_var_binding(name, false);
}

inline ThrowCompletionOr<ECMAScriptFunctionObject*> new_class(VM& vm, Value super_class, ClassExpression const& class_expression, Optional<IdentifierTableIndex> const& lhs_name, ReadonlySpan<Value> element_keys)
{
    auto& interpreter = vm.bytecode_interpreter();
    auto name = class_expression.name();

    // NOTE: NewClass expects classEnv to be active lexical environment
    auto* class_environment = vm.lexical_environment();
    vm.running_execution_context().lexical_environment = vm.running_execution_context().saved_lexical_environments.take_last();

    Optional<DeprecatedFlyString> binding_name;
    DeprecatedFlyString class_name;
    if (!class_expression.has_name() && lhs_name.has_value()) {
        class_name = interpreter.current_executable().get_identifier(lhs_name.value());
    } else {
        binding_name = name;
        class_name = name.is_null() ? ""sv : name;
    }

    return TRY(class_expression.create_class_constructor(vm, class_environment, vm.lexical_environment(), super_class, element_keys, binding_name, class_name));
}

// 13.3.7.1 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
inline ThrowCompletionOr<NonnullGCPtr<Object>> super_call_with_argument_array(VM& vm, Value argument_array, bool is_synthetic)
{
    // 1. Let newTarget be GetNewTarget().
    auto new_target = vm.get_new_target();

    // 2. Assert: Type(newTarget) is Object.
    VERIFY(new_target.is_object());

    // 3. Let func be GetSuperConstructor().
    auto* func = get_super_constructor(vm);

    // 4. Let argList be ? ArgumentListEvaluation of Arguments.
    MarkedVector<Value> arg_list { vm.heap() };
    if (is_synthetic) {
        VERIFY(argument_array.is_object() && is<Array>(argument_array.as_object()));
        auto const& array_value = static_cast<Array const&>(argument_array.as_object());
        auto length = MUST(length_of_array_like(vm, array_value));
        for (size_t i = 0; i < length; ++i)
            arg_list.append(array_value.get_without_side_effects(PropertyKey { i }));
    } else {
        arg_list = argument_list_evaluation(vm, argument_array);
    }

    // 5. If IsConstructor(func) is false, throw a TypeError exception.
    if (!Value(func).is_constructor())
        return vm.throw_completion<TypeError>(ErrorType::NotAConstructor, "Super constructor");

    // 6. Let result be ? Construct(func, argList, newTarget).
    auto result = TRY(construct(vm, static_cast<FunctionObject&>(*func), arg_list.span(), &new_target.as_function()));

    // 7. Let thisER be GetThisEnvironment().
    auto& this_environment = verify_cast<FunctionEnvironment>(*get_this_environment(vm));

    // 8. Perform ? thisER.BindThisValue(result).
    TRY(this_environment.bind_this_value(vm, result));

    // 9. Let F be thisER.[[FunctionObject]].
    auto& f = this_environment.function_object();

    // 10. Assert: F is an ECMAScript function object.
    // NOTE: This is implied by the strong C++ type.

    // 11. Perform ? InitializeInstanceElements(result, F).
    TRY(result->initialize_instance_elements(f));

    // 12. Return result.
    return result;
}

inline ThrowCompletionOr<NonnullGCPtr<Array>> iterator_to_array(VM& vm, Value iterator)
{
    auto& iterator_record = verify_cast<IteratorRecord>(iterator.as_object());

    auto array = MUST(Array::create(*vm.current_realm(), 0));
    size_t index = 0;

    while (true) {
        auto value = TRY(iterator_step_value(vm, iterator_record));
        if (!value.has_value())
            return array;

        MUST(array->create_data_property_or_throw(index, value.release_value()));
        index++;
    }
}

inline ThrowCompletionOr<void> append(VM& vm, Value lhs, Value rhs, bool is_spread)
{
    // Note: This OpCode is used to construct array literals and argument arrays for calls,
    //       containing at least one spread element,
    //       Iterating over such a spread element to unpack it has to be visible by
    //       the user courtesy of
    //       (1) https://tc39.es/ecma262/#sec-runtime-semantics-arrayaccumulation
    //          SpreadElement : ... AssignmentExpression
    //              1. Let spreadRef be ? Evaluation of AssignmentExpression.
    //              2. Let spreadObj be ? GetValue(spreadRef).
    //              3. Let iteratorRecord be ? GetIterator(spreadObj).
    //              4. Repeat,
    //                  a. Let next be ? IteratorStep(iteratorRecord).
    //                  b. If next is false, return nextIndex.
    //                  c. Let nextValue be ? IteratorValue(next).
    //                  d. Perform ! CreateDataPropertyOrThrow(array, ! ToString(𝔽(nextIndex)), nextValue).
    //                  e. Set nextIndex to nextIndex + 1.
    //       (2) https://tc39.es/ecma262/#sec-runtime-semantics-argumentlistevaluation
    //          ArgumentList : ... AssignmentExpression
    //              1. Let list be a new empty List.
    //              2. Let spreadRef be ? Evaluation of AssignmentExpression.
    //              3. Let spreadObj be ? GetValue(spreadRef).
    //              4. Let iteratorRecord be ? GetIterator(spreadObj).
    //              5. Repeat,
    //                  a. Let next be ? IteratorStep(iteratorRecord).
    //                  b. If next is false, return list.
    //                  c. Let nextArg be ? IteratorValue(next).
    //                  d. Append nextArg to list.
    //          ArgumentList : ArgumentList , ... AssignmentExpression
    //             1. Let precedingArgs be ? ArgumentListEvaluation of ArgumentList.
    //             2. Let spreadRef be ? Evaluation of AssignmentExpression.
    //             3. Let iteratorRecord be ? GetIterator(? GetValue(spreadRef)).
    //             4. Repeat,
    //                 a. Let next be ? IteratorStep(iteratorRecord).
    //                 b. If next is false, return precedingArgs.
    //                 c. Let nextArg be ? IteratorValue(next).
    //                 d. Append nextArg to precedingArgs.

    // Note: We know from codegen, that lhs is a plain array with only indexed properties
    auto& lhs_array = lhs.as_array();
    auto lhs_size = lhs_array.indexed_properties().array_like_size();

    if (is_spread) {
        // ...rhs
        size_t i = lhs_size;
        TRY(get_iterator_values(vm, rhs, [&i, &lhs_array](Value iterator_value) -> Optional<Completion> {
            lhs_array.indexed_properties().put(i, iterator_value, default_attributes);
            ++i;
            return {};
        }));
    } else {
        lhs_array.indexed_properties().put(lhs_size, rhs, default_attributes);
    }

    return {};
}

inline ThrowCompletionOr<Value> delete_by_id(Bytecode::Interpreter& interpreter, Value base, IdentifierTableIndex property)
{
    auto& vm = interpreter.vm();

    auto const& identifier = interpreter.current_executable().get_identifier(property);
    bool strict = vm.in_strict_mode();
    auto reference = Reference { base, identifier, {}, strict };

    return TRY(reference.delete_(vm));
}

inline ThrowCompletionOr<Value> delete_by_value(Bytecode::Interpreter& interpreter, Value base, Value property_key_value)
{
    auto& vm = interpreter.vm();

    auto property_key = TRY(property_key_value.to_property_key(vm));
    bool strict = vm.in_strict_mode();
    auto reference = Reference { base, property_key, {}, strict };

    return Value(TRY(reference.delete_(vm)));
}

inline ThrowCompletionOr<Value> delete_by_value_with_this(Bytecode::Interpreter& interpreter, Value base, Value property_key_value, Value this_value)
{
    auto& vm = interpreter.vm();

    auto property_key = TRY(property_key_value.to_property_key(vm));
    bool strict = vm.in_strict_mode();
    auto reference = Reference { base, property_key, this_value, strict };

    return Value(TRY(reference.delete_(vm)));
}

// 14.7.5.9 EnumerateObjectProperties ( O ), https://tc39.es/ecma262/#sec-enumerate-object-properties
inline ThrowCompletionOr<Object*> get_object_property_iterator(VM& vm, Value value)
{
    // While the spec does provide an algorithm, it allows us to implement it ourselves so long as we meet the following invariants:
    //    1- Returned property keys do not include keys that are Symbols
    //    2- Properties of the target object may be deleted during enumeration. A property that is deleted before it is processed by the iterator's next method is ignored
    //    3- If new properties are added to the target object during enumeration, the newly added properties are not guaranteed to be processed in the active enumeration
    //    4- A property name will be returned by the iterator's next method at most once in any enumeration.
    //    5- Enumerating the properties of the target object includes enumerating properties of its prototype, and the prototype of the prototype, and so on, recursively;
    //       but a property of a prototype is not processed if it has the same name as a property that has already been processed by the iterator's next method.
    //    6- The values of [[Enumerable]] attributes are not considered when determining if a property of a prototype object has already been processed.
    //    7- The enumerable property names of prototype objects must be obtained by invoking EnumerateObjectProperties passing the prototype object as the argument.
    //    8- EnumerateObjectProperties must obtain the own property keys of the target object by calling its [[OwnPropertyKeys]] internal method.
    //    9- Property attributes of the target object must be obtained by calling its [[GetOwnProperty]] internal method

    // Invariant 3 effectively allows the implementation to ignore newly added keys, and we do so (similar to other implementations).
    auto object = TRY(value.to_object(vm));
    // Note: While the spec doesn't explicitly require these to be ordered, it says that the values should be retrieved via OwnPropertyKeys,
    //       so we just keep the order consistent anyway.
    OrderedHashTable<PropertyKey> properties;
    OrderedHashTable<PropertyKey> non_enumerable_properties;
    HashTable<NonnullGCPtr<Object>> seen_objects;
    // Collect all keys immediately (invariant no. 5)
    for (auto object_to_check = GCPtr { object.ptr() }; object_to_check && !seen_objects.contains(*object_to_check); object_to_check = TRY(object_to_check->internal_get_prototype_of())) {
        seen_objects.set(*object_to_check);
        for (auto& key : TRY(object_to_check->internal_own_property_keys())) {
            if (key.is_symbol())
                continue;
            auto property_key = TRY(PropertyKey::from_value(vm, key));

            // If there is a non-enumerable property higher up the prototype chain with the same key,
            // we mustn't include this property even if it's enumerable (invariant no. 5 and 6)
            if (non_enumerable_properties.contains(property_key))
                continue;
            if (properties.contains(property_key))
                continue;

            auto descriptor = TRY(object_to_check->internal_get_own_property(property_key));
            if (!*descriptor->enumerable)
                non_enumerable_properties.set(move(property_key));
            else
                properties.set(move(property_key));
        }
    }
    auto& realm = *vm.current_realm();
    auto callback = NativeFunction::create(
        *vm.current_realm(), [items = move(properties)](VM& vm) mutable -> ThrowCompletionOr<Value> {
            auto& realm = *vm.current_realm();
            auto iterated_object_value = vm.this_value();
            if (!iterated_object_value.is_object())
                return vm.throw_completion<InternalError>("Invalid state for GetObjectPropertyIterator.next"sv);

            auto& iterated_object = iterated_object_value.as_object();
            auto result_object = Object::create(realm, nullptr);
            while (true) {
                if (items.is_empty()) {
                    result_object->define_direct_property(vm.names.done, JS::Value(true), default_attributes);
                    return result_object;
                }

                auto key = items.take_first();

                // If the property is deleted, don't include it (invariant no. 2)
                if (!TRY(iterated_object.has_property(key)))
                    continue;

                result_object->define_direct_property(vm.names.done, JS::Value(false), default_attributes);

                if (key.is_number())
                    result_object->define_direct_property(vm.names.value, PrimitiveString::create(vm, TRY_OR_THROW_OOM(vm, String::number(key.as_number()))), default_attributes);
                else if (key.is_string())
                    result_object->define_direct_property(vm.names.value, PrimitiveString::create(vm, key.as_string()), default_attributes);
                else
                    VERIFY_NOT_REACHED(); // We should not have non-string/number keys.

                return result_object;
            }
        },
        1, vm.names.next);
    return vm.heap().allocate<IteratorRecord>(realm, realm, object, callback, false).ptr();
}

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
    auto base_value = interpreter.get(base());
    auto& cache = interpreter.current_executable().property_lookup_caches[m_cache_index];

    interpreter.set(dst(), TRY(get_by_id(interpreter.vm(), m_base_identifier, m_property, base_value, base_value, cache, interpreter.current_executable())));
    return {};
}

ThrowCompletionOr<void> GetByIdWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_value = interpreter.get(m_base);
    auto this_value = interpreter.get(m_this_value);
    auto& cache = interpreter.current_executable().property_lookup_caches[m_cache_index];
    interpreter.set(dst(), TRY(get_by_id(interpreter.vm(), {}, m_property, base_value, this_value, cache, interpreter.current_executable())));
    return {};
}

ThrowCompletionOr<void> GetLength::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_value = interpreter.get(base());
    auto& executable = interpreter.current_executable();
    auto& cache = executable.property_lookup_caches[m_cache_index];

    interpreter.set(dst(), TRY(get_by_id<GetByIdMode::Length>(interpreter.vm(), m_base_identifier, *executable.length_identifier, base_value, base_value, cache, executable)));
    return {};
}

ThrowCompletionOr<void> GetLengthWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_value = interpreter.get(m_base);
    auto this_value = interpreter.get(m_this_value);
    auto& executable = interpreter.current_executable();
    auto& cache = executable.property_lookup_caches[m_cache_index];
    interpreter.set(dst(), TRY(get_by_id<GetByIdMode::Length>(interpreter.vm(), {}, *executable.length_identifier, base_value, this_value, cache, executable)));
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
    interpreter.set(dst(), TRY(get_by_value(interpreter.vm(), m_base_identifier, interpreter.get(m_base), interpreter.get(m_property), interpreter.current_executable())));
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
