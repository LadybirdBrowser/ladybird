/*
 * Copyright (c) 2021-2025, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Bitmap.h>
#include <AK/ByteReader.h>
#include <AK/Debug.h>
#include <AK/Endian.h>
#include <AK/GenericShorthands.h>
#include <AK/MemoryStream.h>
#include <AK/NumericLimits.h>
#include <AK/QuickSort.h>
#include <AK/RedBlackTree.h>
#include <AK/SIMDExtras.h>
#include <AK/ScopedValueRollback.h>
#include <AK/Time.h>
#include <LibCore/File.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/AbstractMachine/BytecodeInterpreter.h>
#include <LibWasm/AbstractMachine/Configuration.h>
#include <LibWasm/AbstractMachine/Operators.h>
#include <LibWasm/Opcode.h>
#include <LibWasm/Printer/Printer.h>
#include <LibWasm/Types.h>

using namespace AK::SIMD;

#ifdef AK_COMPILER_CLANG
#    define TAILCALL [[clang::musttail]]
#    define HAS_TAILCALL
#elif defined(AK_COMPILER_GCC)
#    if ((__GNUC__ > 15) || ((__GNUC__ == 15) && defined(NDEBUG) && !defined(HAS_ADDRESS_SANITIZER)))
#        define TAILCALL [[gnu::musttail]]
#        define HAS_TAILCALL
#    else
#        define TAILCALL
#    endif
#else
#    define TAILCALL
#endif

// Disable direct threading when tail calls are not supported at all (gcc < 15);
// as without guaranteed tailcall optimization we cannot ensure that the stack
// will not grow uncontrollably.
#if !defined(HAS_TAILCALL)
constexpr static auto should_try_to_use_direct_threading = false;
#else
constexpr static auto should_try_to_use_direct_threading = true;
#endif

namespace Wasm {

constexpr auto regname = [](auto regnum) -> ByteString {
    if (regnum == Dispatch::Stack)
        return "stack";
    if (regnum >= Dispatch::CallRecord)
        return ByteString::formatted("cr{}", to_underlying(regnum) - to_underlying(Dispatch::CallRecord));
    return ByteString::formatted("reg{}", to_underlying(regnum));
};

template<typename T>
struct ConvertToRaw {
    T operator()(T value)
    {
        return LittleEndian<T>(value);
    }
};

template<>
struct ConvertToRaw<float> {
    u32 operator()(float value) const { return bit_cast<LittleEndian<u32>>(value); }
};

template<>
struct ConvertToRaw<double> {
    u64 operator()(double value) const { return bit_cast<LittleEndian<u64>>(value); }
};

#define TRAP_IF_NOT(x, ...)                                                                    \
    do {                                                                                       \
        if (trap_if_not(x, #x##sv __VA_OPT__(, ) __VA_ARGS__)) {                               \
            dbgln_if(WASM_TRACE_DEBUG, "Trapped because {} failed, at line {}", #x, __LINE__); \
            return Outcome::Return;                                                            \
        }                                                                                      \
    } while (false)

#define TRAP_IN_LOOP_IF_NOT(x, ...)                                                                    \
    do {                                                                                               \
        if (interpreter.trap_if_not(x, #x##sv __VA_OPT__(, ) __VA_ARGS__)) {                           \
            dbgln_if(WASM_TRACE_DEBUG, "Trapped in loop because {} failed, at line {}", #x, __LINE__); \
            return Outcome::Return;                                                                    \
        }                                                                                              \
    } while (false)

#define XM(name, _, ins, outs)             \
    case Wasm::Instructions::name.value(): \
        in_count = ins;                    \
        out_count = outs;                  \
        break;

#define LOG_INSN_UNGUARDED                                                                    \
    do {                                                                                      \
        LOAD_ADDRESSES();                                                                     \
        warnln("[{:04}]", short_ip.current_ip_value);                                         \
        ssize_t in_count = 0;                                                                 \
        ssize_t out_count = 0;                                                                \
        switch (instruction->opcode().value()) {                                              \
            ENUMERATE_WASM_OPCODES(XM)                                                        \
        }                                                                                     \
        ScopedValueRollback stack { configuration.value_stack() };                            \
        for (ssize_t i = 0; i < in_count; ++i) {                                              \
            auto value = configuration.take_source<source_address_mix>(i, addresses.sources); \
            warnln("       arg{} [{}]: {}", i, regname(addresses.sources[i]), value.value()); \
        }                                                                                     \
        if (out_count == 1) {                                                                 \
            auto dest = addresses.destination;                                                \
            warnln("       dest [{}]", regname(dest));                                        \
        } else if (out_count > 1) {                                                           \
            warnln("       dest [multiple outputs]");                                         \
        }                                                                                     \
    } while (0)

#define LOG_INSN                          \
    do {                                  \
        if constexpr (WASM_TRACE_DEBUG) { \
            LOG_INSN_UNGUARDED;           \
        }                                 \
    } while (0)

#define LOAD_ADDRESSES() auto addresses = addresses_ptr[short_ip.current_ip_value]

void BytecodeInterpreter::interpret(Configuration& configuration)
{
    m_trap = Empty {};
    auto& expression = configuration.frame().expression();
    auto const should_limit_instruction_count = configuration.should_limit_instruction_count();
    if (!expression.compiled_instructions.dispatches.is_empty()) {
        if (expression.compiled_instructions.direct) {
            if (should_limit_instruction_count)
                return interpret_impl<true, true, true>(configuration, expression);
            return interpret_impl<true, false, true>(configuration, expression);
        }
        return interpret_impl<true, false, false>(configuration, expression);
    }
    if (should_limit_instruction_count)
        return interpret_impl<false, true, false>(configuration, expression);
    return interpret_impl<false, false, false>(configuration, expression);
}

constexpr static u32 default_sources_and_destination = (to_underlying(Dispatch::RegisterOrStack::Stack) | (to_underlying(Dispatch::RegisterOrStack::Stack) << 2) | (to_underlying(Dispatch::RegisterOrStack::Stack) << 4));

template<u64 opcode>
struct InstructionHandler { };

struct ShortenedIP {
    u32 current_ip_value;
};

static_assert(sizeof(ShortenedIP) == sizeof(u32));

#define HANDLER_PARAMS(S)                   \
    S(BytecodeInterpreter&, interpreter),   \
        S(Configuration&, configuration),   \
        S(Instruction const*, instruction), \
        S(ShortenedIP, short_ip),           \
        S(Dispatch const*, cc),             \
        S(SourcesAndDestination const*, addresses_ptr)

#define DECOMPOSE_PARAMS(t, n) [[maybe_unused]] t n
#define DECOMPOSE_PARAMS_NAME_ONLY(t, n) n
#define DECOMPOSE_PARAMS_TYPE_ONLY(t, ...) t
#define HANDLE_INSTRUCTION(name, ...)                                                              \
    template<>                                                                                     \
    struct InstructionHandler<Instructions::name.value()> {                                        \
        template<bool HasDynamicInsnLimit, typename Continue, SourceAddressMix source_address_mix> \
        static Outcome operator()(HANDLER_PARAMS(DECOMPOSE_PARAMS));                               \
    };                                                                                             \
    template<bool HasDynamicInsnLimit, typename Continue, SourceAddressMix source_address_mix>     \
    FLATTEN Outcome InstructionHandler<Instructions::name.value()>::operator()(HANDLER_PARAMS(DECOMPOSE_PARAMS))
#define ALIAS_INSTRUCTION(new_name, existing_name)                                                                                                  \
    template<>                                                                                                                                      \
    struct InstructionHandler<Instructions::new_name.value()> {                                                                                     \
        template<bool HasDynamicInsnLimit, typename Continue, SourceAddressMix source_address_mix>                                                  \
        FLATTEN static Outcome operator()(HANDLER_PARAMS(DECOMPOSE_PARAMS))                                                                         \
        {                                                                                                                                           \
            TAILCALL return InstructionHandler<Instructions::existing_name.value()>::operator()<HasDynamicInsnLimit, Continue, source_address_mix>( \
                HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));                                                                                        \
        }                                                                                                                                           \
    };

struct Continue {
    ALWAYS_INLINE FLATTEN static Outcome operator()(BytecodeInterpreter& interpreter, Configuration& configuration, Instruction const*, ShortenedIP short_ip, Dispatch const* cc, SourcesAndDestination const* addresses_ptr)
    {
        short_ip.current_ip_value++;

        auto const instruction = cc[short_ip.current_ip_value].instruction;
        auto const handler = bit_cast<Outcome (*)(HANDLER_PARAMS(DECOMPOSE_PARAMS_TYPE_ONLY))>(cc[short_ip.current_ip_value].handler_ptr);
        TAILCALL return handler(interpreter, configuration, instruction, short_ip, cc, addresses_ptr);
    }
};

struct Skip {
    static Outcome operator()(BytecodeInterpreter&, Configuration&, Instruction const*, ShortenedIP short_ip, Dispatch const*, SourcesAndDestination const*)
    {
        return static_cast<Outcome>(short_ip.current_ip_value);
    }
};

#define continue_(...) Continue::operator()(__VA_ARGS__)

HANDLE_INSTRUCTION(synthetic_end_expression)
{
    LOG_INSN;
    return Outcome::Return;
}

HANDLE_INSTRUCTION(f64_reinterpret_i64)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i64, double, Operators::Reinterpret<double>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_extend8_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i32, i32, Operators::SignExtend<i8>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_extend16_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i32, i32, Operators::SignExtend<i16>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_extend8_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i64, i64, Operators::SignExtend<i8>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_extend16_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i64, i64, Operators::SignExtend<i16>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_extend32_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i64, i64, Operators::SignExtend<i32>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_trunc_sat_f32_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<float, i32, Operators::SaturatingTruncate<i32>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_trunc_sat_f32_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<float, i32, Operators::SaturatingTruncate<u32>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_trunc_sat_f64_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<double, i32, Operators::SaturatingTruncate<i32>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_trunc_sat_f64_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<double, i32, Operators::SaturatingTruncate<u32>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_trunc_sat_f32_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<float, i64, Operators::SaturatingTruncate<i64>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_trunc_sat_f32_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<float, i64, Operators::SaturatingTruncate<u64>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_trunc_sat_f64_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<double, i64, Operators::SaturatingTruncate<i64>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_trunc_sat_f64_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<double, i64, Operators::SaturatingTruncate<u64>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_const)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(Value(instruction->arguments().get<u128>()), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_load)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push<u128, u128, source_address_mix>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_load8x8_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push_mxn<8, 8, MakeSigned>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_load8x8_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push_mxn<8, 8, MakeUnsigned>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_load16x4_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push_mxn<16, 4, MakeSigned>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_load16x4_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push_mxn<16, 4, MakeUnsigned>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_load32x2_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push_mxn<32, 2, MakeSigned>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_load32x2_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push_mxn<32, 2, MakeUnsigned>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_load8_splat)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push_m_splat<8>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_load16_splat)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push_m_splat<16>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_load32_splat)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push_m_splat<32>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_load64_splat)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push_m_splat<64>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_splat)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    interpreter.pop_and_push_m_splat<8, NativeIntegralType>(configuration, *instruction, addresses);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_splat)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    interpreter.pop_and_push_m_splat<16, NativeIntegralType>(configuration, *instruction, addresses);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_splat)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    interpreter.pop_and_push_m_splat<32, NativeIntegralType>(configuration, *instruction, addresses);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_splat)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    interpreter.pop_and_push_m_splat<64, NativeIntegralType>(configuration, *instruction, addresses);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_splat)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    interpreter.pop_and_push_m_splat<32, NativeFloatingType>(configuration, *instruction, addresses);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_splat)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    interpreter.pop_and_push_m_splat<64, NativeFloatingType>(configuration, *instruction, addresses);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_shuffle)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto& arg = instruction->arguments().get<Instruction::ShuffleArgument>();
    auto b = interpreter.pop_vector<u8, MakeUnsigned>(configuration, 0, addresses);
    auto a = interpreter.pop_vector<u8, MakeUnsigned>(configuration, 1, addresses);
    using VectorType = Native128ByteVectorOf<u8, MakeUnsigned>;
    VectorType result;
    for (size_t i = 0; i < 16; ++i)
        if (arg.lanes[i] < 16)
            result[i] = a[arg.lanes[i]];
        else
            result[i] = b[arg.lanes[i] - 16];
    configuration.push_to_destination<source_address_mix>(Value(bit_cast<u128>(result)), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_store)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.pop_and_store<u128, u128>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_ge)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<double, i32, Operators::GreaterThanOrEquals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_clz)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i32, i32, Operators::CountLeadingZeros, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_ctz)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i32, i32, Operators::CountTrailingZeros, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_popcnt)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i32, i32, Operators::PopCount, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_add)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u32, i32, Operators::Add, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_sub)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u32, i32, Operators::Subtract, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_mul)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u32, i32, Operators::Multiply, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_divs)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i32, i32, Operators::Divide, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_divu)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u32, i32, Operators::Divide, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_rems)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i32, i32, Operators::Modulo, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_remu)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u32, i32, Operators::Modulo, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_and)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i32, i32, Operators::BitAnd, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_or)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i32, i32, Operators::BitOr, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_xor)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i32, i32, Operators::BitXor, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_shl)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u32, i32, Operators::BitShiftLeft, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_shrs)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i32, i32, Operators::BitShiftRight, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_shru)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u32, i32, Operators::BitShiftRight, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_rotl)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u32, i32, Operators::BitRotateLeft, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_rotr)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u32, i32, Operators::BitRotateRight, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_clz)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i64, i64, Operators::CountLeadingZeros, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_ctz)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i64, i64, Operators::CountTrailingZeros, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_popcnt)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i64, i64, Operators::PopCount, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_add)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u64, i64, Operators::Add, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_sub)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u64, i64, Operators::Subtract, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_mul)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u64, i64, Operators::Multiply, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_divs)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i64, i64, Operators::Divide, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_divu)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u64, i64, Operators::Divide, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_rems)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i64, i64, Operators::Modulo, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_remu)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u64, i64, Operators::Modulo, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_and)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i64, i64, Operators::BitAnd, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_or)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i64, i64, Operators::BitOr, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_xor)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i64, i64, Operators::BitXor, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_shl)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u64, i64, Operators::BitShiftLeft, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_shrs)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i64, i64, Operators::BitShiftRight, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_shru)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u64, i64, Operators::BitShiftRight, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_rotl)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u64, i64, Operators::BitRotateLeft, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_rotr)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u64, i64, Operators::BitRotateRight, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_abs)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<float, float, Operators::Absolute, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_neg)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<float, float, Operators::Negate, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_ceil)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<float, float, Operators::Ceil, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_floor)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<float, float, Operators::Floor, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_trunc)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<float, float, Operators::Truncate, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_nearest)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<float, float, Operators::NearbyIntegral, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_sqrt)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<float, float, Operators::SquareRoot, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_add)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<float, float, Operators::Add, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_sub)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<float, float, Operators::Subtract, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_mul)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<float, float, Operators::Multiply, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_div)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<float, float, Operators::Divide, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_min)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<float, float, Operators::Minimum, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_max)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<float, float, Operators::Maximum, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_copysign)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<float, float, Operators::CopySign, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_abs)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<double, double, Operators::Absolute, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_neg)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<double, double, Operators::Negate, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_ceil)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<double, double, Operators::Ceil, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_floor)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<double, double, Operators::Floor, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_trunc)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<double, double, Operators::Truncate, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_nearest)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<double, double, Operators::NearbyIntegral, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_sqrt)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<double, double, Operators::SquareRoot, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_add)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<double, double, Operators::Add, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_sub)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<double, double, Operators::Subtract, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_mul)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<double, double, Operators::Multiply, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_div)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<double, double, Operators::Divide, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_min)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<double, double, Operators::Minimum, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_max)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<double, double, Operators::Maximum, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_copysign)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<double, double, Operators::CopySign, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_wrap_i64)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i64, i32, Operators::Wrap<i32>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_trunc_sf32)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<float, i32, Operators::CheckedTruncate<i32>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_trunc_uf32)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<float, i32, Operators::CheckedTruncate<u32>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_trunc_sf64)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<double, i32, Operators::CheckedTruncate<i32>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_trunc_uf64)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<double, i32, Operators::CheckedTruncate<u32>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_trunc_sf32)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<float, i64, Operators::CheckedTruncate<i64>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_trunc_uf32)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<float, i64, Operators::CheckedTruncate<u64>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_trunc_sf64)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<double, i64, Operators::CheckedTruncate<i64>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_trunc_uf64)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<double, i64, Operators::CheckedTruncate<u64>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_extend_si32)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i32, i64, Operators::Extend<i64>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_extend_ui32)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u32, i64, Operators::Extend<i64>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_convert_si32)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i32, float, Operators::Convert<float>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_convert_ui32)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u32, float, Operators::Convert<float>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_convert_si64)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i64, float, Operators::Convert<float>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_convert_ui64)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u64, float, Operators::Convert<float>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_demote_f64)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<double, float, Operators::Demote, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_convert_si32)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i32, double, Operators::Convert<double>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_convert_ui32)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u32, double, Operators::Convert<double>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_convert_si64)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i64, double, Operators::Convert<double>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_convert_ui64)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u64, double, Operators::Convert<double>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_promote_f32)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<float, double, Operators::Promote, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_reinterpret_f32)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<float, i32, Operators::Reinterpret<i32>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_reinterpret_f64)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<double, i64, Operators::Reinterpret<i64>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_reinterpret_i32)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i32, float, Operators::Reinterpret<float>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(local_get)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(configuration.local(instruction->local_index()), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

#define HANDLE_SPECIALIZED_LOCAL_GET(N)                                                                       \
    HANDLE_INSTRUCTION(synthetic_local_get_##N)                                                               \
    {                                                                                                         \
        LOG_INSN;                                                                                             \
        LOAD_ADDRESSES();                                                                                     \
        configuration.push_to_destination<source_address_mix>(configuration.local(N), addresses.destination); \
        TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));                                \
    }

HANDLE_SPECIALIZED_LOCAL_GET(0)
HANDLE_SPECIALIZED_LOCAL_GET(1)
HANDLE_SPECIALIZED_LOCAL_GET(2)
HANDLE_SPECIALIZED_LOCAL_GET(3)
HANDLE_SPECIALIZED_LOCAL_GET(4)
HANDLE_SPECIALIZED_LOCAL_GET(5)
HANDLE_SPECIALIZED_LOCAL_GET(6)
HANDLE_SPECIALIZED_LOCAL_GET(7)

HANDLE_INSTRUCTION(synthetic_argument_get)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(configuration.local(instruction->local_index()), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_const)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(Value(instruction->arguments().unsafe_get<i32>()), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i32_add2local)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(
        Value(static_cast<i32>(Operators::Add {}(
            configuration.local(instruction->local_index()).to<u32>(),
            configuration.local(instruction->arguments().get<LocalIndex>()).to<u32>()))),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i32_addconstlocal)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(Value(static_cast<i32>(Operators::Add {}(configuration.local(instruction->local_index()).to<u32>(), instruction->arguments().unsafe_get<i32>()))), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i32_andconstlocal)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(Value(Operators::BitAnd {}(configuration.local(instruction->local_index()).to<i32>(), instruction->arguments().unsafe_get<i32>())), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i32_sub2local)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(
        Value(static_cast<i32>(Operators::Subtract {}(
            configuration.local(instruction->local_index()).to<u32>(),
            configuration.local(instruction->arguments().get<LocalIndex>()).to<u32>()))),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i32_mul2local)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(
        Value(static_cast<i32>(Operators::Multiply {}(
            configuration.local(instruction->local_index()).to<u32>(),
            configuration.local(instruction->arguments().get<LocalIndex>()).to<u32>()))),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i32_and2local)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(
        Value(Operators::BitAnd {}(
            configuration.local(instruction->local_index()).to<i32>(),
            configuration.local(instruction->arguments().get<LocalIndex>()).to<i32>())),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i32_or2local)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(
        Value(Operators::BitOr {}(
            configuration.local(instruction->local_index()).to<i32>(),
            configuration.local(instruction->arguments().get<LocalIndex>()).to<i32>())),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i32_xor2local)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(
        Value(Operators::BitXor {}(
            configuration.local(instruction->local_index()).to<i32>(),
            configuration.local(instruction->arguments().get<LocalIndex>()).to<i32>())),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i32_shl2local)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(
        Value(Operators::BitShiftLeft {}(
            configuration.local(instruction->local_index()).to<u32>(),
            configuration.local(instruction->arguments().get<LocalIndex>()).to<u32>())),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i32_shru2local)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(
        Value(Operators::BitShiftRight {}(
            configuration.local(instruction->local_index()).to<u32>(),
            configuration.local(instruction->arguments().get<LocalIndex>()).to<u32>())),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i32_shrs2local)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(
        Value(Operators::BitShiftRight {}(
            configuration.local(instruction->local_index()).to<i32>(),
            configuration.local(instruction->arguments().get<LocalIndex>()).to<u32>())),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i32_storelocal)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.store_value(configuration, *instruction, ConvertToRaw<i32> {}(configuration.local(instruction->local_index()).to<i32>()), 0, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i64_storelocal)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.store_value(configuration, *instruction, ConvertToRaw<i64> {}(configuration.local(instruction->local_index()).to<i64>()), 0, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i64_add2local)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(
        Value(static_cast<i64>(Operators::Add {}(
            configuration.local(instruction->local_index()).to<u64>(),
            configuration.local(instruction->arguments().get<LocalIndex>()).to<u64>()))),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i64_addconstlocal)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(Value(static_cast<i64>(Operators::Add {}(configuration.local(instruction->local_index()).to<u64>(), instruction->arguments().unsafe_get<i64>()))), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i64_andconstlocal)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(Value(Operators::BitAnd {}(configuration.local(instruction->local_index()).to<i64>(), instruction->arguments().unsafe_get<i64>())), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i64_sub2local)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(
        Value(static_cast<i64>(Operators::Subtract {}(
            configuration.local(instruction->local_index()).to<u64>(),
            configuration.local(instruction->arguments().get<LocalIndex>()).to<u64>()))),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i64_mul2local)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(
        Value(static_cast<i64>(Operators::Multiply {}(
            configuration.local(instruction->local_index()).to<u64>(),
            configuration.local(instruction->arguments().get<LocalIndex>()).to<u64>()))),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i64_and2local)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(
        Value(Operators::BitAnd {}(
            configuration.local(instruction->local_index()).to<i64>(),
            configuration.local(instruction->arguments().get<LocalIndex>()).to<i64>())),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i64_or2local)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(
        Value(Operators::BitOr {}(
            configuration.local(instruction->local_index()).to<i64>(),
            configuration.local(instruction->arguments().get<LocalIndex>()).to<i64>())),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i64_xor2local)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(
        Value(Operators::BitXor {}(
            configuration.local(instruction->local_index()).to<i64>(),
            configuration.local(instruction->arguments().get<LocalIndex>()).to<i64>())),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i64_shl2local)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(
        Value(Operators::BitShiftLeft {}(
            configuration.local(instruction->local_index()).to<u64>(),
            configuration.local(instruction->arguments().get<LocalIndex>()).to<u64>())),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i64_shru2local)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(
        Value(Operators::BitShiftRight {}(
            configuration.local(instruction->local_index()).to<u64>(),
            configuration.local(instruction->arguments().get<LocalIndex>()).to<u64>())),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_i64_shrs2local)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(
        Value(Operators::BitShiftRight {}(
            configuration.local(instruction->local_index()).to<i64>(),
            configuration.local(instruction->arguments().get<LocalIndex>()).to<u64>())),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_local_seti32_const)
{
    LOG_INSN;
    configuration.local(instruction->local_index()) = Value(instruction->arguments().unsafe_get<i32>());
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_local_seti64_const)
{
    LOG_INSN;
    configuration.local(instruction->local_index()) = Value(instruction->arguments().unsafe_get<i64>());
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_call_00)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto index = instruction->arguments().get<FunctionIndex>();
    auto address = configuration.frame().module().functions()[index.value()];
    dbgln_if(WASM_TRACE_DEBUG, "[{}] call_00(#{} -> {})", short_ip.current_ip_value, index.value(), address.value());
    if (interpreter.call_address(configuration, address, addresses, BytecodeInterpreter::CallAddressSource::DirectCall, BytecodeInterpreter::CallType::UsingRegisters) == Outcome::Return)
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_call_01)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto index = instruction->arguments().get<FunctionIndex>();
    auto address = configuration.frame().module().functions()[index.value()];
    dbgln_if(WASM_TRACE_DEBUG, "[{}] call_01(#{} -> {})", short_ip.current_ip_value, index.value(), address.value());
    if (interpreter.call_address(configuration, address, addresses, BytecodeInterpreter::CallAddressSource::DirectCall, BytecodeInterpreter::CallType::UsingRegisters) == Outcome::Return)
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_call_10)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto index = instruction->arguments().get<FunctionIndex>();
    auto address = configuration.frame().module().functions()[index.value()];
    dbgln_if(WASM_TRACE_DEBUG, "[{}] call_10(#{} -> {})", short_ip.current_ip_value, index.value(), address.value());
    if (interpreter.call_address(configuration, address, addresses, BytecodeInterpreter::CallAddressSource::DirectCall, BytecodeInterpreter::CallType::UsingRegisters) == Outcome::Return)
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_call_11)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto index = instruction->arguments().get<FunctionIndex>();
    auto address = configuration.frame().module().functions()[index.value()];
    dbgln_if(WASM_TRACE_DEBUG, "[{}] call_11(#{} -> {})", short_ip.current_ip_value, index.value(), address.value());
    if (interpreter.call_address(configuration, address, addresses, BytecodeInterpreter::CallAddressSource::DirectCall, BytecodeInterpreter::CallType::UsingRegisters) == Outcome::Return)
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_call_20)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto index = instruction->arguments().get<FunctionIndex>();
    auto address = configuration.frame().module().functions()[index.value()];
    dbgln_if(WASM_TRACE_DEBUG, "[{}] call_20(#{} -> {})", short_ip.current_ip_value, index.value(), address.value());
    if (interpreter.call_address(configuration, address, addresses, BytecodeInterpreter::CallAddressSource::DirectCall, BytecodeInterpreter::CallType::UsingRegisters) == Outcome::Return)
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_call_21)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto index = instruction->arguments().get<FunctionIndex>();
    auto address = configuration.frame().module().functions()[index.value()];
    dbgln_if(WASM_TRACE_DEBUG, "[{}] call_21(#{} -> {})", short_ip.current_ip_value, index.value(), address.value());
    if (interpreter.call_address(configuration, address, addresses, BytecodeInterpreter::CallAddressSource::DirectCall, BytecodeInterpreter::CallType::UsingRegisters) == Outcome::Return)
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_call_30)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto index = instruction->arguments().get<FunctionIndex>();
    auto address = configuration.frame().module().functions()[index.value()];
    dbgln_if(WASM_TRACE_DEBUG, "[{}] call_30(#{} -> {})", short_ip.current_ip_value, index.value(), address.value());
    if (interpreter.call_address(configuration, address, addresses, BytecodeInterpreter::CallAddressSource::DirectCall, BytecodeInterpreter::CallType::UsingRegisters) == Outcome::Return)
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_call_31)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto index = instruction->arguments().get<FunctionIndex>();
    auto address = configuration.frame().module().functions()[index.value()];
    dbgln_if(WASM_TRACE_DEBUG, "[{}] call_31(#{} -> {})", short_ip.current_ip_value, index.value(), address.value());
    if (interpreter.call_address(configuration, address, addresses, BytecodeInterpreter::CallAddressSource::DirectCall, BytecodeInterpreter::CallType::UsingRegisters) == Outcome::Return)
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(unreachable)
{
    LOG_INSN;
    interpreter.set_trap("Unreachable"sv);
    return Outcome::Return;
}

HANDLE_INSTRUCTION(nop)
{
    LOG_INSN;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(local_set)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    // bounds checked by verifier.
    configuration.local(instruction->local_index()) = configuration.take_source<source_address_mix>(0, addresses.sources);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_argument_set)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    // bounds checked by verifier.
    configuration.local(instruction->local_index()) = configuration.take_source<source_address_mix>(0, addresses.sources);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

#define HANDLE_SPECIALIZED_LOCAL_SET(N)                                                               \
    HANDLE_INSTRUCTION(synthetic_local_set_##N)                                                       \
    {                                                                                                 \
        LOG_INSN;                                                                                     \
        LOAD_ADDRESSES();                                                                             \
        configuration.local(N) = configuration.take_source<source_address_mix>(0, addresses.sources); \
        TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));                        \
    }

HANDLE_SPECIALIZED_LOCAL_SET(0)
HANDLE_SPECIALIZED_LOCAL_SET(1)
HANDLE_SPECIALIZED_LOCAL_SET(2)
HANDLE_SPECIALIZED_LOCAL_SET(3)
HANDLE_SPECIALIZED_LOCAL_SET(4)
HANDLE_SPECIALIZED_LOCAL_SET(5)
HANDLE_SPECIALIZED_LOCAL_SET(6)
HANDLE_SPECIALIZED_LOCAL_SET(7)

HANDLE_INSTRUCTION(synthetic_local_copy)
{
    LOG_INSN;
    // local.get a; local.set b -> copy local a to local b directly
    configuration.local(instruction->arguments().get<LocalIndex>()) = configuration.local(instruction->local_index());
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_const)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(Value(instruction->arguments().unsafe_get<i64>()), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_const)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(Value(instruction->arguments().unsafe_get<float>()), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_const)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    configuration.push_to_destination<source_address_mix>(Value(instruction->arguments().unsafe_get<double>()), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(block)
{
    LOG_INSN;
    auto& args = instruction->arguments().unsafe_get<Instruction::StructuredInstructionArgs>();
    auto& meta = args.meta.unchecked_value();
    auto label = Label(meta.arity, args.end_ip, configuration.value_stack().size() - meta.parameter_count);
    configuration.label_stack().unchecked_append(move(label));
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(loop)
{
    LOG_INSN;
    auto& args = instruction->arguments().get<Instruction::StructuredInstructionArgs>();
    size_t params = args.meta->parameter_count;
    configuration.label_stack().unchecked_append(Label(params, short_ip.current_ip_value + 1, configuration.value_stack().size() - params));
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(if_)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto& args = instruction->arguments().unsafe_get<Instruction::StructuredInstructionArgs>();
    auto& meta = args.meta.value();

    auto value = configuration.take_source<source_address_mix>(0, addresses.sources).template to<i32>();
    auto end_label = Label(meta.arity, args.end_ip.value(), configuration.value_stack().size() - meta.parameter_count);
    if (value == 0) {
        if (args.else_ip.has_value()) {
            short_ip.current_ip_value = args.else_ip->value() - 1;
            configuration.label_stack().unchecked_append(end_label);
        } else {
            short_ip.current_ip_value = args.end_ip.value();
        }
    } else {
        configuration.label_stack().unchecked_append(end_label);
    }
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(structured_end)
{
    LOG_INSN;
    configuration.label_stack().take_last();
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(structured_else)
{
    LOG_INSN;
    auto label = configuration.label_stack().take_last();
    // Jump to the end label
    short_ip.current_ip_value = label.continuation().value() - 1;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(return_)
{
    LOG_INSN;
    configuration.label_stack().shrink(configuration.frame().label_index() + 1, true);
    return Outcome::Return;
}

HANDLE_INSTRUCTION(br)
{
    LOG_INSN;
    short_ip.current_ip_value = interpreter.branch_to_label<true>(configuration, instruction->arguments().unsafe_get<Instruction::BranchArgs>().label, short_ip.current_ip_value).value();
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_br_nostack)
{
    LOG_INSN;
    short_ip.current_ip_value = interpreter.branch_to_label<false>(configuration, instruction->arguments().unsafe_get<Instruction::BranchArgs>().label, short_ip.current_ip_value).value();
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(br_if)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    // bounds checked by verifier.
    auto cond = configuration.take_source<source_address_mix>(0, addresses.sources).template to<i32>();
    short_ip.current_ip_value = interpreter.branch_to_label<true>(configuration, instruction->arguments().unsafe_get<Instruction::BranchArgs>().label, short_ip.current_ip_value, cond != 0).value();
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_br_if_nostack)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    // bounds checked by verifier.
    auto cond = configuration.take_source<source_address_mix>(0, addresses.sources).template to<i32>();
    short_ip.current_ip_value = interpreter.branch_to_label<false>(configuration, instruction->arguments().unsafe_get<Instruction::BranchArgs>().label, short_ip.current_ip_value, cond != 0).value();
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(br_table)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto& args = instruction->arguments().get<Instruction::TableBranchArgs>();
    auto i = configuration.take_source<source_address_mix>(0, addresses.sources).template to<u32>();

    if (i >= args.labels.size()) {
        short_ip.current_ip_value = interpreter.branch_to_label<true>(configuration, args.default_, short_ip.current_ip_value).value();
        TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
    }
    short_ip.current_ip_value = interpreter.branch_to_label<true>(configuration, args.labels[i], short_ip.current_ip_value).value();
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(call)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto index = instruction->arguments().get<FunctionIndex>();
    auto address = configuration.frame().module().functions()[index.value()];
    dbgln_if(WASM_TRACE_DEBUG, "call({})", address.value());
    if (interpreter.call_address(configuration, address, addresses) == Outcome::Return)
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_call_with_record_0)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto index = instruction->arguments().get<FunctionIndex>();
    auto address = configuration.frame().module().functions()[index.value()];
    dbgln_if(WASM_TRACE_DEBUG, "call.with_record.0({})", address.value());
    if (interpreter.call_address(configuration, address, addresses, BytecodeInterpreter::CallAddressSource::DirectCall, BytecodeInterpreter::CallType::UsingCallRecord) == Outcome::Return)
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_call_with_record_1)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto index = instruction->arguments().get<FunctionIndex>();
    auto address = configuration.frame().module().functions()[index.value()];
    dbgln_if(WASM_TRACE_DEBUG, "call.with_record.1({})", address.value());
    if (interpreter.call_address(configuration, address, addresses, BytecodeInterpreter::CallAddressSource::DirectCall, BytecodeInterpreter::CallType::UsingCallRecord) == Outcome::Return)
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(return_call)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto index = instruction->arguments().get<FunctionIndex>();
    auto address = configuration.frame().module().functions()[index.value()];
    configuration.label_stack().shrink(configuration.frame().label_index(), true);
    dbgln_if(WASM_TRACE_DEBUG, "tail call({})", address.value());
    switch (auto const outcome = interpreter.call_address(configuration, address, addresses, BytecodeInterpreter::CallAddressSource::DirectTailCall)) {
    default:
        // Some IP we have to continue from.
        short_ip.current_ip_value = to_underlying(outcome) - 1;
        addresses = { .sources_and_destination = default_sources_and_destination };
        cc = configuration.frame().expression().compiled_instructions.dispatches.data();
        addresses_ptr = configuration.frame().expression().compiled_instructions.src_dst_mappings.data();
        [[fallthrough]];
    case Outcome::Continue:
        TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
    case Outcome::Return:
        return Outcome::Return;
    }
}

HANDLE_INSTRUCTION(call_indirect)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto& args = instruction->arguments().get<Instruction::IndirectCallArgs>();
    auto table_address = configuration.frame().module().tables()[args.table.value()];
    auto table_instance = configuration.store().get(table_address);
    // bounds checked by verifier.
    auto index = configuration.take_source<source_address_mix>(0, addresses.sources).template to<i32>();
    TRAP_IN_LOOP_IF_NOT(index >= 0);
    TRAP_IN_LOOP_IF_NOT(static_cast<size_t>(index) < table_instance->elements().size());
    auto& element = table_instance->elements()[index];
    TRAP_IN_LOOP_IF_NOT(element.ref().template has<Reference::Func>());
    auto address = element.ref().template get<Reference::Func>().address;
    auto const& type_actual = configuration.store().get(address)->visit([](auto& f) -> decltype(auto) { return f.type(); });
    auto const& type_expected = configuration.frame().module().types()[args.type.value()].unsafe_function();
    TRAP_IN_LOOP_IF_NOT(type_actual.parameters().size() == type_expected.parameters().size());
    TRAP_IN_LOOP_IF_NOT(type_actual.results().size() == type_expected.results().size());
    TRAP_IN_LOOP_IF_NOT(type_actual.parameters() == type_expected.parameters());
    TRAP_IN_LOOP_IF_NOT(type_actual.results() == type_expected.results());

    dbgln_if(WASM_TRACE_DEBUG, "call_indirect({} -> {})", index, address.value());
    if (interpreter.call_address(configuration, address, addresses, BytecodeInterpreter::CallAddressSource::IndirectCall) == Outcome::Return)
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(return_call_indirect)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto& args = instruction->arguments().get<Instruction::IndirectCallArgs>();
    auto table_address = configuration.frame().module().tables()[args.table.value()];
    auto table_instance = configuration.store().get(table_address);
    // bounds checked by verifier.
    auto index = configuration.take_source<source_address_mix>(0, addresses.sources).template to<i32>();
    TRAP_IN_LOOP_IF_NOT(index >= 0);
    TRAP_IN_LOOP_IF_NOT(static_cast<size_t>(index) < table_instance->elements().size());
    auto& element = table_instance->elements()[index];
    TRAP_IN_LOOP_IF_NOT(element.ref().template has<Reference::Func>());
    auto address = element.ref().template get<Reference::Func>().address;
    auto const& type_actual = configuration.store().get(address)->visit([](auto& f) -> decltype(auto) { return f.type(); });
    auto const& type_expected = configuration.frame().module().types()[args.type.value()].unsafe_function();
    TRAP_IN_LOOP_IF_NOT(type_actual.parameters().size() == type_expected.parameters().size());
    TRAP_IN_LOOP_IF_NOT(type_actual.results().size() == type_expected.results().size());
    TRAP_IN_LOOP_IF_NOT(type_actual.parameters() == type_expected.parameters());
    TRAP_IN_LOOP_IF_NOT(type_actual.results() == type_expected.results());

    configuration.label_stack().shrink(configuration.frame().label_index(), true);
    dbgln_if(WASM_TRACE_DEBUG, "tail call_indirect({} -> {})", index, address.value());
    switch (auto const outcome = interpreter.call_address(configuration, address, addresses, BytecodeInterpreter::CallAddressSource::IndirectTailCall)) {
    default:
        // Some IP we have to continue from.
        short_ip.current_ip_value = to_underlying(outcome) - 1;
        addresses = { .sources_and_destination = default_sources_and_destination };
        cc = configuration.frame().expression().compiled_instructions.dispatches.data();
        addresses_ptr = configuration.frame().expression().compiled_instructions.src_dst_mappings.data();
        [[fallthrough]];
    case Outcome::Continue:
        TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
    case Outcome::Return:
        return Outcome::Return;
    }
}

HANDLE_INSTRUCTION(i32_load)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push<i32, i32, source_address_mix>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_load)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push<i64, i64, source_address_mix>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_load)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push<float, float, source_address_mix>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_load)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push<double, double, source_address_mix>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_load8_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push<i8, i32, source_address_mix>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_load8_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push<u8, i32, source_address_mix>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_load16_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push<i16, i32, source_address_mix>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_load16_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push<u16, i32, source_address_mix>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_load8_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push<i8, i64, source_address_mix>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_load8_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push<u8, i64, source_address_mix>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_load16_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push<i16, i64, source_address_mix>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_load16_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push<u16, i64, source_address_mix>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_load32_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push<i32, i64, source_address_mix>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_load32_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push<u32, i64, source_address_mix>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_store)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.pop_and_store<i32, i32>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_store)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.pop_and_store<i64, i64>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_store)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.pop_and_store<float, float>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_store)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.pop_and_store<double, double>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_store8)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.pop_and_store<i32, i8>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_store16)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.pop_and_store<i32, i16>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_store8)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.pop_and_store<i64, i8>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_store16)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.pop_and_store<i64, i16>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_store32)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.pop_and_store<i64, i32>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(local_tee)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto value = configuration.source_value<source_address_mix>(0, addresses.sources); // bounds checked by verifier.
    auto local_index = instruction->local_index();
    dbgln_if(WASM_TRACE_DEBUG, "stack:peek -> locals({})", local_index.value());
    configuration.local(local_index) = value;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(synthetic_argument_tee)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto value = configuration.source_value<source_address_mix>(0, addresses.sources); // bounds checked by verifier.
    auto local_index = instruction->local_index();
    dbgln_if(WASM_TRACE_DEBUG, "stack:peek -> locals({})", local_index.value());
    configuration.local(local_index) = value;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(global_get)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto global_index = instruction->arguments().get<GlobalIndex>();
    // This check here is for const expressions. In non-const expressions,
    // a validation error would have been thrown.
    TRAP_IN_LOOP_IF_NOT(global_index < configuration.frame().module().globals().size());
    auto address = configuration.frame().module().globals()[global_index.value()];
    dbgln_if(WASM_TRACE_DEBUG, "global({}) -> stack", address.value());
    auto global = configuration.store().get(address);
    configuration.push_to_destination<source_address_mix>(global->value(), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(global_set)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto global_index = instruction->arguments().get<GlobalIndex>();
    auto address = configuration.frame().module().globals()[global_index.value()];
    // bounds checked by verifier.
    auto value = configuration.take_source<source_address_mix>(0, addresses.sources);
    dbgln_if(WASM_TRACE_DEBUG, "stack -> global({})", address.value());
    auto global = configuration.store().get(address);
    global->set_value(value);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(memory_size)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto& args = instruction->arguments().unsafe_get<Instruction::MemoryIndexArgument>();
    auto address = configuration.frame().module().memories().data()[args.memory_index.value()];
    auto instance = configuration.store().get(address);
    auto pages = instance->size() / Constants::page_size;
    dbgln_if(WASM_TRACE_DEBUG, "memory.size -> stack({})", pages);
    configuration.push_to_destination<source_address_mix>(Value(static_cast<i32>(pages)), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(memory_grow)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto& args = instruction->arguments().unsafe_get<Instruction::MemoryIndexArgument>();
    auto address = configuration.frame().module().memories().data()[args.memory_index.value()];
    auto instance = configuration.store().get(address);
    i32 old_pages = instance->size() / Constants::page_size;
    auto& entry = configuration.source_value<source_address_mix>(0, addresses.sources); // bounds checked by verifier.
    auto new_pages = entry.template to<i32>();
    dbgln_if(WASM_TRACE_DEBUG, "memory.grow({}), previously {} pages...", new_pages, old_pages);
    if (instance->grow(new_pages * Constants::page_size))
        entry = Value(old_pages);
    else
        entry = Value(-1);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(memory_fill)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    {
        auto& args = instruction->arguments().unsafe_get<Instruction::MemoryIndexArgument>();
        auto address = configuration.frame().module().memories().data()[args.memory_index.value()];
        auto instance = configuration.store().get(address);
        // bounds checked by verifier.
        auto const count = configuration.take_source<source_address_mix>(0, addresses.sources).template to<u32>();
        auto const value = static_cast<u8>(configuration.take_source<source_address_mix>(1, addresses.sources).template to<u32>());
        auto const destination_offset = configuration.take_source<source_address_mix>(2, addresses.sources).template to<u32>();

        Checked<u64> checked_end = destination_offset;
        checked_end += count;
        TRAP_IN_LOOP_IF_NOT(!checked_end.has_overflow() && static_cast<size_t>(checked_end.value()) <= instance->data().size());

        if (count == 0)
            TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));

        for (u64 i = 0; i < count; ++i) {
            if (interpreter.store_to_memory(*instance, destination_offset + i, value))
                return Outcome::Return;
        }
    }

    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(memory_copy)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto& args = instruction->arguments().unsafe_get<Instruction::MemoryCopyArgs>();
    auto source_address = configuration.frame().module().memories().data()[args.src_index.value()];
    auto destination_address = configuration.frame().module().memories().data()[args.dst_index.value()];
    auto source_instance = configuration.store().get(source_address);
    auto destination_instance = configuration.store().get(destination_address);

    // bounds checked by verifier.
    auto count = configuration.take_source<source_address_mix>(0, addresses.sources).template to<i32>();
    auto source_offset = configuration.take_source<source_address_mix>(1, addresses.sources).template to<i32>();
    auto destination_offset = configuration.take_source<source_address_mix>(2, addresses.sources).template to<i32>();

    Checked<size_t> source_position = source_offset;
    source_position.saturating_add(count);
    Checked<size_t> destination_position = destination_offset;
    destination_position.saturating_add(count);
    TRAP_IN_LOOP_IF_NOT(source_position <= source_instance->data().size());
    TRAP_IN_LOOP_IF_NOT(destination_position <= destination_instance->data().size());

    if (count == 0)
        TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));

    if (destination_offset <= source_offset) {
        for (auto i = 0; i < count; ++i) {
            auto value = source_instance->data()[source_offset + i];
            if (interpreter.store_to_memory(*destination_instance, destination_offset + i, value))
                return Outcome::Return;
        }
    } else {
        for (auto i = count - 1; i >= 0; --i) {
            auto value = source_instance->data()[source_offset + i];
            if (interpreter.store_to_memory(*destination_instance, destination_offset + i, value))
                return Outcome::Return;
        }
    }

    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(memory_init)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto& args = instruction->arguments().unsafe_get<Instruction::MemoryInitArgs>();
    auto& data_address = configuration.frame().module().datas()[args.data_index.value()];
    auto& data = *configuration.store().get(data_address);
    auto memory_address = configuration.frame().module().memories().data()[args.memory_index.value()];
    auto memory = configuration.store().unsafe_get(memory_address);
    // bounds checked by verifier.
    auto count = configuration.take_source<source_address_mix>(0, addresses.sources).template to<u32>();
    auto source_offset = configuration.take_source<source_address_mix>(1, addresses.sources).template to<u32>();
    auto destination_offset = configuration.take_source<source_address_mix>(2, addresses.sources).template to<u32>();

    Checked<size_t> source_position = source_offset;
    source_position.saturating_add(count);
    Checked<size_t> destination_position = destination_offset;
    destination_position.saturating_add(count);
    TRAP_IN_LOOP_IF_NOT(source_position <= data.data().size());
    TRAP_IN_LOOP_IF_NOT(destination_position <= memory->data().size());

    if (count == 0)
        TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));

    for (size_t i = 0; i < (size_t)count; ++i) {
        auto value = data.data()[source_offset + i];
        if (interpreter.store_to_memory(*memory, destination_offset + i, value))
            return Outcome::Return;
    }
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(data_drop)
{
    LOG_INSN;
    auto data_index = instruction->arguments().get<DataIndex>();
    auto data_address = configuration.frame().module().datas()[data_index.value()];
    *configuration.store().get(data_address) = DataInstance({});
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(elem_drop)
{
    LOG_INSN;
    auto elem_index = instruction->arguments().get<ElementIndex>();
    auto address = configuration.frame().module().elements()[elem_index.value()];
    auto elem = configuration.store().get(address);
    *configuration.store().get(address) = ElementInstance(elem->type(), {});
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(table_init)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto& args = instruction->arguments().get<Instruction::TableElementArgs>();
    auto table_address = configuration.frame().module().tables()[args.table_index.value()];
    auto table = configuration.store().get(table_address);
    auto element_address = configuration.frame().module().elements()[args.element_index.value()];
    auto element = configuration.store().get(element_address);
    // bounds checked by verifier.
    auto count = configuration.take_source<source_address_mix>(0, addresses.sources).template to<u32>();
    auto source_offset = configuration.take_source<source_address_mix>(1, addresses.sources).template to<u32>();
    auto destination_offset = configuration.take_source<source_address_mix>(2, addresses.sources).template to<u32>();

    Checked<u32> checked_source_offset = source_offset;
    Checked<u32> checked_destination_offset = destination_offset;
    checked_source_offset += count;
    checked_destination_offset += count;
    TRAP_IN_LOOP_IF_NOT(!checked_source_offset.has_overflow() && checked_source_offset <= (u32)element->references().size());
    TRAP_IN_LOOP_IF_NOT(!checked_destination_offset.has_overflow() && checked_destination_offset <= (u32)table->elements().size());

    for (u32 i = 0; i < count; ++i)
        table->elements()[destination_offset + i] = element->references()[source_offset + i];
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(table_copy)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto& args = instruction->arguments().get<Instruction::TableTableArgs>();
    auto source_address = configuration.frame().module().tables()[args.rhs.value()];
    auto destination_address = configuration.frame().module().tables()[args.lhs.value()];
    auto source_instance = configuration.store().get(source_address);
    auto destination_instance = configuration.store().get(destination_address);

    // bounds checked by verifier.
    auto count = configuration.take_source<source_address_mix>(0, addresses.sources).template to<u32>();
    auto source_offset = configuration.take_source<source_address_mix>(1, addresses.sources).template to<u32>();
    auto destination_offset = configuration.take_source<source_address_mix>(2, addresses.sources).template to<u32>();

    Checked<size_t> source_position = source_offset;
    source_position.saturating_add(count);
    Checked<size_t> destination_position = destination_offset;
    destination_position.saturating_add(count);
    TRAP_IN_LOOP_IF_NOT(source_position <= source_instance->elements().size());
    TRAP_IN_LOOP_IF_NOT(destination_position <= destination_instance->elements().size());

    if (count == 0)
        TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));

    if (destination_offset <= source_offset) {
        for (u32 i = 0; i < count; ++i) {
            auto value = source_instance->elements()[source_offset + i];
            destination_instance->elements()[destination_offset + i] = value;
        }
    } else {
        for (u32 i = count - 1; i != NumericLimits<u32>::max(); --i) {
            auto value = source_instance->elements()[source_offset + i];
            destination_instance->elements()[destination_offset + i] = value;
        }
    }

    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(table_fill)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto table_index = instruction->arguments().get<TableIndex>();
    auto address = configuration.frame().module().tables()[table_index.value()];
    auto table = configuration.store().get(address);
    // bounds checked by verifier.
    auto count = configuration.take_source<source_address_mix>(0, addresses.sources).template to<u32>();
    auto value = configuration.take_source<source_address_mix>(1, addresses.sources);
    auto start = configuration.take_source<source_address_mix>(2, addresses.sources).template to<u32>();

    Checked<u32> checked_offset = start;
    checked_offset += count;
    TRAP_IN_LOOP_IF_NOT(!checked_offset.has_overflow() && checked_offset <= (u32)table->elements().size());

    for (u32 i = 0; i < count; ++i)
        table->elements()[start + i] = value.template to<Reference>();
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(table_set)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    // bounds checked by verifier.
    auto ref = configuration.take_source<source_address_mix>(0, addresses.sources);
    auto index = (size_t)(configuration.take_source<source_address_mix>(1, addresses.sources).template to<i32>());
    auto table_index = instruction->arguments().get<TableIndex>();
    auto address = configuration.frame().module().tables()[table_index.value()];
    auto table = configuration.store().get(address);
    TRAP_IN_LOOP_IF_NOT(index < table->elements().size());
    table->elements()[index] = ref.template to<Reference>();
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(table_get)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    // bounds checked by verifier.
    auto& index_value = configuration.source_value<source_address_mix>(0, addresses.sources);
    auto index = static_cast<size_t>(index_value.template to<i32>());
    auto table_index = instruction->arguments().get<TableIndex>();
    auto address = configuration.frame().module().tables()[table_index.value()];
    auto table = configuration.store().get(address);
    TRAP_IN_LOOP_IF_NOT(index < table->elements().size());
    index_value = Value(table->elements()[index]);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(table_grow)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    // bounds checked by verifier.
    auto size = configuration.take_source<source_address_mix>(0, addresses.sources).template to<u32>();
    auto fill_value = configuration.take_source<source_address_mix>(1, addresses.sources);
    auto table_index = instruction->arguments().get<TableIndex>();
    auto address = configuration.frame().module().tables()[table_index.value()];
    auto table = configuration.store().get(address);
    auto previous_size = table->elements().size();
    auto did_grow = table->grow(size, fill_value.template to<Reference>());
    if (!did_grow) {
        configuration.push_to_destination<source_address_mix>(Value(-1), addresses.destination);
    } else {
        configuration.push_to_destination<source_address_mix>(Value(static_cast<i32>(previous_size)), addresses.destination);
    }
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(table_size)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto table_index = instruction->arguments().get<TableIndex>();
    auto address = configuration.frame().module().tables()[table_index.value()];
    auto table = configuration.store().get(address);
    configuration.push_to_destination<source_address_mix>(Value(static_cast<i32>(table->elements().size())), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(ref_null)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto type = instruction->arguments().get<ValueType>();
    configuration.push_to_destination<source_address_mix>(Value(Reference(Reference::Null { type })), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(ref_func)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto index = instruction->arguments().get<FunctionIndex>().value();
    auto& functions = configuration.frame().module().functions();
    auto address = functions[index];
    configuration.push_to_destination<source_address_mix>(Value(Reference { Reference::Func { address, configuration.store().get_module_for(address) } }), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(ref_is_null)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    // bounds checked by verifier.
    auto ref = configuration.take_source<source_address_mix>(0, addresses.sources);
    configuration.push_to_destination<source_address_mix>(
        Value(static_cast<i32>(ref.template to<Reference>().ref().template has<Reference::Null>() ? 1 : 0)),
        addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(drop)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    // bounds checked by verifier.
    configuration.take_source<source_address_mix>(0, addresses.sources);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(select)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    // Note: The type seems to only be used for validation.
    auto value = configuration.take_source<source_address_mix>(0, addresses.sources).template to<i32>(); // bounds checked by verifier.
    dbgln_if(WASM_TRACE_DEBUG, "select({})", value);
    auto rhs = configuration.take_source<source_address_mix>(1, addresses.sources);
    auto& lhs = configuration.source_value<source_address_mix>(2, addresses.sources); // bounds checked by verifier.
    lhs = value != 0 ? lhs : rhs;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(select_typed)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    // Note: The type seems to only be used for validation.
    auto value = configuration.take_source<source_address_mix>(0, addresses.sources).template to<i32>(); // bounds checked by verifier.
    dbgln_if(WASM_TRACE_DEBUG, "select_typed({})", value);
    auto rhs = configuration.take_source<source_address_mix>(1, addresses.sources);
    auto& lhs = configuration.source_value<source_address_mix>(2, addresses.sources); // bounds checked by verifier.
    lhs = value != 0 ? lhs : rhs;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_eqz)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i32, i32, Operators::EqualsZero, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_eq)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i32, i32, Operators::Equals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_ne)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i32, i32, Operators::NotEquals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_lts)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i32, i32, Operators::LessThan, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_ltu)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u32, i32, Operators::LessThan, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_gts)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i32, i32, Operators::GreaterThan, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_gtu)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u32, i32, Operators::GreaterThan, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_les)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i32, i32, Operators::LessThanOrEquals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_leu)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u32, i32, Operators::LessThanOrEquals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_ges)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i32, i32, Operators::GreaterThanOrEquals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32_geu)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u32, i32, Operators::GreaterThanOrEquals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_eqz)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<i64, i32, Operators::EqualsZero, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_eq)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i64, i32, Operators::Equals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_ne)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i64, i32, Operators::NotEquals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_lts)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i64, i32, Operators::LessThan, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_ltu)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u64, i32, Operators::LessThan, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_gts)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i64, i32, Operators::GreaterThan, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_gtu)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u64, i32, Operators::GreaterThan, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_les)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i64, i32, Operators::LessThanOrEquals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_leu)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u64, i32, Operators::LessThanOrEquals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_ges)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<i64, i32, Operators::GreaterThanOrEquals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64_geu)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u64, i32, Operators::GreaterThanOrEquals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_eq)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<float, i32, Operators::Equals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_ne)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<float, i32, Operators::NotEquals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_lt)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<float, i32, Operators::LessThan, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_gt)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<float, i32, Operators::GreaterThan, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_le)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<float, i32, Operators::LessThanOrEquals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32_ge)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<float, i32, Operators::GreaterThanOrEquals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_eq)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<double, i32, Operators::Equals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_ne)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<double, i32, Operators::NotEquals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_lt)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<double, i32, Operators::LessThan, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_gt)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<double, i32, Operators::GreaterThan, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64_le)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<double, i32, Operators::LessThanOrEquals, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_extmul_high_i16x8_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<4, Operators::Multiply, Operators::VectorExt::High, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_extmul_low_i16x8_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<4, Operators::Multiply, Operators::VectorExt::Low, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_eq)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::Equals>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_ne)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::NotEquals>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_lt_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::LessThan, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_gt_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::GreaterThan, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_le_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::LessThanOrEquals, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_ge_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<2, Operators::GreaterThanOrEquals, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_abs)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<2, Operators::Absolute>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_neg)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<2, Operators::Negate, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_all_true)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, i32, Operators::VectorAllTrue<2>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_add)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<2, Operators::Add, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_sub)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<2, Operators::Subtract, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_mul)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<2, Operators::Multiply, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_extend_low_i32x4_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerExt<2, Operators::VectorExt::Low, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_extend_high_i32x4_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerExt<2, Operators::VectorExt::High, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_extend_low_i32x4_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerExt<2, Operators::VectorExt::Low, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_extend_high_i32x4_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerExt<2, Operators::VectorExt::High, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_extmul_low_i32x4_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<2, Operators::Multiply, Operators::VectorExt::Low, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_extmul_high_i32x4_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<2, Operators::Multiply, Operators::VectorExt::High, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_extmul_low_i32x4_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<2, Operators::Multiply, Operators::VectorExt::Low, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_extmul_high_i32x4_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<2, Operators::Multiply, Operators::VectorExt::High, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_eq)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::Equals>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_ne)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::NotEquals>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_lt)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::LessThan>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_gt)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::GreaterThan>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_le)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::LessThanOrEquals>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_ge)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<4, Operators::GreaterThanOrEquals>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_min)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Minimum>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_max)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Maximum>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_eq)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::Equals>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_ne)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::NotEquals>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_lt)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::LessThan>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_gt)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::GreaterThan>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_le)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::LessThanOrEquals>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_ge)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatCmpOp<2, Operators::GreaterThanOrEquals>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_min)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Minimum>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_max)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Maximum>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_div)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Divide>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_mul)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Multiply>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_sub)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Subtract>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_add)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::Add>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_pmin)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::PseudoMinimum>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_pmax)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<4, Operators::PseudoMaximum>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_div)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Divide>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_mul)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Multiply>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_sub)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Subtract>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_add)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::Add>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_pmin)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::PseudoMinimum>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_pmax)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorFloatBinaryOp<2, Operators::PseudoMaximum>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_ceil)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Ceil>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_floor)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Floor>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_trunc)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Truncate>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_nearest)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::NearbyIntegral>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_sqrt)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::SquareRoot>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_neg)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Negate>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_abs)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorFloatUnaryOp<4, Operators::Absolute>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_ceil)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Ceil>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_floor)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Floor>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_trunc)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Truncate>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_nearest)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::NearbyIntegral>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_sqrt)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::SquareRoot>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_neg)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Negate>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_abs)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorFloatUnaryOp<2, Operators::Absolute>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_and)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::BitAnd, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_or)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::BitOr, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_xor)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::BitXor, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_not)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::BitNot, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_andnot)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::BitAndNot, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_bitselect)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    // bounds checked by verifier.
    auto mask = configuration.take_source<source_address_mix>(0, addresses.sources).template to<u128>();
    auto false_vector = configuration.take_source<source_address_mix>(1, addresses.sources).template to<u128>();
    auto true_vector = configuration.take_source<source_address_mix>(2, addresses.sources).template to<u128>();
    u128 result = (true_vector & mask) | (false_vector & ~mask);
    configuration.push_to_destination<source_address_mix>(Value(result), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_any_true)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto vector = configuration.take_source<source_address_mix>(0, addresses.sources).template to<u128>(); // bounds checked by verifier.
    configuration.push_to_destination<source_address_mix>(Value(static_cast<i32>(vector != 0)), addresses.destination);
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_load8_lane)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push_lane_n<8>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_load16_lane)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push_lane_n<16>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_load32_lane)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push_lane_n<32>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_load64_lane)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push_lane_n<64>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_load32_zero)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push_zero_n<32>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_load64_zero)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.load_and_push_zero_n<64>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_store8_lane)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.pop_and_store_lane_n<8>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_store16_lane)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.pop_and_store_lane_n<16>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_store32_lane)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.pop_and_store_lane_n<32>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(v128_store64_lane)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.pop_and_store_lane_n<64>(configuration, *instruction, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_trunc_sat_f32x4_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorConvertOp<4, 4, u32, f32, Operators::SaturatingTruncate<i32>>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_trunc_sat_f32x4_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorConvertOp<4, 4, u32, f32, Operators::SaturatingTruncate<u32>>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_bitmask)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, i32, Operators::VectorBitmask<16>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_bitmask)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, i32, Operators::VectorBitmask<8>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_bitmask)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, i32, Operators::VectorBitmask<4>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_bitmask)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, i32, Operators::VectorBitmask<2>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_dot_i16x8_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorDotProduct<4>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_narrow_i16x8_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorNarrow<16, i8>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_narrow_i16x8_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorNarrow<16, u8>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_narrow_i32x4_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorNarrow<8, i16>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_narrow_i32x4_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorNarrow<8, u16>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_q15mulr_sat_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<i16, Operators::Q15Mul>, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_convert_i32x4_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorConvertOp<4, 4, u32, i32, Operators::Convert<f32>>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_convert_i32x4_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorConvertOp<4, 4, u32, u32, Operators::Convert<f32>>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_convert_low_i32x4_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorConvertOp<2, 4, u64, i32, Operators::Convert<f64>>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_convert_low_i32x4_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorConvertOp<2, 4, u64, u32, Operators::Convert<f64>>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_demote_f64x2_zero)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorConvertOp<4, 2, u32, f64, Operators::Convert<f32>>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_promote_low_f32x4)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorConvertOp<2, 4, u64, f32, Operators::Convert<f64>>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_trunc_sat_f64x2_s_zero)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorConvertOp<4, 2, u32, f64, Operators::SaturatingTruncate<i32>>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_trunc_sat_f64x2_u_zero)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorConvertOp<4, 2, u32, f64, Operators::SaturatingTruncate<u32>>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}
HANDLE_INSTRUCTION(i8x16_shl)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorShiftLeft<16>, source_address_mix, i32>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_shr_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorShiftRight<16, MakeUnsigned>, source_address_mix, i32>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_shr_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorShiftRight<16, MakeSigned>, source_address_mix, i32>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_shl)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorShiftLeft<8>, source_address_mix, i32>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_shr_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorShiftRight<8, MakeUnsigned>, source_address_mix, i32>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_shr_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorShiftRight<8, MakeSigned>, source_address_mix, i32>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_shl)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorShiftLeft<4>, source_address_mix, i32>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_shr_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorShiftRight<4, MakeUnsigned>, source_address_mix, i32>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_shr_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorShiftRight<4, MakeSigned>, source_address_mix, i32>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_shl)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorShiftLeft<2>, source_address_mix, i32>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_shr_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorShiftRight<2, MakeUnsigned>, source_address_mix, i32>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_shr_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorShiftRight<2, MakeSigned>, source_address_mix, i32>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_swizzle)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorSwizzle, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_extract_lane_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, i8, Operators::VectorExtractLane<16, MakeSigned>, source_address_mix>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_extract_lane_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u8, Operators::VectorExtractLane<16, MakeUnsigned>, source_address_mix>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_extract_lane_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, i16, Operators::VectorExtractLane<8, MakeSigned>, source_address_mix>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_extract_lane_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u16, Operators::VectorExtractLane<8, MakeUnsigned>, source_address_mix>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_extract_lane)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, i32, Operators::VectorExtractLane<4, MakeSigned>, source_address_mix>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_extract_lane)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, i64, Operators::VectorExtractLane<2, MakeSigned>, source_address_mix>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_extract_lane)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, float, Operators::VectorExtractLaneFloat<4>, source_address_mix>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_extract_lane)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, double, Operators::VectorExtractLaneFloat<2>, source_address_mix>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_replace_lane)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<16, i32>, source_address_mix, i32>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_replace_lane)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<8, i32>, source_address_mix, i32>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_replace_lane)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<4>, source_address_mix, i32>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i64x2_replace_lane)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<2>, source_address_mix, i64>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_replace_lane)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<4, float>, source_address_mix, float>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_replace_lane)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorReplaceLane<2, double>, source_address_mix, double>(configuration, addresses, instruction->arguments().get<Instruction::LaneIndex>().lane))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_eq)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::Equals>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_ne)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::NotEquals>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_lt_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::LessThan, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_lt_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::LessThan, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_gt_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::GreaterThan, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_gt_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::GreaterThan, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_le_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::LessThanOrEquals, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_le_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::LessThanOrEquals, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_ge_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::GreaterThanOrEquals, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_ge_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<16, Operators::GreaterThanOrEquals, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_abs)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<16, Operators::Absolute>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_neg)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<16, Operators::Negate>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_all_true)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, i32, Operators::VectorAllTrue<16>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_popcnt)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<16, Operators::PopCount>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_add)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Add>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_sub)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Subtract>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_avgr_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Average, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_add_sat_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::SaturatingOp<i8, Operators::Add>, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_add_sat_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::SaturatingOp<u8, Operators::Add>, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_sub_sat_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::SaturatingOp<i8, Operators::Subtract>, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_sub_sat_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::SaturatingOp<u8, Operators::Subtract>, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_min_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Minimum, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_min_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Minimum, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_max_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Maximum, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i8x16_max_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<16, Operators::Maximum, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_eq)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::Equals>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_ne)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::NotEquals>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_lt_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::LessThan, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_lt_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::LessThan, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_gt_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::GreaterThan, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_gt_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::GreaterThan, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_le_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::LessThanOrEquals, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_le_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::LessThanOrEquals, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_ge_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::GreaterThanOrEquals, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_ge_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<8, Operators::GreaterThanOrEquals, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_abs)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<8, Operators::Absolute>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_neg)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<8, Operators::Negate>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_all_true)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, i32, Operators::VectorAllTrue<8>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_add)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Add>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_sub)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Subtract>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_mul)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Multiply>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_avgr_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Average, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_add_sat_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<i16, Operators::Add>, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_add_sat_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<u16, Operators::Add>, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_sub_sat_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<i16, Operators::Subtract>, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_sub_sat_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::SaturatingOp<u16, Operators::Subtract>, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_min_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Minimum, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_min_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Minimum, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_max_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Maximum, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_max_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<8, Operators::Maximum, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_extend_low_i8x16_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerExt<8, Operators::VectorExt::Low, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_extend_high_i8x16_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerExt<8, Operators::VectorExt::High, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_extend_low_i8x16_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerExt<8, Operators::VectorExt::Low, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_extend_high_i8x16_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerExt<8, Operators::VectorExt::High, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_extadd_pairwise_i8x16_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerExtOpPairwise<8, Operators::Add, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_extadd_pairwise_i8x16_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerExtOpPairwise<8, Operators::Add, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_extmul_low_i8x16_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<8, Operators::Multiply, Operators::VectorExt::Low, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_extmul_high_i8x16_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<8, Operators::Multiply, Operators::VectorExt::High, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_extmul_low_i8x16_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<8, Operators::Multiply, Operators::VectorExt::Low, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i16x8_extmul_high_i8x16_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<8, Operators::Multiply, Operators::VectorExt::High, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_eq)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::Equals>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_ne)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::NotEquals>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_lt_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::LessThan, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_lt_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::LessThan, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_gt_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::GreaterThan, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_gt_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::GreaterThan, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_le_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::LessThanOrEquals, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_le_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::LessThanOrEquals, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_ge_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::GreaterThanOrEquals, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_ge_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorCmpOp<4, Operators::GreaterThanOrEquals, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_abs)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<4, Operators::Absolute>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_neg)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerUnaryOp<4, Operators::Negate, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_all_true)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, i32, Operators::VectorAllTrue<4>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_add)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Add, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_sub)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Subtract, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_mul)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Multiply, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_min_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Minimum, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_min_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Minimum, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_max_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Maximum, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_max_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerBinaryOp<4, Operators::Maximum, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_extend_low_i16x8_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerExt<4, Operators::VectorExt::Low, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_extend_high_i16x8_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerExt<4, Operators::VectorExt::High, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_extend_low_i16x8_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerExt<4, Operators::VectorExt::Low, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_extend_high_i16x8_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerExt<4, Operators::VectorExt::High, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_extadd_pairwise_i16x8_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerExtOpPairwise<4, Operators::Add, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_extadd_pairwise_i16x8_u)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.unary_operation<u128, u128, Operators::VectorIntegerExtOpPairwise<4, Operators::Add, MakeUnsigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_extmul_low_i16x8_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<4, Operators::Multiply, Operators::VectorExt::Low, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_extmul_high_i16x8_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorIntegerExtOp<4, Operators::Multiply, Operators::VectorExt::High, MakeSigned>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

ALIAS_INSTRUCTION(i8x16_relaxed_swizzle, i8x16_swizzle)
ALIAS_INSTRUCTION(i32x4_relaxed_trunc_f32x4_s, i32x4_trunc_sat_f32x4_s)
ALIAS_INSTRUCTION(i32x4_relaxed_trunc_f32x4_u, i32x4_trunc_sat_f32x4_u)
ALIAS_INSTRUCTION(i32x4_relaxed_trunc_f64x2_s_zero, i32x4_trunc_sat_f64x2_s_zero)
ALIAS_INSTRUCTION(i32x4_relaxed_trunc_f64x2_u_zero, i32x4_trunc_sat_f64x2_u_zero)

HANDLE_INSTRUCTION(f32x4_relaxed_madd)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto c = configuration.take_source<source_address_mix>(0, addresses.sources).template to<u128>();
    auto a = configuration.take_source<source_address_mix>(1, addresses.sources).template to<u128>();
    auto& b_slot = configuration.source_value<source_address_mix>(2, addresses.sources);
    auto b = b_slot.template to<u128>();
    b_slot = Value { Operators::VectorMultiplyAdd<4> {}(a, b, c) };
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f32x4_relaxed_nmadd)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto c = configuration.take_source<source_address_mix>(0, addresses.sources).template to<u128>();
    auto a = configuration.take_source<source_address_mix>(1, addresses.sources).template to<u128>();
    auto& b_slot = configuration.source_value<source_address_mix>(2, addresses.sources);
    auto b = b_slot.template to<u128>();
    b_slot = Value { Operators::VectorMultiplySub<4> {}(a, b, c) };
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_relaxed_madd)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto c = configuration.take_source<source_address_mix>(0, addresses.sources).template to<u128>();
    auto a = configuration.take_source<source_address_mix>(1, addresses.sources).template to<u128>();
    auto& b_slot = configuration.source_value<source_address_mix>(2, addresses.sources);
    auto b = b_slot.template to<u128>();
    b_slot = Value { Operators::VectorMultiplyAdd<2> {}(a, b, c) };
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(f64x2_relaxed_nmadd)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto c = configuration.take_source<source_address_mix>(0, addresses.sources).template to<u128>();
    auto a = configuration.take_source<source_address_mix>(1, addresses.sources).template to<u128>();
    auto& b_slot = configuration.source_value<source_address_mix>(2, addresses.sources);
    auto b = b_slot.template to<u128>();
    b_slot = Value { Operators::VectorMultiplySub<2> {}(a, b, c) };
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

ALIAS_INSTRUCTION(i8x16_relaxed_laneselect, v128_bitselect)
ALIAS_INSTRUCTION(i16x8_relaxed_laneselect, v128_bitselect)
ALIAS_INSTRUCTION(i32x4_relaxed_laneselect, v128_bitselect)
ALIAS_INSTRUCTION(i64x2_relaxed_laneselect, v128_bitselect)
ALIAS_INSTRUCTION(f32x4_relaxed_min, f32x4_min)
ALIAS_INSTRUCTION(f32x4_relaxed_max, f32x4_max)
ALIAS_INSTRUCTION(f64x2_relaxed_min, f64x2_min)
ALIAS_INSTRUCTION(f64x2_relaxed_max, f64x2_max)
ALIAS_INSTRUCTION(i16x8_relaxed_q15mulr_s, i16x8_q15mulr_sat_s)

HANDLE_INSTRUCTION(i16x8_relaxed_dot_i8x16_i7x16_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    if (interpreter.binary_numeric_operation<u128, u128, Operators::VectorDotProduct<8>, source_address_mix>(configuration, addresses))
        return Outcome::Return;
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(i32x4_relaxed_dot_i8x16_i7x16_add_s)
{
    LOG_INSN;
    LOAD_ADDRESSES();
    auto acc = configuration.take_source<source_address_mix>(0, addresses.sources).template to<u128>();
    auto rhs = configuration.take_source<source_address_mix>(1, addresses.sources).template to<u128>(); // bounds checked by verifier.
    auto& lhs_slot = configuration.source_value<source_address_mix>(2, addresses.sources);
    lhs_slot = Value { Operators::VectorRelaxedDotI8I7AddS {}(lhs_slot.template to<u128>(), rhs, acc) };
    TAILCALL return continue_(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(throw_ref)
{
    LOG_INSN;
    interpreter.set_trap("Not Implemented: Proposal 'Exception-handling'"sv);
    return Outcome::Return;
}

HANDLE_INSTRUCTION(throw_)
{
    LOG_INSN;
    {
        auto tag_address = configuration.frame().module().tags()[instruction->arguments().get<TagIndex>().value()];
        auto& tag_instance = *configuration.store().get(tag_address);
        auto& type = tag_instance.type();
        auto values = Vector<Value>(configuration.value_stack().span().slice_from_end(type.parameters().size()));
        configuration.value_stack().shrink(configuration.value_stack().size() - type.parameters().size());
        auto exception_address = configuration.store().allocate(tag_instance, move(values));
        if (!exception_address.has_value()) {
            interpreter.set_trap("Out of memory"sv);
            return Outcome::Return;
        }
        configuration.value_stack().append(Value(Reference { Reference::Exception { *exception_address } }));
    }
    TAILCALL return InstructionHandler<Instructions::throw_ref.value()>::operator()<HasDynamicInsnLimit, Continue, SourceAddressMix::Any>(HANDLER_PARAMS(DECOMPOSE_PARAMS_NAME_ONLY));
}

HANDLE_INSTRUCTION(try_table)
{
    LOG_INSN;
    interpreter.set_trap("Not Implemented: Proposal 'Exception-handling'"sv);
    return Outcome::Return;
}

template<u64 opcode, bool HasDynamicInsnLimit, typename Continue, SourceAddressMix mix, typename... Args>
constexpr static auto handle_instruction(Args&&... a)
{
    return InstructionHandler<opcode>::template operator()<HasDynamicInsnLimit, Continue, mix>(forward<Args>(a)...);
}

template<bool HasCompiledList, bool HasDynamicInsnLimit, bool HaveDirectThreadingInfo>
FLATTEN void BytecodeInterpreter::interpret_impl(Configuration& configuration, Expression const& expression)
{
    auto& instructions = expression.instructions();
    u64 executed_instructions = 0;
    ShortenedIP short_ip { .current_ip_value = static_cast<u32>(configuration.ip()) };

    auto cc = expression.compiled_instructions.dispatches.data();
    auto addresses_ptr = expression.compiled_instructions.src_dst_mappings.data();

    if constexpr (HaveDirectThreadingInfo) {
        static_assert(HasCompiledList, "Direct threading requires a compiled instruction list");
        auto const instruction = cc[short_ip.current_ip_value].instruction;
        auto const handler = bit_cast<Outcome (*)(HANDLER_PARAMS(DECOMPOSE_PARAMS_TYPE_ONLY))>(cc[short_ip.current_ip_value].handler_ptr);
        handler(*this, configuration, instruction, short_ip, cc, addresses_ptr);
        return;
    }

    while (true) {
        if constexpr (HasDynamicInsnLimit) {
            if (executed_instructions++ >= Constants::max_allowed_executed_instructions_per_call) [[unlikely]] {
                m_trap = Trap::from_string("Exceeded maximum allowed number of instructions");
                return;
            }
        }
        // bounds checked by loop condition.
        auto const instruction = HasCompiledList
            ? cc[short_ip.current_ip_value].instruction
            : &instructions.data()[short_ip.current_ip_value];
        auto const opcode = (HasCompiledList && !HaveDirectThreadingInfo
                ? cc[short_ip.current_ip_value].instruction_opcode
                : instruction->opcode())
                                .value();

#define RUN_NEXT_INSTRUCTION()       \
    {                                \
        ++short_ip.current_ip_value; \
        break;                       \
    }

#define HANDLE_INSTRUCTION_NEW(name, ...)                                                                                                                                                \
    case Instructions::name.value(): {                                                                                                                                                   \
        auto outcome = handle_instruction<Instructions::name.value(), HasDynamicInsnLimit, Skip, SourceAddressMix::Any>(*this, configuration, instruction, short_ip, cc, addresses_ptr); \
        if (outcome == Outcome::Return)                                                                                                                                                  \
            return;                                                                                                                                                                      \
        short_ip.current_ip_value = to_underlying(outcome);                                                                                                                              \
        if constexpr (Instructions::name == Instructions::return_call || Instructions::name == Instructions::return_call_indirect) {                                                     \
            cc = configuration.frame().expression().compiled_instructions.dispatches.data();                                                                                             \
            addresses_ptr = configuration.frame().expression().compiled_instructions.src_dst_mappings.data();                                                                            \
        }                                                                                                                                                                                \
        RUN_NEXT_INSTRUCTION();                                                                                                                                                          \
    }

        dbgln_if(WASM_TRACE_DEBUG, "Executing instruction {} at current_ip_value {}", instruction_name(instruction->opcode()), short_ip.current_ip_value);
        if ((opcode & Instructions::SyntheticInstructionBase.value()) != Instructions::SyntheticInstructionBase.value())
            __builtin_prefetch(&instruction->arguments(), /* read */ 0, /* low temporal locality */ 1);

        switch (opcode) {
            ENUMERATE_WASM_OPCODES(HANDLE_INSTRUCTION_NEW)
        default:
            dbgln("Bad opcode {} in insn {} (ip {})", opcode, instruction_name(instruction->opcode()), short_ip.current_ip_value);
            VERIFY_NOT_REACHED();
        }
    }
}

template<bool NeedsStackAdjustment>
InstructionPointer BytecodeInterpreter::branch_to_label(Configuration& configuration, LabelIndex index, InstructionPointer current_ip, bool actually_branching)
{
    dbgln_if(WASM_TRACE_DEBUG, "Branch to label with index {}...", index.value());
    auto& label_stack = configuration.label_stack();
    label_stack.unsafe_shrink(actually_branching ? label_stack.size() - index.value() : label_stack.size());
    auto const& label = configuration.label_stack().unsafe_last();
    dbgln_if(WASM_TRACE_DEBUG, "...which is actually IP {}, and has {} result(s)", label.continuation().value(), label.arity());

    if constexpr (NeedsStackAdjustment) {
        if (actually_branching)
            configuration.value_stack().remove(label.stack_height(), configuration.value_stack().size() - label.stack_height() - label.arity());
    }
    return actually_branching ? label.continuation().value() - 1 : current_ip;
}

template<typename ReadType, typename PushType, SourceAddressMix mix>
bool BytecodeInterpreter::load_and_push(Configuration& configuration, Instruction const& instruction, SourcesAndDestination const& addresses)
{
    auto& arg = instruction.arguments().unsafe_get<Instruction::MemoryArgument>();
    auto& address = configuration.frame().module().memories().data()[arg.memory_index.value()];
    auto memory = configuration.store().unsafe_get(address);
    auto& entry = configuration.source_value<mix>(0, addresses.sources); // bounds checked by verifier.
    auto base = entry.template to<i32>();
    u64 instance_address = static_cast<u64>(bit_cast<u32>(base)) + arg.offset;
    dbgln_if(WASM_TRACE_DEBUG, "load({} : {}) -> stack", instance_address, sizeof(ReadType));
    if (instance_address + sizeof(ReadType) > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        dbgln_if(WASM_TRACE_DEBUG, "LibWasm: load_and_push - Memory access out of bounds (expected {} to be less than or equal to {})", instance_address + sizeof(ReadType), memory->size());
        return true;
    }
    auto slice = memory->data().bytes().slice(instance_address, sizeof(ReadType));
    entry = Value(static_cast<PushType>(read_value<ReadType>(slice)));
    dbgln_if(WASM_TRACE_DEBUG, "  loaded value: {}", entry.value());
    return false;
}

template<typename TDst, typename TSrc>
ALWAYS_INLINE static TDst convert_vector(TSrc v)
{
    return __builtin_convertvector(v, TDst);
}

template<size_t M, size_t N, template<typename> typename SetSign>
bool BytecodeInterpreter::load_and_push_mxn(Configuration& configuration, Instruction const& instruction, SourcesAndDestination const& addresses)
{
    auto& arg = instruction.arguments().unsafe_get<Instruction::MemoryArgument>();
    auto& address = configuration.frame().module().memories().data()[arg.memory_index.value()];
    auto memory = configuration.store().unsafe_get(address);
    auto& entry = configuration.source_value<SourceAddressMix::Any>(0, addresses.sources); // bounds checked by verifier.
    auto base = entry.template to<i32>();
    u64 instance_address = static_cast<u64>(bit_cast<u32>(base)) + arg.offset;
    dbgln_if(WASM_TRACE_DEBUG, "vec-load({} : {}) -> stack", instance_address, M * N / 8);
    if (instance_address + M * N / 8 > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        dbgln("LibWasm: load_and_push_mxn - Memory access out of bounds (expected {} to be less than or equal to {})", instance_address + M * N / 8, memory->size());
        return true;
    }
    auto slice = memory->data().bytes().slice(instance_address, M * N / 8);
    using V64 = NativeVectorType<M, N, SetSign>;
    using V128 = NativeVectorType<M * 2, N, SetSign>;

    V64 bytes { 0 };
    if (bit_cast<FlatPtr>(slice.data()) % sizeof(V64) == 0)
        bytes = *bit_cast<V64*>(slice.data());
    else
        ByteReader::load(slice.data(), bytes);

    entry = Value(bit_cast<u128>(convert_vector<V128>(bytes)));
    dbgln_if(WASM_TRACE_DEBUG, "  loaded value: {}", entry.value());
    return false;
}

template<size_t N>
bool BytecodeInterpreter::load_and_push_lane_n(Configuration& configuration, Instruction const& instruction, SourcesAndDestination const& addresses)
{
    auto memarg_and_lane = instruction.arguments().unsafe_get<Instruction::MemoryAndLaneArgument>();
    auto& address = configuration.frame().module().memories().data()[memarg_and_lane.memory.memory_index.value()];
    auto memory = configuration.store().unsafe_get(address);
    // bounds checked by verifier.
    auto vector = configuration.take_source<SourceAddressMix::Any>(0, addresses.sources).template to<u128>();
    auto base = configuration.take_source<SourceAddressMix::Any>(1, addresses.sources).template to<u32>();
    u64 instance_address = static_cast<u64>(bit_cast<u32>(base)) + memarg_and_lane.memory.offset;
    dbgln_if(WASM_TRACE_DEBUG, "load-lane({} : {}, lane {}) -> stack", instance_address, N / 8, memarg_and_lane.lane);
    if (instance_address + N / 8 > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        dbgln("LibWasm: load_and_push_lane_n - Memory access out of bounds (expected {} to be less than or equal to {})", instance_address + N / 8, memory->size());
        return true;
    }
    auto slice = memory->data().bytes().slice(instance_address, N / 8);
    auto dst = bit_cast<u8*>(&vector) + memarg_and_lane.lane * N / 8;
    memcpy(dst, slice.data(), N / 8);
    dbgln_if(WASM_TRACE_DEBUG, "  loaded value: {}", vector);
    configuration.push_to_destination<SourceAddressMix::Any>(Value(vector), addresses.destination);
    return false;
}

template<size_t N>
bool BytecodeInterpreter::load_and_push_zero_n(Configuration& configuration, Instruction const& instruction, SourcesAndDestination const& addresses)
{
    auto memarg_and_lane = instruction.arguments().unsafe_get<Instruction::MemoryArgument>();
    auto& address = configuration.frame().module().memories().data()[memarg_and_lane.memory_index.value()];
    auto memory = configuration.store().unsafe_get(address);
    // bounds checked by verifier.
    auto base = configuration.take_source<SourceAddressMix::Any>(0, addresses.sources).template to<u32>();
    u64 instance_address = static_cast<u64>(bit_cast<u32>(base)) + memarg_and_lane.offset;
    dbgln_if(WASM_TRACE_DEBUG, "load-zero({} : {}) -> stack", instance_address, N / 8);
    if (instance_address + N / 8 > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        dbgln("LibWasm: load_and_push_zero_n - Memory access out of bounds (expected {} to be less than or equal to {})", instance_address + N / 8, memory->size());
        return true;
    }
    auto slice = memory->data().bytes().slice(instance_address, N / 8);
    u128 vector = 0;
    memcpy(&vector, slice.data(), N / 8);
    dbgln_if(WASM_TRACE_DEBUG, "  loaded value: {}", vector);
    configuration.push_to_destination<SourceAddressMix::Any>(Value(vector), addresses.destination);
    return false;
}

template<size_t M>
bool BytecodeInterpreter::load_and_push_m_splat(Configuration& configuration, Instruction const& instruction, SourcesAndDestination const& addresses)
{
    auto& arg = instruction.arguments().unsafe_get<Instruction::MemoryArgument>();
    auto& address = configuration.frame().module().memories().data()[arg.memory_index.value()];
    auto memory = configuration.store().unsafe_get(address);
    auto& entry = configuration.source_value<SourceAddressMix::Any>(0, addresses.sources); // bounds checked by verifier.
    auto base = entry.template to<i32>();
    u64 instance_address = static_cast<u64>(bit_cast<u32>(base)) + arg.offset;
    dbgln_if(WASM_TRACE_DEBUG, "vec-splat({} : {}) -> stack", instance_address, M / 8);
    if (instance_address + M / 8 > memory->size()) {
        m_trap = Trap::from_string("Memory access out of bounds");
        dbgln("LibWasm: load_and_push_m_splat - Memory access out of bounds (expected {} to be less than or equal to {})", instance_address + M / 8, memory->size());
        return true;
    }
    auto slice = memory->data().bytes().slice(instance_address, M / 8);
    auto value = read_value<NativeIntegralType<M>>(slice);
    dbgln_if(WASM_TRACE_DEBUG, "  loaded value: {}", value);
    set_top_m_splat<M, NativeIntegralType>(configuration, value, addresses);
    return false;
}

template<size_t M, template<size_t> typename NativeType>
void BytecodeInterpreter::set_top_m_splat(Wasm::Configuration& configuration, NativeType<M> value, SourcesAndDestination const& addresses)
{
    auto push = [&](auto result) {
        configuration.source_value<SourceAddressMix::Any>(0, addresses.sources) = Value(bit_cast<u128>(result));
    };

    if constexpr (IsFloatingPoint<NativeType<32>>) {
        if constexpr (M == 32) // 32 -> 32x4
            push(expand4(value));
        else if constexpr (M == 64) // 64 -> 64x2
            push(f64x2 { value, value });
        else
            static_assert(DependentFalse<NativeType<M>>, "Invalid vector size");
    } else {
        if constexpr (M == 8) // 8 -> 8x4 -> 32x4
            push(expand4(bit_cast<u32>(u8x4 { value, value, value, value })));
        else if constexpr (M == 16) // 16 -> 16x2 -> 32x4
            push(expand4(bit_cast<u32>(u16x2 { value, value })));
        else if constexpr (M == 32) // 32 -> 32x4
            push(expand4(value));
        else if constexpr (M == 64) // 64 -> 64x2
            push(u64x2 { value, value });
        else
            static_assert(DependentFalse<NativeType<M>>, "Invalid vector size");
    }
}

template<size_t M, template<size_t> typename NativeType>
void BytecodeInterpreter::pop_and_push_m_splat(Wasm::Configuration& configuration, Instruction const&, SourcesAndDestination const& addresses)
{
    using PopT = Conditional<M <= 32, NativeType<32>, NativeType<64>>;
    using ReadT = NativeType<M>;
    auto entry = configuration.source_value<SourceAddressMix::Any>(0, addresses.sources);
    auto value = static_cast<ReadT>(entry.template to<PopT>());
    dbgln_if(WASM_TRACE_DEBUG, "stack({}) -> splat({})", value, M);
    set_top_m_splat<M, NativeType>(configuration, value, addresses);
}

template<typename M, template<typename> typename SetSign, typename VectorType>
VectorType BytecodeInterpreter::pop_vector(Configuration& configuration, size_t source, SourcesAndDestination const& addresses)
{
    // bounds checked by verifier.
    return bit_cast<VectorType>(configuration.take_source<SourceAddressMix::Any>(source, addresses.sources).template to<u128>());
}

Outcome BytecodeInterpreter::call_address(Configuration& configuration, FunctionAddress address, SourcesAndDestination const& addresses, CallAddressSource source, CallType call_type)
{
    TRAP_IF_NOT(m_stack_info.size_free() >= Constants::minimum_stack_space_to_keep_free, "{}: {}", Constants::stack_exhaustion_message);
    Result result { Trap::from_string("") };
    Outcome final_outcome = Outcome::Continue;
    {
        Optional<ScopedValueRollback<decltype(configuration.regs)>> regs_rollback;

        if (call_type == CallType::UsingRegisters || call_type == CallType::UsingCallRecord)
            regs_rollback = ScopedValueRollback { configuration.regs };

        auto instance = configuration.store().get(address);
        FunctionType const* type { nullptr };
        instance->visit([&](auto const& function) { type = &function.type(); });
        if (source == CallAddressSource::IndirectCall || source == CallAddressSource::IndirectTailCall) {
            TRAP_IF_NOT(type->parameters().size() <= configuration.value_stack().size());
        }
        Vector<Value, ArgumentsStaticSize> args;

        if (call_type == CallType::UsingCallRecord) {
            configuration.take_call_record(args);
            args.shrink(type->parameters().size(), true);
        } else {
            configuration.get_arguments_allocation_if_possible(args, type->parameters().size());

            {
                auto param_count = type->parameters().size();
                if (param_count) {
                    args.ensure_capacity(param_count);
                    if (call_type == CallType::UsingRegisters) {
                        args.resize_and_keep_capacity(param_count);
                        for (size_t i = 0; i < param_count; ++i)
                            args[param_count - i - 1] = configuration.take_source<SourceAddressMix::Any>(i, addresses.sources);
                    } else {
                        auto span = configuration.value_stack().span().slice_from_end(param_count);
                        for (auto& value : span)
                            args.unchecked_append(value);

                        configuration.value_stack().remove(configuration.value_stack().size() - span.size(), span.size());
                    }
                }
            }
        }

        if (source == CallAddressSource::DirectTailCall || source == CallAddressSource::IndirectTailCall) {
            auto prep_outcome = configuration.prepare_call(address, args, true);
            if (prep_outcome.is_error()) {
                m_trap = prep_outcome.release_error();
                return Outcome::Return;
            }

            final_outcome = Outcome::Return; // At this point we can only ever return (unless we succeed in tail-calling).
            if (prep_outcome.value().has_value()) {
                result = prep_outcome.value()->function()(configuration, args);
                configuration.release_arguments_allocation(args);
            } else {
                configuration.ip() = 0;
                return static_cast<Outcome>(0); // Continue from IP 0 in the new frame.
            }
        } else {
            if (instance->has<WasmFunction>()) {
                CallFrameHandle handle { *this, configuration };
                result = configuration.call(*this, address, args);
            } else {
                result = configuration.call(*this, address, args);
                configuration.release_arguments_allocation(args);
            }
        }

        if (result.is_trap()) {
            m_trap = move(result.trap());
            return Outcome::Return;
        }
    }

    if (!result.values().is_empty()) {
        if (call_type == CallType::UsingRegisters || call_type == CallType::UsingCallRecord || result.values().size() == 1) {
            configuration.push_to_destination<SourceAddressMix::Any>(result.values().take_first(), addresses.destination);
        } else {
            configuration.value_stack().ensure_capacity(configuration.value_stack().size() + result.values().size());
            for (auto& entry : result.values().in_reverse())
                configuration.value_stack().unchecked_append(entry);
        }
    }

    return final_outcome;
}

template<typename PopTypeLHS, typename PushType, typename Operator, SourceAddressMix mix, typename PopTypeRHS, typename... Args>
bool BytecodeInterpreter::binary_numeric_operation(Configuration& configuration, SourcesAndDestination const& addresses, Args&&... args)
{
    // bounds checked by Nor.
    auto rhs = configuration.take_source<mix>(0, addresses.sources).template to<PopTypeRHS>();
    auto& lhs_slot = configuration.source_value<mix>(1, addresses.sources); // bounds checked by verifier.
    auto lhs = lhs_slot.template to<PopTypeLHS>();
    PushType result;
    auto call_result = Operator { forward<Args>(args)... }(lhs, rhs);
    if constexpr (IsSpecializationOf<decltype(call_result), AK::ErrorOr>) {
        if (call_result.is_error())
            return trap_if_not(false, call_result.error());
        result = call_result.release_value();
    } else {
        result = call_result;
    }
    dbgln_if(WASM_TRACE_DEBUG, "{} {} {} = {}", lhs, Operator::name(), rhs, result);
    lhs_slot = Value(result);
    return false;
}

template<typename PopType, typename PushType, typename Operator, SourceAddressMix mix, size_t input_arg, typename... Args>
bool BytecodeInterpreter::unary_operation(Configuration& configuration, SourcesAndDestination const& addresses, Args&&... args)
{
    auto& entry = configuration.source_value<mix>(input_arg, addresses.sources); // bounds checked by verifier.
    auto value = entry.template to<PopType>();
    auto call_result = Operator { forward<Args>(args)... }(value);
    PushType result;
    if constexpr (IsSpecializationOf<decltype(call_result), AK::ErrorOr>) {
        if (call_result.is_error())
            return trap_if_not(false, call_result.error());
        result = call_result.release_value();
    } else {
        result = call_result;
    }
    dbgln_if(WASM_TRACE_DEBUG, "map({}) {} = {}", Operator::name(), value, result);
    entry = Value(result);
    return false;
}

template<typename PopT, typename StoreT>
bool BytecodeInterpreter::pop_and_store(Configuration& configuration, Instruction const& instruction, SourcesAndDestination const& addresses)
{
    // bounds checked by verifier.
    auto entry = configuration.take_source<SourceAddressMix::Any>(0, addresses.sources);
    auto value = ConvertToRaw<StoreT> {}(entry.template to<PopT>());
    return store_value(configuration, instruction, value, 1, addresses);
}

template<typename StoreT>
bool BytecodeInterpreter::store_value(Configuration& configuration, Instruction const& instruction, StoreT value, size_t address_source, SourcesAndDestination const& addresses)
{
    auto& memarg = instruction.arguments().unsafe_get<Instruction::MemoryArgument>();
    dbgln_if(WASM_TRACE_DEBUG, "stack({}) -> temporary({}b)", value, sizeof(StoreT));
    auto base = configuration.take_source<SourceAddressMix::Any>(address_source, addresses.sources).template to<i32>();
    return store_to_memory(configuration, memarg, { &value, sizeof(StoreT) }, base);
}

template<size_t N>
bool BytecodeInterpreter::pop_and_store_lane_n(Configuration& configuration, Instruction const& instruction, SourcesAndDestination const& addresses)
{
    auto& memarg_and_lane = instruction.arguments().unsafe_get<Instruction::MemoryAndLaneArgument>();
    // bounds checked by verifier.
    auto vector = configuration.take_source<SourceAddressMix::Any>(0, addresses.sources).template to<u128>();
    auto src = bit_cast<u8*>(&vector) + memarg_and_lane.lane * N / 8;
    auto base = configuration.take_source<SourceAddressMix::Any>(1, addresses.sources).template to<u32>();
    return store_to_memory(configuration, memarg_and_lane.memory, { src, N / 8 }, base);
}

bool BytecodeInterpreter::store_to_memory(Configuration& configuration, Instruction::MemoryArgument const& arg, ReadonlyBytes data, u32 base)
{
    auto const& address = configuration.frame().module().memories().data()[arg.memory_index.value()];
    auto memory = configuration.store().unsafe_get(address);
    u64 instance_address = static_cast<u64>(base) + arg.offset;
    return store_to_memory(*memory, instance_address, data);
}

template<typename T>
bool BytecodeInterpreter::store_to_memory(MemoryInstance& memory, u64 address, T value)
{
    Checked addition { address };
    size_t data_size;
    if constexpr (IsSame<ReadonlyBytes, T>)
        data_size = value.size();
    else
        data_size = sizeof(T);

    addition += data_size;
    if (addition.has_overflow() || addition.value() > memory.size()) [[unlikely]] {
        m_trap = Trap::from_string("Memory access out of bounds");
        dbgln("LibWasm: store_to_memory - Memory access out of bounds (expected 0 <= {} and {} <= {})", address, address + data_size, memory.size());
        return true;
    }

    dbgln_if(WASM_TRACE_DEBUG, "temporary({}b) -> store({})", data_size, address);
    if constexpr (IsSame<ReadonlyBytes, T>)
        (void)value.copy_to(memory.data().bytes().slice(address, data_size));
    else
        memcpy(memory.data().bytes().offset_pointer(address), &value, data_size);
    return false;
}

template<typename T>
T BytecodeInterpreter::read_value(ReadonlyBytes data)
{
    VERIFY(sizeof(T) <= data.size());
    if (bit_cast<FlatPtr>(data.data()) % alignof(T)) {
        alignas(T) u8 buf[sizeof(T)];
        memcpy(buf, data.data(), sizeof(T));
        return bit_cast<LittleEndian<T>>(buf);
    }
    return *bit_cast<LittleEndian<T> const*>(data.data());
}

template<>
float BytecodeInterpreter::read_value<float>(ReadonlyBytes data)
{
    return bit_cast<float>(read_value<u32>(data));
}

template<>
double BytecodeInterpreter::read_value<double>(ReadonlyBytes data)
{
    return bit_cast<double>(read_value<u64>(data));
}

CompiledInstructions try_compile_instructions(Expression const& expression, Span<FunctionType const> functions)
{
    CompiledInstructions result;

    result.dispatches.ensure_capacity(expression.instructions().size());
    result.src_dst_mappings.ensure_capacity(expression.instructions().size());
    result.extra_instruction_storage.ensure_capacity(expression.instructions().size());

    i32 i32_const_value { 0 };
    i64 i64_const_value { 0 };
    LocalIndex local_index_0 { 0 };
    LocalIndex local_index_1 { 0 };
    enum class InsnPatternState {
        Nothing,
        GetLocal,
        GetLocalI32Const,
        GetLocalI64Const,
        GetLocalx2,
        I32Const,
        I32ConstGetLocal,
        I64Const,
        I64ConstGetLocal,
    } pattern_state { InsnPatternState::Nothing };
    static Instruction nop { Instructions::nop };

    size_t calls_in_expression = 0;

    auto const set_default_dispatch = [&result](Instruction const& instruction, size_t index = NumericLimits<size_t>::max()) {
        if (index < result.dispatches.size()) {
            result.dispatches[index] = { { .instruction_opcode = instruction.opcode() }, &instruction };
            result.src_dst_mappings[index] = { .sources = { Dispatch::Stack, Dispatch::Stack, Dispatch::Stack }, .destination = Dispatch::Stack };
        } else {
            result.dispatches.append({ { .instruction_opcode = instruction.opcode() }, &instruction });
            result.src_dst_mappings.append({ .sources = { Dispatch::Stack, Dispatch::Stack, Dispatch::Stack }, .destination = Dispatch::Stack });
        }
    };

    for (auto& instruction : expression.instructions()) {
        if (instruction.opcode() == Instructions::call) {
            auto& function = functions[instruction.arguments().get<FunctionIndex>().value()];
            if (function.results().size() <= 1 && function.parameters().size() < 4) {
                pattern_state = InsnPatternState::Nothing;
                OpCode op { Instructions::synthetic_call_00.value() + function.parameters().size() * 2 + function.results().size() };
                result.extra_instruction_storage.unchecked_append(Instruction(
                    op,
                    instruction.arguments()));
                set_default_dispatch(result.extra_instruction_storage.unsafe_last());
                continue;
            }

            calls_in_expression++;
        }

        switch (pattern_state) {
        case InsnPatternState::Nothing:
            if (instruction.opcode() == Instructions::local_get) {
                local_index_0 = instruction.local_index();
                pattern_state = InsnPatternState::GetLocal;
            } else if (instruction.opcode() == Instructions::i32_const) {
                i32_const_value = instruction.arguments().get<i32>();
                pattern_state = InsnPatternState::I32Const;
            } else if (instruction.opcode() == Instructions::i64_const) {
                i64_const_value = instruction.arguments().get<i64>();
                pattern_state = InsnPatternState::I64Const;
            }
            break;
        case InsnPatternState::GetLocal:
            if (instruction.opcode() == Instructions::local_get) {
                local_index_1 = instruction.local_index();
                pattern_state = InsnPatternState::GetLocalx2;
            } else if (instruction.opcode() == Instructions::i32_const) {
                i32_const_value = instruction.arguments().get<i32>();
                pattern_state = InsnPatternState::GetLocalI32Const;
            } else if (instruction.opcode() == Instructions::i64_const) {
                i64_const_value = instruction.arguments().get<i64>();
                pattern_state = InsnPatternState::GetLocalI64Const;
            } else if (instruction.opcode() == Instructions::i32_store) {
                // `local.get a; i32.store m` -> `i32.storelocal a m`.
                set_default_dispatch(nop, result.dispatches.size() - 1);
                result.extra_instruction_storage.unchecked_append(Instruction(
                    Instructions::synthetic_i32_storelocal,
                    local_index_0,
                    instruction.arguments()));

                set_default_dispatch(result.extra_instruction_storage.unsafe_last());
                pattern_state = InsnPatternState::Nothing;
                continue;
            } else if (instruction.opcode() == Instructions::i64_store) {
                // `local.get a; i64.store m` -> `i64.storelocal a m`.
                set_default_dispatch(nop, result.dispatches.size() - 1);
                result.extra_instruction_storage.unchecked_append(Instruction(
                    Instructions::synthetic_i64_storelocal,
                    local_index_0,
                    instruction.arguments()));

                set_default_dispatch(result.extra_instruction_storage.unsafe_last());
                pattern_state = InsnPatternState::Nothing;
                continue;
            } else if (instruction.opcode() == Instructions::local_set) {
                // `local.get a; local.set b` -> `local_copy a b`.
                set_default_dispatch(nop, result.dispatches.size() - 1);
                result.extra_instruction_storage.unchecked_append(Instruction(
                    Instructions::synthetic_local_copy,
                    local_index_0,
                    instruction.local_index()));

                set_default_dispatch(result.extra_instruction_storage.unsafe_last());
                pattern_state = InsnPatternState::Nothing;
                continue;
            } else {
                pattern_state = InsnPatternState::Nothing;
            }
            break;
        case InsnPatternState::GetLocalx2: {
            auto make_2local_synthetic = [&](OpCode synthetic_op) {
                set_default_dispatch(nop, result.dispatches.size() - 1);
                set_default_dispatch(nop, result.dispatches.size() - 2);
                result.extra_instruction_storage.unchecked_append(Instruction {
                    synthetic_op,
                    local_index_0,
                    local_index_1,
                });
                set_default_dispatch(result.extra_instruction_storage.unsafe_last());
                pattern_state = InsnPatternState::Nothing;
            };
            if (instruction.opcode() == Instructions::i32_add) {
                // `local.get a; local.get b; i32.add` -> `i32.add_2local a b`.
                make_2local_synthetic(Instructions::synthetic_i32_add2local);
                continue;
            }
            if (instruction.opcode() == Instructions::i32_sub) {
                // `local.get a; local.get b; i32.sub` -> `i32.sub_2local a b`.
                make_2local_synthetic(Instructions::synthetic_i32_sub2local);
                continue;
            }
            if (instruction.opcode() == Instructions::i32_mul) {
                // `local.get a; local.get b; i32.mul` -> `i32.mul_2local a b`.
                make_2local_synthetic(Instructions::synthetic_i32_mul2local);
                continue;
            }
            if (instruction.opcode() == Instructions::i32_and) {
                // `local.get a; local.get b; i32.and` -> `i32.and_2local a b`.
                make_2local_synthetic(Instructions::synthetic_i32_and2local);
                continue;
            }
            if (instruction.opcode() == Instructions::i32_or) {
                // `local.get a; local.get b; i32.or` -> `i32.or_2local a b`.
                make_2local_synthetic(Instructions::synthetic_i32_or2local);
                continue;
            }
            if (instruction.opcode() == Instructions::i32_xor) {
                // `local.get a; local.get b; i32.xor` -> `i32.xor_2local a b`.
                make_2local_synthetic(Instructions::synthetic_i32_xor2local);
                continue;
            }
            if (instruction.opcode() == Instructions::i32_shl) {
                // `local.get a; local.get b; i32.shl` -> `i32.shl_2local a b`.
                make_2local_synthetic(Instructions::synthetic_i32_shl2local);
                continue;
            }
            if (instruction.opcode() == Instructions::i32_shru) {
                // `local.get a; local.get b; i32.shr_u` -> `i32.shru_2local a b`.
                make_2local_synthetic(Instructions::synthetic_i32_shru2local);
                continue;
            }
            if (instruction.opcode() == Instructions::i32_shrs) {
                // `local.get a; local.get b; i32.shr_s` -> `i32.shrs_2local a b`.
                make_2local_synthetic(Instructions::synthetic_i32_shrs2local);
                continue;
            }
            if (instruction.opcode() == Instructions::i64_add) {
                // `local.get a; local.get b; i64.add` -> `i64.add_2local a b`.
                make_2local_synthetic(Instructions::synthetic_i64_add2local);
                continue;
            }
            if (instruction.opcode() == Instructions::i64_sub) {
                // `local.get a; local.get b; i64.sub` -> `i64.sub_2local a b`.
                make_2local_synthetic(Instructions::synthetic_i64_sub2local);
                continue;
            }
            if (instruction.opcode() == Instructions::i64_mul) {
                // `local.get a; local.get b; i64.mul` -> `i64.mul_2local a b`.
                make_2local_synthetic(Instructions::synthetic_i64_mul2local);
                continue;
            }
            if (instruction.opcode() == Instructions::i64_and) {
                // `local.get a; local.get b; i64.and` -> `i64.and_2local a b`.
                make_2local_synthetic(Instructions::synthetic_i64_and2local);
                continue;
            }
            if (instruction.opcode() == Instructions::i64_or) {
                // `local.get a; local.get b; i64.or` -> `i64.or_2local a b`.
                make_2local_synthetic(Instructions::synthetic_i64_or2local);
                continue;
            }
            if (instruction.opcode() == Instructions::i64_xor) {
                // `local.get a; local.get b; i64.xor` -> `i64.xor_2local a b`.
                make_2local_synthetic(Instructions::synthetic_i64_xor2local);
                continue;
            }
            if (instruction.opcode() == Instructions::i64_shl) {
                // `local.get a; local.get b; i64.shl` -> `i64.shl_2local a b`.
                make_2local_synthetic(Instructions::synthetic_i64_shl2local);
                continue;
            }
            if (instruction.opcode() == Instructions::i64_shru) {
                // `local.get a; local.get b; i64.shr_u` -> `i64.shru_2local a b`.
                make_2local_synthetic(Instructions::synthetic_i64_shru2local);
                continue;
            }
            if (instruction.opcode() == Instructions::i64_shrs) {
                // `local.get a; local.get b; i64.shr_s` -> `i64.shrs_2local a b`.
                make_2local_synthetic(Instructions::synthetic_i64_shrs2local);
                continue;
            }
            if (instruction.opcode() == Instructions::i32_store) {
                // `local.get a; i32.store m` -> `i32.storelocal a m`.
                set_default_dispatch(nop, result.dispatches.size() - 1);
                result.extra_instruction_storage.unchecked_append(Instruction(
                    Instructions::synthetic_i32_storelocal,
                    local_index_1,
                    instruction.arguments()));

                set_default_dispatch(result.extra_instruction_storage.unsafe_last());
                pattern_state = InsnPatternState::Nothing;
                continue;
            }
            if (instruction.opcode() == Instructions::i64_store) {
                // `local.get a; i64.store m` -> `i64.storelocal a m`.
                set_default_dispatch(nop, result.dispatches.size() - 1);
                result.extra_instruction_storage.unchecked_append(Instruction(
                    Instructions::synthetic_i64_storelocal,
                    local_index_1,
                    instruction.arguments()));

                set_default_dispatch(result.extra_instruction_storage.unsafe_last());
                pattern_state = InsnPatternState::Nothing;
                continue;
            }
            if (instruction.opcode() == Instructions::i32_const) {
                swap(local_index_0, local_index_1);
                i32_const_value = instruction.arguments().get<i32>();
                pattern_state = InsnPatternState::GetLocalI32Const;
            } else if (instruction.opcode() == Instructions::i64_const) {
                swap(local_index_0, local_index_1);
                i64_const_value = instruction.arguments().get<i64>();
                pattern_state = InsnPatternState::GetLocalI64Const;
            } else {
                pattern_state = InsnPatternState::Nothing;
            }
        } break;
        case InsnPatternState::I32Const:
            if (instruction.opcode() == Instructions::local_get) {
                local_index_0 = instruction.local_index();
                pattern_state = InsnPatternState::I32ConstGetLocal;
            } else if (instruction.opcode() == Instructions::i32_const) {
                i32_const_value = instruction.arguments().get<i32>();
            } else if (instruction.opcode() == Instructions::local_set) {
                // `i32.const a; local.set b` -> `local.seti32_const b a`.
                set_default_dispatch(nop, result.dispatches.size() - 1);
                result.extra_instruction_storage.unchecked_append(Instruction(
                    Instructions::synthetic_local_seti32_const,
                    instruction.local_index(),
                    i32_const_value));
                set_default_dispatch(result.extra_instruction_storage.unsafe_last());
                pattern_state = InsnPatternState::Nothing;
                continue;
            } else {
                pattern_state = InsnPatternState::Nothing;
            }
            break;
        case InsnPatternState::GetLocalI32Const:
            if (instruction.opcode() == Instructions::local_set) {
                // `i32.const a; local.set b` -> `local.seti32_const b a`.
                set_default_dispatch(nop, result.dispatches.size() - 1);
                result.extra_instruction_storage.unchecked_append(Instruction(
                    Instructions::synthetic_local_seti32_const,
                    instruction.local_index(),
                    i32_const_value));
                set_default_dispatch(result.extra_instruction_storage.unsafe_last());
                pattern_state = InsnPatternState::Nothing;
                continue;
            }
            if (instruction.opcode() == Instructions::i32_const) {
                i32_const_value = instruction.arguments().get<i32>();
                pattern_state = InsnPatternState::I32Const;
                break;
            }
            if (instruction.opcode() == Instructions::local_get) {
                local_index_0 = instruction.local_index();
                pattern_state = InsnPatternState::I32ConstGetLocal;
                break;
            }
            [[fallthrough]];
        case InsnPatternState::I32ConstGetLocal:
            if (instruction.opcode() == Instructions::i32_const) {
                i32_const_value = instruction.arguments().get<i32>();
                pattern_state = InsnPatternState::GetLocalI32Const;
            } else if (instruction.opcode() == Instructions::local_get) {
                swap(local_index_0, local_index_1);
                local_index_1 = instruction.local_index();
                pattern_state = InsnPatternState::GetLocalx2;
            } else if (instruction.opcode() == Instructions::i32_add) {
                // `i32.const a; local.get b; i32.add` -> `i32.add_constlocal b a`.
                // Replace the previous two ops with noops, and add i32.add_constlocal.
                set_default_dispatch(nop, result.dispatches.size() - 1);
                set_default_dispatch(nop, result.dispatches.size() - 2);
                result.extra_instruction_storage.unchecked_append(Instruction(
                    Instructions::synthetic_i32_addconstlocal,
                    local_index_0,
                    i32_const_value));

                set_default_dispatch(result.extra_instruction_storage.unsafe_last());
                pattern_state = InsnPatternState::Nothing;
                continue;
            }
            if (instruction.opcode() == Instructions::i32_and) {
                // `i32.const a; local.get b; i32.add` -> `i32.and_constlocal b a`.
                // Replace the previous two ops with noops, and add i32.and_constlocal.
                set_default_dispatch(nop, result.dispatches.size() - 1);
                set_default_dispatch(nop, result.dispatches.size() - 2);
                result.extra_instruction_storage.unchecked_append(Instruction(
                    Instructions::synthetic_i32_andconstlocal,
                    local_index_0,
                    i32_const_value));

                set_default_dispatch(result.extra_instruction_storage.unsafe_last());
                pattern_state = InsnPatternState::Nothing;
                continue;
            }
            pattern_state = InsnPatternState::Nothing;
            break;
        case InsnPatternState::I64Const:
            if (instruction.opcode() == Instructions::local_get) {
                local_index_0 = instruction.local_index();
                pattern_state = InsnPatternState::I64ConstGetLocal;
            } else if (instruction.opcode() == Instructions::i64_const) {
                i64_const_value = instruction.arguments().get<i64>();
            } else if (instruction.opcode() == Instructions::local_set) {
                // `i64.const a; local.set b` -> `local.seti64_const b a`.
                set_default_dispatch(nop, result.dispatches.size() - 1);
                result.extra_instruction_storage.unchecked_append(Instruction(
                    Instructions::synthetic_local_seti64_const,
                    instruction.local_index(),
                    i64_const_value));
                set_default_dispatch(result.extra_instruction_storage.unsafe_last());
                pattern_state = InsnPatternState::Nothing;
                continue;
            } else {
                pattern_state = InsnPatternState::Nothing;
            }
            break;
        case InsnPatternState::GetLocalI64Const:
            if (instruction.opcode() == Instructions::local_set) {
                // `i64.const a; local.set b` -> `local.seti64_const b a`.
                set_default_dispatch(nop, result.dispatches.size() - 1);
                result.extra_instruction_storage.unchecked_append(Instruction(
                    Instructions::synthetic_local_seti64_const,
                    instruction.local_index(),
                    i64_const_value));
                set_default_dispatch(result.extra_instruction_storage.unsafe_last());
                pattern_state = InsnPatternState::Nothing;
                continue;
            }
            if (instruction.opcode() == Instructions::i64_const) {
                i64_const_value = instruction.arguments().get<i64>();
                pattern_state = InsnPatternState::I64Const;
                break;
            }
            if (instruction.opcode() == Instructions::local_get) {
                local_index_0 = instruction.local_index();
                pattern_state = InsnPatternState::I64ConstGetLocal;
                break;
            }
            [[fallthrough]];
        case InsnPatternState::I64ConstGetLocal:
            if (instruction.opcode() == Instructions::i64_const) {
                i64_const_value = instruction.arguments().get<i64>();
                pattern_state = InsnPatternState::GetLocalI64Const;
            } else if (instruction.opcode() == Instructions::local_get) {
                swap(local_index_0, local_index_1);
                local_index_1 = instruction.local_index();
                pattern_state = InsnPatternState::GetLocalx2;
            } else if (instruction.opcode() == Instructions::i64_add) {
                // `i64.const a; local.get b; i64.add` -> `i64.add_constlocal b a`.
                // Replace the previous two ops with noops, and add i64.add_constlocal.
                set_default_dispatch(nop, result.dispatches.size() - 1);
                set_default_dispatch(nop, result.dispatches.size() - 2);
                result.extra_instruction_storage.unchecked_append(Instruction(
                    Instructions::synthetic_i64_addconstlocal,
                    local_index_0,
                    i64_const_value));

                set_default_dispatch(result.extra_instruction_storage.unsafe_last());
                pattern_state = InsnPatternState::Nothing;
                continue;
            } else if (instruction.opcode() == Instructions::i64_and) {
                // `i64.const a; local.get b; i64.and` -> `i64.and_constlocal b a`.
                // Replace the previous two ops with noops, and add i64.and_constlocal.
                set_default_dispatch(nop, result.dispatches.size() - 1);
                set_default_dispatch(nop, result.dispatches.size() - 2);
                result.extra_instruction_storage.unchecked_append(Instruction(
                    Instructions::synthetic_i64_andconstlocal,
                    local_index_0,
                    i64_const_value));

                set_default_dispatch(result.extra_instruction_storage.unsafe_last());
                pattern_state = InsnPatternState::Nothing;
                continue;
            } else {
                pattern_state = InsnPatternState::Nothing;
            }
            break;
        }
        set_default_dispatch(instruction);
    }

    // Remove all nops (that were either added by the above patterns or were already present in the original instructions),
    // and adjust jumps accordingly.
    RedBlackTree<size_t, Empty> nops_to_remove;
    for (size_t i = 0; i < result.dispatches.size(); ++i) {
        if (result.dispatches[i].instruction->opcode() == Instructions::nop)
            nops_to_remove.insert(i, {});
    }

    auto nops_to_remove_it = nops_to_remove.begin();
    size_t offset_accumulated = 0;
    for (size_t i = 0; i < result.dispatches.size(); ++i) {
        if (result.dispatches[i].instruction->opcode() == Instructions::nop) {
            offset_accumulated++;
            ++nops_to_remove_it;
            continue;
        }

        auto& args = result.dispatches[i].instruction->arguments();
        if (auto ptr = args.get_pointer<Instruction::StructuredInstructionArgs>()) {
            auto offset_to = [&](InstructionPointer ip) {
                size_t offset = 0;
                auto it = nops_to_remove_it;
                while (it != nops_to_remove.end() && it.key() < ip.value()) {
                    ++offset;
                    ++it;
                }
                return offset;
            };

            InstructionPointer end_ip = ptr->end_ip.value() - offset_accumulated - offset_to(ptr->end_ip - ptr->else_ip.has_value());
            auto else_ip = ptr->else_ip.map([&](InstructionPointer const& ip) -> InstructionPointer { return ip.value() - offset_accumulated - offset_to(ip - 1); });
            auto instruction = *result.dispatches[i].instruction;
            instruction.arguments() = Instruction::StructuredInstructionArgs {
                .block_type = ptr->block_type,
                .end_ip = end_ip,
                .else_ip = else_ip,
                .meta = ptr->meta,
            };
            result.extra_instruction_storage.unchecked_append(move(instruction));
            result.dispatches[i].instruction = &result.extra_instruction_storage.unsafe_last();
            result.dispatches[i].instruction_opcode = result.dispatches[i].instruction->opcode();
        }
    }

    result.dispatches.remove_all(nops_to_remove, [](auto const& it) { return it.key(); });
    result.src_dst_mappings.remove_all(nops_to_remove, [](auto const& it) { return it.key(); });

    // Rewrite local.* of arguments to argument.* to keep local.* for locals only.
    for (size_t i = 0; i < result.dispatches.size(); ++i) {
        auto& dispatch = result.dispatches[i];
        if (dispatch.instruction->opcode() == Instructions::local_get) {
            auto local_index = dispatch.instruction->local_index();
            if (local_index.value() & LocalArgumentMarker) {
                result.extra_instruction_storage.unchecked_append(Instruction(
                    Instructions::synthetic_argument_get,
                    local_index));
                result.dispatches[i].instruction = &result.extra_instruction_storage.unsafe_last();
                result.dispatches[i].instruction_opcode = result.dispatches[i].instruction->opcode();
            }
        } else if (dispatch.instruction->opcode() == Instructions::local_set) {
            auto local_index = dispatch.instruction->local_index();
            if (local_index.value() & LocalArgumentMarker) {
                result.extra_instruction_storage.unchecked_append(Instruction(
                    Instructions::synthetic_argument_set,
                    local_index));
                result.dispatches[i].instruction = &result.extra_instruction_storage.unsafe_last();
                result.dispatches[i].instruction_opcode = result.dispatches[i].instruction->opcode();
            }
        } else if (dispatch.instruction->opcode() == Instructions::local_tee) {
            auto local_index = dispatch.instruction->local_index();
            if (local_index.value() & LocalArgumentMarker) {
                result.extra_instruction_storage.unchecked_append(Instruction(
                    Instructions::synthetic_argument_tee,
                    local_index));
                result.dispatches[i].instruction = &result.extra_instruction_storage.unsafe_last();
                result.dispatches[i].instruction_opcode = result.dispatches[i].instruction->opcode();
            }
        }
    }

    // Allocate registers for instructions, meeting the following constraints:
    // - Any instruction that produces polymorphic stack, or requires its inputs on the stack must sink all active values to the stack.
    // - All instructions must have the same location for their last input and their destination value (if any).
    // - Any value left at the end of the expression must be on the stack.

    using ValueID = DistinctNumeric<size_t, struct ValueIDTag, AK::DistinctNumericFeature::Comparison, AK::DistinctNumericFeature::Arithmetic, AK::DistinctNumericFeature::Increment>;
    using IP = DistinctNumeric<size_t, struct IPTag, AK::DistinctNumericFeature::Comparison>;

    struct Value {
        ValueID id;
        IP definition_index;
        Vector<IP> uses;
        IP last_use = 0;
        bool was_created_as_a_result_of_polymorphic_stack = false;
    };

    struct ActiveReg {
        ValueID value_id;
        IP end;
        Dispatch::RegisterOrStack reg;
    };

    HashMap<ValueID, Value> values;
    Vector<ValueID> value_stack;
    ValueID next_value_id = 0;
    HashMap<IP, ValueID> instr_to_output_value;
    HashMap<IP, Vector<ValueID>> instr_to_input_values;
    HashMap<IP, Vector<ValueID>> instr_to_dependent_values;

    instr_to_output_value.ensure_capacity(result.dispatches.size());
    instr_to_input_values.ensure_capacity(result.dispatches.size());
    instr_to_dependent_values.ensure_capacity(result.dispatches.size());

    Vector<ValueID> forced_stack_values;

    Vector<ValueID> parent;      // parent[id] -> parent ValueID of id in the alias tree
    Vector<ValueID> rank;        // rank[id] -> rank of the tree rooted at id
    Vector<ValueID> final_roots; // final_roots[id] -> the final root parent of id

    auto ensure_id_space = [&](ValueID id) {
        if (id >= parent.size()) {
            size_t old_size = parent.size();
            parent.resize_with_default_value(id.value() + 1, {});
            rank.resize_with_default_value(id.value() + 1, {});
            final_roots.resize_with_default_value(id.value() + 1, {});
            for (size_t i = old_size; i <= id; ++i) {
                parent[i] = i;
                rank[i] = 0;
                final_roots[i] = i;
            }
        }
    };

    auto find_root = [&parent](this auto& self, ValueID x) -> ValueID {
        if (parent[x.value()] != x)
            parent[x.value()] = self(parent[x.value()]);
        return parent[x.value()];
    };

    auto union_alias = [&](ValueID a, ValueID b) {
        ensure_id_space(max(a, b));

        auto const root_a = find_root(a);
        auto const root_b = find_root(b);

        if (root_a == root_b)
            return;

        if (rank[root_a.value()] < rank[root_b.value()]) {
            parent[root_a.value()] = root_b;
        } else if (rank[root_a.value()] > rank[root_b.value()]) {
            parent[root_b.value()] = root_a;
        } else {
            parent[root_b.value()] = root_a;
            ++rank[root_a.value()];
        }
    };

    HashTable<ValueID> stack_forced_roots;

    Vector<Vector<ValueID>> live_at_instr;
    live_at_instr.resize(result.dispatches.size());

    // Track call record constraints
    HashMap<ValueID, u8> value_to_callrec_slot;

    struct CallInfo {
        size_t call_index;
        size_t param_count;
        size_t result_count;
        size_t earliest_arg_index;
        Vector<ValueID> arg_values;
    };
    Vector<CallInfo> eligible_calls;

    eligible_calls.ensure_capacity(calls_in_expression);

    for (size_t i = 0; i < result.dispatches.size(); ++i) {
        auto& dispatch = result.dispatches[i];
        auto opcode = dispatch.instruction->opcode();
        size_t inputs = 0;
        size_t outputs = 0;
        Vector<ValueID> dependent_ids;

        bool variadic_or_unknown = false;
        bool requires_aliased_destination = true;

        switch (opcode.value()) {
#define M(name, _, ins, outs)                    \
    case Instructions::name.value():             \
        if constexpr (ins == -1 || outs == -1) { \
            variadic_or_unknown = true;          \
        }                                        \
        inputs = max(ins, 0);                    \
        outputs = max(outs, 0);                  \
        break;
            ENUMERATE_WASM_OPCODES(M)
#undef M
        }

        Vector<ValueID> input_ids;

        if (opcode == Instructions::call) {
            auto& type = functions[dispatch.instruction->arguments().get<FunctionIndex>().value()];

            if (type.parameters().size() <= (Dispatch::LastCallRecord - Dispatch::CallRecord + 1)
                && type.results().size() <= 1
                && type.parameters().size() <= value_stack.size()) {

                inputs = type.parameters().size();
                outputs = type.results().size();
                variadic_or_unknown = false;
                requires_aliased_destination = false;

                auto value_stack_copy = value_stack;

                for (size_t j = 0; j < inputs; ++j) {
                    auto input_value = value_stack.take_last();
                    auto& value = values.get(input_value).value();

                    // if this value was created as a result of a polymorphic stack,
                    // we can't actually go and force it to a call record again, so disqualify this call.
                    if (value.was_created_as_a_result_of_polymorphic_stack) {
                        inputs = 0;
                        outputs = 0;
                        variadic_or_unknown = true;
                        value_stack = move(value_stack_copy);
                        goto avoid_optimizing_this_call;
                    }

                    input_ids.append(input_value);
                    dependent_ids.append(input_value);
                    value.uses.append(i);
                    value.last_use = max(value.last_use, i);
                    forced_stack_values.append(input_value);
                }
                instr_to_input_values.set(i, input_ids);
                instr_to_dependent_values.set(i, dependent_ids);

                for (size_t j = 0; j < outputs; ++j) {
                    auto id = next_value_id++;
                    values.set(id, Value { id, i, {}, i });
                    value_stack.append(id);
                    instr_to_output_value.set(i, id);
                    ensure_id_space(id);
                }

                size_t earliest = i;
                ValueID earliest_arg_value = NumericLimits<size_t>::max();
                for (auto value_id : input_ids) {
                    auto& value = values.get(value_id).value();
                    if (earliest > value.definition_index.value()) {
                        earliest = value.definition_index.value();
                        earliest_arg_value = value_id;
                    }
                }

                // Reverse the input_ids to match stack order
                Vector<ValueID> reversed_args;
                for (size_t j = 0; j < inputs; ++j) {
                    reversed_args.append(input_ids[inputs - 1 - j]);
                }

                // Follow the alias root of the earliest arg value to find the first instruction that produced it.
                auto new_earliest = earliest;
                while (true) {
                    auto maybe_inputs = instr_to_input_values.get(new_earliest);
                    if (!maybe_inputs.has_value())
                        break;
                    bool found_earliest = false;
                    for (auto val : maybe_inputs.value()) {
                        auto root = find_root(val);
                        if (root == find_root(earliest_arg_value)) {
                            auto& value = values.get(val).value();
                            if (value.definition_index.value() < new_earliest) {
                                new_earliest = value.definition_index.value();
                                found_earliest = true;
                                break;
                            }
                        }
                    }
                    if (!found_earliest)
                        break;
                }

                eligible_calls.append({ .call_index = i,
                    .param_count = inputs,
                    .result_count = outputs,
                    .earliest_arg_index = new_earliest,
                    .arg_values = reversed_args });

                continue;
            }
        }
    avoid_optimizing_this_call:;

        // Handle the inputs we actually know about.
        size_t j = 0;
        for (; j < inputs && !value_stack.is_empty(); ++j) {
            auto input_value = value_stack.take_last();
            input_ids.append(input_value);
            dependent_ids.append(input_value);
            auto& value = values.get(input_value).value();
            value.uses.append(i);
            value.last_use = max(value.last_use, i);
        }

        inputs -= j;

        if (variadic_or_unknown) {
            for (auto val : value_stack) {
                auto& value = values.get(val).value();
                value.uses.append(i);
                value.last_use = max(value.last_use, i);
                dependent_ids.append(val);
                forced_stack_values.append(val);
                live_at_instr[i].append(val);
            }
            value_stack.clear_with_capacity();
        }

        if (value_stack.size() < inputs) {
            size_t j = 0;
            for (; j < inputs && !value_stack.is_empty(); ++j) {
                auto input_value = value_stack.take_last();
                input_ids.append(input_value);
                dependent_ids.append(input_value);
                auto& value = values.get(input_value).value();
                value.uses.append(i);
                value.last_use = max(value.last_use, i);
            }

            for (; j < inputs; ++j) {
                auto val_id = next_value_id++;
                values.set(val_id, Value { val_id, i, {}, i, true });
                input_ids.append(val_id);
                forced_stack_values.append(val_id);
                ensure_id_space(val_id);
            }

            inputs = 0;
        }

        for (size_t j = 0; j < inputs; ++j) {
            auto input_value = value_stack.take_last();
            input_ids.append(input_value);
            dependent_ids.append(input_value);
            auto& value = values.get(input_value).value();
            value.uses.append(i);
            value.last_use = max(value.last_use, i);
        }
        instr_to_input_values.set(i, input_ids);
        instr_to_dependent_values.set(i, dependent_ids);

        ValueID output_id = NumericLimits<size_t>::max();
        for (size_t j = 0; j < outputs; ++j) {
            auto id = next_value_id++;
            values.set(id, Value { id, i, {}, i });
            value_stack.append(id);
            instr_to_output_value.set(i, id);
            output_id = id;
            ensure_id_space(id);
        }

        // Alias the output with the last input, if one exists.
        if (outputs > 0 && requires_aliased_destination) {
            auto maybe_input_ids = instr_to_input_values.get(i);
            if (maybe_input_ids.has_value() && !maybe_input_ids->is_empty()) {
                auto last_input_id = maybe_input_ids->last();
                union_alias(output_id, last_input_id);

                auto alias_root = find_root(last_input_id);

                // If the last input was created as a result of polymorphic stack, propagate that to the output (as they're aliased).
                auto& output_value = values.get(output_id).value();
                auto const& input_value = values.get(last_input_id).value();
                if (input_value.was_created_as_a_result_of_polymorphic_stack)
                    output_value.was_created_as_a_result_of_polymorphic_stack = true;

                // If any *other* input is forced to alias the output, we have no choice but to place all three on the stack.
                for (size_t j = 0; j < maybe_input_ids->size() - 1; ++j) {
                    auto input_root = find_root((*maybe_input_ids)[j]);
                    if (input_root == alias_root) {
                        stack_forced_roots.set(alias_root);
                        break;
                    }
                }
            }
        }
    }

    forced_stack_values.extend(value_stack);

    // Build conflict graph and select maximum set of non-conflicting calls
    // Prefer calls with more arguments, and among those with equal args, prefer shorter spans

    struct CallScore {
        size_t index;
        size_t param_count;
        size_t span;
    };

    Vector<CallScore> scored_calls;
    for (size_t i = 0; i < eligible_calls.size(); ++i) {
        auto& call = eligible_calls[i];
        size_t span = call.call_index - call.earliest_arg_index;
        scored_calls.append({ i, call.param_count, span });
    }

    // Sort by: more params first, then shorter span
    quick_sort(scored_calls, [](auto const& a, auto const& b) {
        if (a.param_count != b.param_count)
            return a.param_count > b.param_count;
        return a.span < b.span;
    });

    // Greedily select non-conflicting calls in priority order
    Vector<CallInfo*> valid_calls;
    HashTable<size_t> selected_indices;
    size_t max_call_record_size = 0;

    for (auto const& score : scored_calls) {
        auto& call_info = eligible_calls[score.index];
        size_t call_start = call_info.earliest_arg_index;
        size_t call_end = call_info.call_index;

        bool conflicts = false;
        for (auto* other_call : valid_calls) {
            size_t other_start = other_call->earliest_arg_index;
            size_t other_end = other_call->call_index;

            // Check if the ranges overlap
            // Two ranges [a,b] and [c,d] overlap if: NOT (b < c OR d < a)
            if (!(call_end < other_start || other_end < call_start)) {
                conflicts = true;
                break;
            }
        }

        if (!conflicts) {
            valid_calls.append(&call_info);
            selected_indices.set(score.index);
            max_call_record_size = max(max_call_record_size, call_info.param_count);
        }
    }

    // Only apply call record optimization to non-conflicting calls
    HashTable<size_t> calls_with_records;
    for (auto* call_info : valid_calls) {
        calls_with_records.set(call_info->call_index);

        // Mark values for call record slots
        for (size_t j = 0; j < call_info->param_count; ++j) {
            value_to_callrec_slot.set(call_info->arg_values[j], Dispatch::CallRecord + j);
        }

        auto new_call_opcode = call_info->result_count == 0
            ? Instructions::synthetic_call_with_record_0
            : Instructions::synthetic_call_with_record_1;

        auto new_call_insn = Instruction(
            new_call_opcode,
            result.dispatches[call_info->call_index].instruction->arguments());

        result.extra_instruction_storage.unchecked_append(new_call_insn);
        result.dispatches[call_info->call_index].instruction = &result.extra_instruction_storage.unsafe_last();
        result.dispatches[call_info->call_index].instruction_opcode = new_call_opcode;
    }

    result.max_call_rec_size = max_call_record_size;

    for (size_t i = 0; i < final_roots.size(); ++i)
        final_roots[i] = find_root(i);

    HashMap<ValueID, u8> root_to_callrec_slot;
    for (auto const& [value_id, slot] : value_to_callrec_slot) {
        auto root = final_roots[value_id.value()];
        if (auto existing = root_to_callrec_slot.get(root); existing.has_value()) {
            VERIFY(*existing == slot);
        }
        root_to_callrec_slot.set(root, slot);
    }

    value_to_callrec_slot.clear_with_capacity();
    for (size_t i = 0; i < final_roots.size(); ++i) {
        auto root = final_roots[i];
        if (auto slot = root_to_callrec_slot.get(root); slot.has_value()) {
            value_to_callrec_slot.set(ValueID { i }, *slot);
        }
    }

    struct LiveInterval {
        ValueID value_id;
        IP start;
        IP end;
        bool forced_to_stack { false };
    };

    Vector<LiveInterval> intervals;
    intervals.ensure_capacity(values.size());

    for (auto const& [_, value] : values) {
        auto start = value.definition_index;
        auto end = max(start, value.last_use);
        intervals.append({ value.id, start, end });
    }

    for (auto id : forced_stack_values)
        stack_forced_roots.set(final_roots[id.value()]);
    for (auto& interval : intervals)
        interval.forced_to_stack = stack_forced_roots.contains(final_roots[interval.value_id.value()]);

    quick_sort(intervals, [](auto const& a, auto const& b) {
        return a.start < b.start;
    });

    HashMap<ValueID, Dispatch::RegisterOrStack> value_alloc;
    RedBlackTree<size_t, ActiveReg> active_by_end;

    auto expire_old_intervals = [&](IP current_start) {
        while (true) {
            auto it = active_by_end.find_smallest_not_below_iterator(current_start.value());
            if (it.is_end())
                break;
            active_by_end.remove(it.key());
        }
    };

    HashMap<ValueID, Vector<LiveInterval*>> alias_groups;
    for (auto& interval : intervals) {
        auto root = final_roots[interval.value_id.value()];
        alias_groups.ensure(root).append(&interval);
    }

    struct RegisterOccupancy {
        Bitmap occupied;
        Vector<ValueID> roots_at_position;

        bool can_place(IP start, IP end, ValueID root) const
        {
            for (size_t i = start.value(); i <= end.value(); ++i) {
                if (occupied.get(i)) {
                    if (roots_at_position.size() > i && roots_at_position[i].value() != root.value())
                        return false;
                }
            }
            return true;
        }

        void place(IP start, IP end, ValueID root)
        {
            if (roots_at_position.size() <= end.value())
                roots_at_position.resize_with_default_value(end.value() + 1, {});

            occupied.set_range<true>(start.value(), end.value() - start.value() + 1);
            for (size_t i = start.value(); i <= end.value(); ++i)
                roots_at_position[i] = root;
        }
    };

    Array<RegisterOccupancy, Dispatch::CountRegisters> reg_occupancy;

    for (u8 r = 0; r < Dispatch::CountRegisters; ++r) {
        auto bitmap_result = Bitmap::create(result.dispatches.size(), false);
        if (bitmap_result.is_error()) {
            dbgln("Failed to allocate register bitmap of size {} ({}), bailing on register allocation", result.dispatches.size(), bitmap_result.error());
            return {};
        }
        reg_occupancy[r].occupied = bitmap_result.release_value();
    }

    for (auto& [key, group] : alias_groups) {
        // Check if any value in this group needs a call record slot
        Dispatch::RegisterOrStack forced_slot = Dispatch::RegisterOrStack::Stack;
        bool has_callrec_constraint = false;

        for (auto* interval : group) {
            if (auto slot = value_to_callrec_slot.get(interval->value_id); slot.has_value()) {
                forced_slot = static_cast<Dispatch::RegisterOrStack>(*slot);
                has_callrec_constraint = true;
                break;
            }
        }

        if (has_callrec_constraint) {
            // Force all values in this alias group to use the call record slot
            for (auto* interval : group) {
                value_alloc.set(interval->value_id, forced_slot);
            }
            continue;
        }

        auto has_fixed_allocation = false;
        for (auto* interval : group) {
            if (value_alloc.contains(interval->value_id)) {
                has_fixed_allocation = true;
                break;
            }
        }
        if (has_fixed_allocation)
            continue;

        IP group_start = NumericLimits<size_t>::max();
        IP group_end = 0;
        auto group_forced_to_stack = false;

        for (auto* interval : group) {
            group_start = min(group_start, interval->start);
            group_end = max(group_end, interval->end);
            if (interval->forced_to_stack)
                group_forced_to_stack = true;
        }

        expire_old_intervals(group_start);

        Dispatch::RegisterOrStack reg = Dispatch::RegisterOrStack::Stack;
        if (!group_forced_to_stack) {
            Array<bool, Dispatch::CountRegisters> used_regs;
            used_regs.fill(false);

            for (auto const& active_entry : active_by_end) {
                if (active_entry.reg != Dispatch::RegisterOrStack::Stack)
                    used_regs[to_underlying(active_entry.reg)] = true;
            }

            auto group_root = final_roots[key.value()];

            for (u8 r = 0; r < Dispatch::CountRegisters; ++r) {
                if (used_regs[r])
                    continue;

                if (reg_occupancy[r].can_place(group_start, group_end, group_root)) {
                    reg = static_cast<Dispatch::RegisterOrStack>(r);
                    active_by_end.insert(group_end.value(), { key, group_end, reg });
                    reg_occupancy[r].place(group_start, group_end, group_root);
                    break;
                }
            }
        }

        for (auto* interval : group)
            value_alloc.set(interval->value_id, reg);
    }

    size_t max_call_arg_count = 0;
    for (size_t i = 0; i < result.dispatches.size(); ++i) {
        auto& dispatch = result.dispatches[i];
        if (dispatch.instruction->opcode() == Instructions::call
            || dispatch.instruction->opcode() == Instructions::synthetic_call_00
            || dispatch.instruction->opcode() == Instructions::synthetic_call_10
            || dispatch.instruction->opcode() == Instructions::synthetic_call_11
            || dispatch.instruction->opcode() == Instructions::synthetic_call_20
            || dispatch.instruction->opcode() == Instructions::synthetic_call_21
            || dispatch.instruction->opcode() == Instructions::synthetic_call_30
            || dispatch.instruction->opcode() == Instructions::synthetic_call_31) {

            auto target = dispatch.instruction->arguments().get<FunctionIndex>();
            if (target.value() < functions.size()) {
                auto& function = functions[target.value()];
                max_call_arg_count = max(max_call_arg_count, function.parameters().size());
            }
        }

        auto& addr = result.src_dst_mappings[i];
        auto input_ids = instr_to_input_values.get(IP(i)).value_or({});
        if (input_ids.size() <= array_size(addr.sources)) {
            for (size_t j = 0; j < input_ids.size(); ++j) {
                auto reg = value_alloc.get(input_ids[j]).value_or(Dispatch::RegisterOrStack::Stack);
                addr.sources[j] = reg;
            }
        }

        if (auto output_id = instr_to_output_value.get(IP(i)); output_id.has_value())
            addr.destination = value_alloc.get(*output_id).value_or(Dispatch::RegisterOrStack::Stack);
    }

    result.max_call_arg_count = max_call_arg_count;

    // Swap out local.get (0..7) with local.get_[0..7] to avoid one extra load when possible
    for (size_t i = 0; i < result.dispatches.size(); ++i) {
        auto& dispatch = result.dispatches[i];
        if (dispatch.instruction->opcode() == Instructions::local_get) {
            auto local_index = dispatch.instruction->local_index().value();
            if (local_index <= 7) {
                result.extra_instruction_storage.unchecked_append(Instruction(
                    static_cast<OpCode>(Instructions::synthetic_local_get_0.value() + local_index),
                    dispatch.instruction->local_index()));
                result.dispatches[i].instruction = &result.extra_instruction_storage.unsafe_last();
                result.dispatches[i].instruction_opcode = result.dispatches[i].instruction->opcode();
            }
        }
    }

    // Swap out local.set (0..7) with local.set_[0..7] to avoid one extra load when possible
    for (size_t i = 0; i < result.dispatches.size(); ++i) {
        auto& dispatch = result.dispatches[i];
        if (dispatch.instruction->opcode() == Instructions::local_set) {
            auto local_index = dispatch.instruction->local_index().value();
            if (local_index <= 7) {
                result.extra_instruction_storage.unchecked_append(Instruction(
                    static_cast<OpCode>(Instructions::synthetic_local_set_0.value() + local_index),
                    dispatch.instruction->local_index()));
                result.dispatches[i].instruction = &result.extra_instruction_storage.unsafe_last();
                result.dispatches[i].instruction_opcode = result.dispatches[i].instruction->opcode();
            }
        }
    }

    // Swap out br(.if) with synthetic:br(.if).nostack if !args.has_stack_adjustment.
    for (size_t i = 0; i < result.dispatches.size(); ++i) {
        auto& dispatch = result.dispatches[i];
        if ((dispatch.instruction->opcode() == Instructions::br || dispatch.instruction->opcode() == Instructions::br_if)
            && !dispatch.instruction->arguments().get<Instruction::BranchArgs>().has_stack_adjustment) {
            auto new_opcode = dispatch.instruction->opcode() == Instructions::br
                ? Instructions::synthetic_br_nostack
                : Instructions::synthetic_br_if_nostack;
            result.extra_instruction_storage.unchecked_append(Instruction(
                new_opcode,
                dispatch.instruction->arguments()));
            result.dispatches[i].instruction = &result.extra_instruction_storage.unsafe_last();
            result.dispatches[i].instruction_opcode = result.dispatches[i].instruction->opcode();
        }
    }

    if constexpr (should_try_to_use_direct_threading) {
        constexpr auto all_sources_are_registers = [](SourcesAndDestination const& addrs, ssize_t expected_source_count, ssize_t expected_dest_count) -> bool {
            if (expected_source_count < 0 || expected_dest_count > 1)
                return false;
            for (ssize_t i = 0; i < expected_source_count; ++i) {
                if (addrs.sources[i] >= Dispatch::Stack)
                    return false;
            }

            if (expected_dest_count == 1 && addrs.destination >= Dispatch::Stack)
                return false;

            return true;
        };
        constexpr auto all_sources_are_callrec = [](SourcesAndDestination const& addrs, ssize_t expected_source_count, ssize_t expected_dest_count) -> bool {
            if (expected_source_count < 0 || expected_dest_count > 1)
                return false;
            for (ssize_t i = 0; i < expected_source_count; ++i) {
                if (addrs.sources[i] < Dispatch::CallRecord)
                    return false;
            }
            if (expected_dest_count == 1 && addrs.destination < Dispatch::CallRecord)
                return false;
            return true;
        };
        constexpr auto all_sources_are_stack = [](SourcesAndDestination const& addrs, ssize_t expected_source_count, ssize_t expected_dest_count) -> bool {
            if (expected_source_count < 0 || expected_dest_count > 1)
                return false;
            for (ssize_t i = 0; i < expected_source_count; ++i) {
                if (addrs.sources[i] != Dispatch::Stack)
                    return false;
            }

            if (expected_dest_count == 1 && addrs.destination != Dispatch::Stack)
                return false;

            return true;
        };

        for (size_t i = 0; i < result.dispatches.size(); ++i) {
            auto& dispatch = result.dispatches[i];
            auto& addrs = result.src_dst_mappings[i];

#define CASE(name, _, inputs, outputs)                                                                                                                                         \
    case Instructions::name.value():                                                                                                                                           \
        if (all_sources_are_registers(addrs, inputs, outputs))                                                                                                                 \
            dispatch.handler_ptr = bit_cast<FlatPtr>(&InstructionHandler<Instructions::name.value()>::template operator()<false, Continue, SourceAddressMix::AllRegisters>);   \
        else if (all_sources_are_callrec(addrs, inputs, outputs))                                                                                                              \
            dispatch.handler_ptr = bit_cast<FlatPtr>(&InstructionHandler<Instructions::name.value()>::template operator()<false, Continue, SourceAddressMix::AllCallRecords>); \
        else if (all_sources_are_stack(addrs, inputs, outputs))                                                                                                                \
            dispatch.handler_ptr = bit_cast<FlatPtr>(&InstructionHandler<Instructions::name.value()>::template operator()<false, Continue, SourceAddressMix::AllStack>);       \
        else                                                                                                                                                                   \
            dispatch.handler_ptr = bit_cast<FlatPtr>(&InstructionHandler<Instructions::name.value()>::template operator()<false, Continue, SourceAddressMix::Any>);            \
        break;

            switch (dispatch.instruction->opcode().value()) {
                ENUMERATE_WASM_OPCODES(CASE)
            default:
                dbgln("No handler for opcode {}", dispatch.instruction->opcode().value());
                VERIFY_NOT_REACHED();
            }
        }
        result.direct = true;
    }

    // Verify instruction stream.
    struct Mark {
        size_t ip;
        StringView label;
    };

    auto print_instructions_around = [&](size_t start_ish, size_t end_ish, auto... marks) {
        auto sterr = MUST(Core::File::standard_error());
        Printer p(*sterr);
        auto print_range = [&](size_t start_ip, size_t end_ip) {
            for (size_t k = start_ip; k < end_ip; ++k) {
                warn("[{:04}] ", k);
                auto instruction = result.dispatches[k].instruction;
                auto addresses = result.src_dst_mappings[k];

                p.print(*instruction);

                ([&] { if (k == marks.ip) warnln("       ^-- {}", marks.label); }(), ...);

                ssize_t in_count = 0;
                ssize_t out_count = 0;
                switch (instruction->opcode().value()) {
#define XM(name, _, ins, outs)             \
    case Wasm::Instructions::name.value(): \
        in_count = ins;                    \
        out_count = outs;                  \
        break;

                    ENUMERATE_WASM_OPCODES(XM)
                }
                for (ssize_t i = 0; i < in_count; ++i) {
                    warnln("       arg{} [{}]", i, regname(addresses.sources[i]));
                }
                if (out_count == 1) {
                    auto dest = addresses.destination;
                    warnln("       dest [{}]", regname(dest));
                } else if (out_count > 1) {
                    warnln("       dest [multiple outputs]");
                } else if (instruction->opcode() == Instructions::call || instruction->opcode() == Instructions::call_indirect) {
                    if (addresses.destination != Dispatch::Stack)
                        warnln("       dest [{}]", regname(addresses.destination));
                }
            }
        };

        if (start_ish > end_ish)
            swap(start_ish, end_ish);
        auto start_ip = start_ish >= 40 ? start_ish - 40 : 0;
        auto end_ip = min(result.dispatches.size(), end_ish + 10);
        auto skip_start = Optional<size_t> {};
        for (auto ip = start_ip; ip < end_ip; ip += 5) {
            size_t chunk_end = min(end_ip, ip + 5);
            print_range(ip, chunk_end);
            continue;
            bool has_mark = false;
            for (auto const& mark : { marks... }) {
                if (mark.ip >= ip && mark.ip < chunk_end) {
                    has_mark = true;
                    break;
                }
            }
            if (has_mark || ip == start_ip || chunk_end == end_ip) {
                if (skip_start.has_value()) {
                    warnln("... skipping instructions [{:04}..{:04}] ...", *skip_start, ip);
                    skip_start = {};
                }
                print_range(ip, chunk_end);
            } else if (!skip_start.has_value()) {
                skip_start = ip;
            }
        }
    };

    bool used[256] = { false };
    for (size_t i = 0; i < result.dispatches.size(); ++i) {
        auto& dispatch = result.dispatches[i];
        if (dispatch.instruction->opcode() == Instructions::if_) {
            // if (else) (end), verify (else) - 1 points at a synthetic:else_, and (end)-1+(!has-else) points at a synthetic:end.
            auto args = dispatch.instruction->arguments().get<Instruction::StructuredInstructionArgs>();
            if (args.else_ip.has_value()) {
                size_t else_ip = args.else_ip->value() - 1;
                if (result.dispatches[else_ip].instruction->opcode() != Instructions::structured_else) {
                    dbgln("Invalid else_ip target at instruction {}: else_ip {}", i, else_ip);
                    dbgln("Instructions around the invalid else_ip:");
                    print_instructions_around(i, else_ip, Mark { i, "invalid if_"sv }, Mark { else_ip, "this should've been an else"sv }, Mark { else_ip - 1, "previous instruction"sv }, Mark { else_ip + 1, "next instruction"sv });
                    VERIFY_NOT_REACHED();
                }
            }
            size_t end_ip = args.end_ip.value() - 1 + (args.else_ip.has_value() ? 0 : 1);
            if (result.dispatches[end_ip].instruction->opcode() != Instructions::structured_end) {
                dbgln("Invalid end_ip target at instruction {}: end_ip {}", i, end_ip);
                dbgln("Instructions around the invalid end_ip:");
                print_instructions_around(i, end_ip, Mark { i, "invalid if_"sv }, Mark { end_ip, "this should've been an end"sv }, Mark { end_ip - 1, "previous instruction"sv }, Mark { end_ip + 1, "next instruction"sv });
                VERIFY_NOT_REACHED();
            }
        }
        // If the instruction is a call with a callrec, clear used[] for the callrec registers.
        if (dispatch.instruction->opcode() == Instructions::synthetic_call_with_record_0 || dispatch.instruction->opcode() == Instructions::synthetic_call_with_record_1) {
            for (size_t j = to_underlying(Dispatch::CallRecord); j <= to_underlying(Dispatch::LastCallRecord); ++j)
                used[j] = false;
        }

        auto& addr = result.src_dst_mappings[i];

        // for each input, ensure it's not reading from a register that is not marked as used (unless stack).
        ssize_t in_count = 0;
        ssize_t out_count = 0;
        switch (dispatch.instruction->opcode().value()) {
            ENUMERATE_WASM_OPCODES(XM)
        }
        for (ssize_t j = 0; j < in_count; ++j) {
            auto src = addr.sources[j];
            if (src == Dispatch::Stack)
                continue;
            if (!used[to_underlying(src)]) {
                dbgln("Instruction {} reads from register {} which is not populated", i, to_underlying(src));
                dbgln("Instructions around the invalid read:");
                print_instructions_around(i, i, Mark { i, "invalid read here"sv });
                VERIFY_NOT_REACHED();
            }
            used[to_underlying(src)] = false;
        }
        // if the instruction has an output, ensure it's not writing to a register that is marked used.
        if (out_count == 1 || dispatch.instruction->opcode() == Instructions::call || dispatch.instruction->opcode() == Instructions::call_indirect) {
            auto dest = addr.destination;
            if (dest != Dispatch::Stack) {
                if (used[to_underlying(dest)]) {
                    dbgln("Instruction {} writes to register {} which is already populated", i, to_underlying(dest));
                    dbgln("Instructions around the invalid write:");
                    print_instructions_around(i, i, Mark { i, "invalid write here"sv });
                    VERIFY_NOT_REACHED();
                }
                used[to_underlying(dest)] = true;
            }
        }
    }

    return result;
}

}
