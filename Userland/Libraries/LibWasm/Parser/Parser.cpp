/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ConstrainedStream.h>
#include <AK/Debug.h>
#include <AK/Endian.h>
#include <AK/LEB128.h>
#include <AK/MemoryStream.h>
#include <AK/ScopeGuard.h>
#include <AK/ScopeLogger.h>
#include <AK/UFixedBigInt.h>
#include <LibWasm/Types.h>

namespace Wasm {

#define TRY_READ(stream, type, error)                                                                \
    ({                                                                                               \
        /* Ignore -Wshadow to allow nesting the macro. */                                            \
        AK_IGNORE_DIAGNOSTIC("-Wshadow",                                                             \
            auto&& _temporary_result = stream.read_value<type>());                                   \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_temporary_result.release_value())>, \
            "Do not return a reference from a fallible expression");                                 \
        if (_temporary_result.is_error()) [[unlikely]]                                               \
            return with_eof_check(stream, error);                                                    \
        _temporary_result.release_value();                                                           \
    })

ParseError with_eof_check(Stream const& stream, ParseError error_if_not_eof)
{
    if (stream.is_eof())
        return ParseError::UnexpectedEof;
    return error_if_not_eof;
}

template<typename T>
static auto parse_vector(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger;
    if constexpr (requires { T::parse(stream); }) {
        using ResultT = typename decltype(T::parse(stream))::ResultType;
        auto count_or_error = stream.read_value<LEB128<u32>>();
        if (count_or_error.is_error())
            return ParseResult<Vector<ResultT>> { with_eof_check(stream, ParseError::ExpectedSize) };
        size_t count = count_or_error.release_value();

        Vector<ResultT> entries;
        entries.ensure_capacity(count);
        for (size_t i = 0; i < count; ++i) {
            auto result = T::parse(stream);
            if (result.is_error())
                return ParseResult<Vector<ResultT>> { result.error() };
            entries.append(result.release_value());
        }
        return ParseResult<Vector<ResultT>> { move(entries) };
    } else {
        auto count_or_error = stream.read_value<LEB128<u32>>();
        if (count_or_error.is_error())
            return ParseResult<Vector<T>> { with_eof_check(stream, ParseError::ExpectedSize) };
        size_t count = count_or_error.release_value();

        Vector<T> entries;
        entries.ensure_capacity(count);
        for (size_t i = 0; i < count; ++i) {
            if constexpr (IsSame<T, u32>) {
                auto value_or_error = stream.read_value<LEB128<u32>>();
                if (value_or_error.is_error())
                    return ParseResult<Vector<T>> { with_eof_check(stream, ParseError::ExpectedSize) };
                size_t value = value_or_error.release_value();
                entries.append(value);
            } else if constexpr (IsSame<T, ssize_t>) {
                auto value_or_error = stream.read_value<LEB128<ssize_t>>();
                if (value_or_error.is_error())
                    return ParseResult<Vector<T>> { with_eof_check(stream, ParseError::ExpectedSize) };
                ssize_t value = value_or_error.release_value();
                entries.append(value);
            } else if constexpr (IsSame<T, u8>) {
                if (count > Constants::max_allowed_vector_size)
                    return ParseResult<Vector<T>> { ParseError::HugeAllocationRequested };
                entries.resize(count);
                if (stream.read_until_filled({ entries.data(), entries.size() }).is_error())
                    return ParseResult<Vector<T>> { with_eof_check(stream, ParseError::InvalidInput) };
                break; // Note: We read this all in one go!
            }
        }
        return ParseResult<Vector<T>> { move(entries) };
    }
}

static ParseResult<ByteString> parse_name(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger;
    auto data = TRY(parse_vector<u8>(stream));
    auto string = ByteString::copy(data);
    if (!Utf8View(string).validate(Utf8View::AllowSurrogates::No))
        return ParseError::InvalidUtf8;
    return string;
}

ParseResult<ValueType> ValueType::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("ValueType"sv);
    auto tag = TRY_READ(stream, u8, ParseError::ExpectedKindTag);

    switch (tag) {
    case Constants::i32_tag:
        return ValueType(I32);
    case Constants::i64_tag:
        return ValueType(I64);
    case Constants::f32_tag:
        return ValueType(F32);
    case Constants::f64_tag:
        return ValueType(F64);
    case Constants::v128_tag:
        return ValueType(V128);
    case Constants::function_reference_tag:
        return ValueType(FunctionReference);
    case Constants::extern_reference_tag:
        return ValueType(ExternReference);
    default:
        return ParseError::InvalidTag;
    }
}

ParseResult<ResultType> ResultType::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("ResultType"sv);
    auto types = TRY(parse_vector<ValueType>(stream));
    return ResultType { types };
}

ParseResult<FunctionType> FunctionType::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("FunctionType"sv);
    auto tag = TRY_READ(stream, u8, ParseError::ExpectedKindTag);

    if (tag != Constants::function_signature_tag) {
        dbgln("Expected 0x60, but found {:#x}", tag);
        return with_eof_check(stream, ParseError::InvalidTag);
    }

    auto parameters_result = TRY(parse_vector<ValueType>(stream));
    auto results_result = TRY(parse_vector<ValueType>(stream));

    return FunctionType { parameters_result, results_result };
}

ParseResult<Limits> Limits::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("Limits"sv);
    auto flag = TRY_READ(stream, u8, ParseError::ExpectedKindTag);

    if (flag > 1)
        return with_eof_check(stream, ParseError::InvalidTag);

    auto min_or_error = stream.read_value<LEB128<u32>>();
    if (min_or_error.is_error())
        return with_eof_check(stream, ParseError::ExpectedSize);
    size_t min = min_or_error.release_value();

    Optional<u32> max;
    if (flag) {
        auto value_or_error = stream.read_value<LEB128<u32>>();
        if (value_or_error.is_error())
            return with_eof_check(stream, ParseError::ExpectedSize);
        max = value_or_error.release_value();
    }

    return Limits { static_cast<u32>(min), move(max) };
}

ParseResult<MemoryType> MemoryType::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("MemoryType"sv);
    auto limits_result = TRY(Limits::parse(stream));
    return MemoryType { limits_result };
}

ParseResult<TableType> TableType::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("TableType"sv);
    auto type_result = TRY(ValueType::parse(stream));
    if (!type_result.is_reference())
        return ParseError::InvalidType;
    auto limits_result = TRY(Limits::parse(stream));
    return TableType { type_result, limits_result };
}

ParseResult<GlobalType> GlobalType::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("GlobalType"sv);
    auto type_result = TRY(ValueType::parse(stream));
    auto mutable_ = TRY_READ(stream, u8, ParseError::ExpectedKindTag);

    if (mutable_ > 1)
        return with_eof_check(stream, ParseError::InvalidTag);

    return GlobalType { type_result, mutable_ == 0x01 };
}

ParseResult<BlockType> BlockType::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("BlockType"sv);
    auto kind = TRY_READ(stream, u8, ParseError::ExpectedKindTag);

    if (kind == Constants::empty_block_tag)
        return BlockType {};

    {
        FixedMemoryStream value_stream { ReadonlyBytes { &kind, 1 } };
        if (auto value_type = ValueType::parse(value_stream); !value_type.is_error())
            return BlockType { value_type.release_value() };
    }

    ReconsumableStream new_stream { stream };
    new_stream.unread({ &kind, 1 });

    // FIXME: should be an i33. Right now, we're missing a potential last bit at
    // the end. See https://webassembly.github.io/spec/core/binary/instructions.html#binary-blocktype
    i32 index_value = TRY_READ(new_stream, LEB128<i32>, ParseError::ExpectedIndex);

    if (index_value < 0) {
        dbgln("Invalid type index {}", index_value);
        return with_eof_check(stream, ParseError::InvalidIndex);
    }

    return BlockType { TypeIndex(index_value) };
}

ParseResult<Instruction> Instruction::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("Instruction"sv);
    auto byte = TRY_READ(stream, u8, ParseError::ExpectedKindTag);

    OpCode opcode { byte };

    switch (opcode.value()) {
    case Instructions::block.value():
    case Instructions::loop.value():
    case Instructions::if_.value(): {
        auto block_type = TRY(BlockType::parse(stream));
        return Instruction {
            opcode, StructuredInstructionArgs { block_type, {}, {} }
        };
    }
    case Instructions::br.value():
    case Instructions::br_if.value(): {
        // branches with a single label immediate
        auto index = TRY(GenericIndexParser<LabelIndex>::parse(stream));
        return Instruction { opcode, index };
    }
    case Instructions::br_table.value(): {
        // br_table label* label
        auto labels = TRY(parse_vector<GenericIndexParser<LabelIndex>>(stream));
        auto default_label = TRY(GenericIndexParser<LabelIndex>::parse(stream));
        return Instruction { opcode, TableBranchArgs { labels, default_label } };
    }
    case Instructions::call.value(): {
        // call function
        auto function_index = TRY(GenericIndexParser<FunctionIndex>::parse(stream));
        return Instruction { opcode, function_index };
    }
    case Instructions::call_indirect.value(): {
        // call_indirect type table
        auto type_index = TRY(GenericIndexParser<TypeIndex>::parse(stream));
        auto table_index = TRY(GenericIndexParser<TableIndex>::parse(stream));
        return Instruction { opcode, IndirectCallArgs { type_index, table_index } };
    }
    case Instructions::i32_load.value():
    case Instructions::i64_load.value():
    case Instructions::f32_load.value():
    case Instructions::f64_load.value():
    case Instructions::i32_load8_s.value():
    case Instructions::i32_load8_u.value():
    case Instructions::i32_load16_s.value():
    case Instructions::i32_load16_u.value():
    case Instructions::i64_load8_s.value():
    case Instructions::i64_load8_u.value():
    case Instructions::i64_load16_s.value():
    case Instructions::i64_load16_u.value():
    case Instructions::i64_load32_s.value():
    case Instructions::i64_load32_u.value():
    case Instructions::i32_store.value():
    case Instructions::i64_store.value():
    case Instructions::f32_store.value():
    case Instructions::f64_store.value():
    case Instructions::i32_store8.value():
    case Instructions::i32_store16.value():
    case Instructions::i64_store8.value():
    case Instructions::i64_store16.value():
    case Instructions::i64_store32.value(): {
        // op (align [multi-memory: memindex] offset)
        u32 align = TRY_READ(stream, LEB128<u32>, ParseError::InvalidInput);

        // Proposal "multi-memory", if bit 6 of alignment is set, then a memory index follows the alignment.
        auto memory_index = 0;
        if ((align & 0x40) != 0) {
            align &= ~0x40;
            memory_index = TRY_READ(stream, LEB128<u32>, ParseError::InvalidInput);
        }

        auto offset = TRY_READ(stream, LEB128<u32>, ParseError::InvalidInput);

        return Instruction { opcode, MemoryArgument { align, offset, MemoryIndex(memory_index) } };
    }
    case Instructions::local_get.value():
    case Instructions::local_set.value():
    case Instructions::local_tee.value(): {
        auto index = TRY(GenericIndexParser<LocalIndex>::parse(stream));
        return Instruction { opcode, index };
    }
    case Instructions::global_get.value():
    case Instructions::global_set.value(): {
        auto index = TRY(GenericIndexParser<GlobalIndex>::parse(stream));
        return Instruction { opcode, index };
    }
    case Instructions::memory_size.value():
    case Instructions::memory_grow.value(): {
        // op [multi-memory: memindex]|0x00
        auto memory_index = TRY_READ(stream, u8, ParseError::ExpectedKindTag);

        return Instruction { opcode, MemoryIndexArgument { MemoryIndex(memory_index) } };
    }
    case Instructions::i32_const.value(): {
        auto value = TRY_READ(stream, LEB128<i32>, ParseError::ExpectedSignedImmediate);

        return Instruction { opcode, value };
    }
    case Instructions::i64_const.value(): {
        // op literal
        auto value = TRY_READ(stream, LEB128<i64>, ParseError::ExpectedSignedImmediate);

        return Instruction { opcode, value };
    }
    case Instructions::f32_const.value(): {
        // op literal
        auto value = TRY_READ(stream, LittleEndian<u32>, ParseError::ExpectedFloatingImmediate);

        auto floating = bit_cast<float>(static_cast<u32>(value));
        return Instruction { opcode, floating };
    }
    case Instructions::f64_const.value(): {
        // op literal
        auto value = TRY_READ(stream, LittleEndian<u64>, ParseError::ExpectedFloatingImmediate);

        auto floating = bit_cast<double>(static_cast<u64>(value));
        return Instruction { opcode, floating };
    }
    case Instructions::table_get.value():
    case Instructions::table_set.value(): {
        auto index = TRY(GenericIndexParser<TableIndex>::parse(stream));
        return Instruction { opcode, index };
    }
    case Instructions::select_typed.value(): {
        auto types = TRY(parse_vector<ValueType>(stream));
        return Instruction { opcode, types };
    }
    case Instructions::ref_null.value(): {
        auto type = TRY(ValueType::parse(stream));
        if (!type.is_reference())
            return ParseError::InvalidType;

        return Instruction { opcode, type };
    }
    case Instructions::ref_func.value(): {
        auto index = TRY(GenericIndexParser<FunctionIndex>::parse(stream));
        return Instruction { opcode, index };
    }
    case Instructions::structured_end.value():
    case Instructions::structured_else.value():
    case Instructions::ref_is_null.value():
    case Instructions::unreachable.value():
    case Instructions::nop.value():
    case Instructions::return_.value():
    case Instructions::drop.value():
    case Instructions::select.value():
    case Instructions::i32_eqz.value():
    case Instructions::i32_eq.value():
    case Instructions::i32_ne.value():
    case Instructions::i32_lts.value():
    case Instructions::i32_ltu.value():
    case Instructions::i32_gts.value():
    case Instructions::i32_gtu.value():
    case Instructions::i32_les.value():
    case Instructions::i32_leu.value():
    case Instructions::i32_ges.value():
    case Instructions::i32_geu.value():
    case Instructions::i64_eqz.value():
    case Instructions::i64_eq.value():
    case Instructions::i64_ne.value():
    case Instructions::i64_lts.value():
    case Instructions::i64_ltu.value():
    case Instructions::i64_gts.value():
    case Instructions::i64_gtu.value():
    case Instructions::i64_les.value():
    case Instructions::i64_leu.value():
    case Instructions::i64_ges.value():
    case Instructions::i64_geu.value():
    case Instructions::f32_eq.value():
    case Instructions::f32_ne.value():
    case Instructions::f32_lt.value():
    case Instructions::f32_gt.value():
    case Instructions::f32_le.value():
    case Instructions::f32_ge.value():
    case Instructions::f64_eq.value():
    case Instructions::f64_ne.value():
    case Instructions::f64_lt.value():
    case Instructions::f64_gt.value():
    case Instructions::f64_le.value():
    case Instructions::f64_ge.value():
    case Instructions::i32_clz.value():
    case Instructions::i32_ctz.value():
    case Instructions::i32_popcnt.value():
    case Instructions::i32_add.value():
    case Instructions::i32_sub.value():
    case Instructions::i32_mul.value():
    case Instructions::i32_divs.value():
    case Instructions::i32_divu.value():
    case Instructions::i32_rems.value():
    case Instructions::i32_remu.value():
    case Instructions::i32_and.value():
    case Instructions::i32_or.value():
    case Instructions::i32_xor.value():
    case Instructions::i32_shl.value():
    case Instructions::i32_shrs.value():
    case Instructions::i32_shru.value():
    case Instructions::i32_rotl.value():
    case Instructions::i32_rotr.value():
    case Instructions::i64_clz.value():
    case Instructions::i64_ctz.value():
    case Instructions::i64_popcnt.value():
    case Instructions::i64_add.value():
    case Instructions::i64_sub.value():
    case Instructions::i64_mul.value():
    case Instructions::i64_divs.value():
    case Instructions::i64_divu.value():
    case Instructions::i64_rems.value():
    case Instructions::i64_remu.value():
    case Instructions::i64_and.value():
    case Instructions::i64_or.value():
    case Instructions::i64_xor.value():
    case Instructions::i64_shl.value():
    case Instructions::i64_shrs.value():
    case Instructions::i64_shru.value():
    case Instructions::i64_rotl.value():
    case Instructions::i64_rotr.value():
    case Instructions::f32_abs.value():
    case Instructions::f32_neg.value():
    case Instructions::f32_ceil.value():
    case Instructions::f32_floor.value():
    case Instructions::f32_trunc.value():
    case Instructions::f32_nearest.value():
    case Instructions::f32_sqrt.value():
    case Instructions::f32_add.value():
    case Instructions::f32_sub.value():
    case Instructions::f32_mul.value():
    case Instructions::f32_div.value():
    case Instructions::f32_min.value():
    case Instructions::f32_max.value():
    case Instructions::f32_copysign.value():
    case Instructions::f64_abs.value():
    case Instructions::f64_neg.value():
    case Instructions::f64_ceil.value():
    case Instructions::f64_floor.value():
    case Instructions::f64_trunc.value():
    case Instructions::f64_nearest.value():
    case Instructions::f64_sqrt.value():
    case Instructions::f64_add.value():
    case Instructions::f64_sub.value():
    case Instructions::f64_mul.value():
    case Instructions::f64_div.value():
    case Instructions::f64_min.value():
    case Instructions::f64_max.value():
    case Instructions::f64_copysign.value():
    case Instructions::i32_wrap_i64.value():
    case Instructions::i32_trunc_sf32.value():
    case Instructions::i32_trunc_uf32.value():
    case Instructions::i32_trunc_sf64.value():
    case Instructions::i32_trunc_uf64.value():
    case Instructions::i64_extend_si32.value():
    case Instructions::i64_extend_ui32.value():
    case Instructions::i64_trunc_sf32.value():
    case Instructions::i64_trunc_uf32.value():
    case Instructions::i64_trunc_sf64.value():
    case Instructions::i64_trunc_uf64.value():
    case Instructions::f32_convert_si32.value():
    case Instructions::f32_convert_ui32.value():
    case Instructions::f32_convert_si64.value():
    case Instructions::f32_convert_ui64.value():
    case Instructions::f32_demote_f64.value():
    case Instructions::f64_convert_si32.value():
    case Instructions::f64_convert_ui32.value():
    case Instructions::f64_convert_si64.value():
    case Instructions::f64_convert_ui64.value():
    case Instructions::f64_promote_f32.value():
    case Instructions::i32_reinterpret_f32.value():
    case Instructions::i64_reinterpret_f64.value():
    case Instructions::f32_reinterpret_i32.value():
    case Instructions::f64_reinterpret_i64.value():
    case Instructions::i32_extend8_s.value():
    case Instructions::i32_extend16_s.value():
    case Instructions::i64_extend8_s.value():
    case Instructions::i64_extend16_s.value():
    case Instructions::i64_extend32_s.value():
        return Instruction { opcode };
    case 0xfc:
    case 0xfd: {
        // These are multibyte instructions.
        auto selector = TRY_READ(stream, LEB128<u32>, ParseError::InvalidInput);
        OpCode full_opcode = static_cast<u64>(opcode.value()) << 56 | selector;

        switch (full_opcode.value()) {
        case Instructions::i32_trunc_sat_f32_s.value():
        case Instructions::i32_trunc_sat_f32_u.value():
        case Instructions::i32_trunc_sat_f64_s.value():
        case Instructions::i32_trunc_sat_f64_u.value():
        case Instructions::i64_trunc_sat_f32_s.value():
        case Instructions::i64_trunc_sat_f32_u.value():
        case Instructions::i64_trunc_sat_f64_s.value():
        case Instructions::i64_trunc_sat_f64_u.value():
            return Instruction { full_opcode };
        case Instructions::memory_init.value(): {
            auto index = TRY(GenericIndexParser<DataIndex>::parse(stream));

            // Proposal "multi-memory", literal 0x00 is replaced with a memory index.
            auto memory_index = TRY_READ(stream, u8, ParseError::InvalidInput);

            return Instruction { full_opcode, MemoryInitArgs { index, MemoryIndex(memory_index) } };
        }
        case Instructions::data_drop.value(): {
            auto index = TRY(GenericIndexParser<DataIndex>::parse(stream));
            return Instruction { full_opcode, index };
        }
        case Instructions::memory_copy.value(): {
            // Proposal "multi-memory", literal 0x00 is replaced with two memory indices, destination and source, respectively.
            MemoryIndex indices[] = { 0, 0 };

            for (size_t i = 0; i < 2; ++i) {
                auto memory_index = TRY_READ(stream, u8, ParseError::InvalidInput);
                indices[i] = memory_index;
            }
            return Instruction { full_opcode, MemoryCopyArgs { indices[1], indices[0] } };
        }
        case Instructions::memory_fill.value(): {
            // Proposal "multi-memory", literal 0x00 is replaced with a memory index.
            auto memory_index = TRY_READ(stream, u8, ParseError::InvalidInput);
            return Instruction { full_opcode, MemoryIndexArgument { MemoryIndex { memory_index } } };
        }
        case Instructions::table_init.value(): {
            auto element_index = TRY(GenericIndexParser<ElementIndex>::parse(stream));
            auto table_index = TRY(GenericIndexParser<TableIndex>::parse(stream));
            return Instruction { full_opcode, TableElementArgs { element_index, table_index } };
        }
        case Instructions::elem_drop.value(): {
            auto element_index = TRY(GenericIndexParser<ElementIndex>::parse(stream));
            return Instruction { full_opcode, element_index };
        }
        case Instructions::table_copy.value(): {
            auto lhs = TRY(GenericIndexParser<TableIndex>::parse(stream));
            auto rhs = TRY(GenericIndexParser<TableIndex>::parse(stream));
            return Instruction { full_opcode, TableTableArgs { lhs, rhs } };
        }
        case Instructions::table_grow.value():
        case Instructions::table_size.value():
        case Instructions::table_fill.value(): {
            auto index = TRY(GenericIndexParser<TableIndex>::parse(stream));
            return Instruction { full_opcode, index };
        }
        case Instructions::v128_load.value():
        case Instructions::v128_load8x8_s.value():
        case Instructions::v128_load8x8_u.value():
        case Instructions::v128_load16x4_s.value():
        case Instructions::v128_load16x4_u.value():
        case Instructions::v128_load32x2_s.value():
        case Instructions::v128_load32x2_u.value():
        case Instructions::v128_load8_splat.value():
        case Instructions::v128_load16_splat.value():
        case Instructions::v128_load32_splat.value():
        case Instructions::v128_load64_splat.value():
        case Instructions::v128_load32_zero.value():
        case Instructions::v128_load64_zero.value():
        case Instructions::v128_store.value(): {
            // op (align [multi-memory memindex] offset)
            u32 align = TRY_READ(stream, LEB128<u32>, ParseError::ExpectedIndex);

            // Proposal "multi-memory", if bit 6 of alignment is set, then a memory index follows the alignment.
            auto memory_index = 0;
            if ((align & 0x20) != 0) {
                align &= ~0x20;
                memory_index = TRY_READ(stream, LEB128<u32>, ParseError::InvalidInput);
            }

            auto offset = TRY_READ(stream, LEB128<u32>, ParseError::ExpectedIndex);

            return Instruction { full_opcode, MemoryArgument { align, offset, MemoryIndex(memory_index) } };
        }
        case Instructions::v128_load8_lane.value():
        case Instructions::v128_load16_lane.value():
        case Instructions::v128_load32_lane.value():
        case Instructions::v128_load64_lane.value():
        case Instructions::v128_store8_lane.value():
        case Instructions::v128_store16_lane.value():
        case Instructions::v128_store32_lane.value():
        case Instructions::v128_store64_lane.value(): {
            // op (align [multi-memory: memindex] offset) (index)
            u32 align = TRY_READ(stream, LEB128<u32>, ParseError::ExpectedIndex);

            // Proposal "multi-memory", if bit 6 of alignment is set, then a memory index follows the alignment.
            auto memory_index = 0;
            if ((align & 0x20) != 0) {
                align &= ~0x20;
                memory_index = TRY_READ(stream, LEB128<u32>, ParseError::InvalidInput);
            }

            auto offset = TRY_READ(stream, LEB128<u32>, ParseError::ExpectedIndex);
            auto index = TRY_READ(stream, u8, ParseError::InvalidInput);

            return Instruction { full_opcode, MemoryAndLaneArgument { { align, offset, MemoryIndex(memory_index) }, index } };
        }
        case Instructions::v128_const.value(): {
            // op (literal:16)
            auto value = TRY_READ(stream, LittleEndian<u128>, ParseError::InvalidImmediate);
            return Instruction { full_opcode, value };
        }
        case Instructions::i8x16_shuffle.value(): {
            // op 16x(lane)
            u8 lanes[16];
            for (size_t i = 0; i < 16; ++i) {
                auto value = TRY_READ(stream, u8, ParseError::InvalidInput);
                lanes[i] = value;
            }
            return Instruction { full_opcode, ShuffleArgument(lanes) };
        }
        case Instructions::i8x16_extract_lane_s.value():
        case Instructions::i8x16_extract_lane_u.value():
        case Instructions::i8x16_replace_lane.value():
        case Instructions::i16x8_extract_lane_s.value():
        case Instructions::i16x8_extract_lane_u.value():
        case Instructions::i16x8_replace_lane.value():
        case Instructions::i32x4_extract_lane.value():
        case Instructions::i32x4_replace_lane.value():
        case Instructions::i64x2_extract_lane.value():
        case Instructions::i64x2_replace_lane.value():
        case Instructions::f32x4_extract_lane.value():
        case Instructions::f32x4_replace_lane.value():
        case Instructions::f64x2_extract_lane.value():
        case Instructions::f64x2_replace_lane.value(): {
            // op (lane)
            auto lane = TRY_READ(stream, u8, ParseError::InvalidInput);
            return Instruction { full_opcode, LaneIndex { lane } };
        }
        case Instructions::i8x16_swizzle.value():
        case Instructions::i8x16_splat.value():
        case Instructions::i16x8_splat.value():
        case Instructions::i32x4_splat.value():
        case Instructions::i64x2_splat.value():
        case Instructions::f32x4_splat.value():
        case Instructions::f64x2_splat.value():
        case Instructions::i8x16_eq.value():
        case Instructions::i8x16_ne.value():
        case Instructions::i8x16_lt_s.value():
        case Instructions::i8x16_lt_u.value():
        case Instructions::i8x16_gt_s.value():
        case Instructions::i8x16_gt_u.value():
        case Instructions::i8x16_le_s.value():
        case Instructions::i8x16_le_u.value():
        case Instructions::i8x16_ge_s.value():
        case Instructions::i8x16_ge_u.value():
        case Instructions::i16x8_eq.value():
        case Instructions::i16x8_ne.value():
        case Instructions::i16x8_lt_s.value():
        case Instructions::i16x8_lt_u.value():
        case Instructions::i16x8_gt_s.value():
        case Instructions::i16x8_gt_u.value():
        case Instructions::i16x8_le_s.value():
        case Instructions::i16x8_le_u.value():
        case Instructions::i16x8_ge_s.value():
        case Instructions::i16x8_ge_u.value():
        case Instructions::i32x4_eq.value():
        case Instructions::i32x4_ne.value():
        case Instructions::i32x4_lt_s.value():
        case Instructions::i32x4_lt_u.value():
        case Instructions::i32x4_gt_s.value():
        case Instructions::i32x4_gt_u.value():
        case Instructions::i32x4_le_s.value():
        case Instructions::i32x4_le_u.value():
        case Instructions::i32x4_ge_s.value():
        case Instructions::i32x4_ge_u.value():
        case Instructions::f32x4_eq.value():
        case Instructions::f32x4_ne.value():
        case Instructions::f32x4_lt.value():
        case Instructions::f32x4_gt.value():
        case Instructions::f32x4_le.value():
        case Instructions::f32x4_ge.value():
        case Instructions::f64x2_eq.value():
        case Instructions::f64x2_ne.value():
        case Instructions::f64x2_lt.value():
        case Instructions::f64x2_gt.value():
        case Instructions::f64x2_le.value():
        case Instructions::f64x2_ge.value():
        case Instructions::v128_not.value():
        case Instructions::v128_and.value():
        case Instructions::v128_andnot.value():
        case Instructions::v128_or.value():
        case Instructions::v128_xor.value():
        case Instructions::v128_bitselect.value():
        case Instructions::v128_any_true.value():
        case Instructions::f32x4_demote_f64x2_zero.value():
        case Instructions::f64x2_promote_low_f32x4.value():
        case Instructions::i8x16_abs.value():
        case Instructions::i8x16_neg.value():
        case Instructions::i8x16_popcnt.value():
        case Instructions::i8x16_all_true.value():
        case Instructions::i8x16_bitmask.value():
        case Instructions::i8x16_narrow_i16x8_s.value():
        case Instructions::i8x16_narrow_i16x8_u.value():
        case Instructions::f32x4_ceil.value():
        case Instructions::f32x4_floor.value():
        case Instructions::f32x4_trunc.value():
        case Instructions::f32x4_nearest.value():
        case Instructions::i8x16_shl.value():
        case Instructions::i8x16_shr_s.value():
        case Instructions::i8x16_shr_u.value():
        case Instructions::i8x16_add.value():
        case Instructions::i8x16_add_sat_s.value():
        case Instructions::i8x16_add_sat_u.value():
        case Instructions::i8x16_sub.value():
        case Instructions::i8x16_sub_sat_s.value():
        case Instructions::i8x16_sub_sat_u.value():
        case Instructions::f64x2_ceil.value():
        case Instructions::f64x2_floor.value():
        case Instructions::i8x16_min_s.value():
        case Instructions::i8x16_min_u.value():
        case Instructions::i8x16_max_s.value():
        case Instructions::i8x16_max_u.value():
        case Instructions::f64x2_trunc.value():
        case Instructions::i8x16_avgr_u.value():
        case Instructions::i16x8_extadd_pairwise_i8x16_s.value():
        case Instructions::i16x8_extadd_pairwise_i8x16_u.value():
        case Instructions::i32x4_extadd_pairwise_i16x8_s.value():
        case Instructions::i32x4_extadd_pairwise_i16x8_u.value():
        case Instructions::i16x8_abs.value():
        case Instructions::i16x8_neg.value():
        case Instructions::i16x8_q15mulr_sat_s.value():
        case Instructions::i16x8_all_true.value():
        case Instructions::i16x8_bitmask.value():
        case Instructions::i16x8_narrow_i32x4_s.value():
        case Instructions::i16x8_narrow_i32x4_u.value():
        case Instructions::i16x8_extend_low_i8x16_s.value():
        case Instructions::i16x8_extend_high_i8x16_s.value():
        case Instructions::i16x8_extend_low_i8x16_u.value():
        case Instructions::i16x8_extend_high_i8x16_u.value():
        case Instructions::i16x8_shl.value():
        case Instructions::i16x8_shr_s.value():
        case Instructions::i16x8_shr_u.value():
        case Instructions::i16x8_add.value():
        case Instructions::i16x8_add_sat_s.value():
        case Instructions::i16x8_add_sat_u.value():
        case Instructions::i16x8_sub.value():
        case Instructions::i16x8_sub_sat_s.value():
        case Instructions::i16x8_sub_sat_u.value():
        case Instructions::f64x2_nearest.value():
        case Instructions::i16x8_mul.value():
        case Instructions::i16x8_min_s.value():
        case Instructions::i16x8_min_u.value():
        case Instructions::i16x8_max_s.value():
        case Instructions::i16x8_max_u.value():
        case Instructions::i16x8_avgr_u.value():
        case Instructions::i16x8_extmul_low_i8x16_s.value():
        case Instructions::i16x8_extmul_high_i8x16_s.value():
        case Instructions::i16x8_extmul_low_i8x16_u.value():
        case Instructions::i16x8_extmul_high_i8x16_u.value():
        case Instructions::i32x4_abs.value():
        case Instructions::i32x4_neg.value():
        case Instructions::i32x4_all_true.value():
        case Instructions::i32x4_bitmask.value():
        case Instructions::i32x4_extend_low_i16x8_s.value():
        case Instructions::i32x4_extend_high_i16x8_s.value():
        case Instructions::i32x4_extend_low_i16x8_u.value():
        case Instructions::i32x4_extend_high_i16x8_u.value():
        case Instructions::i32x4_shl.value():
        case Instructions::i32x4_shr_s.value():
        case Instructions::i32x4_shr_u.value():
        case Instructions::i32x4_add.value():
        case Instructions::i32x4_sub.value():
        case Instructions::i32x4_mul.value():
        case Instructions::i32x4_min_s.value():
        case Instructions::i32x4_min_u.value():
        case Instructions::i32x4_max_s.value():
        case Instructions::i32x4_max_u.value():
        case Instructions::i32x4_dot_i16x8_s.value():
        case Instructions::i32x4_extmul_low_i16x8_s.value():
        case Instructions::i32x4_extmul_high_i16x8_s.value():
        case Instructions::i32x4_extmul_low_i16x8_u.value():
        case Instructions::i32x4_extmul_high_i16x8_u.value():
        case Instructions::i64x2_abs.value():
        case Instructions::i64x2_neg.value():
        case Instructions::i64x2_all_true.value():
        case Instructions::i64x2_bitmask.value():
        case Instructions::i64x2_extend_low_i32x4_s.value():
        case Instructions::i64x2_extend_high_i32x4_s.value():
        case Instructions::i64x2_extend_low_i32x4_u.value():
        case Instructions::i64x2_extend_high_i32x4_u.value():
        case Instructions::i64x2_shl.value():
        case Instructions::i64x2_shr_s.value():
        case Instructions::i64x2_shr_u.value():
        case Instructions::i64x2_add.value():
        case Instructions::i64x2_sub.value():
        case Instructions::i64x2_mul.value():
        case Instructions::i64x2_eq.value():
        case Instructions::i64x2_ne.value():
        case Instructions::i64x2_lt_s.value():
        case Instructions::i64x2_gt_s.value():
        case Instructions::i64x2_le_s.value():
        case Instructions::i64x2_ge_s.value():
        case Instructions::i64x2_extmul_low_i32x4_s.value():
        case Instructions::i64x2_extmul_high_i32x4_s.value():
        case Instructions::i64x2_extmul_low_i32x4_u.value():
        case Instructions::i64x2_extmul_high_i32x4_u.value():
        case Instructions::f32x4_abs.value():
        case Instructions::f32x4_neg.value():
        case Instructions::f32x4_sqrt.value():
        case Instructions::f32x4_add.value():
        case Instructions::f32x4_sub.value():
        case Instructions::f32x4_mul.value():
        case Instructions::f32x4_div.value():
        case Instructions::f32x4_min.value():
        case Instructions::f32x4_max.value():
        case Instructions::f32x4_pmin.value():
        case Instructions::f32x4_pmax.value():
        case Instructions::f64x2_abs.value():
        case Instructions::f64x2_neg.value():
        case Instructions::f64x2_sqrt.value():
        case Instructions::f64x2_add.value():
        case Instructions::f64x2_sub.value():
        case Instructions::f64x2_mul.value():
        case Instructions::f64x2_div.value():
        case Instructions::f64x2_min.value():
        case Instructions::f64x2_max.value():
        case Instructions::f64x2_pmin.value():
        case Instructions::f64x2_pmax.value():
        case Instructions::i32x4_trunc_sat_f32x4_s.value():
        case Instructions::i32x4_trunc_sat_f32x4_u.value():
        case Instructions::f32x4_convert_i32x4_s.value():
        case Instructions::f32x4_convert_i32x4_u.value():
        case Instructions::i32x4_trunc_sat_f64x2_s_zero.value():
        case Instructions::i32x4_trunc_sat_f64x2_u_zero.value():
        case Instructions::f64x2_convert_low_i32x4_s.value():
        case Instructions::f64x2_convert_low_i32x4_u.value():
            // op
            return Instruction { full_opcode };
        default:
            return ParseError::UnknownInstruction;
        }
    }
    }
    return ParseError::UnknownInstruction;
}

ParseResult<CustomSection> CustomSection::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("CustomSection"sv);
    auto name = TRY(parse_name(stream));

    ByteBuffer data_buffer;
    if (data_buffer.try_resize(64).is_error())
        return ParseError::OutOfMemory;

    while (!stream.is_eof()) {
        char buf[16];
        auto span_or_error = stream.read_some({ buf, 16 });
        if (span_or_error.is_error())
            break;
        auto size = span_or_error.release_value().size();
        if (size == 0)
            break;
        if (data_buffer.try_append(buf, size).is_error())
            return ParseError::HugeAllocationRequested;
    }

    return CustomSection(name, move(data_buffer));
}

ParseResult<TypeSection> TypeSection::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("TypeSection"sv);
    auto types = TRY(parse_vector<FunctionType>(stream));
    return TypeSection { types };
}

ParseResult<ImportSection::Import> ImportSection::Import::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("Import"sv);
    auto module = TRY(parse_name(stream));
    auto name = TRY(parse_name(stream));
    auto tag = TRY_READ(stream, u8, ParseError::ExpectedKindTag);

    switch (tag) {
    case Constants::extern_function_tag: {
        auto index = TRY(GenericIndexParser<TypeIndex>::parse(stream));
        return Import { module, name, index };
    }
    case Constants::extern_table_tag:
        return parse_with_type<TableType>(stream, module, name);
    case Constants::extern_memory_tag:
        return parse_with_type<MemoryType>(stream, module, name);
    case Constants::extern_global_tag:
        return parse_with_type<GlobalType>(stream, module, name);
    default:
        return ParseError::InvalidTag;
    }
}

ParseResult<ImportSection> ImportSection::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("ImportSection"sv);
    auto imports = TRY(parse_vector<Import>(stream));
    return ImportSection { imports };
}

ParseResult<FunctionSection> FunctionSection::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("FunctionSection"sv);
    auto indices = TRY(parse_vector<u32>(stream));

    Vector<TypeIndex> typed_indices;
    typed_indices.ensure_capacity(indices.size());
    for (auto entry : indices)
        typed_indices.append(entry);

    return FunctionSection { move(typed_indices) };
}

ParseResult<TableSection::Table> TableSection::Table::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("Table"sv);
    auto type = TRY(TableType::parse(stream));
    return Table { type };
}

ParseResult<TableSection> TableSection::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("TableSection"sv);
    auto tables = TRY(parse_vector<Table>(stream));
    return TableSection { tables };
}

ParseResult<MemorySection::Memory> MemorySection::Memory::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("Memory"sv);
    auto type = TRY(MemoryType::parse(stream));
    return Memory { type };
}

ParseResult<MemorySection> MemorySection::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("MemorySection"sv);
    auto memories = TRY(parse_vector<Memory>(stream));
    return MemorySection { memories };
}

ParseResult<Expression> Expression::parse(Stream& stream, Optional<size_t> size_hint)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("Expression"sv);

    InstructionPointer ip { 0 };
    Vector<InstructionPointer> stack;
    Vector<Instruction> instructions;
    if (size_hint.has_value())
        instructions.ensure_capacity(size_hint.release_value());
    while (true) {
        auto instruction = TRY(Instruction::parse(stream));
        switch (instruction.opcode().value()) {
        case Instructions::block.value():
        case Instructions::loop.value():
        case Instructions::if_.value():
            stack.append(ip);
            break;
        case Instructions::structured_end.value(): {
            if (stack.is_empty())
                return Expression { move(instructions) };
            auto entry = stack.take_last();
            auto& args = instructions[entry.value()].arguments().get<Instruction::StructuredInstructionArgs>();
            // Patch the end_ip of the last structured instruction
            args.end_ip = ip + (args.else_ip.has_value() ? 1 : 0);
            break;
        }
        case Instructions::structured_else.value(): {
            if (stack.is_empty())
                return ParseError::UnknownInstruction;
            auto entry = stack.last();
            auto& args = instructions[entry.value()].arguments().get<Instruction::StructuredInstructionArgs>();
            args.else_ip = ip + 1;
            break;
        }
        }
        instructions.append(move(instruction));
        ++ip;
    }

    return Expression { move(instructions) };
}

ParseResult<GlobalSection::Global> GlobalSection::Global::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("Global"sv);
    auto type = TRY(GlobalType::parse(stream));
    auto exprs = TRY(Expression::parse(stream));
    return Global { type, exprs };
}

ParseResult<GlobalSection> GlobalSection::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("GlobalSection"sv);
    auto result = TRY(parse_vector<Global>(stream));
    return GlobalSection { result };
}

ParseResult<ExportSection::Export> ExportSection::Export::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("Export"sv);
    auto name = TRY(parse_name(stream));
    auto tag = TRY_READ(stream, u8, ParseError::ExpectedKindTag);

    auto index = TRY_READ(stream, LEB128<u32>, ParseError::ExpectedIndex);

    switch (tag) {
    case Constants::extern_function_tag:
        return Export { name, ExportDesc { FunctionIndex { index } } };
    case Constants::extern_table_tag:
        return Export { name, ExportDesc { TableIndex { index } } };
    case Constants::extern_memory_tag:
        return Export { name, ExportDesc { MemoryIndex { index } } };
    case Constants::extern_global_tag:
        return Export { name, ExportDesc { GlobalIndex { index } } };
    default:
        return ParseError::InvalidTag;
    }
}

ParseResult<ExportSection> ExportSection::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("ExportSection"sv);
    auto result = TRY(parse_vector<Export>(stream));
    return ExportSection { result };
}

ParseResult<StartSection::StartFunction> StartSection::StartFunction::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("StartFunction"sv);
    auto index = TRY(GenericIndexParser<FunctionIndex>::parse(stream));
    return StartFunction { index };
}

ParseResult<StartSection> StartSection::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("StartSection"sv);
    auto result = TRY(StartFunction::parse(stream));
    return StartSection { result };
}

ParseResult<ElementSection::Element> ElementSection::Element::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("Element"sv);
    auto tag = TRY_READ(stream, LEB128<u32>, ParseError::ExpectedKindTag);

    if (tag > 0x07)
        return ParseError::InvalidTag;

    auto has_passive = (tag & 0x01) != 0;
    auto has_explicit_index = (tag & 0x02) != 0;
    auto has_exprs = (tag & 0x04) != 0;

    Variant<Active, Passive, Declarative> mode = Passive {};
    if (has_passive) {
        if (has_explicit_index) {
            mode = Declarative {};
        } else {
            mode = Passive {};
        }
    } else {
        TableIndex table_index = 0;
        if (has_explicit_index)
            table_index = TRY(GenericIndexParser<TableIndex>::parse(stream));
        auto expression = TRY(Expression::parse(stream));
        mode = Active { table_index, expression };
    }

    auto type = ValueType(ValueType::FunctionReference);
    if (has_passive || has_explicit_index) {
        if (has_exprs) {
            type = TRY(ValueType::parse(stream));
        } else {
            auto extern_ = TRY_READ(stream, u8, ParseError::InvalidType);
            // Make sure that this is a function, as it's technically only the
            // allowed one.
            if (extern_ != 0x00) {
                return ParseError::InvalidType;
            }
            type = ValueType(ValueType::FunctionReference);
        }
    }

    Vector<Expression> items;
    if (!has_exprs) {
        auto indices = TRY(parse_vector<GenericIndexParser<FunctionIndex>>(stream));
        for (auto& index : indices) {
            Vector<Instruction> instructions { Instruction(Instructions::ref_func, index) };
            items.empend(move(instructions));
        }
    } else {
        items = TRY(parse_vector<Expression>(stream));
    }

    return Element { type, move(items), move(mode) };
}

ParseResult<ElementSection> ElementSection::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("ElementSection"sv);
    auto result = TRY(parse_vector<Element>(stream));
    return ElementSection { result };
}

ParseResult<Locals> Locals::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("Locals"sv);
    auto count = TRY_READ(stream, LEB128<u32>, ParseError::InvalidSize);

    if (count > Constants::max_allowed_function_locals_per_type)
        return ParseError::HugeAllocationRequested;

    auto type = TRY(ValueType::parse(stream));

    return Locals { count, type };
}

ParseResult<CodeSection::Func> CodeSection::Func::parse(Stream& stream, size_t size_hint)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("Func"sv);
    auto locals = TRY(parse_vector<Locals>(stream));
    auto body = TRY(Expression::parse(stream, size_hint));
    return Func { move(locals), move(body) };
}

ParseResult<CodeSection::Code> CodeSection::Code::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("Code"sv);
    auto size = TRY_READ(stream, LEB128<u32>, ParseError::InvalidSize);

    // Emprically, if there are `size` bytes to be read, then there's around
    // `size / 2` instructions, so we pass that as our size hint.
    auto func = TRY(Func::parse(stream, size / 2));

    return Code { size, move(func) };
}

ParseResult<CodeSection> CodeSection::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("CodeSection"sv);
    auto result = TRY(parse_vector<Code>(stream));
    return CodeSection { move(result) };
}

ParseResult<DataSection::Data> DataSection::Data::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("Data"sv);
    auto tag = TRY_READ(stream, LEB128<u32>, ParseError::ExpectedKindTag);

    if (tag > 0x02)
        return ParseError::InvalidTag;

    if (tag == 0x00) {
        auto expr = TRY(Expression::parse(stream));
        auto init = TRY(parse_vector<u8>(stream));
        return Data { Active { init, { 0 }, expr } };
    }
    if (tag == 0x01) {
        auto init = TRY(parse_vector<u8>(stream));
        return Data { Passive { init } };
    }
    if (tag == 0x02) {
        auto index = TRY(GenericIndexParser<MemoryIndex>::parse(stream));
        auto expr = TRY(Expression::parse(stream));
        auto init = TRY(parse_vector<u8>(stream));
        return Data { Active { init, index, expr } };
    }
    VERIFY_NOT_REACHED();
}

ParseResult<DataSection> DataSection::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("DataSection"sv);
    auto data = TRY(parse_vector<Data>(stream));
    return DataSection { data };
}

ParseResult<DataCountSection> DataCountSection::parse([[maybe_unused]] Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("DataCountSection"sv);
    auto value_or_error = stream.read_value<LEB128<u32>>();
    if (value_or_error.is_error()) {
        if (stream.is_eof()) {
            // The section simply didn't contain anything.
            return DataCountSection { {} };
        }
        return ParseError::ExpectedSize;
    }
    u32 value = value_or_error.release_value();

    return DataCountSection { value };
}

ParseResult<SectionId> SectionId::parse(Stream& stream)
{
    u8 id = TRY_READ(stream, u8, ParseError::ExpectedIndex);
    switch (id) {
    case 0x00:
        return SectionId(SectionIdKind::Custom);
    case 0x01:
        return SectionId(SectionIdKind::Type);
    case 0x02:
        return SectionId(SectionIdKind::Import);
    case 0x03:
        return SectionId(SectionIdKind::Function);
    case 0x04:
        return SectionId(SectionIdKind::Table);
    case 0x05:
        return SectionId(SectionIdKind::Memory);
    case 0x06:
        return SectionId(SectionIdKind::Global);
    case 0x07:
        return SectionId(SectionIdKind::Export);
    case 0x08:
        return SectionId(SectionIdKind::Start);
    case 0x09:
        return SectionId(SectionIdKind::Element);
    case 0x0a:
        return SectionId(SectionIdKind::Code);
    case 0x0b:
        return SectionId(SectionIdKind::Data);
    case 0x0c:
        return SectionId(SectionIdKind::DataCount);
    default:
        return ParseError::InvalidIndex;
    }
}

ParseResult<NonnullRefPtr<Module>> Module::parse(Stream& stream)
{
    ScopeLogger<WASM_BINPARSER_DEBUG> logger("Module"sv);
    u8 buf[4];
    if (stream.read_until_filled({ buf, 4 }).is_error())
        return with_eof_check(stream, ParseError::InvalidInput);
    if (Bytes { buf, 4 } != wasm_magic.span())
        return with_eof_check(stream, ParseError::InvalidModuleMagic);

    if (stream.read_until_filled({ buf, 4 }).is_error())
        return with_eof_check(stream, ParseError::InvalidInput);
    if (Bytes { buf, 4 } != wasm_version.span())
        return with_eof_check(stream, ParseError::InvalidModuleVersion);

    auto last_section_id = SectionId::SectionIdKind::Custom;
    auto module_ptr = make_ref_counted<Module>();
    auto& module = *module_ptr;

    while (!stream.is_eof()) {
        auto section_id = TRY(SectionId::parse(stream));
        size_t section_size = TRY_READ(stream, LEB128<u32>, ParseError::ExpectedSize);
        auto section_stream = ConstrainedStream { MaybeOwned<Stream>(stream), section_size };

        if (section_id.kind() != SectionId::SectionIdKind::Custom && section_id.kind() == last_section_id)
            return ParseError::DuplicateSection;

        switch (section_id.kind()) {
        case SectionId::SectionIdKind::Custom:
            module.custom_sections().append(TRY(CustomSection::parse(section_stream)));
            break;
        case SectionId::SectionIdKind::Type:
            module.type_section() = TRY(TypeSection::parse(section_stream));
            break;
        case SectionId::SectionIdKind::Import:
            module.import_section() = TRY(ImportSection::parse(section_stream));
            break;
        case SectionId::SectionIdKind::Function:
            module.function_section() = TRY(FunctionSection::parse(section_stream));
            break;
        case SectionId::SectionIdKind::Table:
            module.table_section() = TRY(TableSection::parse(section_stream));
            break;
        case SectionId::SectionIdKind::Memory:
            module.memory_section() = TRY(MemorySection::parse(section_stream));
            break;
        case SectionId::SectionIdKind::Global:
            module.global_section() = TRY(GlobalSection::parse(section_stream));
            break;
        case SectionId::SectionIdKind::Export:
            module.export_section() = TRY(ExportSection::parse(section_stream));
            break;
        case SectionId::SectionIdKind::Start:
            module.start_section() = TRY(StartSection::parse(section_stream));
            break;
        case SectionId::SectionIdKind::Element:
            module.element_section() = TRY(ElementSection::parse(section_stream));
            break;
        case SectionId::SectionIdKind::Code:
            module.code_section() = TRY(CodeSection::parse(section_stream));
            break;
        case SectionId::SectionIdKind::Data:
            module.data_section() = TRY(DataSection::parse(section_stream));
            break;
        case SectionId::SectionIdKind::DataCount:
            module.data_count_section() = TRY(DataCountSection::parse(section_stream));
            break;
        default:
            return ParseError::InvalidIndex;
        }
        if (section_id.kind() != SectionId::SectionIdKind::Custom) {
            if (section_id.kind() < last_section_id)
                return ParseError::SectionOutOfOrder;
            last_section_id = section_id.kind();
        }
        if (section_stream.remaining() != 0)
            return ParseError::SectionSizeMismatch;
    }

    return module_ptr;
}

ByteString parse_error_to_byte_string(ParseError error)
{
    switch (error) {
    case ParseError::UnexpectedEof:
        return "Unexpected end-of-file";
    case ParseError::ExpectedIndex:
        return "Expected a valid index value";
    case ParseError::ExpectedKindTag:
        return "Expected a valid kind tag";
    case ParseError::ExpectedSize:
        return "Expected a valid LEB128-encoded size";
    case ParseError::ExpectedValueOrTerminator:
        return "Expected either a terminator or a value";
    case ParseError::InvalidIndex:
        return "An index parsed was semantically invalid";
    case ParseError::InvalidInput:
        return "Input data contained invalid bytes";
    case ParseError::InvalidModuleMagic:
        return "Incorrect module magic (did not match \\0asm)";
    case ParseError::InvalidModuleVersion:
        return "Incorrect module version";
    case ParseError::InvalidSize:
        return "A parsed size did not make sense in context";
    case ParseError::InvalidTag:
        return "A parsed tag did not make sense in context";
    case ParseError::InvalidType:
        return "A parsed type did not make sense in context";
    case ParseError::HugeAllocationRequested:
        return "Parsing caused an attempt to allocate a very big chunk of memory, likely malformed data";
    case ParseError::OutOfMemory:
        return "The parser hit an OOM condition";
    case ParseError::ExpectedFloatingImmediate:
        return "Expected a floating point immediate";
    case ParseError::ExpectedSignedImmediate:
        return "Expected a signed integer immediate";
    case ParseError::InvalidImmediate:
        return "A parsed instruction immediate was invalid for the instruction it was used for";
    case ParseError::SectionSizeMismatch:
        return "A parsed section did not fulfill its expected size";
    case ParseError::InvalidUtf8:
        return "A parsed string was not valid UTF-8";
    case ParseError::UnknownInstruction:
        return "A parsed instruction was not known to this parser";
    case ParseError::DuplicateSection:
        return "Two sections of the same type were encountered";
    case ParseError::SectionOutOfOrder:
        return "A section encountered was not in the correct ordering";
    }
    return "Unknown error";
}
}
