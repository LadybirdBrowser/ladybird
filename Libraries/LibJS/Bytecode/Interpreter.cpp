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
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Bytecode/Label.h>
#include <LibJS/Bytecode/Op.h>
#include <LibJS/Bytecode/PropertyAccess.h>
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
        builder.appendff("\033[34m{}~{}\033[0m", executable.local_variable_names[operand.index() - executable.local_index_base].name, operand.index() - executable.local_index_base);
        break;
    case Operand::Type::Argument:
        builder.appendff("\033[34marg{}\033[0m", operand.index() - executable.argument_index_base);
        break;
    case Operand::Type::Constant: {
        builder.append("\033[36m"sv);
        auto value = executable.constants[operand.index() - executable.number_of_registers];
        if (value.is_special_empty_value())
            builder.append("<Empty>"sv);
        else if (value.is_boolean())
            builder.appendff("Bool({})", value.as_bool() ? "true"sv : "false"sv);
        else if (value.is_int32())
            builder.appendff("Int32({})", value.as_i32());
        else if (value.is_double())
            builder.appendff("Double({})", value.as_double());
        else if (value.is_bigint())
            builder.appendff("BigInt({})", MUST(value.as_bigint().to_string()));
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

ALWAYS_INLINE static ThrowCompletionOr<bool> loosely_inequals(VM& vm, Value src1, Value src2)
{
    if (src1.tag() == src2.tag()) {
        if (src1.is_int32() || src1.is_object() || src1.is_boolean() || src1.is_nullish())
            return src1.encoded() != src2.encoded();
    }
    return !TRY(is_loosely_equal(vm, src1, src2));
}

ALWAYS_INLINE static ThrowCompletionOr<bool> loosely_equals(VM& vm, Value src1, Value src2)
{
    if (src1.tag() == src2.tag()) {
        if (src1.is_int32() || src1.is_object() || src1.is_boolean() || src1.is_nullish())
            return src1.encoded() == src2.encoded();
    }
    return TRY(is_loosely_equal(vm, src1, src2));
}

ALWAYS_INLINE static ThrowCompletionOr<bool> strict_inequals(VM&, Value src1, Value src2)
{
    if (src1.tag() == src2.tag()) {
        if (src1.is_int32() || src1.is_object() || src1.is_boolean() || src1.is_nullish())
            return src1.encoded() != src2.encoded();
    }
    return !is_strictly_equal(src1, src2);
}

ALWAYS_INLINE static ThrowCompletionOr<bool> strict_equals(VM&, Value src1, Value src2)
{
    if (src1.tag() == src2.tag()) {
        if (src1.is_int32() || src1.is_object() || src1.is_boolean() || src1.is_nullish())
            return src1.encoded() == src2.encoded();
    }
    return is_strictly_equal(src1, src2);
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
    return m_running_execution_context->registers_and_constants_and_locals_arguments.data()[op.index()];
}

ALWAYS_INLINE void Interpreter::set(Operand op, Value value)
{
    m_running_execution_context->registers_and_constants_and_locals_arguments.data()[op.index()] = value;
}

ALWAYS_INLINE Value Interpreter::do_yield(Value value, Optional<Label> continuation)
{
    // FIXME: If we get a pointer, which is not accurately representable as a double
    //        will cause this to explode
    auto continuation_value = continuation.has_value() ? Value(continuation->address()) : js_null();
    return vm().heap().allocate<GeneratorResult>(value, continuation_value, false).ptr();
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

    // 9. Suspend the currently running execution context.
    // 10. Push scriptContext onto the execution context stack; scriptContext is now the running execution context.
    TRY(vm.push_execution_context(*script_context, {}));

    // 13. If result.[[Type]] is normal, then
    if (executable) {
        // a. Set result to Completion(Evaluation of script).
        auto result_or_error = run_executable(*script_context, *executable, {}, {});
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

NEVER_INLINE Interpreter::HandleExceptionResponse Interpreter::handle_exception(u32& program_counter, Value exception)
{
    reg(Register::exception()) = exception;
    m_running_execution_context->scheduled_jump = {};
    auto handlers = current_executable().exception_handlers_for_offset(program_counter);
    if (!handlers.has_value()) {
        return HandleExceptionResponse::ExitFromExecutable;
    }
    auto& handler = handlers->handler_offset;
    auto& finalizer = handlers->finalizer_offset;

    VERIFY(!running_execution_context().unwind_contexts.is_empty());
    auto& unwind_context = running_execution_context().unwind_contexts.last();
    VERIFY(unwind_context.executable == &current_executable());

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

    u32& program_counter = running_execution_context.program_counter;
    program_counter = entry_point;

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

        handle_Mov: {
            auto& instruction = *reinterpret_cast<Op::Mov const*>(&bytecode[program_counter]);
            set(instruction.dst(), get(instruction.src()));
            DISPATCH_NEXT(Mov);
        }

        handle_End: {
            auto& instruction = *reinterpret_cast<Op::End const*>(&bytecode[program_counter]);
            accumulator() = get(instruction.value());
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
        if (result.is_error()) [[unlikely]] {                                                                           \
            if (handle_exception(program_counter, result.error_value()) == HandleExceptionResponse::ExitFromExecutable) \
                return;                                                                                                 \
            goto start;                                                                                                 \
        }                                                                                                               \
        if (result.value())                                                                                             \
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
            if (auto exception = reg(Register::exception()); !exception.is_special_empty_value()) {
                if (handle_exception(program_counter, exception) == HandleExceptionResponse::ExitFromExecutable)
                    return;
                goto start;
            }
            if (!saved_return_value().is_special_empty_value()) {
                do_return(saved_return_value());
                if (auto handlers = executable.exception_handlers_for_offset(program_counter); handlers.has_value()) {
                    if (auto finalizer = handlers.value().finalizer_offset; finalizer.has_value()) {
                        VERIFY(!running_execution_context.unwind_contexts.is_empty());
                        auto& unwind_context = running_execution_context.unwind_contexts.last();
                        VERIFY(unwind_context.executable == &current_executable());
                        reg(Register::saved_return_value()) = reg(Register::return_value());
                        reg(Register::return_value()) = js_special_empty_value();
                        program_counter = finalizer.value();
                        // the unwind_context will be pop'ed when entering the finally block
                        goto start;
                    }
                }
                return;
            }
            auto const old_scheduled_jump = running_execution_context.previously_scheduled_jumps.take_last();
            if (m_running_execution_context->scheduled_jump.has_value()) {
                program_counter = m_running_execution_context->scheduled_jump.value();
                m_running_execution_context->scheduled_jump = {};
            } else {
                program_counter = instruction.resume_target().address();
                // set the scheduled jump to the old value if we continue
                // where we left it
                m_running_execution_context->scheduled_jump = old_scheduled_jump;
            }
            goto start;
        }

        handle_ScheduleJump: {
            auto& instruction = *reinterpret_cast<Op::ScheduleJump const*>(&bytecode[program_counter]);
            m_running_execution_context->scheduled_jump = instruction.target().address();
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
            if (result.is_error()) [[unlikely]] {                                                                           \
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
            HANDLE_INSTRUCTION(Call);
            HANDLE_INSTRUCTION(CallBuiltin);
            HANDLE_INSTRUCTION(CallConstruct);
            HANDLE_INSTRUCTION(CallConstructWithArgumentArray);
            HANDLE_INSTRUCTION(CallDirectEval);
            HANDLE_INSTRUCTION(CallDirectEvalWithArgumentArray);
            HANDLE_INSTRUCTION(CallWithArgumentArray);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(Catch);
            HANDLE_INSTRUCTION(ConcatString);
            HANDLE_INSTRUCTION(CopyObjectExcludingProperties);
            HANDLE_INSTRUCTION(CreateImmutableBinding);
            HANDLE_INSTRUCTION(CreateMutableBinding);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(CreateLexicalEnvironment);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(CreateVariableEnvironment);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(CreatePrivateEnvironment);
            HANDLE_INSTRUCTION(CreateVariable);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(CreateRestParams);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(CreateArguments);
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
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(GetCompletionFields);
            HANDLE_INSTRUCTION(GetGlobal);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(GetImportMeta);
            HANDLE_INSTRUCTION(GetIterator);
            HANDLE_INSTRUCTION(GetLength);
            HANDLE_INSTRUCTION(GetLengthWithThis);
            HANDLE_INSTRUCTION(GetMethod);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(GetNewTarget);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(GetNextMethodFromIteratorRecord);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(GetObjectFromIteratorRecord);
            HANDLE_INSTRUCTION(GetObjectPropertyIterator);
            HANDLE_INSTRUCTION(GetPrivateById);
            HANDLE_INSTRUCTION(GetBinding);
            HANDLE_INSTRUCTION(GetInitializedBinding);
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
            HANDLE_INSTRUCTION(IteratorNextUnpack);
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

#define HANDLE_PUT_KIND_BY_ID(kind) HANDLE_INSTRUCTION(Put##kind##ById);
#define HANDLE_PUT_KIND_BY_NUMERIC_ID(kind) HANDLE_INSTRUCTION(Put##kind##ByNumericId);
#define HANDLE_PUT_KIND_BY_VALUE(kind) HANDLE_INSTRUCTION(Put##kind##ByValue);
#define HANDLE_PUT_KIND_BY_VALUE_WITH_THIS(kind) HANDLE_INSTRUCTION(Put##kind##ByValueWithThis);
#define HANDLE_PUT_KIND_BY_ID_WITH_THIS(kind) HANDLE_INSTRUCTION(Put##kind##ByIdWithThis);
#define HANDLE_PUT_KIND_BY_NUMERIC_ID_WITH_THIS(kind) HANDLE_INSTRUCTION(Put##kind##ByNumericIdWithThis);

            JS_ENUMERATE_PUT_KINDS(HANDLE_PUT_KIND_BY_ID)
            JS_ENUMERATE_PUT_KINDS(HANDLE_PUT_KIND_BY_NUMERIC_ID)
            JS_ENUMERATE_PUT_KINDS(HANDLE_PUT_KIND_BY_ID_WITH_THIS)
            JS_ENUMERATE_PUT_KINDS(HANDLE_PUT_KIND_BY_NUMERIC_ID_WITH_THIS)
            JS_ENUMERATE_PUT_KINDS(HANDLE_PUT_KIND_BY_VALUE)
            JS_ENUMERATE_PUT_KINDS(HANDLE_PUT_KIND_BY_VALUE_WITH_THIS)

            HANDLE_INSTRUCTION(PutBySpread);
            HANDLE_INSTRUCTION(PutPrivateById);
            HANDLE_INSTRUCTION(ResolveSuperBase);
            HANDLE_INSTRUCTION(ResolveThisBinding);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(RestoreScheduledJump);
            HANDLE_INSTRUCTION(RightShift);
            HANDLE_INSTRUCTION_WITHOUT_EXCEPTION_CHECK(SetCompletionType);
            HANDLE_INSTRUCTION(SetGlobal);
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

Utf16FlyString const& Interpreter::get_identifier(IdentifierTableIndex index) const
{
    return m_running_execution_context->identifier_table.data()[index.value];
}

Interpreter::ResultAndReturnRegister Interpreter::run_executable(ExecutionContext& context, Executable& executable, Optional<size_t> entry_point, Value initial_accumulator_value)
{
    dbgln_if(JS_BYTECODE_DEBUG, "Bytecode::Interpreter will run unit {}", &executable);

    // NOTE: This is how we "push" a new execution context onto the interpreter stack.
    TemporaryChange restore_running_execution_context { m_running_execution_context, &context };

    context.executable = executable;
    context.global_object = realm().global_object();
    context.global_declarative_environment = realm().global_environment().declarative_record();
    context.identifier_table = executable.identifier_table->identifiers();

    u32 registers_and_constants_and_locals_count = executable.number_of_registers + executable.constants.size() + executable.local_variable_names.size();
    VERIFY(registers_and_constants_and_locals_count <= context.registers_and_constants_and_locals_and_arguments_span().size());

    context.registers_and_constants_and_locals_arguments = context.registers_and_constants_and_locals_and_arguments_span();

    reg(Register::accumulator()) = initial_accumulator_value;
    reg(Register::return_value()) = js_special_empty_value();

    // NOTE: We only copy the `this` value from ExecutionContext if it's not already set.
    //       If we are re-entering an async/generator context, the `this` value
    //       may have already been cached by a ResolveThisBinding instruction,
    //       and subsequent instructions expect this value to be set.
    if (reg(Register::this_value()).is_special_empty_value())
        reg(Register::this_value()) = context.this_value.value_or(js_special_empty_value());

    auto* registers_and_constants_and_locals_and_arguments = context.registers_and_constants_and_locals_and_arguments();
    for (size_t i = 0; i < executable.constants.size(); ++i) {
        registers_and_constants_and_locals_and_arguments[executable.number_of_registers + i] = executable.constants[i];
    }

    run_bytecode(entry_point.value_or(0));

    dbgln_if(JS_BYTECODE_DEBUG, "Bytecode::Interpreter did run unit {}", context.executable);

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
        current_executable(),
        running_execution_context().lexical_environment);
    running_execution_context().previously_scheduled_jumps.append(m_running_execution_context->scheduled_jump);
    m_running_execution_context->scheduled_jump = {};
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
    m_running_execution_context->scheduled_jump = running_execution_context().previously_scheduled_jumps.take_last();
}

void Interpreter::leave_finally()
{
    reg(Register::exception()) = js_special_empty_value();
    m_running_execution_context->scheduled_jump = running_execution_context().previously_scheduled_jumps.take_last();
}

void Interpreter::enter_object_environment(Object& object)
{
    auto& old_environment = running_execution_context().lexical_environment;
    running_execution_context().saved_lexical_environments.append(old_environment);
    running_execution_context().lexical_environment = new_object_environment(object, true, old_environment);
}

ThrowCompletionOr<GC::Ref<Bytecode::Executable>> compile(VM& vm, ASTNode const& node, FunctionKind kind, Utf16FlyString const& name)
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

ALWAYS_INLINE ThrowCompletionOr<GC::Ref<Object>> base_object_for_get(VM& vm, Value base_value, Optional<IdentifierTableIndex> base_identifier, IdentifierTableIndex property_identifier, Executable const& executable)
{
    if (auto base_object = base_object_for_get_impl(vm, base_value))
        return GC::Ref { *base_object };

    // NOTE: At this point this is guaranteed to throw (null or undefined).
    return throw_null_or_undefined_property_get(vm, base_value, base_identifier, property_identifier, executable);
}

ALWAYS_INLINE ThrowCompletionOr<GC::Ref<Object>> base_object_for_get(VM& vm, Value base_value, Optional<IdentifierTableIndex> base_identifier, Value property, Executable const& executable)
{
    if (auto base_object = base_object_for_get_impl(vm, base_value))
        return GC::Ref { *base_object };

    // NOTE: At this point this is guaranteed to throw (null or undefined).
    return throw_null_or_undefined_property_get(vm, base_value, base_identifier, property, executable);
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
                case TypedArrayBase::Kind::Float16Array:
                    return fast_typed_array_get_element<f16>(typed_array, index);
                case TypedArrayBase::Kind::Float32Array:
                    return fast_typed_array_get_element<float>(typed_array, index);
                case TypedArrayBase::Kind::Float64Array:
                    return fast_typed_array_get_element<double>(typed_array, index);
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

inline ThrowCompletionOr<Value> get_global(Interpreter& interpreter, IdentifierTableIndex identifier_index, Strict strict, GlobalVariableCache& cache)
{
    auto& vm = interpreter.vm();
    auto& binding_object = interpreter.global_object();
    auto& declarative_record = interpreter.global_declarative_environment();

    auto& shape = binding_object.shape();
    if (cache.environment_serial_number == declarative_record.environment_serial_number()) {

        // OPTIMIZATION: For global var bindings, if the shape of the global object hasn't changed,
        //               we can use the cached property offset.
        if (&shape == cache.entries[0].shape && (!shape.is_dictionary() || shape.dictionary_generation() == cache.entries[0].shape_dictionary_generation.value())) {
            auto value = binding_object.get_direct(cache.entries[0].property_offset.value());
            if (value.is_accessor())
                return TRY(call(vm, value.as_accessor().getter(), js_undefined()));
            return value;
        }

        // OPTIMIZATION: For global lexical bindings, if the global declarative environment hasn't changed,
        //               we can use the cached environment binding index.
        if (cache.has_environment_binding_index) {
            if (cache.in_module_environment) {
                auto module = vm.running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>();
                return (*module)->environment()->get_binding_value_direct(vm, cache.environment_binding_index);
            }
            return declarative_record.get_binding_value_direct(vm, cache.environment_binding_index);
        }
    }

    cache.environment_serial_number = declarative_record.environment_serial_number();

    auto& identifier = interpreter.get_identifier(identifier_index);

    if (auto* module = vm.running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>()) {
        // NOTE: GetGlobal is used to access variables stored in the module environment and global environment.
        //       The module environment is checked first since it precedes the global environment in the environment chain.
        auto& module_environment = *(*module)->environment();
        Optional<size_t> index;
        if (TRY(module_environment.has_binding(identifier, &index))) {
            if (index.has_value()) {
                cache.environment_binding_index = static_cast<u32>(index.value());
                cache.has_environment_binding_index = true;
                cache.in_module_environment = true;
                return TRY(module_environment.get_binding_value_direct(vm, index.value()));
            }
            return TRY(module_environment.get_binding_value(vm, identifier, strict == Strict::Yes));
        }
    }

    Optional<size_t> offset;
    if (TRY(declarative_record.has_binding(identifier, &offset))) {
        cache.environment_binding_index = static_cast<u32>(offset.value());
        cache.has_environment_binding_index = true;
        cache.in_module_environment = false;
        return TRY(declarative_record.get_binding_value(vm, identifier, strict == Strict::Yes));
    }

    if (TRY(binding_object.has_property(identifier))) {
        CacheableGetPropertyMetadata cacheable_metadata;
        auto value = TRY(binding_object.internal_get(identifier, js_undefined(), &cacheable_metadata));
        if (cacheable_metadata.type == CacheableGetPropertyMetadata::Type::GetOwnProperty) {
            cache.entries[0].shape = shape;
            cache.entries[0].property_offset = cacheable_metadata.property_offset.value();

            if (shape.is_dictionary()) {
                cache.entries[0].shape_dictionary_generation = shape.dictionary_generation();
            }
        }
        return value;
    }

    return vm.throw_completion<ReferenceError>(ErrorType::UnknownIdentifier, identifier);
}

template<PutKind kind>
ThrowCompletionOr<void> put_by_property_key(VM& vm, Value base, Value this_value, Value value, Optional<Utf16FlyString const&> const& base_identifier, PropertyKey name, Strict strict, PropertyLookupCache* caches = nullptr)
{
    // Better error message than to_object would give
    if (strict == Strict::Yes && base.is_nullish()) [[unlikely]]
        return vm.throw_completion<TypeError>(ErrorType::ReferenceNullishSetProperty, name, base.to_string_without_side_effects());

    // a. Let baseObj be ? ToObject(V.[[Base]]).
    auto maybe_object = base.to_object(vm);
    if (maybe_object.is_error()) [[unlikely]]
        return throw_null_or_undefined_property_access(vm, base, base_identifier, name);
    auto object = maybe_object.release_value();

    if constexpr (kind == PutKind::Getter || kind == PutKind::Setter) {
        // The generator should only pass us functions for getters and setters.
        VERIFY(value.is_function());
    }
    switch (kind) {
    case PutKind::Getter: {
        auto& function = value.as_function();
        if (is<ECMAScriptFunctionObject>(function) && static_cast<ECMAScriptFunctionObject const&>(function).name().is_empty())
            static_cast<ECMAScriptFunctionObject*>(&function)->set_name(Utf16String::formatted("get {}", name));
        object->define_direct_accessor(name, &function, nullptr, Attribute::Configurable | Attribute::Enumerable);
        break;
    }
    case PutKind::Setter: {
        auto& function = value.as_function();
        if (is<ECMAScriptFunctionObject>(function) && static_cast<ECMAScriptFunctionObject const&>(function).name().is_empty())
            static_cast<ECMAScriptFunctionObject*>(&function)->set_name(Utf16String::formatted("set {}", name));
        object->define_direct_accessor(name, nullptr, &function, Attribute::Configurable | Attribute::Enumerable);
        break;
    }
    case PutKind::Normal: {
        auto this_value_object = MUST(this_value.to_object(vm));
        auto& from_shape = this_value_object->shape();
        if (caches) [[likely]] {
            for (auto& cache : caches->entries) {
                switch (cache.type) {
                case PropertyLookupCache::Entry::Type::Empty:
                    break;
                case PropertyLookupCache::Entry::Type::ChangePropertyInPrototypeChain: {
                    auto cached_prototype = cache.prototype.ptr();
                    if (!cached_prototype) [[unlikely]]
                        break;
                    // OPTIMIZATION: If the prototype chain hasn't been mutated in a way that would invalidate the cache, we can use it.
                    bool can_use_cache = [&]() -> bool {
                        if (&object->shape() != cache.shape) [[unlikely]]
                            return false;

                        if (cache.shape->is_dictionary()) {
                            VERIFY(cache.shape_dictionary_generation.has_value());
                            if (object->shape().dictionary_generation() != cache.shape_dictionary_generation.value()) [[unlikely]]
                                return false;
                        }

                        auto cached_prototype_chain_validity = cache.prototype_chain_validity.ptr();
                        if (!cached_prototype_chain_validity) [[unlikely]]
                            return false;
                        if (!cached_prototype_chain_validity->is_valid()) [[unlikely]]
                            return false;
                        return true;
                    }();
                    if (can_use_cache) [[likely]] {
                        auto value_in_prototype = cached_prototype->get_direct(cache.property_offset.value());
                        if (value_in_prototype.is_accessor()) [[unlikely]] {
                            (void)TRY(call(vm, value_in_prototype.as_accessor().setter(), this_value, value));
                            return {};
                        }
                    }
                    break;
                }
                case PropertyLookupCache::Entry::Type::ChangeOwnProperty: {
                    if (cache.shape != &object->shape()) [[unlikely]]
                        break;

                    if (cache.shape->is_dictionary()) {
                        VERIFY(cache.shape_dictionary_generation.has_value());
                        if (cache.shape->dictionary_generation() != cache.shape_dictionary_generation.value())
                            break;
                    }

                    auto value_in_object = object->get_direct(cache.property_offset.value());
                    if (value_in_object.is_accessor()) [[unlikely]] {
                        (void)TRY(call(vm, value_in_object.as_accessor().setter(), this_value, value));
                    } else {
                        object->put_direct(*cache.property_offset, value);
                    }
                    return {};
                }
                case PropertyLookupCache::Entry::Type::AddOwnProperty: {
                    // OPTIMIZATION: If the object's shape is the same as the one cached before adding the new property, we can
                    //               reuse the resulting shape from the cache.
                    if (cache.from_shape != &object->shape()) [[unlikely]]
                        break;
                    auto cached_shape = cache.shape.ptr();
                    if (!cached_shape) [[unlikely]]
                        break;

                    if (cache.shape->is_dictionary()) {
                        VERIFY(cache.shape_dictionary_generation.has_value());
                        if (object->shape().dictionary_generation() != cache.shape_dictionary_generation.value())
                            break;
                    }

                    // The cache is invalid if the prototype chain has been mutated, since such a mutation could have added a setter for the property.
                    auto cached_prototype_chain_validity = cache.prototype_chain_validity.ptr();
                    if (cached_prototype_chain_validity && !cached_prototype_chain_validity->is_valid()) [[unlikely]]
                        break;
                    object->unsafe_set_shape(*cached_shape);
                    object->put_direct(*cache.property_offset, value);
                    return {};
                }
                default:
                    VERIFY_NOT_REACHED();
                }
            }
        }

        CacheableSetPropertyMetadata cacheable_metadata;
        bool succeeded = TRY(object->internal_set(name, value, this_value, &cacheable_metadata));

        auto get_cache_slot = [&] -> PropertyLookupCache::Entry& {
            for (size_t i = caches->entries.size() - 1; i >= 1; --i) {
                caches->entries[i] = caches->entries[i - 1];
            }
            caches->entries[0] = {};
            return caches->entries[0];
        };

        if (succeeded && caches && cacheable_metadata.type == CacheableSetPropertyMetadata::Type::AddOwnProperty) {
            auto& cache = get_cache_slot();
            cache.type = PropertyLookupCache::Entry::Type::AddOwnProperty;
            cache.from_shape = from_shape;
            cache.property_offset = cacheable_metadata.property_offset.value();
            cache.shape = &object->shape();
            if (cacheable_metadata.prototype) {
                cache.prototype_chain_validity = *cacheable_metadata.prototype->shape().prototype_chain_validity();
            }
            if (cache.shape->is_dictionary()) {
                cache.shape_dictionary_generation = cache.shape->dictionary_generation();
            }
        }

        // If internal_set() caused object's shape change, we can no longer be sure
        // that collected metadata is valid, e.g. if setter in prototype chain added
        // property with the same name into the object itself.
        if (succeeded && caches && &from_shape == &object->shape()) {
            auto& cache = get_cache_slot();
            switch (cacheable_metadata.type) {
            case CacheableSetPropertyMetadata::Type::AddOwnProperty:
                // Something went wrong if we ended up here, because cacheable addition of a new property should've changed the shape.
                VERIFY_NOT_REACHED();
                break;
            case CacheableSetPropertyMetadata::Type::ChangeOwnProperty:
                cache.type = PropertyLookupCache::Entry::Type::ChangeOwnProperty;
                cache.shape = object->shape();
                cache.property_offset = cacheable_metadata.property_offset.value();

                if (cache.shape->is_dictionary()) {
                    cache.shape_dictionary_generation = cache.shape->dictionary_generation();
                }
                break;
            case CacheableSetPropertyMetadata::Type::ChangePropertyInPrototypeChain:
                cache.type = PropertyLookupCache::Entry::Type::ChangePropertyInPrototypeChain;
                cache.shape = object->shape();
                cache.property_offset = cacheable_metadata.property_offset.value();
                cache.prototype = *cacheable_metadata.prototype;
                cache.prototype_chain_validity = *cacheable_metadata.prototype->shape().prototype_chain_validity();

                if (cache.shape->is_dictionary()) {
                    cache.shape_dictionary_generation = cache.shape->dictionary_generation();
                }
                break;
            case CacheableSetPropertyMetadata::Type::NotCacheable:
                break;
            default:
                VERIFY_NOT_REACHED();
            }
        }

        if (!succeeded && strict == Strict::Yes) [[unlikely]] {
            if (base.is_object())
                return vm.throw_completion<TypeError>(ErrorType::ReferenceNullishSetProperty, name, base.to_string_without_side_effects());
            return vm.throw_completion<TypeError>(ErrorType::ReferencePrimitiveSetProperty, name, base.typeof_(vm)->utf8_string(), base.to_string_without_side_effects());
        }
        break;
    }
    case PutKind::Own:
        object->define_direct_property(name, value, Attribute::Enumerable | Attribute::Writable | Attribute::Configurable);
        break;
    case PutKind::Prototype:
        if (value.is_object() || value.is_null()) [[likely]]
            MUST(object->internal_set_prototype_of(value.is_object() ? &value.as_object() : nullptr));
        break;
    }

    return {};
}

inline ThrowCompletionOr<Value> perform_call(Interpreter& interpreter, Value this_value, Op::CallType call_type, Value callee, ReadonlySpan<Value> argument_values, Strict strict)
{
    auto& vm = interpreter.vm();
    auto& function = callee.as_function();
    Value return_value;
    if (call_type == Op::CallType::DirectEval) {
        if (callee == interpreter.realm().intrinsics().eval_function())
            return_value = TRY(perform_eval(vm, !argument_values.is_empty() ? argument_values[0] : js_undefined(), strict == Strict::Yes ? CallerMode::Strict : CallerMode::NonStrict, EvalMode::Direct));
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
        return vm.throw_completion<TypeError>(ErrorType::IsNotAEvaluatedFrom, callee.to_string_without_side_effects(), callee_type, interpreter.current_executable().get_string(*expression_string));

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

// 15.2.5 Runtime Semantics: InstantiateOrdinaryFunctionExpression, https://tc39.es/ecma262/#sec-runtime-semantics-instantiateordinaryfunctionexpression
static Value instantiate_ordinary_function_expression(Interpreter& interpreter, FunctionNode const& function_node, Utf16FlyString given_name)
{
    auto own_name = function_node.name();
    auto has_own_name = !own_name.is_empty();

    auto const& used_name = has_own_name ? own_name : given_name;

    auto environment = GC::Ref { *interpreter.running_execution_context().lexical_environment };
    if (has_own_name) {
        environment = new_declarative_environment(*environment);
        MUST(environment->create_immutable_binding(interpreter.vm(), own_name, false));
    }

    auto private_environment = interpreter.running_execution_context().private_environment;

    auto closure = ECMAScriptFunctionObject::create_from_function_node(function_node, used_name, interpreter.realm(), environment, private_environment);

    // FIXME: 6. Perform SetFunctionName(closure, name).
    // FIXME: 7. Perform MakeConstructor(closure).

    if (has_own_name)
        MUST(environment->initialize_binding(interpreter.vm(), own_name, closure, Environment::InitializeBindingHint::Normal));

    return closure;
}

inline Value new_function(Interpreter& interpreter, FunctionNode const& function_node, Optional<IdentifierTableIndex> const& lhs_name, Optional<Operand> const& home_object)
{
    auto& vm = interpreter.vm();
    Value value;

    if (!function_node.has_name()) {
        Utf16FlyString name;
        if (lhs_name.has_value())
            name = interpreter.get_identifier(lhs_name.value());
        value = instantiate_ordinary_function_expression(interpreter, function_node, name);
    } else {
        value = ECMAScriptFunctionObject::create_from_function_node(
            function_node,
            function_node.name(),
            *vm.current_realm(),
            vm.lexical_environment(),
            vm.running_execution_context().private_environment);
    }

    if (home_object.has_value()) {
        auto home_object_value = interpreter.get(home_object.value());
        static_cast<ECMAScriptFunctionObject&>(value.as_function()).set_home_object(&home_object_value.as_object());
    }

    return value;
}

template<PutKind kind>
inline ThrowCompletionOr<void> put_by_value(VM& vm, Value base, Optional<Utf16FlyString const&> const& base_identifier, Value property_key_value, Value value, Strict strict)
{
    // OPTIMIZATION: Fast path for simple Int32 indexes in array-like objects.
    if (kind == PutKind::Normal
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

            if (is_valid_integer_index(typed_array, canonical_index)) {
                if (value.is_int32()) {
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
                        break;
                    }
                } else if (value.is_double()) {
                    switch (typed_array.kind()) {
                    case TypedArrayBase::Kind::Float16Array:
                        fast_typed_array_set_element<f16>(typed_array, index, static_cast<f16>(value.as_double()));
                        return {};
                    case TypedArrayBase::Kind::Float32Array:
                        fast_typed_array_set_element<float>(typed_array, index, static_cast<float>(value.as_double()));
                        return {};
                    case TypedArrayBase::Kind::Float64Array:
                        fast_typed_array_set_element<double>(typed_array, index, value.as_double());
                        return {};
                    case TypedArrayBase::Kind::Int8Array:
                        fast_typed_array_set_element<i8>(typed_array, index, MUST(value.to_i8(vm)));
                        return {};
                    case TypedArrayBase::Kind::Int16Array:
                        fast_typed_array_set_element<i16>(typed_array, index, MUST(value.to_i16(vm)));
                        return {};
                    case TypedArrayBase::Kind::Int32Array:
                        fast_typed_array_set_element<i32>(typed_array, index, MUST(value.to_i32(vm)));
                        return {};
                    case TypedArrayBase::Kind::Uint8Array:
                        fast_typed_array_set_element<u8>(typed_array, index, MUST(value.to_u8(vm)));
                        return {};
                    case TypedArrayBase::Kind::Uint16Array:
                        fast_typed_array_set_element<u16>(typed_array, index, MUST(value.to_u16(vm)));
                        return {};
                    case TypedArrayBase::Kind::Uint32Array:
                        fast_typed_array_set_element<u32>(typed_array, index, MUST(value.to_u32(vm)));
                        return {};
                    default:
                        break;
                    }
                }
                // FIXME: Support more TypedArray kinds.
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

    auto property_key = TRY(property_key_value.to_property_key(vm));
    TRY(put_by_property_key<kind>(vm, base, base, value, base_identifier, property_key, strict));
    return {};
}

struct CalleeAndThis {
    Value callee;
    Value this_value;
};

inline ThrowCompletionOr<CalleeAndThis> get_callee_and_this_from_environment(Interpreter& interpreter, Utf16FlyString const& name, Strict strict, EnvironmentCoordinate& cache)
{
    auto& vm = interpreter.vm();

    Value callee = js_undefined();
    Value this_value = js_undefined();

    if (cache.is_valid()) [[likely]] {
        auto const* environment = interpreter.running_execution_context().lexical_environment.ptr();
        for (size_t i = 0; i < cache.hops; ++i)
            environment = environment->outer_environment();
        if (!environment->is_permanently_screwed_by_eval()) [[likely]] {
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

    auto reference = TRY(vm.resolve_binding(name, strict));
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
inline Value new_regexp(VM& vm, ParsedRegex const& parsed_regex, Utf16String pattern, Utf16String flags)
{
    // 1. Let pattern be CodePointsToString(BodyText of RegularExpressionLiteral).
    // 2. Let flags be CodePointsToString(FlagText of RegularExpressionLiteral).

    // 3. Return ! RegExpCreate(pattern, flags).
    auto& realm = *vm.current_realm();
    Regex<ECMA262> regex(parsed_regex.regex, parsed_regex.pattern.to_byte_string(), parsed_regex.flags);
    // NOTE: We bypass RegExpCreate and subsequently RegExpAlloc as an optimization to use the already parsed values.
    auto regexp_object = RegExpObject::create(realm, move(regex), move(pattern), move(flags));
    // RegExpAlloc has these two steps from the 'Legacy RegExp features' proposal.
    regexp_object->set_realm(realm);
    // We don't need to check 'If SameValue(newTarget, thisRealm.[[Intrinsics]].[[%RegExp%]]) is true'
    // here as we know RegExpCreate calls RegExpAlloc with %RegExp% for newTarget.
    regexp_object->set_legacy_features_enabled(true);
    return regexp_object;
}

inline ThrowCompletionOr<void> create_variable(VM& vm, Utf16FlyString const& name, Op::EnvironmentMode mode, bool is_global, bool is_immutable, bool is_strict)
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
    return as<GlobalEnvironment>(vm.variable_environment())->create_global_var_binding(name, false);
}

inline ThrowCompletionOr<ECMAScriptFunctionObject*> new_class(VM& vm, Value super_class, ClassExpression const& class_expression, Optional<IdentifierTableIndex> const& lhs_name, ReadonlySpan<Value> element_keys)
{
    auto& interpreter = vm.bytecode_interpreter();

    // NOTE: NewClass expects classEnv to be active lexical environment
    auto* class_environment = vm.lexical_environment();
    vm.running_execution_context().lexical_environment = vm.running_execution_context().saved_lexical_environments.take_last();

    Optional<Utf16FlyString> binding_name;
    Utf16FlyString class_name;
    if (!class_expression.has_name() && lhs_name.has_value()) {
        class_name = interpreter.get_identifier(lhs_name.value());
    } else {
        class_name = class_expression.name();
        binding_name = class_name;
    }

    return TRY(class_expression.create_class_constructor(vm, class_environment, vm.lexical_environment(), super_class, element_keys, binding_name, class_name));
}

inline ThrowCompletionOr<GC::Ref<Array>> iterator_to_array(VM& vm, Value iterator)
{
    auto& iterator_record = static_cast<IteratorRecord&>(iterator.as_cell());

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

class JS_API PropertyNameIterator final
    : public Object
    , public BuiltinIterator {
    JS_OBJECT(PropertyNameIterator, Object);
    GC_DECLARE_ALLOCATOR(PropertyNameIterator);

public:
    virtual ~PropertyNameIterator() override = default;

    BuiltinIterator* as_builtin_iterator_if_next_is_not_redefined(IteratorRecord const&) override { return this; }
    ThrowCompletionOr<void> next(VM& vm, bool& done, Value& value) override
    {
        while (true) {
            if (m_iterator == m_properties.end()) {
                done = true;
                return {};
            }

            auto const& entry = *m_iterator;
            ScopeGuard remove_first = [&] { ++m_iterator; };

            // If the property is deleted, don't include it (invariant no. 2)
            if (!TRY(m_object->has_property(entry)))
                continue;

            done = false;
            value = entry.to_value(vm);
            return {};
        }
    }

private:
    PropertyNameIterator(JS::Realm& realm, GC::Ref<Object> object, Vector<PropertyKey> properties)
        : Object(realm, nullptr)
        , m_object(object)
        , m_properties(move(properties))
        , m_iterator(m_properties.begin())
    {
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_object);
    }

    GC::Ref<Object> m_object;
    Vector<PropertyKey> m_properties;
    decltype(m_properties.begin()) m_iterator;
};

GC_DEFINE_ALLOCATOR(PropertyNameIterator);

// 14.7.5.9 EnumerateObjectProperties ( O ), https://tc39.es/ecma262/#sec-enumerate-object-properties
inline ThrowCompletionOr<Value> get_object_property_iterator(Interpreter& interpreter, Value value)
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

    auto& vm = interpreter.vm();

    // Invariant 3 effectively allows the implementation to ignore newly added keys, and we do so (similar to other implementations).
    auto object = TRY(value.to_object(vm));
    // Note: While the spec doesn't explicitly require these to be ordered, it says that the values should be retrieved via OwnPropertyKeys,
    //       so we just keep the order consistent anyway.

    size_t estimated_properties_count = 0;
    HashTable<GC::Ref<Object>> seen_objects;
    for (auto object_to_check = GC::Ptr { object.ptr() }; object_to_check && !seen_objects.contains(*object_to_check); object_to_check = TRY(object_to_check->internal_get_prototype_of())) {
        seen_objects.set(*object_to_check);
        estimated_properties_count += object_to_check->own_properties_count();
    }
    seen_objects.clear_with_capacity();

    Vector<PropertyKey> properties;
    properties.ensure_capacity(estimated_properties_count);

    HashTable<PropertyKey> seen_non_enumerable_properties;
    Optional<HashTable<PropertyKey>> seen_properties;
    auto ensure_seen_properties = [&] {
        if (seen_properties.has_value())
            return;
        seen_properties = HashTable<PropertyKey> {};
        seen_properties->ensure_capacity(properties.size());
        for (auto const& property : properties)
            seen_properties->set(property);
    };

    // Collect all keys immediately (invariant no. 5)
    bool in_prototype_chain = false;
    for (auto object_to_check = GC::Ptr { object.ptr() }; object_to_check && !seen_objects.contains(*object_to_check); object_to_check = TRY(object_to_check->internal_get_prototype_of())) {
        seen_objects.set(*object_to_check);
        TRY(object_to_check->for_each_own_property_with_enumerability([&](PropertyKey const& property_key, bool enumerable) -> ThrowCompletionOr<void> {
            if (!enumerable)
                seen_non_enumerable_properties.set(property_key);
            if (in_prototype_chain && enumerable) {
                if (seen_non_enumerable_properties.contains(property_key))
                    return {};
                ensure_seen_properties();
                if (seen_properties->contains(property_key))
                    return {};
            }
            if (enumerable)
                properties.append(property_key);
            if (seen_properties.has_value())
                seen_properties->set(property_key);
            return {};
        }));
        in_prototype_chain = true;
    }

    auto iterator = interpreter.realm().create<PropertyNameIterator>(interpreter.realm(), object, move(properties));

    return vm.heap().allocate<IteratorRecord>(iterator, js_undefined(), false);
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
        dbgln("{}  {} -> {}", String::repeated(' ', indent).release_value(), it.key.to_string(), value);
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
        interpreter.set(m_dst, Value { TRY(op_snake_case(vm, lhs, rhs)) });                     \
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
            auto result = static_cast<i64>(lhs.as_i32()) + static_cast<i64>(rhs.as_i32());
            interpreter.set(m_dst, Value(result, Value::CannotFitInInt32::Indeed));
            return {};
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
            auto result = static_cast<i64>(lhs.as_i32()) * static_cast<i64>(rhs.as_i32());
            interpreter.set(m_dst, Value(result, Value::CannotFitInInt32::Indeed));
            return {};
        }
        interpreter.set(m_dst, Value(lhs.as_double() * rhs.as_double()));
        return {};
    }

    interpreter.set(m_dst, TRY(mul(vm, lhs, rhs)));
    return {};
}

ThrowCompletionOr<void> Div::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const lhs = interpreter.get(m_lhs);
    auto const rhs = interpreter.get(m_rhs);

    if (lhs.is_number() && rhs.is_number()) [[likely]] {
        interpreter.set(m_dst, Value(lhs.as_double() / rhs.as_double()));
        return {};
    }

    interpreter.set(m_dst, TRY(div(vm, lhs, rhs)));
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
            auto result = static_cast<i64>(lhs.as_i32()) - static_cast<i64>(rhs.as_i32());
            interpreter.set(m_dst, Value(result, Value::CannotFitInInt32::Indeed));
            return {};
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
    interpreter.set(m_dst, Value { TRY(less_than(vm, lhs, rhs)) });
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
    interpreter.set(m_dst, Value { TRY(less_than_equals(vm, lhs, rhs)) });
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
    interpreter.set(m_dst, Value { TRY(greater_than(vm, lhs, rhs)) });
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
    interpreter.set(m_dst, Value { TRY(greater_than_equals(vm, lhs, rhs)) });
    return {};
}

static ThrowCompletionOr<Value> not_(VM&, Value value)
{
    return Value(!value.to_boolean());
}

static ThrowCompletionOr<Value> typeof_(VM& vm, Value value)
{
    return value.typeof_(vm);
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
    auto const& name = interpreter.get_identifier(m_name);
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

enum class BindingIsKnownToBeInitialized {
    No,
    Yes,
};

template<BindingIsKnownToBeInitialized binding_is_known_to_be_initialized>
static ThrowCompletionOr<void> get_binding(Interpreter& interpreter, Operand dst, IdentifierTableIndex identifier, Strict strict, EnvironmentCoordinate& cache)
{
    auto& vm = interpreter.vm();

    if (cache.is_valid()) [[likely]] {
        auto const* environment = interpreter.running_execution_context().lexical_environment.ptr();
        for (size_t i = 0; i < cache.hops; ++i)
            environment = environment->outer_environment();
        if (!environment->is_permanently_screwed_by_eval()) [[likely]] {
            Value value;
            if constexpr (binding_is_known_to_be_initialized == BindingIsKnownToBeInitialized::No) {
                value = TRY(static_cast<DeclarativeEnvironment const&>(*environment).get_binding_value_direct(vm, cache.index));
            } else {
                value = static_cast<DeclarativeEnvironment const&>(*environment).get_initialized_binding_value_direct(cache.index);
            }
            interpreter.set(dst, value);
            return {};
        }
        cache = {};
    }

    auto& executable = interpreter.current_executable();
    auto reference = TRY(vm.resolve_binding(executable.get_identifier(identifier), strict));
    if (reference.environment_coordinate().has_value())
        cache = reference.environment_coordinate().value();
    interpreter.set(dst, TRY(reference.get_value(vm)));
    return {};
}

ThrowCompletionOr<void> GetBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return get_binding<BindingIsKnownToBeInitialized::No>(interpreter, m_dst, m_identifier, strict(), m_cache);
}

ThrowCompletionOr<void> GetInitializedBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return get_binding<BindingIsKnownToBeInitialized::Yes>(interpreter, m_dst, m_identifier, strict(), m_cache);
}

ThrowCompletionOr<void> GetCalleeAndThisFromEnvironment::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto callee_and_this = TRY(get_callee_and_this_from_environment(
        interpreter,
        interpreter.get_identifier(m_identifier),
        strict(),
        m_cache));
    interpreter.set(m_callee, callee_and_this.callee);
    interpreter.set(m_this_value, callee_and_this.this_value);
    return {};
}

ThrowCompletionOr<void> GetGlobal::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.set(dst(), TRY(get_global(interpreter, m_identifier, strict(), interpreter.current_executable().global_variable_caches[m_cache_index])));
    return {};
}

ThrowCompletionOr<void> SetGlobal::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto& binding_object = interpreter.global_object();
    auto& declarative_record = interpreter.global_declarative_environment();

    auto& cache = interpreter.current_executable().global_variable_caches[m_cache_index];
    auto& shape = binding_object.shape();
    auto src = interpreter.get(m_src);

    if (cache.environment_serial_number == declarative_record.environment_serial_number()) {
        // OPTIMIZATION: For global var bindings, if the shape of the global object hasn't changed,
        //               we can use the cached property offset.
        if (&shape == cache.entries[0].shape && (!shape.is_dictionary() || shape.dictionary_generation() == cache.entries[0].shape_dictionary_generation.value())) {
            auto value = binding_object.get_direct(cache.entries[0].property_offset.value());
            if (value.is_accessor())
                TRY(call(vm, value.as_accessor().setter(), &binding_object, src));
            else
                binding_object.put_direct(cache.entries[0].property_offset.value(), src);
            return {};
        }

        // OPTIMIZATION: For global lexical bindings, if the global declarative environment hasn't changed,
        //               we can use the cached environment binding index.
        if (cache.has_environment_binding_index) {
            if (cache.in_module_environment) {
                auto module = vm.running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>();
                TRY((*module)->environment()->set_mutable_binding_direct(vm, cache.environment_binding_index, src, strict() == Strict::Yes));
            } else {
                TRY(declarative_record.set_mutable_binding_direct(vm, cache.environment_binding_index, src, strict() == Strict::Yes));
            }
            return {};
        }
    }

    cache.environment_serial_number = declarative_record.environment_serial_number();

    auto& identifier = interpreter.get_identifier(m_identifier);

    if (auto* module = vm.running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>()) {
        // NOTE: GetGlobal is used to access variables stored in the module environment and global environment.
        //       The module environment is checked first since it precedes the global environment in the environment chain.
        auto& module_environment = *(*module)->environment();
        Optional<size_t> index;
        if (TRY(module_environment.has_binding(identifier, &index))) {
            if (index.has_value()) {
                cache.environment_binding_index = static_cast<u32>(index.value());
                cache.has_environment_binding_index = true;
                cache.in_module_environment = true;
                return TRY(module_environment.set_mutable_binding_direct(vm, index.value(), src, strict() == Strict::Yes));
            }
            return TRY(module_environment.set_mutable_binding(vm, identifier, src, strict() == Strict::Yes));
        }
    }

    Optional<size_t> offset;
    if (TRY(declarative_record.has_binding(identifier, &offset))) {
        cache.environment_binding_index = static_cast<u32>(offset.value());
        cache.has_environment_binding_index = true;
        cache.in_module_environment = false;
        TRY(declarative_record.set_mutable_binding(vm, identifier, src, strict() == Strict::Yes));
        return {};
    }

    if (TRY(binding_object.has_property(identifier))) {
        CacheableSetPropertyMetadata cacheable_metadata;
        auto success = TRY(binding_object.internal_set(identifier, src, &binding_object, &cacheable_metadata));
        if (!success && strict() == Strict::Yes) {
            // Note: Nothing like this in the spec, this is here to produce nicer errors instead of the generic one thrown by Object::set().

            auto property_or_error = binding_object.internal_get_own_property(identifier);
            if (!property_or_error.is_error()) {
                auto property = property_or_error.release_value();
                if (property.has_value() && !property->writable.value_or(true)) {
                    return vm.throw_completion<TypeError>(ErrorType::DescWriteNonWritable, identifier);
                }
            }
            return vm.throw_completion<TypeError>(ErrorType::ObjectSetReturnedFalse);
        }
        if (cacheable_metadata.type == CacheableSetPropertyMetadata::Type::ChangeOwnProperty) {
            cache.entries[0].shape = shape;
            cache.entries[0].property_offset = cacheable_metadata.property_offset.value();

            if (shape.is_dictionary()) {
                cache.entries[0].shape_dictionary_generation = shape.dictionary_generation();
            }
        }
        return {};
    }

    auto reference = TRY(vm.resolve_binding(identifier, strict(), &declarative_record));
    TRY(reference.put_value(vm, src));

    return {};
}

ThrowCompletionOr<void> DeleteVariable::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const& string = interpreter.get_identifier(m_identifier);
    auto reference = TRY(vm.resolve_binding(string, strict()));
    interpreter.set(dst(), Value(TRY(reference.delete_(vm))));
    return {};
}

void CreateLexicalEnvironment::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto make_and_swap_envs = [&](auto& old_environment) {
        auto declarative_environment = new_declarative_environment(*old_environment).ptr();
        declarative_environment->ensure_capacity(m_capacity);
        GC::Ptr<Environment> environment = declarative_environment;
        swap(old_environment, environment);
        return environment;
    };
    auto& running_execution_context = interpreter.running_execution_context();
    running_execution_context.saved_lexical_environments.append(make_and_swap_envs(running_execution_context.lexical_environment));
    if (m_dst.has_value())
        interpreter.set(*m_dst, running_execution_context.lexical_environment);
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
    auto const& name = interpreter.get_identifier(m_identifier);
    return create_variable(interpreter.vm(), name, m_mode, m_is_global, m_is_immutable, m_is_strict);
}

void CreateRestParams::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto const arguments = interpreter.running_execution_context().arguments;
    auto arguments_count = interpreter.running_execution_context().passed_argument_count;
    auto array = MUST(Array::create(interpreter.realm(), 0));
    for (size_t rest_index = m_rest_index; rest_index < arguments_count; ++rest_index)
        array->indexed_properties().append(arguments[rest_index]);
    interpreter.set(m_dst, array);
}

void CreateArguments::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto const& function = interpreter.running_execution_context().function;
    auto const arguments = interpreter.running_execution_context().arguments;
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
        return;
    }

    if (m_is_immutable) {
        MUST(environment->create_immutable_binding(interpreter.vm(), interpreter.vm().names.arguments.as_string(), false));
    } else {
        MUST(environment->create_mutable_binding(interpreter.vm(), interpreter.vm().names.arguments.as_string(), false));
    }
    MUST(environment->initialize_binding(interpreter.vm(), interpreter.vm().names.arguments.as_string(), arguments_object, Environment::InitializeBindingHint::Normal));
}

template<EnvironmentMode environment_mode, BindingInitializationMode initialization_mode>
static ThrowCompletionOr<void> initialize_or_set_binding(Interpreter& interpreter, IdentifierTableIndex identifier_index, Strict strict, Value value, EnvironmentCoordinate& cache)
{
    auto& vm = interpreter.vm();

    auto* environment = environment_mode == EnvironmentMode::Lexical
        ? interpreter.running_execution_context().lexical_environment.ptr()
        : interpreter.running_execution_context().variable_environment.ptr();

    if (cache.is_valid()) [[likely]] {
        for (size_t i = 0; i < cache.hops; ++i)
            environment = environment->outer_environment();
        if (!environment->is_permanently_screwed_by_eval()) [[likely]] {
            if constexpr (initialization_mode == BindingInitializationMode::Initialize) {
                TRY(static_cast<DeclarativeEnvironment&>(*environment).initialize_binding_direct(vm, cache.index, value, Environment::InitializeBindingHint::Normal));
            } else {
                TRY(static_cast<DeclarativeEnvironment&>(*environment).set_mutable_binding_direct(vm, cache.index, value, strict == Strict::Yes));
            }
            return {};
        }
        cache = {};
    }

    auto reference = TRY(vm.resolve_binding(interpreter.get_identifier(identifier_index), strict, environment));
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
    return initialize_or_set_binding<EnvironmentMode::Lexical, BindingInitializationMode::Initialize>(interpreter, m_identifier, strict(), interpreter.get(m_src), m_cache);
}

ThrowCompletionOr<void> InitializeVariableBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return initialize_or_set_binding<EnvironmentMode::Var, BindingInitializationMode::Initialize>(interpreter, m_identifier, strict(), interpreter.get(m_src), m_cache);
}

ThrowCompletionOr<void> SetLexicalBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return initialize_or_set_binding<EnvironmentMode::Lexical, BindingInitializationMode::Set>(interpreter, m_identifier, strict(), interpreter.get(m_src), m_cache);
}

ThrowCompletionOr<void> SetVariableBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return initialize_or_set_binding<EnvironmentMode::Var, BindingInitializationMode::Set>(interpreter, m_identifier, strict(), interpreter.get(m_src), m_cache);
}

ThrowCompletionOr<void> GetById::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_value = interpreter.get(base());
    auto& cache = interpreter.current_executable().property_lookup_caches[m_cache_index];

    interpreter.set(dst(), TRY(get_by_id<GetByIdMode::Normal>(interpreter.vm(), [&] { return interpreter.get_identifier(m_base_identifier); }, [&] { return interpreter.get_identifier(m_property); }, base_value, base_value, cache)));
    return {};
}

ThrowCompletionOr<void> GetByIdWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_value = interpreter.get(m_base);
    auto this_value = interpreter.get(m_this_value);
    auto& cache = interpreter.current_executable().property_lookup_caches[m_cache_index];
    interpreter.set(dst(), TRY(get_by_id<GetByIdMode::Normal>(interpreter.vm(), [] { return Optional<Utf16FlyString const&> {}; }, [&] { return interpreter.get_identifier(m_property); }, base_value, this_value, cache)));
    return {};
}

ThrowCompletionOr<void> GetLength::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_value = interpreter.get(base());
    auto& executable = interpreter.current_executable();
    auto& cache = executable.property_lookup_caches[m_cache_index];

    interpreter.set(dst(), TRY(get_by_id<GetByIdMode::Length>(interpreter.vm(), [&] { return interpreter.get_identifier(m_base_identifier); }, [&] { return executable.get_identifier(*executable.length_identifier); }, base_value, base_value, cache)));
    return {};
}

ThrowCompletionOr<void> GetLengthWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto base_value = interpreter.get(m_base);
    auto this_value = interpreter.get(m_this_value);
    auto& executable = interpreter.current_executable();
    auto& cache = executable.property_lookup_caches[m_cache_index];
    interpreter.set(dst(), TRY(get_by_id<GetByIdMode::Length>(interpreter.vm(), [] { return Optional<Utf16FlyString const&> {}; }, [&] { return executable.get_identifier(*executable.length_identifier); }, base_value, this_value, cache)));
    return {};
}

ThrowCompletionOr<void> GetPrivateById::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const& name = interpreter.get_identifier(m_property);
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
    auto private_name = private_environment->resolve_private_identifier(interpreter.get_identifier(m_property));
    interpreter.set(dst(), Value(base.as_object().private_element_find(private_name) != nullptr));
    return {};
}

ThrowCompletionOr<void> PutBySpread::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto value = interpreter.get(m_src);
    auto base = interpreter.get(m_base);

    // a. Let baseObj be ? ToObject(V.[[Base]]).
    auto object = TRY(base.to_object(vm));

    TRY(object->copy_data_properties(vm, value, {}));

    return {};
}

#define DEFINE_PUT_KIND_BY_ID(kind)                                                                              \
    ThrowCompletionOr<void> Put##kind##ById::execute_impl(Bytecode::Interpreter& interpreter) const              \
    {                                                                                                            \
        auto& vm = interpreter.vm();                                                                             \
        auto value = interpreter.get(m_src);                                                                     \
        auto base = interpreter.get(m_base);                                                                     \
        auto base_identifier = interpreter.get_identifier(m_base_identifier);                                    \
        PropertyKey name { interpreter.get_identifier(m_property), PropertyKey::StringMayBeNumber::No };         \
        auto& cache = interpreter.current_executable().property_lookup_caches[m_cache_index];                    \
        TRY(put_by_property_key<PutKind::kind>(vm, base, base, value, base_identifier, name, strict(), &cache)); \
        return {};                                                                                               \
    }                                                                                                            \
    ByteString Put##kind##ById::to_byte_string_impl(Bytecode::Executable const& executable) const                \
    {                                                                                                            \
        return ByteString::formatted("Put" #kind "ById {}, {}, {}",                                              \
            format_operand("base"sv, m_base, executable),                                                        \
            executable.identifier_table->get(m_property),                                                        \
            format_operand("src"sv, m_src, executable));                                                         \
    }

JS_ENUMERATE_PUT_KINDS(DEFINE_PUT_KIND_BY_ID)

#define DEFINE_PUT_KIND_BY_NUMERIC_ID(kind)                                                                      \
    ThrowCompletionOr<void> Put##kind##ByNumericId::execute_impl(Bytecode::Interpreter& interpreter) const       \
    {                                                                                                            \
        auto& vm = interpreter.vm();                                                                             \
        auto value = interpreter.get(m_src);                                                                     \
        auto base = interpreter.get(m_base);                                                                     \
        auto base_identifier = interpreter.get_identifier(m_base_identifier);                                    \
        PropertyKey name { m_property };                                                                         \
        auto& cache = interpreter.current_executable().property_lookup_caches[m_cache_index];                    \
        TRY(put_by_property_key<PutKind::kind>(vm, base, base, value, base_identifier, name, strict(), &cache)); \
        return {};                                                                                               \
    }                                                                                                            \
    ByteString Put##kind##ByNumericId::to_byte_string_impl(Bytecode::Executable const& executable) const         \
    {                                                                                                            \
        return ByteString::formatted("Put" #kind "ByNumericId {}, {}, {}",                                       \
            format_operand("base"sv, m_base, executable),                                                        \
            m_property,                                                                                          \
            format_operand("src"sv, m_src, executable));                                                         \
    }

JS_ENUMERATE_PUT_KINDS(DEFINE_PUT_KIND_BY_NUMERIC_ID)

#define DEFINE_PUT_KIND_BY_NUMERIC_ID_WITH_THIS(kind)                                                                        \
    ThrowCompletionOr<void> Put##kind##ByNumericIdWithThis::execute_impl(Bytecode::Interpreter& interpreter) const           \
    {                                                                                                                        \
        auto& vm = interpreter.vm();                                                                                         \
        auto value = interpreter.get(m_src);                                                                                 \
        auto base = interpreter.get(m_base);                                                                                 \
        PropertyKey name { m_property };                                                                                     \
        auto& cache = interpreter.current_executable().property_lookup_caches[m_cache_index];                                \
        TRY(put_by_property_key<PutKind::kind>(vm, base, interpreter.get(m_this_value), value, {}, name, strict(), &cache)); \
        return {};                                                                                                           \
    }                                                                                                                        \
    ByteString Put##kind##ByNumericIdWithThis::to_byte_string_impl(Bytecode::Executable const& executable) const             \
    {                                                                                                                        \
        return ByteString::formatted("Put" #kind "ByNumericIdWithThis {}, {}, {}, {}",                                       \
            format_operand("base"sv, m_base, executable),                                                                    \
            m_property,                                                                                                      \
            format_operand("src"sv, m_src, executable),                                                                      \
            format_operand("this"sv, m_this_value, executable));                                                             \
    }

JS_ENUMERATE_PUT_KINDS(DEFINE_PUT_KIND_BY_NUMERIC_ID_WITH_THIS)

#define DEFINE_PUT_KIND_BY_ID_WITH_THIS(kind)                                                                                \
    ThrowCompletionOr<void> Put##kind##ByIdWithThis::execute_impl(Bytecode::Interpreter& interpreter) const                  \
    {                                                                                                                        \
        auto& vm = interpreter.vm();                                                                                         \
        auto value = interpreter.get(m_src);                                                                                 \
        auto base = interpreter.get(m_base);                                                                                 \
        PropertyKey name { interpreter.get_identifier(m_property), PropertyKey::StringMayBeNumber::No };                     \
        auto& cache = interpreter.current_executable().property_lookup_caches[m_cache_index];                                \
        TRY(put_by_property_key<PutKind::kind>(vm, base, interpreter.get(m_this_value), value, {}, name, strict(), &cache)); \
        return {};                                                                                                           \
    }                                                                                                                        \
    ByteString Put##kind##ByIdWithThis::to_byte_string_impl(Bytecode::Executable const& executable) const                    \
    {                                                                                                                        \
        return ByteString::formatted("Put" #kind "ByIdWithThis {}, {}, {}, {}",                                              \
            format_operand("base"sv, m_base, executable),                                                                    \
            executable.identifier_table->get(m_property),                                                                    \
            format_operand("src"sv, m_src, executable),                                                                      \
            format_operand("this"sv, m_this_value, executable));                                                             \
    }

JS_ENUMERATE_PUT_KINDS(DEFINE_PUT_KIND_BY_ID_WITH_THIS)

ThrowCompletionOr<void> PutPrivateById::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto value = interpreter.get(m_src);
    auto object = TRY(interpreter.get(m_base).to_object(vm));
    auto name = interpreter.get_identifier(m_property);
    auto private_reference = make_private_reference(vm, object, name);
    TRY(private_reference.put_value(vm, value));
    return {};
}

ThrowCompletionOr<void> DeleteById::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto const& identifier = interpreter.get_identifier(m_property);
    auto reference = Reference { interpreter.get(m_base), identifier, {}, strict() };
    interpreter.set(dst(), Value(TRY(reference.delete_(vm))));
    return {};
}

ThrowCompletionOr<void> DeleteByIdWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto base_value = interpreter.get(m_base);
    auto const& identifier = interpreter.get_identifier(m_property);
    auto reference = Reference { base_value, identifier, interpreter.get(m_this_value), strict() };
    interpreter.set(dst(), Value(TRY(reference.delete_(vm))));
    return {};
}

ThrowCompletionOr<void> ResolveThisBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& cached_this_value = interpreter.reg(Register::this_value());
    if (!cached_this_value.is_special_empty_value())
        return {};
    // OPTIMIZATION: Because the value of 'this' cannot be reassigned during a function execution, it's
    //               resolved once and then saved for subsequent use.
    auto& running_execution_context = interpreter.running_execution_context();
    if (auto function = running_execution_context.function; function && is<ECMAScriptFunctionObject>(*function) && !static_cast<ECMAScriptFunctionObject&>(*function).allocates_function_environment()) {
        cached_this_value = running_execution_context.this_value.value();
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
    auto& env = as<FunctionEnvironment>(*get_this_environment(vm));

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
    case Builtin::MathImul:
        return TRY(MathObject::imul_impl(interpreter.vm(), interpreter.get(arguments[0]), interpreter.get(arguments[1])));
    case Builtin::MathRandom:
        return MathObject::random_impl();
    case Builtin::MathRound:
        return TRY(MathObject::round_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathSqrt:
        return TRY(MathObject::sqrt_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathSin:
        return TRY(MathObject::sin_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathCos:
        return TRY(MathObject::cos_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::MathTan:
        return TRY(MathObject::tan_impl(interpreter.vm(), interpreter.get(arguments[0])));
    case Builtin::ArrayIteratorPrototypeNext:
    case Builtin::MapIteratorPrototypeNext:
    case Builtin::SetIteratorPrototypeNext:
    case Builtin::StringIteratorPrototypeNext:
        VERIFY_NOT_REACHED();
    case Builtin::OrdinaryHasInstance:
        VERIFY_NOT_REACHED();
    case Bytecode::Builtin::__Count:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

template<CallType call_type>
static ThrowCompletionOr<void> execute_call(
    Bytecode::Interpreter& interpreter,
    Value callee,
    Value this_value,
    ReadonlySpan<Operand> arguments,
    Operand dst,
    Optional<StringTableIndex> const& expression_string,
    Strict strict)
{
    TRY(throw_if_needed_for_call(interpreter, callee, call_type, expression_string));

    auto& function = callee.as_function();

    ExecutionContext* callee_context = nullptr;
    size_t registers_and_constants_and_locals_count = 0;
    size_t argument_count = arguments.size();
    TRY(function.get_stack_frame_size(registers_and_constants_and_locals_count, argument_count));
    ALLOCATE_EXECUTION_CONTEXT_ON_NATIVE_STACK_WITHOUT_CLEARING_ARGS(callee_context, registers_and_constants_and_locals_count, max(arguments.size(), argument_count));

    auto* callee_context_argument_values = callee_context->arguments.data();
    auto const callee_context_argument_count = callee_context->arguments.size();
    auto const insn_argument_count = arguments.size();

    for (size_t i = 0; i < insn_argument_count; ++i)
        callee_context_argument_values[i] = interpreter.get(arguments[i]);
    for (size_t i = insn_argument_count; i < callee_context_argument_count; ++i)
        callee_context_argument_values[i] = js_undefined();
    callee_context->passed_argument_count = insn_argument_count;

    Value retval;
    if (call_type == CallType::DirectEval && callee == interpreter.realm().intrinsics().eval_function()) {
        retval = TRY(perform_eval(interpreter.vm(), !callee_context->arguments.is_empty() ? callee_context->arguments[0] : js_undefined(), strict == Strict::Yes ? CallerMode::Strict : CallerMode::NonStrict, EvalMode::Direct));
    } else if (call_type == CallType::Construct) {
        retval = TRY(function.internal_construct(*callee_context, function));
    } else {
        retval = TRY(function.internal_call(*callee_context, this_value));
    }
    interpreter.set(dst, retval);
    return {};
}

ThrowCompletionOr<void> Call::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return execute_call<CallType::Call>(interpreter, interpreter.get(m_callee), interpreter.get(m_this_value), { m_arguments, m_argument_count }, m_dst, m_expression_string, strict());
}

NEVER_INLINE ThrowCompletionOr<void> CallConstruct::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return execute_call<CallType::Construct>(interpreter, interpreter.get(m_callee), js_undefined(), { m_arguments, m_argument_count }, m_dst, m_expression_string, strict());
}

ThrowCompletionOr<void> CallDirectEval::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return execute_call<CallType::DirectEval>(interpreter, interpreter.get(m_callee), interpreter.get(m_this_value), { m_arguments, m_argument_count }, m_dst, m_expression_string, strict());
}

ThrowCompletionOr<void> CallBuiltin::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto callee = interpreter.get(m_callee);

    if (m_argument_count == Bytecode::builtin_argument_count(m_builtin) && callee.is_object() && interpreter.realm().get_builtin_value(m_builtin) == &callee.as_object()) {
        interpreter.set(dst(), TRY(dispatch_builtin_call(interpreter, m_builtin, { m_arguments, m_argument_count })));
        return {};
    }

    return execute_call<CallType::Call>(interpreter, callee, interpreter.get(m_this_value), { m_arguments, m_argument_count }, m_dst, m_expression_string, strict());
}

template<CallType call_type>
static ThrowCompletionOr<void> call_with_argument_array(
    Bytecode::Interpreter& interpreter,
    Value callee,
    Value this_value,
    Value arguments,
    Operand dst,
    Optional<StringTableIndex> const& expression_string,
    Strict strict)
{
    TRY(throw_if_needed_for_call(interpreter, callee, call_type, expression_string));

    auto& function = callee.as_function();

    auto& argument_array = arguments.as_array();
    auto argument_array_length = argument_array.indexed_properties().array_like_size();

    ExecutionContext* callee_context = nullptr;
    size_t argument_count = argument_array_length;
    size_t registers_and_constants_and_locals_count = 0;
    TRY(function.get_stack_frame_size(registers_and_constants_and_locals_count, argument_count));
    ALLOCATE_EXECUTION_CONTEXT_ON_NATIVE_STACK_WITHOUT_CLEARING_ARGS(callee_context, registers_and_constants_and_locals_count, max(argument_array_length, argument_count));

    auto* callee_context_argument_values = callee_context->arguments.data();
    auto const callee_context_argument_count = callee_context->arguments.size();
    auto const insn_argument_count = argument_array_length;

    for (size_t i = 0; i < insn_argument_count; ++i) {
        if (auto maybe_value = argument_array.indexed_properties().get(i); maybe_value.has_value())
            callee_context_argument_values[i] = maybe_value.release_value().value;
        else
            callee_context_argument_values[i] = js_undefined();
    }
    for (size_t i = insn_argument_count; i < callee_context_argument_count; ++i)
        callee_context_argument_values[i] = js_undefined();
    callee_context->passed_argument_count = insn_argument_count;

    Value retval;
    if (call_type == CallType::DirectEval && callee == interpreter.realm().intrinsics().eval_function()) {
        auto& vm = interpreter.vm();
        retval = TRY(perform_eval(vm, !callee_context->arguments.is_empty() ? callee_context->arguments[0] : js_undefined(), strict == Strict::Yes ? CallerMode::Strict : CallerMode::NonStrict, EvalMode::Direct));
    } else if (call_type == CallType::Construct) {
        retval = TRY(function.internal_construct(*callee_context, function));
    } else {
        retval = TRY(function.internal_call(*callee_context, this_value));
    }

    interpreter.set(dst, retval);
    return {};
}

ThrowCompletionOr<void> CallWithArgumentArray::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return call_with_argument_array<CallType::Call>(interpreter, interpreter.get(callee()), interpreter.get(this_value()), interpreter.get(arguments()), dst(), expression_string(), strict());
}

ThrowCompletionOr<void> CallDirectEvalWithArgumentArray::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return call_with_argument_array<CallType::DirectEval>(interpreter, interpreter.get(callee()), interpreter.get(this_value()), interpreter.get(arguments()), dst(), expression_string(), strict());
}

ThrowCompletionOr<void> CallConstructWithArgumentArray::execute_impl(Bytecode::Interpreter& interpreter) const
{
    return call_with_argument_array<CallType::Construct>(interpreter, interpreter.get(callee()), js_undefined(), interpreter.get(arguments()), dst(), expression_string(), strict());
}

// 13.3.7.1 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
ThrowCompletionOr<void> SuperCallWithArgumentArray::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();

    // 1. Let newTarget be GetNewTarget().
    auto new_target = vm.get_new_target();

    // 2. Assert: Type(newTarget) is Object.
    VERIFY(new_target.is_object());

    // 3. Let func be GetSuperConstructor().
    auto* func = get_super_constructor(vm);

    // NON-STANDARD: We're doing this step earlier to streamline control flow.
    // 5. If IsConstructor(func) is false, throw a TypeError exception.
    if (!Value(func).is_constructor())
        return vm.throw_completion<TypeError>(ErrorType::NotAConstructor, "Super constructor");

    auto& function = static_cast<FunctionObject&>(*func);

    // 4. Let argList be ? ArgumentListEvaluation of Arguments.
    auto& argument_array = interpreter.get(m_arguments).as_array();
    size_t argument_array_length = 0;

    if (m_is_synthetic) {
        argument_array_length = MUST(length_of_array_like(vm, argument_array));
    } else {
        argument_array_length = argument_array.indexed_properties().array_like_size();
    }

    ExecutionContext* callee_context = nullptr;
    size_t argument_count = argument_array_length;
    size_t registers_and_constants_and_locals_count = 0;
    TRY(function.get_stack_frame_size(registers_and_constants_and_locals_count, argument_count));
    ALLOCATE_EXECUTION_CONTEXT_ON_NATIVE_STACK_WITHOUT_CLEARING_ARGS(callee_context, registers_and_constants_and_locals_count, max(argument_array_length, argument_count));

    auto* callee_context_argument_values = callee_context->arguments.data();
    auto const callee_context_argument_count = callee_context->arguments.size();
    auto const insn_argument_count = argument_array_length;

    if (m_is_synthetic) {
        for (size_t i = 0; i < insn_argument_count; ++i)
            callee_context_argument_values[i] = argument_array.get_without_side_effects(PropertyKey { i });
    } else {
        for (size_t i = 0; i < insn_argument_count; ++i) {
            if (auto maybe_value = argument_array.indexed_properties().get(i); maybe_value.has_value())
                callee_context_argument_values[i] = maybe_value.release_value().value;
            else
                callee_context_argument_values[i] = js_undefined();
        }
    }
    for (size_t i = insn_argument_count; i < callee_context_argument_count; ++i)
        callee_context_argument_values[i] = js_undefined();
    callee_context->passed_argument_count = insn_argument_count;

    // 6. Let result be ? Construct(func, argList, newTarget).
    auto result = TRY(function.internal_construct(*callee_context, new_target.as_function()));

    // 7. Let thisER be GetThisEnvironment().
    auto& this_environment = as<FunctionEnvironment>(*get_this_environment(vm));

    // 8. Perform ? thisER.BindThisValue(result).
    TRY(this_environment.bind_this_value(vm, result));

    // 9. Let F be thisER.[[FunctionObject]].
    auto& f = this_environment.function_object();

    // 10. Assert: F is an ECMAScript function object.
    // NOTE: This is implied by the strong C++ type.

    // 11. Perform ? InitializeInstanceElements(result, F).
    TRY(result->initialize_instance_elements(f));

    // 12. Return result.
    interpreter.set(m_dst, result);
    return {};
}

void NewFunction::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.set(dst(), new_function(interpreter, m_function_node, m_lhs_name, m_home_object));
}

void Return::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.do_return(interpreter.get(m_value));
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
    if (value.is_special_empty_value())
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
    auto yielded_value = interpreter.get(m_value).is_special_empty_value() ? js_undefined() : interpreter.get(m_value);
    interpreter.do_return(
        interpreter.do_yield(yielded_value, m_continuation_label));
}

void PrepareYield::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto value = interpreter.get(m_value).is_special_empty_value() ? js_undefined() : interpreter.get(m_value);
    interpreter.set(m_dest, interpreter.do_yield(value, {}));
}

void Await::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto yielded_value = interpreter.get(m_argument).is_special_empty_value() ? js_undefined() : interpreter.get(m_argument);
    // FIXME: If we get a pointer, which is not accurately representable as a double
    //        will cause this to explode
    auto continuation_value = Value(m_continuation_label.address());
    auto result = interpreter.vm().heap().allocate<GeneratorResult>(yielded_value, continuation_value, true);
    interpreter.do_return(result);
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

#define DEFINE_PUT_KIND_BY_VALUE(kind)                                                                 \
    ThrowCompletionOr<void> Put##kind##ByValue::execute_impl(Bytecode::Interpreter& interpreter) const \
    {                                                                                                  \
        auto& vm = interpreter.vm();                                                                   \
        auto value = interpreter.get(m_src);                                                           \
        auto base = interpreter.get(m_base);                                                           \
        auto base_identifier = interpreter.get_identifier(m_base_identifier);                          \
        auto property = interpreter.get(m_property);                                                   \
        TRY(put_by_value<PutKind::kind>(vm, base, base_identifier, property, value, strict()));        \
        return {};                                                                                     \
    }                                                                                                  \
    ByteString Put##kind##ByValue::to_byte_string_impl(Bytecode::Executable const& executable) const   \
    {                                                                                                  \
        return ByteString::formatted("Put" #kind "ByValue {}, {}, {}",                                 \
            format_operand("base"sv, m_base, executable),                                              \
            format_operand("property"sv, m_property, executable),                                      \
            format_operand("src"sv, m_src, executable));                                               \
    }

JS_ENUMERATE_PUT_KINDS(DEFINE_PUT_KIND_BY_VALUE)

#define DEFINE_PUT_KIND_BY_VALUE_WITH_THIS(kind)                                                               \
    ThrowCompletionOr<void> Put##kind##ByValueWithThis::execute_impl(Bytecode::Interpreter& interpreter) const \
    {                                                                                                          \
        auto& vm = interpreter.vm();                                                                           \
        auto value = interpreter.get(m_src);                                                                   \
        auto base = interpreter.get(m_base);                                                                   \
        auto this_value = interpreter.get(m_this_value);                                                       \
        auto property_key = TRY(interpreter.get(m_property).to_property_key(vm));                              \
        TRY(put_by_property_key<PutKind::kind>(vm, base, this_value, value, {}, property_key, strict()));      \
        return {};                                                                                             \
    }                                                                                                          \
    ByteString Put##kind##ByValueWithThis::to_byte_string_impl(Bytecode::Executable const& executable) const   \
    {                                                                                                          \
        return ByteString::formatted("Put" #kind "ByValueWithThis {}, {}, {}, {}",                             \
            format_operand("base"sv, m_base, executable),                                                      \
            format_operand("property"sv, m_property, executable),                                              \
            format_operand("src"sv, m_src, executable),                                                        \
            format_operand("this"sv, m_this_value, executable));                                               \
    }

JS_ENUMERATE_PUT_KINDS(DEFINE_PUT_KIND_BY_VALUE_WITH_THIS)

ThrowCompletionOr<void> DeleteByValue::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto property_key = TRY(interpreter.get(m_property).to_property_key(vm));
    auto reference = Reference { interpreter.get(m_base), property_key, {}, strict() };
    interpreter.set(m_dst, Value(TRY(reference.delete_(vm))));
    return {};
}

ThrowCompletionOr<void> DeleteByValueWithThis::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto property_key_value = interpreter.get(m_property);
    auto property_key = TRY(property_key_value.to_property_key(vm));
    auto reference = Reference { interpreter.get(m_base), property_key, interpreter.get(m_this_value), strict() };
    interpreter.set(dst(), Value(TRY(reference.delete_(vm))));
    return {};
}

ThrowCompletionOr<void> GetIterator::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    interpreter.set(dst(), TRY(get_iterator(vm, interpreter.get(iterable()), m_hint)));
    return {};
}

void GetObjectFromIteratorRecord::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& iterator_record = static_cast<IteratorRecord&>(interpreter.get(m_iterator_record).as_cell());
    interpreter.set(m_object, iterator_record.iterator);
}

void GetNextMethodFromIteratorRecord::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& iterator_record = static_cast<IteratorRecord&>(interpreter.get(m_iterator_record).as_cell());
    interpreter.set(m_next_method, iterator_record.next_method);
}

ThrowCompletionOr<void> GetMethod::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto identifier = interpreter.get_identifier(m_property);
    auto method = TRY(interpreter.get(m_object).get_method(vm, identifier));
    interpreter.set(dst(), method ?: js_undefined());
    return {};
}

ThrowCompletionOr<void> GetObjectPropertyIterator::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto iterator_record = TRY(get_object_property_iterator(interpreter, interpreter.get(object())));
    interpreter.set(dst(), iterator_record);
    return {};
}

ThrowCompletionOr<void> IteratorClose::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto& iterator = static_cast<IteratorRecord&>(interpreter.get(m_iterator_record).as_cell());

    // FIXME: Return the value of the resulting completion. (Note that m_completion_value can be empty!)
    TRY(iterator_close(vm, iterator, Completion { m_completion_type, m_completion_value.value_or(js_undefined()) }));
    return {};
}

ThrowCompletionOr<void> AsyncIteratorClose::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto& iterator = static_cast<IteratorRecord&>(interpreter.get(m_iterator_record).as_cell());

    // FIXME: Return the value of the resulting completion. (Note that m_completion_value can be empty!)
    TRY(async_iterator_close(vm, iterator, Completion { m_completion_type, m_completion_value.value_or(js_undefined()) }));
    return {};
}

ThrowCompletionOr<void> IteratorNext::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto& iterator_record = static_cast<IteratorRecord&>(interpreter.get(m_iterator_record).as_cell());
    interpreter.set(dst(), TRY(iterator_next(vm, iterator_record)));
    return {};
}

ThrowCompletionOr<void> IteratorNextUnpack::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& vm = interpreter.vm();
    auto& iterator_record = static_cast<IteratorRecord&>(interpreter.get(m_iterator_record).as_cell());
    auto iteration_result_or_done = TRY(iterator_step(vm, iterator_record));
    if (iteration_result_or_done.has<IterationDone>()) {
        interpreter.set(dst_done(), Value(true));
        return {};
    }
    auto& iteration_result = iteration_result_or_done.get<IterationResult>();
    interpreter.set(dst_done(), TRY(iteration_result.done));
    interpreter.set(dst_value(), TRY(iteration_result.value));
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

    if (m_cache.is_valid()) [[likely]] {
        auto const* environment = interpreter.running_execution_context().lexical_environment.ptr();
        for (size_t i = 0; i < m_cache.hops; ++i)
            environment = environment->outer_environment();
        if (!environment->is_permanently_screwed_by_eval()) [[likely]] {
            auto value = TRY(static_cast<DeclarativeEnvironment const&>(*environment).get_binding_value_direct(vm, m_cache.index));
            interpreter.set(dst(), value.typeof_(vm));
            return {};
        }
        m_cache = {};
    }

    // 1. Let val be the result of evaluating UnaryExpression.
    auto reference = TRY(vm.resolve_binding(interpreter.get_identifier(m_identifier), strict()));

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
    interpreter.set(dst(), value.typeof_(vm));
    return {};
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
    return ByteString::formatted("NewPrimitiveArray {}, {}",
        format_operand("dst"sv, dst(), executable),
        format_value_list("elements"sv, elements()));
}

ByteString AddPrivateName::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("AddPrivateName {}", executable.identifier_table->get(m_name));
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
    return ByteString::formatted("NewRegExp {}, source:\"{}\" flags:\"{}\"",
        format_operand("dst"sv, dst(), executable),
        executable.get_string(m_source_index),
        executable.get_string(m_flags_index));
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

ByteString GetInitializedBinding::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetInitializedBinding {}, {}",
        format_operand("dst"sv, dst(), executable),
        executable.identifier_table->get(m_identifier));
}

ByteString GetGlobal::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetGlobal {}, {}", format_operand("dst"sv, dst(), executable),
        executable.identifier_table->get(m_identifier));
}

ByteString SetGlobal::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("SetGlobal {}, {}",
        executable.identifier_table->get(m_identifier),
        format_operand("src"sv, src(), executable));
}

ByteString DeleteVariable::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("DeleteVariable {}", executable.identifier_table->get(m_identifier));
}

ByteString CreateLexicalEnvironment::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    if (m_dst.has_value())
        return ByteString::formatted("CreateLexicalEnvironment {}", format_operand("dst"sv, *m_dst, executable));
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

ByteString PutBySpread::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("PutBySpread {}, {}",
        format_operand("base"sv, m_base, executable),
        format_operand("src"sv, m_src, executable));
}

ByteString PutPrivateById::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted(
        "PutPrivateById {}, {}, {}",
        format_operand("base"sv, m_base, executable),
        executable.identifier_table->get(m_property),
        format_operand("src"sv, m_src, executable));
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

ByteString Call::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("Call {}, {}, {}, ",
        format_operand("dst"sv, m_dst, executable),
        format_operand("callee"sv, m_callee, executable),
        format_operand("this"sv, m_this_value, executable));

    builder.append(format_operand_list("args"sv, { m_arguments, m_argument_count }, executable));

    if (m_expression_string.has_value()) {
        builder.appendff(", `{}`", executable.get_string(m_expression_string.value()));
    }

    return builder.to_byte_string();
}

ByteString CallConstruct::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("CallConstruct {}, {}, ",
        format_operand("dst"sv, m_dst, executable),
        format_operand("callee"sv, m_callee, executable));

    builder.append(format_operand_list("args"sv, { m_arguments, m_argument_count }, executable));

    if (m_expression_string.has_value()) {
        builder.appendff(", `{}`", executable.get_string(m_expression_string.value()));
    }

    return builder.to_byte_string();
}

ByteString CallDirectEval::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("CallDirectEval {}, {}, {}, ",
        format_operand("dst"sv, m_dst, executable),
        format_operand("callee"sv, m_callee, executable),
        format_operand("this"sv, m_this_value, executable));

    builder.append(format_operand_list("args"sv, { m_arguments, m_argument_count }, executable));

    if (m_expression_string.has_value()) {
        builder.appendff(", `{}`", executable.get_string(m_expression_string.value()));
    }

    return builder.to_byte_string();
}

ByteString CallBuiltin::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("CallBuiltin {}, {}, {}, ",
        format_operand("dst"sv, m_dst, executable),
        format_operand("callee"sv, m_callee, executable),
        format_operand("this"sv, m_this_value, executable));

    builder.append(format_operand_list("args"sv, { m_arguments, m_argument_count }, executable));

    builder.appendff(", (builtin:{})", m_builtin);

    if (m_expression_string.has_value()) {
        builder.appendff(", `{}`", executable.get_string(m_expression_string.value()));
    }

    return builder.to_byte_string();
}

ByteString CallWithArgumentArray::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("CallWithArgumentArray {}, {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("callee"sv, m_callee, executable),
        format_operand("this"sv, m_this_value, executable),
        format_operand("arguments"sv, m_arguments, executable));

    if (m_expression_string.has_value())
        builder.appendff(" ({})", executable.get_string(m_expression_string.value()));
    return builder.to_byte_string();
}

ByteString CallDirectEvalWithArgumentArray::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("CallDirectEvalWithArgumentArray {}, {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("callee"sv, m_callee, executable),
        format_operand("this"sv, m_this_value, executable),
        format_operand("arguments"sv, m_arguments, executable));

    if (m_expression_string.has_value())
        builder.appendff(" ({})", executable.get_string(m_expression_string.value()));
    return builder.to_byte_string();
}

ByteString CallConstructWithArgumentArray::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    StringBuilder builder;
    builder.appendff("CallConstructWithArgumentArray {}, {}, {}",
        format_operand("dst"sv, m_dst, executable),
        format_operand("callee"sv, m_callee, executable),
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
        builder.appendff(" name:{}", m_function_node.name());
    if (m_lhs_name.has_value())
        builder.appendff(" lhs_name:{}", executable.get_identifier(m_lhs_name.value()));
    if (m_home_object.has_value())
        builder.appendff(", {}", format_operand("home_object"sv, m_home_object.value(), executable));
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
        builder.appendff(", lhs_name:{}", executable.get_identifier(m_lhs_name.value()));
    return builder.to_byte_string();
}

ByteString Return::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("Return {}", format_operand("value"sv, m_value, executable));
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

ByteString IteratorNextUnpack::to_byte_string_impl(Executable const& executable) const
{
    return ByteString::formatted("IteratorNextUnpack {}, {}, {}",
        format_operand("dst_value"sv, m_dst_value, executable),
        format_operand("dst_done"sv, m_dst_done, executable),
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

void GetCompletionFields::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto const& completion_cell = static_cast<CompletionCell const&>(interpreter.get(m_completion).as_cell());
    interpreter.set(m_value_dst, completion_cell.completion().value());
    interpreter.set(m_type_dst, Value(to_underlying(completion_cell.completion().type())));
}

ByteString GetCompletionFields::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("GetCompletionFields {}, {}, {}",
        format_operand("value_dst"sv, m_value_dst, executable),
        format_operand("type_dst"sv, m_type_dst, executable),
        format_operand("completion"sv, m_completion, executable));
}

void SetCompletionType::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& completion_cell = static_cast<CompletionCell&>(interpreter.get(m_completion).as_cell());
    auto completion = completion_cell.completion();
    completion_cell.set_completion(Completion { m_type, completion.value() });
}

ByteString SetCompletionType::to_byte_string_impl(Bytecode::Executable const& executable) const
{
    return ByteString::formatted("SetCompletionType {}, type={}",
        format_operand("completion"sv, m_completion, executable),
        to_underlying(m_type));
}

ByteString CreateImmutableBinding::to_byte_string_impl(Executable const& executable) const
{
    return ByteString::formatted("CreateImmutableBinding {} {} (strict: {})",
        format_operand("environment"sv, m_environment, executable),
        executable.get_identifier(m_identifier),
        m_strict);
}

ThrowCompletionOr<void> CreateImmutableBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& environment = as<Environment>(interpreter.get(m_environment).as_cell());
    return environment.create_immutable_binding(interpreter.vm(), interpreter.get_identifier(m_identifier), m_strict);
}

ByteString CreateMutableBinding::to_byte_string_impl(Executable const& executable) const
{
    return ByteString::formatted("CreateMutableBinding {} {} (can_be_deleted: {})",
        format_operand("environment"sv, m_environment, executable),
        executable.get_identifier(m_identifier),
        m_can_be_deleted);
}

ThrowCompletionOr<void> CreateMutableBinding::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto& environment = as<Environment>(interpreter.get(m_environment).as_cell());
    return environment.create_mutable_binding(interpreter.vm(), interpreter.get_identifier(m_identifier), m_can_be_deleted);
}

}
