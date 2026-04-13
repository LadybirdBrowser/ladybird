/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Bytecode/AsmInterpreter/AsmInterpreter.h>
#include <LibJS/Bytecode/Builtins.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/Op.h>
#include <LibJS/Bytecode/PropertyAccess.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/ModuleEnvironment.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Reference.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
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
extern "C" void asm_interpreter_entry(u8 const* bytecode, u32 entry_point, Value* values, VM* vm);
#endif

bool AsmInterpreter::is_available()
{
    return HAS_ASM_INTERPRETER;
}

void AsmInterpreter::run(VM& vm, [[maybe_unused]] size_t entry_point)
{
#if !HAS_ASM_INTERPRETER
    (void)vm;
    VERIFY_NOT_REACHED();
#else
#    ifdef JS_ASMINT_SLOW_PATH_COUNTERS
    if (!s_stats.registered) {
        s_stats.registered = true;
        atexit(print_asm_slow_path_stats);
    }
#    endif

    auto& context = vm.running_execution_context();
    auto* bytecode = context.executable->bytecode.data();
    auto* values = context.registers_and_constants_and_locals_and_arguments_span().data();

    asm_interpreter_entry(bytecode, static_cast<u32>(entry_point), values, &vm);
#endif
}

}

// ===== Slow path functions callable from assembly =====
// All slow path functions follow the same convention:
//   i64 func(VM* vm, u32 pc)
//   Returns >= 0: new program counter to dispatch to
//   Returns < 0: should exit the asm interpreter

using namespace JS;
using namespace JS::Bytecode;

static i64 handle_asm_exception(VM& vm, u32 pc, Value exception)
{
    auto response = vm.handle_exception(pc, exception);
    if (response == VM::HandleExceptionResponse::ExitFromExecutable)
        return -1;
    // ContinueInThisExecutable: new pc is in the execution context
    return static_cast<i64>(vm.running_execution_context().program_counter);
}

// Helper: execute a throwing instruction and handle errors
template<typename InsnType>
static i64 execute_throwing(VM& vm, u32 pc)
{
    vm.running_execution_context().program_counter = pc;
    auto* bytecode = vm.current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<InsnType const*>(&bytecode[pc]);
    auto result = insn.execute_impl(vm);
    if (result.is_error()) [[unlikely]]
        return handle_asm_exception(vm, pc, result.error_value());
    if constexpr (InsnType::IsVariableLength)
        return static_cast<i64>(pc + insn.length());
    else
        return static_cast<i64>(pc + sizeof(InsnType));
}

// Helper: execute a non-throwing instruction
template<typename InsnType>
static i64 execute_nonthrowing(VM& vm, u32 pc)
{
    vm.running_execution_context().program_counter = pc;
    auto* bytecode = vm.current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<InsnType const*>(&bytecode[pc]);
    insn.execute_impl(vm);
    if constexpr (InsnType::IsVariableLength)
        return static_cast<i64>(pc + insn.length());
    else
        return static_cast<i64>(pc + sizeof(InsnType));
}

// Slow path wrappers: optionally bump per-opcode counter, then delegate.
#ifdef JS_ASMINT_SLOW_PATH_COUNTERS
template<typename InsnType>
ALWAYS_INLINE static i64 slow_path_throwing(VM& vm, u32 pc)
{
    ++s_stats.slow_path_by_type[static_cast<u8>(Instruction::Type { vm.current_executable().bytecode[pc] })];
    return execute_throwing<InsnType>(vm, pc);
}

template<typename InsnType>
ALWAYS_INLINE static i64 slow_path_nonthrowing(VM& vm, u32 pc)
{
    ++s_stats.slow_path_by_type[static_cast<u8>(Instruction::Type { vm.current_executable().bytecode[pc] })];
    return execute_nonthrowing<InsnType>(vm, pc);
}

ALWAYS_INLINE static void bump_slow_path(VM& vm, u32 pc)
{
    ++s_stats.slow_path_by_type[static_cast<u8>(Instruction::Type { vm.current_executable().bytecode[pc] })];
}
#else
template<typename InsnType>
ALWAYS_INLINE static i64 slow_path_throwing(VM& vm, u32 pc)
{
    return execute_throwing<InsnType>(vm, pc);
}

template<typename InsnType>
ALWAYS_INLINE static i64 slow_path_nonthrowing(VM& vm, u32 pc)
{
    return execute_nonthrowing<InsnType>(vm, pc);
}

ALWAYS_INLINE static void bump_slow_path(VM&, u32) { }
#endif

extern "C" {

// Forward declarations for all functions called from assembly.
i64 asm_fallback_handler(VM*, u32 pc);
i64 asm_slow_path_add(VM*, u32 pc);
i64 asm_slow_path_sub(VM*, u32 pc);
i64 asm_slow_path_mul(VM*, u32 pc);
i64 asm_slow_path_div(VM*, u32 pc);
i64 asm_slow_path_increment(VM*, u32 pc);
i64 asm_slow_path_decrement(VM*, u32 pc);
i64 asm_slow_path_less_than(VM*, u32 pc);
i64 asm_slow_path_less_than_equals(VM*, u32 pc);
i64 asm_slow_path_greater_than(VM*, u32 pc);
i64 asm_slow_path_greater_than_equals(VM*, u32 pc);
i64 asm_slow_path_jump_less_than(VM*, u32 pc);
i64 asm_slow_path_jump_greater_than(VM*, u32 pc);
i64 asm_slow_path_jump_less_than_equals(VM*, u32 pc);
i64 asm_slow_path_jump_greater_than_equals(VM*, u32 pc);
i64 asm_slow_path_jump_loosely_equals(VM*, u32 pc);
i64 asm_slow_path_jump_loosely_inequals(VM*, u32 pc);
i64 asm_slow_path_jump_strictly_equals(VM*, u32 pc);
i64 asm_slow_path_jump_strictly_inequals(VM*, u32 pc);
i64 asm_slow_path_set_lexical_environment(VM*, u32 pc);
i64 asm_slow_path_postfix_increment(VM*, u32 pc);
i64 asm_slow_path_get_by_id(VM*, u32 pc);
i64 asm_slow_path_put_by_id(VM*, u32 pc);
i64 asm_slow_path_get_by_value(VM*, u32 pc);
i64 asm_slow_path_get_length(VM*, u32 pc);
i64 asm_try_get_global_env_binding(VM*, u32 pc);
i64 asm_try_set_global_env_binding(VM*, u32 pc);
i64 asm_slow_path_get_global(VM*, u32 pc);
i64 asm_slow_path_set_global(VM*, u32 pc);
i64 asm_slow_path_call(VM*, u32 pc);
#define DECLARE_CALL_BUILTIN_SLOW_PATH(name, snake_case_name, ...) \
    i64 asm_slow_path_call_builtin_##snake_case_name(VM*, u32 pc);
JS_ENUMERATE_BUILTINS(DECLARE_CALL_BUILTIN_SLOW_PATH)
#undef DECLARE_CALL_BUILTIN_SLOW_PATH
i64 asm_slow_path_get_object_property_iterator(VM*, u32 pc);
i64 asm_slow_path_object_property_iterator_next(VM*, u32 pc);
i64 asm_slow_path_call_construct(VM*, u32 pc);
i64 asm_slow_path_new_object(VM*, u32 pc);
i64 asm_slow_path_cache_object_shape(VM*, u32 pc);
i64 asm_slow_path_init_object_literal_property(VM*, u32 pc);
i64 asm_slow_path_new_array(VM*, u32 pc);
i64 asm_slow_path_bitwise_xor(VM*, u32 pc);
i64 asm_slow_path_bitwise_and(VM*, u32 pc);
i64 asm_slow_path_bitwise_or(VM*, u32 pc);
i64 asm_slow_path_left_shift(VM*, u32 pc);
i64 asm_slow_path_right_shift(VM*, u32 pc);
i64 asm_slow_path_unsigned_right_shift(VM*, u32 pc);
i64 asm_slow_path_mod(VM*, u32 pc);
i64 asm_slow_path_strictly_equals(VM*, u32 pc);
i64 asm_slow_path_strictly_inequals(VM*, u32 pc);
i64 asm_slow_path_unary_minus(VM*, u32 pc);
i64 asm_slow_path_postfix_decrement(VM*, u32 pc);
i64 asm_slow_path_to_int32(VM*, u32 pc);
i64 asm_slow_path_put_by_value(VM*, u32 pc);
i64 asm_try_put_by_value_holey_array(VM*, u32 pc);
u64 asm_helper_to_boolean(u64 encoded_value);
u64 asm_helper_math_exp(u64 encoded_value);
u64 asm_helper_empty_string(u64);
u64 asm_helper_single_ascii_character_string(u64 encoded_value);
u64 asm_helper_single_utf16_code_unit_string(u64 encoded_value);
i64 asm_try_inline_call(VM*, u32 pc);
i64 asm_try_put_by_id_cache(VM*, u32 pc);
i64 asm_try_get_by_id_cache(VM*, u32 pc);
i64 asm_slow_path_initialize_lexical_binding(VM*, u32 pc);

i64 asm_try_get_by_value_typed_array(VM*, u32 pc);
i64 asm_slow_path_get_initialized_binding(VM*, u32 pc);
i64 asm_slow_path_get_binding(VM*, u32 pc);
i64 asm_slow_path_set_lexical_binding(VM*, u32 pc);
i64 asm_slow_path_bitwise_not(VM*, u32 pc);
i64 asm_slow_path_unary_plus(VM*, u32 pc);
i64 asm_slow_path_throw_if_tdz(VM*, u32 pc);
i64 asm_slow_path_throw_if_not_object(VM*, u32 pc);
i64 asm_slow_path_throw_if_nullish(VM*, u32 pc);
i64 asm_slow_path_loosely_equals(VM*, u32 pc);
i64 asm_slow_path_loosely_inequals(VM*, u32 pc);
i64 asm_slow_path_get_callee_and_this(VM*, u32 pc);
i64 asm_try_put_by_value_typed_array(VM*, u32 pc);
i64 asm_slow_path_get_private_by_id(VM*, u32 pc);
i64 asm_slow_path_put_private_by_id(VM*, u32 pc);
i64 asm_slow_path_instance_of(VM*, u32 pc);
i64 asm_slow_path_resolve_this_binding(VM*, u32 pc);

// ===== Fallback handler for opcodes without DSL handlers =====
// NB: Opcodes with DSL handlers are dispatched directly and never reach here.
i64 asm_fallback_handler(VM* vm, u32 pc)
{
    auto& ctx = vm->running_execution_context();
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
        auto result = typed.execute_impl(*vm);
        return handle_asm_exception(*vm, pc, result.error_value());
    }
    case Instruction::Type::Await: {
        auto& typed = *reinterpret_cast<Op::Await const*>(&bytecode[pc]);
        typed.execute_impl(*vm);
        return -1;
    }
    case Instruction::Type::Yield: {
        auto& typed = *reinterpret_cast<Op::Yield const*>(&bytecode[pc]);
        typed.execute_impl(*vm);
        return -1;
    }

    // Non-throwing instructions
    case Instruction::Type::AddPrivateName:
        return execute_nonthrowing<Op::AddPrivateName>(*vm, pc);
    case Instruction::Type::Catch:
        return execute_nonthrowing<Op::Catch>(*vm, pc);
    case Instruction::Type::CreateAsyncFromSyncIterator:
        return execute_nonthrowing<Op::CreateAsyncFromSyncIterator>(*vm, pc);
    case Instruction::Type::CreateLexicalEnvironment:
        return execute_nonthrowing<Op::CreateLexicalEnvironment>(*vm, pc);
    case Instruction::Type::CreateVariableEnvironment:
        return execute_nonthrowing<Op::CreateVariableEnvironment>(*vm, pc);
    case Instruction::Type::CreatePrivateEnvironment:
        return execute_nonthrowing<Op::CreatePrivateEnvironment>(*vm, pc);
    case Instruction::Type::CreateRestParams:
        return execute_nonthrowing<Op::CreateRestParams>(*vm, pc);
    case Instruction::Type::CreateArguments:
        return execute_nonthrowing<Op::CreateArguments>(*vm, pc);
    case Instruction::Type::GetCompletionFields:
        return execute_nonthrowing<Op::GetCompletionFields>(*vm, pc);
    case Instruction::Type::GetImportMeta:
        return execute_nonthrowing<Op::GetImportMeta>(*vm, pc);
    case Instruction::Type::GetNewTarget:
        return execute_nonthrowing<Op::GetNewTarget>(*vm, pc);
    case Instruction::Type::GetTemplateObject:
        return execute_nonthrowing<Op::GetTemplateObject>(*vm, pc);
    case Instruction::Type::IsCallable:
        return execute_nonthrowing<Op::IsCallable>(*vm, pc);
    case Instruction::Type::IsConstructor:
        return execute_nonthrowing<Op::IsConstructor>(*vm, pc);
    case Instruction::Type::LeavePrivateEnvironment:
        return execute_nonthrowing<Op::LeavePrivateEnvironment>(*vm, pc);
    case Instruction::Type::NewFunction:
        return execute_nonthrowing<Op::NewFunction>(*vm, pc);
    case Instruction::Type::NewObjectWithNoPrototype:
        return execute_nonthrowing<Op::NewObjectWithNoPrototype>(*vm, pc);
    case Instruction::Type::NewPrimitiveArray:
        return execute_nonthrowing<Op::NewPrimitiveArray>(*vm, pc);
    case Instruction::Type::NewRegExp:
        return execute_nonthrowing<Op::NewRegExp>(*vm, pc);
    case Instruction::Type::NewReferenceError:
        return execute_nonthrowing<Op::NewReferenceError>(*vm, pc);
    case Instruction::Type::NewTypeError:
        return execute_nonthrowing<Op::NewTypeError>(*vm, pc);
    case Instruction::Type::SetCompletionType:
        return execute_nonthrowing<Op::SetCompletionType>(*vm, pc);
    case Instruction::Type::ToBoolean:
        return execute_nonthrowing<Op::ToBoolean>(*vm, pc);
    case Instruction::Type::Typeof:
        return execute_nonthrowing<Op::Typeof>(*vm, pc);

    // Throwing instructions
    case Instruction::Type::ArrayAppend:
        return execute_throwing<Op::ArrayAppend>(*vm, pc);
    case Instruction::Type::ToString:
        return execute_throwing<Op::ToString>(*vm, pc);
    case Instruction::Type::ToPrimitiveWithStringHint:
        return execute_throwing<Op::ToPrimitiveWithStringHint>(*vm, pc);
    case Instruction::Type::CallConstructWithArgumentArray:
        return execute_throwing<Op::CallConstructWithArgumentArray>(*vm, pc);
    case Instruction::Type::CallDirectEval:
        return execute_throwing<Op::CallDirectEval>(*vm, pc);
    case Instruction::Type::CallDirectEvalWithArgumentArray:
        return execute_throwing<Op::CallDirectEvalWithArgumentArray>(*vm, pc);
    case Instruction::Type::CallWithArgumentArray:
        return execute_throwing<Op::CallWithArgumentArray>(*vm, pc);
    case Instruction::Type::ConcatString:
        return execute_throwing<Op::ConcatString>(*vm, pc);
    case Instruction::Type::CopyObjectExcludingProperties:
        return execute_throwing<Op::CopyObjectExcludingProperties>(*vm, pc);
    case Instruction::Type::CreateDataPropertyOrThrow:
        return execute_throwing<Op::CreateDataPropertyOrThrow>(*vm, pc);
    case Instruction::Type::CreateImmutableBinding:
        return execute_throwing<Op::CreateImmutableBinding>(*vm, pc);
    case Instruction::Type::CreateMutableBinding:
        return execute_throwing<Op::CreateMutableBinding>(*vm, pc);
    case Instruction::Type::CreateVariable:
        return execute_throwing<Op::CreateVariable>(*vm, pc);
    case Instruction::Type::DeleteById:
        return execute_throwing<Op::DeleteById>(*vm, pc);
    case Instruction::Type::DeleteByValue:
        return execute_throwing<Op::DeleteByValue>(*vm, pc);
    case Instruction::Type::DeleteVariable:
        return execute_throwing<Op::DeleteVariable>(*vm, pc);
    case Instruction::Type::EnterObjectEnvironment:
        return execute_throwing<Op::EnterObjectEnvironment>(*vm, pc);
    case Instruction::Type::Exp:
        return execute_throwing<Op::Exp>(*vm, pc);
    case Instruction::Type::GetByIdWithThis:
        return execute_throwing<Op::GetByIdWithThis>(*vm, pc);
    case Instruction::Type::GetByValueWithThis:
        return execute_throwing<Op::GetByValueWithThis>(*vm, pc);
    case Instruction::Type::GetIterator:
        return execute_throwing<Op::GetIterator>(*vm, pc);
    case Instruction::Type::GetLengthWithThis:
        return execute_throwing<Op::GetLengthWithThis>(*vm, pc);
    case Instruction::Type::GetMethod:
        return execute_throwing<Op::GetMethod>(*vm, pc);
    case Instruction::Type::GetObjectPropertyIterator:
        return execute_throwing<Op::GetObjectPropertyIterator>(*vm, pc);
    case Instruction::Type::ObjectPropertyIteratorNext:
        return execute_throwing<Op::ObjectPropertyIteratorNext>(*vm, pc);
    case Instruction::Type::HasPrivateId:
        return execute_throwing<Op::HasPrivateId>(*vm, pc);
    case Instruction::Type::ImportCall:
        return execute_throwing<Op::ImportCall>(*vm, pc);
    case Instruction::Type::In:
        return execute_throwing<Op::In>(*vm, pc);
    case Instruction::Type::InitializeVariableBinding:
        return execute_throwing<Op::InitializeVariableBinding>(*vm, pc);
    case Instruction::Type::IteratorClose:
        return execute_throwing<Op::IteratorClose>(*vm, pc);
    case Instruction::Type::IteratorNext:
        return execute_throwing<Op::IteratorNext>(*vm, pc);
    case Instruction::Type::IteratorNextUnpack:
        return execute_throwing<Op::IteratorNextUnpack>(*vm, pc);
    case Instruction::Type::IteratorToArray:
        return execute_throwing<Op::IteratorToArray>(*vm, pc);
    case Instruction::Type::NewArrayWithLength:
        return execute_throwing<Op::NewArrayWithLength>(*vm, pc);
    case Instruction::Type::NewClass:
        return execute_throwing<Op::NewClass>(*vm, pc);
    case Instruction::Type::PutByIdWithThis:
        return execute_throwing<Op::PutByIdWithThis>(*vm, pc);
    case Instruction::Type::PutBySpread:
        return execute_throwing<Op::PutBySpread>(*vm, pc);
    case Instruction::Type::PutByValueWithThis:
        return execute_throwing<Op::PutByValueWithThis>(*vm, pc);
    case Instruction::Type::ResolveSuperBase:
        return execute_throwing<Op::ResolveSuperBase>(*vm, pc);
    case Instruction::Type::SetVariableBinding:
        return execute_throwing<Op::SetVariableBinding>(*vm, pc);
    case Instruction::Type::SuperCallWithArgumentArray:
        return execute_throwing<Op::SuperCallWithArgumentArray>(*vm, pc);
    case Instruction::Type::ThrowConstAssignment:
        return execute_throwing<Op::ThrowConstAssignment>(*vm, pc);
    case Instruction::Type::ToLength:
        return execute_throwing<Op::ToLength>(*vm, pc);
    case Instruction::Type::ToObject:
        return execute_throwing<Op::ToObject>(*vm, pc);
    case Instruction::Type::TypeofBinding:
        return execute_throwing<Op::TypeofBinding>(*vm, pc);

    default:
        VERIFY_NOT_REACHED();
    }
}

// ===== Specific slow paths for asm-optimized instructions =====
// These are called from asm handlers when the fast path fails.
// Convention: i64 func(VM*, u32 pc)
//   Returns >= 0: new pc
//   Returns < 0: exit

i64 asm_slow_path_add(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::Add>(*vm, pc);
}

i64 asm_slow_path_sub(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::Sub>(*vm, pc);
}

i64 asm_slow_path_mul(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::Mul>(*vm, pc);
}

i64 asm_slow_path_div(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::Div>(*vm, pc);
}

i64 asm_slow_path_less_than(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::LessThan>(*vm, pc);
}

i64 asm_slow_path_less_than_equals(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::LessThanEquals>(*vm, pc);
}

i64 asm_slow_path_greater_than(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::GreaterThan>(*vm, pc);
}

i64 asm_slow_path_greater_than_equals(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::GreaterThanEquals>(*vm, pc);
}

i64 asm_slow_path_increment(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::Increment>(*vm, pc);
}

i64 asm_slow_path_decrement(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::Decrement>(*vm, pc);
}

// Comparison jump slow paths - these are terminators (execute_impl returns void),
// so they need custom handling instead of the generic slow_path_throwing template.
#define DEFINE_JUMP_COMPARISON_SLOW_PATH(snake_name, op_name, compare_call)      \
    i64 asm_slow_path_jump_##snake_name(VM* vm, u32 pc)                          \
    {                                                                            \
        bump_slow_path(*vm, pc);                                                 \
        auto* bytecode = vm->current_executable().bytecode.data();               \
        auto& insn = *reinterpret_cast<Op::Jump##op_name const*>(&bytecode[pc]); \
        auto lhs = vm->get(insn.lhs());                                          \
        auto rhs = vm->get(insn.rhs());                                          \
        auto result = compare_call;                                              \
        if (result.is_error()) [[unlikely]]                                      \
            return handle_asm_exception(*vm, pc, result.error_value());          \
        if (result.value())                                                      \
            return static_cast<i64>(insn.true_target().address());               \
        return static_cast<i64>(insn.false_target().address());                  \
    }

DEFINE_JUMP_COMPARISON_SLOW_PATH(less_than, LessThan, less_than(VM::the(), lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(greater_than, GreaterThan, greater_than(VM::the(), lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(less_than_equals, LessThanEquals, less_than_equals(VM::the(), lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(greater_than_equals, GreaterThanEquals, greater_than_equals(VM::the(), lhs, rhs))
DEFINE_JUMP_COMPARISON_SLOW_PATH(loosely_equals, LooselyEquals, is_loosely_equal(VM::the(), lhs, rhs))
#undef DEFINE_JUMP_COMPARISON_SLOW_PATH

i64 asm_slow_path_jump_loosely_inequals(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::JumpLooselyInequals const*>(&bytecode[pc]);
    auto lhs = vm->get(insn.lhs());
    auto rhs = vm->get(insn.rhs());
    auto result = is_loosely_equal(VM::the(), lhs, rhs);
    if (result.is_error()) [[unlikely]]
        return handle_asm_exception(*vm, pc, result.error_value());
    if (!result.value())
        return static_cast<i64>(insn.true_target().address());
    return static_cast<i64>(insn.false_target().address());
}

i64 asm_slow_path_jump_strictly_equals(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::JumpStrictlyEquals const*>(&bytecode[pc]);
    auto lhs = vm->get(insn.lhs());
    auto rhs = vm->get(insn.rhs());
    if (is_strictly_equal(lhs, rhs))
        return static_cast<i64>(insn.true_target().address());
    return static_cast<i64>(insn.false_target().address());
}

i64 asm_slow_path_jump_strictly_inequals(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::JumpStrictlyInequals const*>(&bytecode[pc]);
    auto lhs = vm->get(insn.lhs());
    auto rhs = vm->get(insn.rhs());
    if (!is_strictly_equal(lhs, rhs))
        return static_cast<i64>(insn.true_target().address());
    return static_cast<i64>(insn.false_target().address());
}

// ===== Dedicated slow paths for hot instructions =====

i64 asm_slow_path_set_lexical_environment(VM* vm, u32 pc)
{
    return slow_path_nonthrowing<Op::SetLexicalEnvironment>(*vm, pc);
}

i64 asm_slow_path_get_initialized_binding(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::GetInitializedBinding>(*vm, pc);
}

i64 asm_slow_path_loosely_equals(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::LooselyEquals>(*vm, pc);
}

i64 asm_slow_path_loosely_inequals(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::LooselyInequals>(*vm, pc);
}

i64 asm_slow_path_get_callee_and_this(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::GetCalleeAndThisFromEnvironment>(*vm, pc);
}

i64 asm_slow_path_postfix_increment(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::PostfixIncrement>(*vm, pc);
}

i64 asm_slow_path_get_by_id(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::GetById>(*vm, pc);
}

i64 asm_slow_path_put_by_id(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::PutById>(*vm, pc);
}

i64 asm_slow_path_get_by_value(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::GetByValue>(*vm, pc);
}

i64 asm_slow_path_get_length(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::GetLength>(*vm, pc);
}

i64 asm_try_get_global_env_binding(VM* vm, u32 pc)
{
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetGlobal const*>(&bytecode[pc]);
    auto& cache = *bit_cast<GlobalVariableCache*>(insn.cache());

    if (!cache.has_environment_binding_index) [[unlikely]]
        return 1;

    auto& current_vm = *vm;
    ThrowCompletionOr<Value> result = js_undefined();
    if (cache.in_module_environment) {
        auto module = current_vm.running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>();
        if (!module) [[unlikely]]
            return 1;
        result = (*module)->environment()->get_binding_value_direct(current_vm, cache.environment_binding_index);
    } else {
        result = vm->global_declarative_environment().get_binding_value_direct(current_vm, cache.environment_binding_index);
    }
    if (result.is_error()) [[unlikely]]
        return 1;
    vm->set(insn.dst(), result.value());
    return 0;
}

i64 asm_slow_path_get_global(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::GetGlobal>(*vm, pc);
}

i64 asm_try_set_global_env_binding(VM* vm, u32 pc)
{
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::SetGlobal const*>(&bytecode[pc]);
    auto& cache = *bit_cast<GlobalVariableCache*>(insn.cache());

    if (!cache.has_environment_binding_index) [[unlikely]]
        return 1;

    auto& current_vm = *vm;
    auto src = vm->get(insn.src());
    ThrowCompletionOr<void> result;
    if (cache.in_module_environment) {
        auto module = current_vm.running_execution_context().script_or_module.get_pointer<GC::Ref<Module>>();
        if (!module) [[unlikely]]
            return 1;
        result = (*module)->environment()->set_mutable_binding_direct(current_vm, cache.environment_binding_index, src, insn.strict() == Strict::Yes);
    } else {
        result = vm->global_declarative_environment().set_mutable_binding_direct(current_vm, cache.environment_binding_index, src, insn.strict() == Strict::Yes);
    }
    if (result.is_error()) [[unlikely]]
        return 1;
    return 0;
}

i64 asm_slow_path_set_global(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::SetGlobal>(*vm, pc);
}

i64 asm_slow_path_call(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::Call>(*vm, pc);
}

i64 asm_slow_path_get_object_property_iterator(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::GetObjectPropertyIterator>(*vm, pc);
}

i64 asm_slow_path_object_property_iterator_next(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::ObjectPropertyIteratorNext>(*vm, pc);
}

#define DEFINE_CALL_BUILTIN_SLOW_PATH(name, snake_case_name, ...)    \
    i64 asm_slow_path_call_builtin_##snake_case_name(VM* vm, u32 pc) \
    {                                                                \
        return slow_path_throwing<Op::CallBuiltin##name>(*vm, pc);   \
    }
JS_ENUMERATE_BUILTINS(DEFINE_CALL_BUILTIN_SLOW_PATH)
#undef DEFINE_CALL_BUILTIN_SLOW_PATH

i64 asm_slow_path_call_construct(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::CallConstruct>(*vm, pc);
}

i64 asm_slow_path_new_object(VM* vm, u32 pc)
{
    return slow_path_nonthrowing<Op::NewObject>(*vm, pc);
}

i64 asm_slow_path_cache_object_shape(VM* vm, u32 pc)
{
    return slow_path_nonthrowing<Op::CacheObjectShape>(*vm, pc);
}

i64 asm_slow_path_init_object_literal_property(VM* vm, u32 pc)
{
    return slow_path_nonthrowing<Op::InitObjectLiteralProperty>(*vm, pc);
}

i64 asm_slow_path_new_array(VM* vm, u32 pc)
{
    bump_slow_path(*vm, pc);
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& typed = *reinterpret_cast<Op::NewArray const*>(&bytecode[pc]);
    typed.execute_impl(*vm);
    return static_cast<i64>(pc + typed.length());
}

i64 asm_slow_path_bitwise_xor(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::BitwiseXor>(*vm, pc);
}

i64 asm_slow_path_bitwise_and(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::BitwiseAnd>(*vm, pc);
}

i64 asm_slow_path_bitwise_or(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::BitwiseOr>(*vm, pc);
}

i64 asm_slow_path_left_shift(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::LeftShift>(*vm, pc);
}

i64 asm_slow_path_right_shift(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::RightShift>(*vm, pc);
}

i64 asm_slow_path_unsigned_right_shift(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::UnsignedRightShift>(*vm, pc);
}

i64 asm_slow_path_mod(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::Mod>(*vm, pc);
}

i64 asm_slow_path_strictly_equals(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::StrictlyEquals>(*vm, pc);
}

i64 asm_slow_path_strictly_inequals(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::StrictlyInequals>(*vm, pc);
}

i64 asm_slow_path_unary_minus(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::UnaryMinus>(*vm, pc);
}

i64 asm_slow_path_postfix_decrement(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::PostfixDecrement>(*vm, pc);
}

i64 asm_slow_path_to_int32(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::ToInt32>(*vm, pc);
}

i64 asm_slow_path_put_by_value(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::PutByValue>(*vm, pc);
}

i64 asm_try_put_by_value_holey_array(VM* vm, u32 pc)
{
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::PutByValue const*>(&bytecode[pc]);

    auto base = vm->get(insn.base());
    if (!base.is_object()) [[unlikely]]
        return 1;

    auto property = vm->get(insn.property());
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

    array.indexed_put(index, vm->get(insn.src()));
    return 0;
}

// Try to inline a JS-to-JS call by building the callee frame through the
// shared VM::push_inline_frame() helper. Returns 0 on success (callee frame
// pushed) and 1 on failure (caller should keep handling the Call itself).
i64 asm_try_inline_call(VM* vm, u32 pc)
{
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::Call const*>(&bytecode[pc]);

    auto callee = vm->get(insn.callee());
    if (!callee.is_object()) [[unlikely]]
        return 1;

    auto& callee_object = callee.as_object();
    if (!is<ECMAScriptFunctionObject>(callee_object)) [[unlikely]]
        return 1;

    auto& callee_function = static_cast<ECMAScriptFunctionObject&>(callee_object);
    if (!callee_function.can_inline_call()) [[unlikely]]
        return 1;

    auto* callee_context = vm->push_inline_frame(
        callee_function,
        callee_function.inline_call_executable(),
        insn.arguments(),
        pc + insn.length(),
        insn.dst().raw(),
        vm->get(insn.this_value()),
        nullptr,
        false);

    return callee_context ? 0 : 1;
}

// Fast cache-only PutById. Tries all cache entries for ChangeOwnProperty and
// AddOwnProperty. Returns 0 on cache hit, 1 on miss (caller should use full slow path).
i64 asm_try_put_by_id_cache(VM* vm, u32 pc)
{
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::PutById const*>(&bytecode[pc]);
    auto base = vm->get(insn.base());
    if (!base.is_object()) [[unlikely]]
        return 1;
    auto& object = base.as_object();
    auto value = vm->get(insn.src());
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
i64 asm_try_get_by_id_cache(VM* vm, u32 pc)
{
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetById const*>(&bytecode[pc]);
    auto base = vm->get(insn.base());
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
            vm->set(insn.dst(), value);
            return 0;
        } else if (&shape == entry.shape) {
            if (shape.is_dictionary()
                && shape.dictionary_generation() != entry.shape_dictionary_generation)
                continue;
            auto value = object.get_direct(entry.property_offset);
            if (value.is_accessor()) [[unlikely]]
                return 1;
            vm->set(insn.dst(), value);
            return 0;
        }
    }
    return 1;
}

i64 asm_slow_path_get_binding(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::GetBinding>(*vm, pc);
}

i64 asm_slow_path_initialize_lexical_binding(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::InitializeLexicalBinding>(*vm, pc);
}

i64 asm_slow_path_set_lexical_binding(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::SetLexicalBinding>(*vm, pc);
}

i64 asm_slow_path_bitwise_not(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::BitwiseNot>(*vm, pc);
}

i64 asm_slow_path_unary_plus(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::UnaryPlus>(*vm, pc);
}

i64 asm_slow_path_throw_if_tdz(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::ThrowIfTDZ>(*vm, pc);
}

i64 asm_slow_path_throw_if_not_object(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::ThrowIfNotObject>(*vm, pc);
}

i64 asm_slow_path_throw_if_nullish(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::ThrowIfNullish>(*vm, pc);
}

// Fast path for GetByValue on typed arrays.
// Returns 0 on success (result stored in dst), 1 on miss (fall to slow path).
i64 asm_try_get_by_value_typed_array(VM* vm, u32 pc)
{
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetByValue const*>(&bytecode[pc]);

    auto base = vm->get(insn.base());
    if (!base.is_object()) [[unlikely]]
        return 1;

    auto property = vm->get(insn.property());
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
        vm->set(insn.dst(), js_undefined());
        return 0;
    }

    if (!is_valid_integer_index(typed_array, CanonicalIndex { CanonicalIndex::Type::Index, index })) [[unlikely]] {
        vm->set(insn.dst(), js_undefined());
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

    vm->set(insn.dst(), result);
    return 0;
}

// Fast path for PutByValue on typed arrays.
// Returns 0 on success, 1 on miss (fall to slow path).
i64 asm_try_put_by_value_typed_array(VM* vm, u32 pc)
{
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::PutByValue const*>(&bytecode[pc]);

    auto base = vm->get(insn.base());
    if (!base.is_object()) [[unlikely]]
        return 1;

    auto property = vm->get(insn.property());
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
    auto value = vm->get(insn.src());

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

i64 asm_slow_path_instance_of(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::InstanceOf>(*vm, pc);
}

i64 asm_slow_path_resolve_this_binding(VM* vm, u32 pc)
{
    return slow_path_throwing<Op::ResolveThisBinding>(*vm, pc);
}

// Direct handler for GetPrivateById: bypasses Reference indirection.
i64 asm_slow_path_get_private_by_id(VM* vm, u32 pc)
{
    vm->running_execution_context().program_counter = pc;
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::GetPrivateById const*>(&bytecode[pc]);

    auto base_value = vm->get(insn.base());
    auto& current_vm = *vm;

    if (!base_value.is_object()) [[unlikely]] {
        auto object = base_value.to_object(current_vm);
        if (object.is_error())
            return handle_asm_exception(*vm, pc, object.error_value());
        auto const& name = current_vm.get_identifier(insn.property());
        auto private_name = make_private_reference(current_vm, base_value, name);
        auto result = private_name.get_value(current_vm);
        if (result.is_error()) [[unlikely]]
            return handle_asm_exception(*vm, pc, result.error_value());
        vm->set(insn.dst(), result.release_value());
        return static_cast<i64>(pc + sizeof(Op::GetPrivateById));
    }

    auto const& name = current_vm.get_identifier(insn.property());
    auto private_environment = current_vm.running_execution_context().private_environment;
    VERIFY(private_environment);
    auto private_name = private_environment->resolve_private_identifier(name);
    auto result = base_value.as_object().private_get(private_name);
    if (result.is_error()) [[unlikely]]
        return handle_asm_exception(*vm, pc, result.error_value());
    vm->set(insn.dst(), result.release_value());
    return static_cast<i64>(pc + sizeof(Op::GetPrivateById));
}

// Direct handler for PutPrivateById: bypasses Reference indirection.
i64 asm_slow_path_put_private_by_id(VM* vm, u32 pc)
{
    vm->running_execution_context().program_counter = pc;
    auto* bytecode = vm->current_executable().bytecode.data();
    auto& insn = *reinterpret_cast<Op::PutPrivateById const*>(&bytecode[pc]);

    auto base_value = vm->get(insn.base());
    auto& current_vm = *vm;
    auto value = vm->get(insn.src());

    if (!base_value.is_object()) [[unlikely]] {
        auto object = base_value.to_object(current_vm);
        if (object.is_error())
            return handle_asm_exception(*vm, pc, object.error_value());
        auto const& name = current_vm.get_identifier(insn.property());
        auto private_reference = make_private_reference(current_vm, object.release_value(), name);
        auto result = private_reference.put_value(current_vm, value);
        if (result.is_error()) [[unlikely]]
            return handle_asm_exception(*vm, pc, result.error_value());
        return static_cast<i64>(pc + sizeof(Op::PutPrivateById));
    }

    auto const& name = current_vm.get_identifier(insn.property());
    auto private_environment = current_vm.running_execution_context().private_environment;
    VERIFY(private_environment);
    auto private_name = private_environment->resolve_private_identifier(name);
    auto result = base_value.as_object().private_set(private_name, value);
    if (result.is_error()) [[unlikely]]
        return handle_asm_exception(*vm, pc, result.error_value());
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
    return bit_cast<u64>(Value(&VM::the().empty_string()));
}

u64 asm_helper_single_ascii_character_string(u64 encoded_value)
{
    return bit_cast<u64>(Value(&VM::the().single_ascii_character_string(static_cast<u8>(encoded_value))));
}

u64 asm_helper_single_utf16_code_unit_string(u64 encoded_value)
{
    char16_t code_unit = static_cast<char16_t>(encoded_value);
    return bit_cast<u64>(Value(PrimitiveString::create(VM::the(), Utf16View(&code_unit, 1))));
}

} // extern "C"
