/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Bytecode/AsmInterpreter/AsmInterpreter.h>
#include <LibJS/Bytecode/Builtins.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Bytecode/Op.h>
#include <LibJS/Bytecode/PropertyAccess.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/ModuleEnvironment.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Reference.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <math.h>
#include <stdlib.h>

// ===== Slow path hit counters (for profiling) =====
// Define JS_ASMINT_SLOW_PATH_COUNTERS to enable per-opcode slow path
// counters. They are printed on exit when the asm interpreter is active.
#ifdef JS_ASMINT_SLOW_PATH_COUNTERS
static struct AsmSlowPathStats {
    u64 fallback_by_type[256] {};
    u64 slow_path_by_type[256] {};
    bool registered {};
} s_stats;

static void print_asm_slow_path_stats()
{
    if (getenv("LIBJS_USE_CPP_INTERPRETER"))
        return;

    fprintf(stderr, "\n=== AsmInterpreter slow path stats ===\n");

    static char const* const s_type_names[] = {
#    define __BYTECODE_OP(op) #op,
        ENUMERATE_BYTECODE_OPS(__BYTECODE_OP)
#    undef __BYTECODE_OP
    };

    struct Entry {
        char const* name;
        u64 count;
    };
    Entry entries[512];
    size_t num_entries = 0;

    for (size_t i = 0; i < 256; ++i) {
        if (s_stats.fallback_by_type[i] > 0)
            entries[num_entries++] = { s_type_names[i], s_stats.fallback_by_type[i] };
    }

    for (size_t i = 0; i < 256; ++i) {
        if (s_stats.slow_path_by_type[i] > 0)
            entries[num_entries++] = { s_type_names[i], s_stats.slow_path_by_type[i] };
    }

    // Bubble sort by count descending (small array, no need for qsort)
    for (size_t i = 0; i < num_entries; ++i)
        for (size_t j = i + 1; j < num_entries; ++j)
            if (entries[j].count > entries[i].count) {
                auto tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }

    for (size_t i = 0; i < num_entries; ++i)
        fprintf(stderr, "  %12llu  %s\n", static_cast<unsigned long long>(entries[i].count), entries[i].name);

    fprintf(stderr, "===\n\n");
}
#endif

namespace JS::Bytecode {

// The asm interpreter is available on x86_64 (non-Windows) and AArch64.
// Win64 uses a different ABI that the x86_64 backend doesn't support yet.
#if ARCH(AARCH64)
#    define HAS_ASM_INTERPRETER 1
#elif ARCH(X86_64) && !defined(_WIN32)
#    define HAS_ASM_INTERPRETER 1
#else
#    define HAS_ASM_INTERPRETER 0
#endif

#if HAS_ASM_INTERPRETER
// Defined in generated assembly (asmint_x86_64.S or asmint_aarch64.S)
extern "C" void asm_interpreter_entry(u8 const* bytecode, u32 entry_point, Value* values, Interpreter* interp);
#endif

bool AsmInterpreter::is_available()
{
    return HAS_ASM_INTERPRETER;
}

void AsmInterpreter::run(Interpreter& interp, [[maybe_unused]] size_t entry_point)
{
#if !HAS_ASM_INTERPRETER
    (void)interp;
    VERIFY_NOT_REACHED();
#else
#    ifdef JS_ASMINT_SLOW_PATH_COUNTERS
    if (!s_stats.registered) {
        s_stats.registered = true;
        atexit(print_asm_slow_path_stats);
    }
#    endif

    auto& context = interp.running_execution_context();
    auto* bytecode = context.executable->bytecode.data();
    auto* values = context.registers_and_constants_and_locals_and_arguments_span().data();

    asm_interpreter_entry(bytecode, static_cast<u32>(entry_point), values, &interp);
#endif
}

}

// ===== Slow path functions callable from assembly =====
// All slow path functions follow the same convention:
//   i64 func(Interpreter* interp, u32 pc)
//   Returns >= 0: new program counter to dispatch to
//   Returns < 0: should exit the asm interpreter

using namespace JS;
using namespace JS::Bytecode;

static i64 handle_asm_exception(Interpreter& interp, u32 pc, Value exception)
{
    auto response = interp.handle_exception(pc, exception);
    if (response == Interpreter::HandleExceptionResponse::ExitFromExecutable)
        return -1;
    // ContinueInThisExecutable: new pc is in the execution context
    return static_cast<i64>(interp.running_execution_context().program_counter);
}

// Helper: execute a throwing instruction and handle errors
template<typename InsnType>
static i64 execute_throwing(Interpreter& interp, u32 pc)
{
    interp.running_execution_context().program_counter = pc;
    auto* bytecode = interp.current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<InsnType const*>(&bytecode[pc]);
    auto result = insn.execute_impl(interp);
    if (result.is_error()) [[unlikely]]
        return handle_asm_exception(interp, pc, result.error_value());
    if constexpr (InsnType::IsVariableLength)
        return static_cast<i64>(pc + insn.length());
    else
        return static_cast<i64>(pc + sizeof(InsnType));
}

// Helper: execute a non-throwing instruction
template<typename InsnType>
static i64 execute_nonthrowing(Interpreter& interp, u32 pc)
{
    interp.running_execution_context().program_counter = pc;
    auto* bytecode = interp.current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<InsnType const*>(&bytecode[pc]);
    insn.execute_impl(interp);
    if constexpr (InsnType::IsVariableLength)
        return static_cast<i64>(pc + insn.length());
    else
        return static_cast<i64>(pc + sizeof(InsnType));
}

// Slow path wrappers: optionally bump per-opcode counter, then delegate.
#ifdef JS_ASMINT_SLOW_PATH_COUNTERS
template<typename InsnType>
ALWAYS_INLINE static i64 slow_path_throwing(Interpreter& interp, u32 pc)
{
    ++s_stats.slow_path_by_type[static_cast<u8>(Instruction::Type { interp.current_executable().bytecode[pc] })];
    return execute_throwing<InsnType>(interp, pc);
}

template<typename InsnType>
ALWAYS_INLINE static i64 slow_path_nonthrowing(Interpreter& interp, u32 pc)
{
    ++s_stats.slow_path_by_type[static_cast<u8>(Instruction::Type { interp.current_executable().bytecode[pc] })];
    return execute_nonthrowing<InsnType>(interp, pc);
}

ALWAYS_INLINE static void bump_slow_path(Interpreter& interp, u32 pc)
{
    ++s_stats.slow_path_by_type[static_cast<u8>(Instruction::Type { interp.current_executable().bytecode[pc] })];
}
#else
template<typename InsnType>
ALWAYS_INLINE static i64 slow_path_throwing(Interpreter& interp, u32 pc)
{
    return execute_throwing<InsnType>(interp, pc);
}

template<typename InsnType>
ALWAYS_INLINE static i64 slow_path_nonthrowing(Interpreter& interp, u32 pc)
{
    return execute_nonthrowing<InsnType>(interp, pc);
}

ALWAYS_INLINE static void bump_slow_path(Interpreter&, u32) { }
#endif

extern "C" {

// Forward declarations for all functions called from assembly.
i64 asm_fallback_handler(Interpreter*, u32 pc);
i64 asm_slow_path_add(Interpreter*, u32 pc);
i64 asm_slow_path_sub(Interpreter*, u32 pc);
i64 asm_slow_path_mul(Interpreter*, u32 pc);
i64 asm_slow_path_div(Interpreter*, u32 pc);
i64 asm_slow_path_increment(Interpreter*, u32 pc);
i64 asm_slow_path_decrement(Interpreter*, u32 pc);
i64 asm_slow_path_less_than(Interpreter*, u32 pc);
i64 asm_slow_path_less_than_equals(Interpreter*, u32 pc);
i64 asm_slow_path_greater_than(Interpreter*, u32 pc);
i64 asm_slow_path_greater_than_equals(Interpreter*, u32 pc);
i64 asm_slow_path_jump_less_than(Interpreter*, u32 pc);
i64 asm_slow_path_jump_greater_than(Interpreter*, u32 pc);
i64 asm_slow_path_jump_less_than_equals(Interpreter*, u32 pc);
i64 asm_slow_path_jump_greater_than_equals(Interpreter*, u32 pc);
i64 asm_slow_path_jump_loosely_equals(Interpreter*, u32 pc);
i64 asm_slow_path_jump_loosely_inequals(Interpreter*, u32 pc);
i64 asm_slow_path_jump_strictly_equals(Interpreter*, u32 pc);
i64 asm_slow_path_jump_strictly_inequals(Interpreter*, u32 pc);
i64 asm_slow_path_set_lexical_environment(Interpreter*, u32 pc);
i64 asm_slow_path_postfix_increment(Interpreter*, u32 pc);
i64 asm_slow_path_get_by_id(Interpreter*, u32 pc);
i64 asm_slow_path_put_by_id(Interpreter*, u32 pc);
i64 asm_slow_path_get_by_value(Interpreter*, u32 pc);
i64 asm_slow_path_get_length(Interpreter*, u32 pc);
i64 asm_try_get_global_env_binding(Interpreter*, u32 pc);
i64 asm_try_set_global_env_binding(Interpreter*, u32 pc);
i64 asm_slow_path_get_global(Interpreter*, u32 pc);
i64 asm_slow_path_set_global(Interpreter*, u32 pc);
i64 asm_slow_path_call(Interpreter*, u32 pc);
#define DECLARE_CALL_BUILTIN_SLOW_PATH(name, snake_case_name, ...) \
    i64 asm_slow_path_call_builtin_##snake_case_name(Interpreter*, u32 pc);
JS_ENUMERATE_BUILTINS(DECLARE_CALL_BUILTIN_SLOW_PATH)
#undef DECLARE_CALL_BUILTIN_SLOW_PATH
i64 asm_slow_path_get_object_property_iterator(Interpreter*, u32 pc);
i64 asm_slow_path_object_property_iterator_next(Interpreter*, u32 pc);
i64 asm_slow_path_call_construct(Interpreter*, u32 pc);
i64 asm_slow_path_new_object(Interpreter*, u32 pc);
i64 asm_slow_path_cache_object_shape(Interpreter*, u32 pc);
i64 asm_slow_path_init_object_literal_property(Interpreter*, u32 pc);
i64 asm_slow_path_new_array(Interpreter*, u32 pc);
i64 asm_slow_path_bitwise_xor(Interpreter*, u32 pc);
i64 asm_slow_path_bitwise_and(Interpreter*, u32 pc);
i64 asm_slow_path_bitwise_or(Interpreter*, u32 pc);
i64 asm_slow_path_left_shift(Interpreter*, u32 pc);
i64 asm_slow_path_right_shift(Interpreter*, u32 pc);
i64 asm_slow_path_unsigned_right_shift(Interpreter*, u32 pc);
i64 asm_slow_path_mod(Interpreter*, u32 pc);
i64 asm_slow_path_strictly_equals(Interpreter*, u32 pc);
i64 asm_slow_path_strictly_inequals(Interpreter*, u32 pc);
i64 asm_slow_path_unary_minus(Interpreter*, u32 pc);
i64 asm_slow_path_postfix_decrement(Interpreter*, u32 pc);
i64 asm_slow_path_to_int32(Interpreter*, u32 pc);
i64 asm_slow_path_put_by_value(Interpreter*, u32 pc);
i64 asm_try_put_by_value_holey_array(Interpreter*, u32 pc);
u64 asm_helper_to_boolean(u64 encoded_value);
u64 asm_helper_math_exp(u64 encoded_value);
u64 asm_helper_empty_string(u64);
u64 asm_helper_single_ascii_character_string(u64 encoded_value);
u64 asm_helper_single_utf16_code_unit_string(u64 encoded_value);
i64 asm_try_inline_call(Interpreter*, u32 pc);
i64 asm_pop_inline_frame(Interpreter*, u32 pc);
i64 asm_pop_inline_frame_end(Interpreter*, u32 pc);
i64 asm_try_put_by_id_cache(Interpreter*, u32 pc);
i64 asm_try_get_by_id_cache(Interpreter*, u32 pc);
i64 asm_slow_path_initialize_lexical_binding(Interpreter*, u32 pc);

i64 asm_try_get_by_value_typed_array(Interpreter*, u32 pc);
i64 asm_slow_path_get_initialized_binding(Interpreter*, u32 pc);
i64 asm_slow_path_get_binding(Interpreter*, u32 pc);
i64 asm_slow_path_set_lexical_binding(Interpreter*, u32 pc);
i64 asm_slow_path_bitwise_not(Interpreter*, u32 pc);
i64 asm_slow_path_unary_plus(Interpreter*, u32 pc);
i64 asm_slow_path_throw_if_tdz(Interpreter*, u32 pc);
i64 asm_slow_path_throw_if_not_object(Interpreter*, u32 pc);
i64 asm_slow_path_throw_if_nullish(Interpreter*, u32 pc);
i64 asm_slow_path_loosely_equals(Interpreter*, u32 pc);
i64 asm_slow_path_loosely_inequals(Interpreter*, u32 pc);
i64 asm_slow_path_get_callee_and_this(Interpreter*, u32 pc);
i64 asm_try_put_by_value_typed_array(Interpreter*, u32 pc);
i64 asm_slow_path_get_private_by_id(Interpreter*, u32 pc);
i64 asm_slow_path_put_private_by_id(Interpreter*, u32 pc);
i64 asm_slow_path_instance_of(Interpreter*, u32 pc);
i64 asm_slow_path_resolve_this_binding(Interpreter*, u32 pc);

// ===== Fallback handler for opcodes without DSL handlers =====
// NB: Opcodes with DSL handlers are dispatched directly and never reach here.
i64 asm_fallback_handler(Interpreter* interp, u32 pc)
{
    auto& ctx = interp->running_execution_context();
    ctx.program_counter = pc;
    auto* bytecode = ctx.executable->bytecode.data();
    auto& insn = *reinterpret_cast<Instruction const*>(&bytecode[pc]);
#ifdef JS_ASMINT_SLOW_PATH_COUNTERS
    ++s_stats.fallback_by_type[to_underlying(insn.type())];
#endif

    switch (insn.type()) {
    // Terminators
    case Instruction::Type::Throw: {
        auto& typed = *reinterpret_cast<Op::Throw const*>(&bytecode[pc]);
        auto result = typed.execute_impl(*interp);
        return handle_asm_exception(*interp, pc, result.error_value());
    }
    case Instruction::Type::Await: {
        auto& typed = *reinterpret_cast<Op::Await const*>(&bytecode[pc]);
        typed.execute_impl(*interp);
        return -1;
    }
    case Instruction::Type::Yield: {
        auto& typed = *reinterpret_cast<Op::Yield const*>(&bytecode[pc]);
        typed.execute_impl(*interp);
        return -1;
    }

    // Non-throwing instructions
    case Instruction::Type::AddPrivateName:
        return execute_nonthrowing<Op::AddPrivateName>(*interp, pc);
    case Instruction::Type::Catch:
        return execute_nonthrowing<Op::Catch>(*interp, pc);
    case Instruction::Type::CreateAsyncFromSyncIterator:
        return execute_nonthrowing<Op::CreateAsyncFromSyncIterator>(*interp, pc);
    case Instruction::Type::CreateLexicalEnvironment:
        return execute_nonthrowing<Op::CreateLexicalEnvironment>(*interp, pc);
    case Instruction::Type::CreateVariableEnvironment:
        return execute_nonthrowing<Op::CreateVariableEnvironment>(*interp, pc);
    case Instruction::Type::CreatePrivateEnvironment:
        return execute_nonthrowing<Op::CreatePrivateEnvironment>(*interp, pc);
    case Instruction::Type::CreateRestParams:
        return execute_nonthrowing<Op::CreateRestParams>(*interp, pc);
    case Instruction::Type::CreateArguments:
        return execute_nonthrowing<Op::CreateArguments>(*interp, pc);
    case Instruction::Type::GetCompletionFields:
        return execute_nonthrowing<Op::GetCompletionFields>(*interp, pc);
    case Instruction::Type::GetImportMeta:
        return execute_nonthrowing<Op::GetImportMeta>(*interp, pc);
    case Instruction::Type::GetNewTarget:
        return execute_nonthrowing<Op::GetNewTarget>(*interp, pc);
    case Instruction::Type::GetTemplateObject:
        return execute_nonthrowing<Op::GetTemplateObject>(*interp, pc);
    case Instruction::Type::IsCallable:
        return execute_nonthrowing<Op::IsCallable>(*interp, pc);
    case Instruction::Type::IsConstructor:
        return execute_nonthrowing<Op::IsConstructor>(*interp, pc);
    case Instruction::Type::LeavePrivateEnvironment:
        return execute_nonthrowing<Op::LeavePrivateEnvironment>(*interp, pc);
    case Instruction::Type::NewFunction:
        return execute_nonthrowing<Op::NewFunction>(*interp, pc);
    case Instruction::Type::NewObjectWithNoPrototype:
        return execute_nonthrowing<Op::NewObjectWithNoPrototype>(*interp, pc);
    case Instruction::Type::NewPrimitiveArray:
        return execute_nonthrowing<Op::NewPrimitiveArray>(*interp, pc);
    case Instruction::Type::NewRegExp:
        return execute_nonthrowing<Op::NewRegExp>(*interp, pc);
    case Instruction::Type::NewReferenceError:
        return execute_nonthrowing<Op::NewReferenceError>(*interp, pc);
    case Instruction::Type::NewTypeError:
        return execute_nonthrowing<Op::NewTypeError>(*interp, pc);
    case Instruction::Type::SetCompletionType:
        return execute_nonthrowing<Op::SetCompletionType>(*interp, pc);
    case Instruction::Type::ToBoolean:
        return execute_nonthrowing<Op::ToBoolean>(*interp, pc);
    case Instruction::Type::Typeof:
        return execute_nonthrowing<Op::Typeof>(*interp, pc);

    // Throwing instructions
    case Instruction::Type::ArrayAppend:
        return execute_throwing<Op::ArrayAppend>(*interp, pc);
    case Instruction::Type::ToString:
        return execute_throwing<Op::ToString>(*interp, pc);
    case Instruction::Type::ToPrimitiveWithStringHint:
        return execute_throwing<Op::ToPrimitiveWithStringHint>(*interp, pc);
    case Instruction::Type::CallConstructWithArgumentArray:
        return execute_throwing<Op::CallConstructWithArgumentArray>(*interp, pc);
    case Instruction::Type::CallDirectEval:
        return execute_throwing<Op::CallDirectEval>(*interp, pc);
    case Instruction::Type::CallDirectEvalWithArgumentArray:
        return execute_throwing<Op::CallDirectEvalWithArgumentArray>(*interp, pc);
    case Instruction::Type::CallWithArgumentArray:
        return execute_throwing<Op::CallWithArgumentArray>(*interp, pc);
    case Instruction::Type::ConcatString:
        return execute_throwing<Op::ConcatString>(*interp, pc);
    case Instruction::Type::CopyObjectExcludingProperties:
        return execute_throwing<Op::CopyObjectExcludingProperties>(*interp, pc);
    case Instruction::Type::CreateDataPropertyOrThrow:
        return execute_throwing<Op::CreateDataPropertyOrThrow>(*interp, pc);
    case Instruction::Type::CreateImmutableBinding:
        return execute_throwing<Op::CreateImmutableBinding>(*interp, pc);
    case Instruction::Type::CreateMutableBinding:
        return execute_throwing<Op::CreateMutableBinding>(*interp, pc);
    case Instruction::Type::CreateVariable:
        return execute_throwing<Op::CreateVariable>(*interp, pc);
    case Instruction::Type::DeleteById:
        return execute_throwing<Op::DeleteById>(*interp, pc);
    case Instruction::Type::DeleteByValue:
        return execute_throwing<Op::DeleteByValue>(*interp, pc);
    case Instruction::Type::DeleteVariable:
        return execute_throwing<Op::DeleteVariable>(*interp, pc);
    case Instruction::Type::EnterObjectEnvironment:
        return execute_throwing<Op::EnterObjectEnvironment>(*interp, pc);
    case Instruction::Type::Exp:
        return execute_throwing<Op::Exp>(*interp, pc);
    case Instruction::Type::GetByIdWithThis:
        return execute_throwing<Op::GetByIdWithThis>(*interp, pc);
    case Instruction::Type::GetByValueWithThis:
        return execute_throwing<Op::GetByValueWithThis>(*interp, pc);
    case Instruction::Type::GetIterator:
        return execute_throwing<Op::GetIterator>(*interp, pc);
    case Instruction::Type::GetLengthWithThis:
        return execute_throwing<Op::GetLengthWithThis>(*interp, pc);
    case Instruction::Type::GetMethod:
        return execute_throwing<Op::GetMethod>(*interp, pc);
    case Instruction::Type::GetObjectPropertyIterator:
        return execute_throwing<Op::GetObjectPropertyIterator>(*interp, pc);
    case Instruction::Type::ObjectPropertyIteratorNext:
        return execute_throwing<Op::ObjectPropertyIteratorNext>(*interp, pc);
    case Instruction::Type::HasPrivateId:
        return execute_throwing<Op::HasPrivateId>(*interp, pc);
    case Instruction::Type::ImportCall:
        return execute_throwing<Op::ImportCall>(*interp, pc);
    case Instruction::Type::In:
        return execute_throwing<Op::In>(*interp, pc);
    case Instruction::Type::InitializeVariableBinding:
        return execute_throwing<Op::InitializeVariableBinding>(*interp, pc);
    case Instruction::Type::IteratorClose:
        return execute_throwing<Op::IteratorClose>(*interp, pc);
    case Instruction::Type::IteratorNext:
        return execute_throwing<Op::IteratorNext>(*interp, pc);
    case Instruction::Type::IteratorNextUnpack:
        return execute_throwing<Op::IteratorNextUnpack>(*interp, pc);
    case Instruction::Type::IteratorToArray:
        return execute_throwing<Op::IteratorToArray>(*interp, pc);
    case Instruction::Type::NewArrayWithLength:
        return execute_throwing<Op::NewArrayWithLength>(*interp, pc);
    case Instruction::Type::NewClass:
        return execute_throwing<Op::NewClass>(*interp, pc);
    case Instruction::Type::PutByIdWithThis:
        return execute_throwing<Op::PutByIdWithThis>(*interp, pc);
    case Instruction::Type::PutBySpread:
        return execute_throwing<Op::PutBySpread>(*interp, pc);
    case Instruction::Type::PutByValueWithThis:
        return execute_throwing<Op::PutByValueWithThis>(*interp, pc);
    case Instruction::Type::ResolveSuperBase:
        return execute_throwing<Op::ResolveSuperBase>(*interp, pc);
    case Instruction::Type::SetVariableBinding:
        return execute_throwing<Op::SetVariableBinding>(*interp, pc);
    case Instruction::Type::SuperCallWithArgumentArray:
        return execute_throwing<Op::SuperCallWithArgumentArray>(*interp, pc);
    case Instruction::Type::ThrowConstAssignment:
        return execute_throwing<Op::ThrowConstAssignment>(*interp, pc);
    case Instruction::Type::ToLength:
        return execute_throwing<Op::ToLength>(*interp, pc);
    case Instruction::Type::ToObject:
        return execute_throwing<Op::ToObject>(*interp, pc);
    case Instruction::Type::TypeofBinding:
        return execute_throwing<Op::TypeofBinding>(*interp, pc);

    default:
        VERIFY_NOT_REACHED();
    }
}

// ===== Specific slow paths for asm-optimized instructions =====
// These are called from asm handlers when the fast path fails.
// Convention: i64 func(Interpreter*, u32 pc)
//   Returns >= 0: new pc
//   Returns < 0: exit

i64 asm_slow_path_add(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::Add>(*interp, pc);
}

i64 asm_slow_path_sub(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::Sub>(*interp, pc);
}

i64 asm_slow_path_mul(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::Mul>(*interp, pc);
}

i64 asm_slow_path_div(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::Div>(*interp, pc);
}

i64 asm_slow_path_less_than(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::LessThan>(*interp, pc);
}

i64 asm_slow_path_less_than_equals(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::LessThanEquals>(*interp, pc);
}

i64 asm_slow_path_greater_than(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::GreaterThan>(*interp, pc);
}

i64 asm_slow_path_greater_than_equals(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::GreaterThanEquals>(*interp, pc);
}

i64 asm_slow_path_increment(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::Increment>(*interp, pc);
}

i64 asm_slow_path_decrement(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::Decrement>(*interp, pc);
}

// Comparison jump slow paths - these are terminators (execute_impl returns void),
// so they need custom handling instead of the generic slow_path_throwing template.
#define DEFINE_JUMP_COMPARISON_SLOW_PATH(snake_name, op_name, compare_call)      \
    i64 asm_slow_path_jump_##snake_name(Interpreter* interp, u32 pc)             \
    {                                                                            \
        bump_slow_path(*interp, pc);                                             \
        auto* bytecode = interp->current_executable().bytecode.data();           \
        auto& insn = *reinterpret_cast<Op::Jump##op_name const*>(&bytecode[pc]); \
        auto lhs = interp->get(insn.lhs());                                      \
        auto rhs = interp->get(insn.rhs());                                      \
        auto result = compare_call;                                              \
        if (result.is_error()) [[unlikely]]                                      \
            return handle_asm_exception(*interp, pc, result.error_value());      \
        if (result.value())                                                      \
            return static_cast<i64>(insn.true_target().address());               \
        return static_cast<i64>(insn.false_target().address());                  \
    }

DEFINE_JUMP_COMPARISON_SLOW_PATH(less_than, LessThan, less_than(Interpreter::vm(), lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(greater_than, GreaterThan, greater_than(Interpreter::vm(), lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(less_than_equals, LessThanEquals, less_than_equals(Interpreter::vm(), lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(greater_than_equals, GreaterThanEquals, greater_than_equals(Interpreter::vm(), lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(loosely_equals, LooselyEquals, is_loosely_equal(Interpreter::vm(), lhs, rhs))
#undef DEFINE_JUMP_COMPARISON_SLOW_PATH

i64 asm_slow_path_jump_loosely_inequals(Interpreter* interp, u32 pc)
{
    bump_slow_path(*interp, pc);
    auto* bytecode = interp->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::JumpLooselyInequals const*>(&bytecode[pc]);
    auto lhs = interp->get(insn.lhs());
    auto rhs = interp->get(insn.rhs());
    auto result = is_loosely_equal(Interpreter::vm(), lhs, rhs);
    if (result.is_error()) [[unlikely]]
        return handle_asm_exception(*interp, pc, result.error_value());
    if (!result.value())
        return static_cast<i64>(insn.true_target().address());
    return static_cast<i64>(insn.false_target().address());
}

i64 asm_slow_path_jump_strictly_equals(Interpreter* interp, u32 pc)
{
    bump_slow_path(*interp, pc);
    auto* bytecode = interp->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::JumpStrictlyEquals const*>(&bytecode[pc]);
    auto lhs = interp->get(insn.lhs());
    auto rhs = interp->get(insn.rhs());
    if (is_strictly_equal(lhs, rhs))
        return static_cast<i64>(insn.true_target().address());
    return static_cast<i64>(insn.false_target().address());
}

i64 asm_slow_path_jump_strictly_inequals(Interpreter* interp, u32 pc)
{
    bump_slow_path(*interp, pc);
    auto* bytecode = interp->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::JumpStrictlyInequals const*>(&bytecode[pc]);
    auto lhs = interp->get(insn.lhs());
    auto rhs = interp->get(insn.rhs());
    if (!is_strictly_equal(lhs, rhs))
        return static_cast<i64>(insn.true_target().address());
    return static_cast<i64>(insn.false_target().address());
}

// ===== Dedicated slow paths for hot instructions =====

i64 asm_slow_path_set_lexical_environment(Interpreter* interp, u32 pc)
{
    return slow_path_nonthrowing<Op::SetLexicalEnvironment>(*interp, pc);
}

i64 asm_slow_path_get_initialized_binding(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::GetInitializedBinding>(*interp, pc);
}

i64 asm_slow_path_loosely_equals(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::LooselyEquals>(*interp, pc);
}

i64 asm_slow_path_loosely_inequals(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::LooselyInequals>(*interp, pc);
}

i64 asm_slow_path_get_callee_and_this(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::GetCalleeAndThisFromEnvironment>(*interp, pc);
}

i64 asm_slow_path_postfix_increment(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::PostfixIncrement>(*interp, pc);
}

i64 asm_slow_path_get_by_id(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::GetById>(*interp, pc);
}

i64 asm_slow_path_put_by_id(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::PutById>(*interp, pc);
}

i64 asm_slow_path_get_by_value(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::GetByValue>(*interp, pc);
}

i64 asm_slow_path_get_length(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::GetLength>(*interp, pc);
}

i64 asm_try_get_global_env_binding(Interpreter* interp, u32 pc)
{
    auto* bytecode = interp->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetGlobal const*>(&bytecode[pc]);
    auto& cache = *bit_cast<GlobalVariableCache*>(insn.cache());

    if (!cache.has_environment_binding_index) [[unlikely]]
        return 1;

    auto& vm = Interpreter::vm();
    ThrowCompletionOr<Value> result = js_undefined();
    if (cache.in_module_environment) {
        auto module = vm.running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>();
        if (!module) [[unlikely]]
            return 1;
        result = (*module)->environment()->get_binding_value_direct(vm, cache.environment_binding_index);
    } else {
        result = interp->global_declarative_environment().get_binding_value_direct(vm, cache.environment_binding_index);
    }
    if (result.is_error()) [[unlikely]]
        return 1;
    interp->set(insn.dst(), result.value());
    return 0;
}

i64 asm_slow_path_get_global(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::GetGlobal>(*interp, pc);
}

i64 asm_try_set_global_env_binding(Interpreter* interp, u32 pc)
{
    auto* bytecode = interp->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::SetGlobal const*>(&bytecode[pc]);
    auto& cache = *bit_cast<GlobalVariableCache*>(insn.cache());

    if (!cache.has_environment_binding_index) [[unlikely]]
        return 1;

    auto& vm = Interpreter::vm();
    auto src = interp->get(insn.src());
    ThrowCompletionOr<void> result;
    if (cache.in_module_environment) {
        auto module = vm.running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>();
        if (!module) [[unlikely]]
            return 1;
        result = (*module)->environment()->set_mutable_binding_direct(vm, cache.environment_binding_index, src, insn.strict() == Strict::Yes);
    } else {
        result = interp->global_declarative_environment().set_mutable_binding_direct(vm, cache.environment_binding_index, src, insn.strict() == Strict::Yes);
    }
    if (result.is_error()) [[unlikely]]
        return 1;
    return 0;
}

i64 asm_slow_path_set_global(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::SetGlobal>(*interp, pc);
}

i64 asm_slow_path_call(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::Call>(*interp, pc);
}

i64 asm_slow_path_get_object_property_iterator(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::GetObjectPropertyIterator>(*interp, pc);
}

i64 asm_slow_path_object_property_iterator_next(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::ObjectPropertyIteratorNext>(*interp, pc);
}

#define DEFINE_CALL_BUILTIN_SLOW_PATH(name, snake_case_name, ...)                 \
    i64 asm_slow_path_call_builtin_##snake_case_name(Interpreter* interp, u32 pc) \
    {                                                                             \
        return slow_path_throwing<Op::CallBuiltin##name>(*interp, pc);            \
    }
JS_ENUMERATE_BUILTINS(DEFINE_CALL_BUILTIN_SLOW_PATH)
#undef DEFINE_CALL_BUILTIN_SLOW_PATH

i64 asm_slow_path_call_construct(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::CallConstruct>(*interp, pc);
}

i64 asm_slow_path_new_object(Interpreter* interp, u32 pc)
{
    return slow_path_nonthrowing<Op::NewObject>(*interp, pc);
}

i64 asm_slow_path_cache_object_shape(Interpreter* interp, u32 pc)
{
    return slow_path_nonthrowing<Op::CacheObjectShape>(*interp, pc);
}

i64 asm_slow_path_init_object_literal_property(Interpreter* interp, u32 pc)
{
    return slow_path_nonthrowing<Op::InitObjectLiteralProperty>(*interp, pc);
}

i64 asm_slow_path_new_array(Interpreter* interp, u32 pc)
{
    bump_slow_path(*interp, pc);
    auto* bytecode = interp->current_executable().bytecode.data();
    auto& typed = *reinterpret_cast<Op::NewArray const*>(&bytecode[pc]);
    typed.execute_impl(*interp);
    return static_cast<i64>(pc + typed.length());
}

i64 asm_slow_path_bitwise_xor(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::BitwiseXor>(*interp, pc);
}

i64 asm_slow_path_bitwise_and(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::BitwiseAnd>(*interp, pc);
}

i64 asm_slow_path_bitwise_or(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::BitwiseOr>(*interp, pc);
}

i64 asm_slow_path_left_shift(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::LeftShift>(*interp, pc);
}

i64 asm_slow_path_right_shift(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::RightShift>(*interp, pc);
}

i64 asm_slow_path_unsigned_right_shift(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::UnsignedRightShift>(*interp, pc);
}

i64 asm_slow_path_mod(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::Mod>(*interp, pc);
}

i64 asm_slow_path_strictly_equals(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::StrictlyEquals>(*interp, pc);
}

i64 asm_slow_path_strictly_inequals(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::StrictlyInequals>(*interp, pc);
}

i64 asm_slow_path_unary_minus(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::UnaryMinus>(*interp, pc);
}

i64 asm_slow_path_postfix_decrement(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::PostfixDecrement>(*interp, pc);
}

i64 asm_slow_path_to_int32(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::ToInt32>(*interp, pc);
}

i64 asm_slow_path_put_by_value(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::PutByValue>(*interp, pc);
}

i64 asm_try_put_by_value_holey_array(Interpreter* interp, u32 pc)
{
    auto* bytecode = interp->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::PutByValue const*>(&bytecode[pc]);

    auto base = interp->get(insn.base());
    if (!base.is_object()) [[unlikely]]
        return 1;

    auto property = interp->get(insn.property());
    if (!property.is_non_negative_int32()) [[unlikely]]
        return 1;

    auto& object = base.as_object();
    if (!is<JS::Array>(object)) [[unlikely]]
        return 1;

    auto& array = static_cast<JS::Array&>(object);
    if (array.is_proxy_target()
        || !array.default_prototype_chain_intact()
        || !array.extensible()
        || array.may_interfere_with_indexed_property_access()
        || array.indexed_storage_kind() != IndexedStorageKind::Holey) [[unlikely]]
        return 1;

    auto index = static_cast<u32>(property.as_i32());
    if (index >= array.indexed_array_like_size()) [[unlikely]]
        return 1;

    array.indexed_put(index, interp->get(insn.src()));
    return 0;
}

// Try to inline a JS-to-JS call. Returns 0 on success (callee frame pushed),
// 1 on failure (caller should fall through to slow path).
i64 asm_try_inline_call(Interpreter* interp, u32 pc)
{
    auto* bytecode = interp->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::Call const*>(&bytecode[pc]);
    auto callee = interp->get(insn.callee());
    if (!callee.is_object())
        return 1;
    auto& callee_object = callee.as_object();
    if (!is<ECMAScriptFunctionObject>(callee_object))
        return 1;
    auto& callee_function = static_cast<ECMAScriptFunctionObject&>(callee_object);
    if (callee_function.kind() != FunctionKind::Normal
        || callee_function.is_class_constructor()
        || !callee_function.bytecode_executable())
        return 1;

    u32 return_pc = pc + insn.length();

    auto* callee_context = interp->push_inline_frame(
        callee_function, *callee_function.bytecode_executable(),
        insn.arguments(), return_pc, insn.dst().raw(),
        interp->get(insn.this_value()), nullptr, false);

    if (!callee_context) [[unlikely]]
        return 1;

    // NB: push_inline_frame does NOT update m_running_execution_context.
    //     The C++ interpreter's try_inline_call does it, so we do too.
    interp->set_running_execution_context(callee_context);
    return 0;
}

// Pop an inline frame after Return. Returns 0 on success.
i64 asm_pop_inline_frame(Interpreter* interp, u32 pc)
{
    auto* bytecode = interp->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::Return const*>(&bytecode[pc]);
    auto value = interp->get(insn.value());
    if (value.is_special_empty_value())
        value = js_undefined();
    interp->pop_inline_frame(value);
    return 0;
}

// Pop an inline frame after End. Returns 0 on success.
i64 asm_pop_inline_frame_end(Interpreter* interp, u32 pc)
{
    auto* bytecode = interp->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::End const*>(&bytecode[pc]);
    auto value = interp->get(insn.value());
    if (value.is_special_empty_value())
        value = js_undefined();
    interp->pop_inline_frame(value);
    return 0;
}

// Fast cache-only PutById. Tries all cache entries for ChangeOwnProperty and
// AddOwnProperty. Returns 0 on cache hit, 1 on miss (caller should use full slow path).
i64 asm_try_put_by_id_cache(Interpreter* interp, u32 pc)
{
    auto* bytecode = interp->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::PutById const*>(&bytecode[pc]);
    auto base = interp->get(insn.base());
    if (!base.is_object()) [[unlikely]]
        return 1;
    auto& object = base.as_object();
    auto value = interp->get(insn.src());
    auto& cache = *bit_cast<PropertyLookupCache*>(insn.cache());

    for (size_t i = 0; i < cache.entries.size(); ++i) {
        auto& entry = cache.entries[i];
        switch (cache.types[i]) {
        case PropertyLookupCache::Entry::Type::ChangeOwnProperty: {
            auto cached_shape = entry.shape.ptr();
            if (cached_shape != &object.shape()) [[unlikely]]
                continue;
            if (cached_shape->is_dictionary()
                && cached_shape->dictionary_generation() != entry.shape_dictionary_generation)
                continue;
            auto current = object.get_direct(entry.property_offset);
            if (current.is_accessor()) [[unlikely]]
                return 1;
            object.put_direct(entry.property_offset, value);
            return 0;
        }
        case PropertyLookupCache::Entry::Type::AddOwnProperty: {
            if (entry.from_shape != &object.shape()) [[unlikely]]
                continue;
            auto cached_shape = entry.shape.ptr();
            if (!cached_shape) [[unlikely]]
                continue;
            if (!object.extensible()) [[unlikely]]
                continue;
            if (cached_shape->is_dictionary()
                && object.shape().dictionary_generation() != entry.shape_dictionary_generation)
                continue;
            auto pcv = entry.prototype_chain_validity.ptr();
            if (pcv && !pcv->is_valid()) [[unlikely]]
                continue;
            object.unsafe_set_shape(*cached_shape);
            object.put_direct(entry.property_offset, value);
            return 0;
        }
        default:
            continue;
        }
    }
    return 1;
}

// Fast cache-only GetById. Tries all cache entries for own-property and prototype
// chain lookups. On cache hit, writes the result to the dst operand and returns 0.
// On miss, returns 1 (caller should use full slow path).
i64 asm_try_get_by_id_cache(Interpreter* interp, u32 pc)
{
    auto* bytecode = interp->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetById const*>(&bytecode[pc]);
    auto base = interp->get(insn.base());
    if (!base.is_object()) [[unlikely]]
        return 1;
    auto& object = base.as_object();
    auto& shape = object.shape();
    auto& cache = *bit_cast<PropertyLookupCache*>(insn.cache());

    for (auto& entry : cache.entries) {
        auto cached_prototype = entry.prototype.ptr();
        if (cached_prototype) {
            if (&shape != entry.shape) [[unlikely]]
                continue;
            if (shape.is_dictionary()
                && shape.dictionary_generation() != entry.shape_dictionary_generation)
                continue;
            auto pcv = entry.prototype_chain_validity.ptr();
            if (!pcv || !pcv->is_valid()) [[unlikely]]
                continue;
            auto value = cached_prototype->get_direct(entry.property_offset);
            if (value.is_accessor()) [[unlikely]]
                return 1;
            interp->set(insn.dst(), value);
            return 0;
        } else if (&shape == entry.shape) {
            if (shape.is_dictionary()
                && shape.dictionary_generation() != entry.shape_dictionary_generation)
                continue;
            auto value = object.get_direct(entry.property_offset);
            if (value.is_accessor()) [[unlikely]]
                return 1;
            interp->set(insn.dst(), value);
            return 0;
        }
    }
    return 1;
}

i64 asm_slow_path_get_binding(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::GetBinding>(*interp, pc);
}

i64 asm_slow_path_initialize_lexical_binding(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::InitializeLexicalBinding>(*interp, pc);
}

i64 asm_slow_path_set_lexical_binding(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::SetLexicalBinding>(*interp, pc);
}

i64 asm_slow_path_bitwise_not(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::BitwiseNot>(*interp, pc);
}

i64 asm_slow_path_unary_plus(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::UnaryPlus>(*interp, pc);
}

i64 asm_slow_path_throw_if_tdz(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::ThrowIfTDZ>(*interp, pc);
}

i64 asm_slow_path_throw_if_not_object(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::ThrowIfNotObject>(*interp, pc);
}

i64 asm_slow_path_throw_if_nullish(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::ThrowIfNullish>(*interp, pc);
}

// Fast path for GetByValue on typed arrays.
// Returns 0 on success (result stored in dst), 1 on miss (fall to slow path).
i64 asm_try_get_by_value_typed_array(Interpreter* interp, u32 pc)
{
    auto* bytecode = interp->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetByValue const*>(&bytecode[pc]);

    auto base = interp->get(insn.base());
    if (!base.is_object()) [[unlikely]]
        return 1;

    auto property = interp->get(insn.property());
    if (!property.is_non_negative_int32()) [[unlikely]]
        return 1;

    auto& object = base.as_object();
    if (!object.is_typed_array()) [[unlikely]]
        return 1;

    auto& typed_array = static_cast<TypedArrayBase&>(object);
    auto index = static_cast<u32>(property.as_i32());

    // Fast path: fixed-length typed array with cached data pointer
    auto const& array_length = typed_array.array_length();
    if (array_length.is_auto()) [[unlikely]]
        return 1;

    auto length = array_length.length();
    if (index >= length) [[unlikely]] {
        interp->set(insn.dst(), js_undefined());
        return 0;
    }

    if (!is_valid_integer_index(typed_array, CanonicalIndex { CanonicalIndex::Type::Index, index })) [[unlikely]] {
        interp->set(insn.dst(), js_undefined());
        return 0;
    }

    auto* buffer = typed_array.viewed_array_buffer();
    auto const* data = buffer->buffer().data() + typed_array.byte_offset();

    Value result;
    switch (typed_array.kind()) {
    case TypedArrayBase::Kind::Uint8Array:
    case TypedArrayBase::Kind::Uint8ClampedArray:
        result = Value(static_cast<i32>(data[index]));
        break;
    case TypedArrayBase::Kind::Int8Array:
        result = Value(static_cast<i32>(reinterpret_cast<i8 const*>(data)[index]));
        break;
    case TypedArrayBase::Kind::Uint16Array:
        result = Value(static_cast<i32>(reinterpret_cast<u16 const*>(data)[index]));
        break;
    case TypedArrayBase::Kind::Int16Array:
        result = Value(static_cast<i32>(reinterpret_cast<i16 const*>(data)[index]));
        break;
    case TypedArrayBase::Kind::Uint32Array:
        result = Value(static_cast<double>(reinterpret_cast<u32 const*>(data)[index]));
        break;
    case TypedArrayBase::Kind::Int32Array:
        result = Value(reinterpret_cast<i32 const*>(data)[index]);
        break;
    case TypedArrayBase::Kind::Float32Array:
        result = Value(static_cast<double>(reinterpret_cast<float const*>(data)[index]));
        break;
    case TypedArrayBase::Kind::Float64Array:
        result = Value(reinterpret_cast<double const*>(data)[index]);
        break;
    default:
        return 1;
    }

    interp->set(insn.dst(), result);
    return 0;
}

// Fast path for PutByValue on typed arrays.
// Returns 0 on success, 1 on miss (fall to slow path).
i64 asm_try_put_by_value_typed_array(Interpreter* interp, u32 pc)
{
    auto* bytecode = interp->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::PutByValue const*>(&bytecode[pc]);

    auto base = interp->get(insn.base());
    if (!base.is_object()) [[unlikely]]
        return 1;

    auto property = interp->get(insn.property());
    if (!property.is_non_negative_int32()) [[unlikely]]
        return 1;

    auto& object = base.as_object();
    if (!object.is_typed_array()) [[unlikely]]
        return 1;

    auto& typed_array = static_cast<TypedArrayBase&>(object);
    auto index = static_cast<u32>(property.as_i32());

    auto const& array_length = typed_array.array_length();
    if (array_length.is_auto()) [[unlikely]]
        return 1;

    if (index >= array_length.length()) [[unlikely]]
        return 0;

    if (!is_valid_integer_index(typed_array, CanonicalIndex { CanonicalIndex::Type::Index, index })) [[unlikely]]
        return 0;

    auto* buffer = typed_array.viewed_array_buffer();
    auto* data = buffer->buffer().data() + typed_array.byte_offset();
    auto value = interp->get(insn.src());

    if (value.is_int32()) {
        auto int_val = value.as_i32();
        switch (typed_array.kind()) {
        case TypedArrayBase::Kind::Uint8Array:
            data[index] = static_cast<u8>(int_val);
            return 0;
        case TypedArrayBase::Kind::Uint8ClampedArray:
            data[index] = static_cast<u8>(clamp(int_val, 0, 255));
            return 0;
        case TypedArrayBase::Kind::Int8Array:
            reinterpret_cast<i8*>(data)[index] = static_cast<i8>(int_val);
            return 0;
        case TypedArrayBase::Kind::Uint16Array:
            reinterpret_cast<u16*>(data)[index] = static_cast<u16>(int_val);
            return 0;
        case TypedArrayBase::Kind::Int16Array:
            reinterpret_cast<i16*>(data)[index] = static_cast<i16>(int_val);
            return 0;
        case TypedArrayBase::Kind::Uint32Array:
            reinterpret_cast<u32*>(data)[index] = static_cast<u32>(int_val);
            return 0;
        case TypedArrayBase::Kind::Int32Array:
            reinterpret_cast<i32*>(data)[index] = int_val;
            return 0;
        default:
            break;
        }
    } else if (value.is_double()) {
        auto dbl_val = value.as_double();
        switch (typed_array.kind()) {
        case TypedArrayBase::Kind::Float32Array:
            reinterpret_cast<float*>(data)[index] = static_cast<float>(dbl_val);
            return 0;
        case TypedArrayBase::Kind::Float64Array:
            reinterpret_cast<double*>(data)[index] = dbl_val;
            return 0;
        default:
            break;
        }
    }

    return 1;
}

i64 asm_slow_path_instance_of(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::InstanceOf>(*interp, pc);
}

i64 asm_slow_path_resolve_this_binding(Interpreter* interp, u32 pc)
{
    return slow_path_throwing<Op::ResolveThisBinding>(*interp, pc);
}

// Direct handler for GetPrivateById: bypasses Reference indirection.
i64 asm_slow_path_get_private_by_id(Interpreter* interp, u32 pc)
{
    interp->running_execution_context().program_counter = pc;
    auto* bytecode = interp->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetPrivateById const*>(&bytecode[pc]);

    auto base_value = interp->get(insn.base());
    auto& vm = interp->vm();

    if (!base_value.is_object()) [[unlikely]] {
        auto object = base_value.to_object(vm);
        if (object.is_error())
            return handle_asm_exception(*interp, pc, object.error_value());
        auto const& name = interp->get_identifier(insn.property());
        auto private_name = make_private_reference(vm, base_value, name);
        auto result = private_name.get_value(vm);
        if (result.is_error()) [[unlikely]]
            return handle_asm_exception(*interp, pc, result.error_value());
        interp->set(insn.dst(), result.release_value());
        return static_cast<i64>(pc + sizeof(Op::GetPrivateById));
    }

    auto const& name = interp->get_identifier(insn.property());
    auto private_environment = vm.running_execution_context().private_environment;
    VERIFY(private_environment);
    auto private_name = private_environment->resolve_private_identifier(name);
    auto result = base_value.as_object().private_get(private_name);
    if (result.is_error()) [[unlikely]]
        return handle_asm_exception(*interp, pc, result.error_value());
    interp->set(insn.dst(), result.release_value());
    return static_cast<i64>(pc + sizeof(Op::GetPrivateById));
}

// Direct handler for PutPrivateById: bypasses Reference indirection.
i64 asm_slow_path_put_private_by_id(Interpreter* interp, u32 pc)
{
    interp->running_execution_context().program_counter = pc;
    auto* bytecode = interp->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::PutPrivateById const*>(&bytecode[pc]);

    auto base_value = interp->get(insn.base());
    auto& vm = interp->vm();
    auto value = interp->get(insn.src());

    if (!base_value.is_object()) [[unlikely]] {
        auto object = base_value.to_object(vm);
        if (object.is_error())
            return handle_asm_exception(*interp, pc, object.error_value());
        auto const& name = interp->get_identifier(insn.property());
        auto private_reference = make_private_reference(vm, object.release_value(), name);
        auto result = private_reference.put_value(vm, value);
        if (result.is_error()) [[unlikely]]
            return handle_asm_exception(*interp, pc, result.error_value());
        return static_cast<i64>(pc + sizeof(Op::PutPrivateById));
    }

    auto const& name = interp->get_identifier(insn.property());
    auto private_environment = vm.running_execution_context().private_environment;
    VERIFY(private_environment);
    auto private_name = private_environment->resolve_private_identifier(name);
    auto result = base_value.as_object().private_set(private_name, value);
    if (result.is_error()) [[unlikely]]
        return handle_asm_exception(*interp, pc, result.error_value());
    return static_cast<i64>(pc + sizeof(Op::PutPrivateById));
}

// Helper: convert value to boolean (called from asm jump handlers)
// Returns 0 (false) or 1 (true). Never throws.
u64 asm_helper_to_boolean(u64 encoded_value)
{
    auto value = bit_cast<Value>(encoded_value);
    return value.to_boolean() ? 1 : 0;
}

u64 asm_helper_math_exp(u64 encoded_value)
{
    auto value = bit_cast<Value>(encoded_value);
    return bit_cast<u64>(Value(::exp(value.as_double())));
}

u64 asm_helper_empty_string(u64)
{
    return bit_cast<u64>(Value(&Interpreter::vm().empty_string()));
}

u64 asm_helper_single_ascii_character_string(u64 encoded_value)
{
    return bit_cast<u64>(Value(&Interpreter::vm().single_ascii_character_string(static_cast<u8>(encoded_value))));
}

u64 asm_helper_single_utf16_code_unit_string(u64 encoded_value)
{
    char16_t code_unit = static_cast<char16_t>(encoded_value);
    return bit_cast<u64>(Value(PrimitiveString::create(Interpreter::vm(), Utf16View(&code_unit, 1))));
}

} // extern "C"
