/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/ByteString.h>
#include <AK/DistinctNumeric.h>
#include <AK/LEB128.h>
#include <AK/Result.h>
#include <AK/String.h>
#include <AK/UFixedBigInt.h>
#include <AK/Variant.h>
#include <AK/WeakPtr.h>
#include <LibWasm/Constants.h>
#include <LibWasm/Export.h>
#include <LibWasm/Forward.h>
#include <LibWasm/Opcode.h>

namespace Wasm {

class Module;

template<size_t M>
using NativeIntegralType = Conditional<M == 8, u8, Conditional<M == 16, u16, Conditional<M == 32, u32, Conditional<M == 64, u64, void>>>>;

template<size_t M>
using NativeFloatingType = Conditional<M == 32, f32, Conditional<M == 64, f64, void>>;

template<size_t M, size_t N, template<typename> typename SetSign, typename ElementType = SetSign<NativeIntegralType<M>>>
using NativeVectorType __attribute__((vector_size(N * sizeof(ElementType)))) = ElementType;

template<size_t M, size_t N, typename ElementType = NativeFloatingType<M>>
using NativeFloatingVectorType __attribute__((vector_size(N * sizeof(ElementType)))) = ElementType;

template<typename T, template<typename> typename SetSign>
using Native128ByteVectorOf = NativeVectorType<sizeof(T) * 8, 16 / sizeof(T), SetSign, T>;

enum class ParseError {
    UnexpectedEof,
    UnknownInstruction,
    ExpectedFloatingImmediate,
    ExpectedIndex,
    ExpectedKindTag,
    ExpectedSignedImmediate,
    ExpectedSize,
    ExpectedValueOrTerminator,
    InvalidImmediate,
    InvalidIndex,
    InvalidInput,
    InvalidModuleMagic,
    InvalidModuleVersion,
    InvalidSize,
    InvalidTag,
    InvalidType,
    HugeAllocationRequested,
    OutOfMemory,
    SectionSizeMismatch,
    InvalidUtf8,
    DuplicateSection,
    SectionOutOfOrder,
};

WASM_API ByteString parse_error_to_byte_string(ParseError);

template<typename T>
using ParseResult = ErrorOr<T, ParseError>;

AK_TYPEDEF_DISTINCT_ORDERED_ID(u32, TypeIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u32, FunctionIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u32, TableIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u32, ElementIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u32, MemoryIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u32, TagIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u32, LocalIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u32, GlobalIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u32, LabelIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u32, DataIndex);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u32, InstructionPointer, Arithmetic, Comparison, Flags, Increment);

constexpr static inline auto LocalArgumentMarker = static_cast<LocalIndex::Type>(1) << (sizeof(LocalIndex::Type) * 8 - 1);

ParseError with_eof_check(Stream const& stream, ParseError error_if_not_eof);

template<typename T>
struct GenericIndexParser {
    static ParseResult<T> parse(Stream& stream)
    {
        auto value_or_error = stream.read_value<LEB128<u32>>();
        if (value_or_error.is_error())
            return with_eof_check(stream, ParseError::ExpectedIndex);
        u32 value = value_or_error.release_value();
        return T { value };
    }
};

class ReconsumableStream : public Stream {
public:
    explicit ReconsumableStream(Stream& stream)
        : m_stream(stream)
    {
    }

    void unread(ReadonlyBytes data) { m_buffer.append(data.data(), data.size()); }

private:
    virtual ErrorOr<Bytes> read_some(Bytes bytes) override
    {
        auto original_bytes = bytes;

        size_t bytes_read_from_buffer = 0;
        if (!m_buffer.is_empty()) {
            auto read_size = min(bytes.size(), m_buffer.size());
            m_buffer.span().slice(0, read_size).copy_to(bytes);
            bytes = bytes.slice(read_size);
            for (size_t i = 0; i < read_size; ++i)
                m_buffer.take_first();
            bytes_read_from_buffer = read_size;
        }

        return original_bytes.trim(TRY(m_stream.read_some(bytes)).size() + bytes_read_from_buffer);
    }

    virtual bool is_eof() const override
    {
        return m_buffer.is_empty() && m_stream.is_eof();
    }

    virtual ErrorOr<void> discard(size_t count) override
    {
        size_t bytes_discarded_from_buffer = 0;
        if (!m_buffer.is_empty()) {
            auto read_size = min(count, m_buffer.size());
            for (size_t i = 0; i < read_size; ++i)
                m_buffer.take_first();
            bytes_discarded_from_buffer = read_size;
        }

        return m_stream.discard(count - bytes_discarded_from_buffer);
    }

    virtual ErrorOr<size_t> write_some(ReadonlyBytes) override
    {
        return Error::from_errno(EBADF);
    }

    virtual bool is_open() const override
    {
        return m_stream.is_open();
    }

    virtual void close() override
    {
        m_stream.close();
    }

    Stream& m_stream;
    Vector<u8, 8> m_buffer;
};

// https://webassembly.github.io/spec/core/bikeshed/#value-types%E2%91%A2
class ValueType {
public:
    enum Kind {
        I32,
        I64,
        F32,
        F64,
        V128,
        FunctionReference,
        ExternReference,
        ExceptionReference,
        TypeUseReference,
        UnsupportedHeapReference, // Stub for wasm-gc proposal's reference types.
    };

    explicit ValueType(Kind kind)
        : m_kind(kind)
    {
    }

    template<typename T>
    explicit ValueType(Kind kind, T argument)
        : m_kind(kind)
        , m_argument(move(argument))
    {
    }

    bool operator==(ValueType const&) const = default;

    auto is_reference() const { return m_kind == ExternReference || m_kind == FunctionReference || m_kind == TypeUseReference || m_kind == UnsupportedHeapReference; }
    auto is_vector() const { return m_kind == V128; }
    auto is_numeric() const { return !is_reference() && !is_vector(); }
    auto is_typeuse() const { return m_kind == TypeUseReference; }
    auto kind() const { return m_kind; }

    auto unsafe_typeindex() const { return m_argument.unsafe_get<TypeIndex>(); }

    static ParseResult<ValueType> parse(Stream& stream);

    ByteString kind_name() const
    {
        switch (m_kind) {
        case I32:
            return "i32";
        case I64:
            return "i64";
        case F32:
            return "f32";
        case F64:
            return "f64";
        case V128:
            return "v128";
        case FunctionReference:
            return "funcref";
        case ExternReference:
            return "externref";
        case ExceptionReference:
            return "exnref";
        case TypeUseReference:
            return ByteString::formatted("ref null {}", unsafe_typeindex().value());
        case UnsupportedHeapReference:
            return "todo.heapref";
        }
        VERIFY_NOT_REACHED();
    }

private:
    Kind m_kind;
    Variant<TypeIndex, Empty> m_argument;
};

// https://webassembly.github.io/spec/core/bikeshed/#result-types%E2%91%A2
class ResultType {
public:
    explicit ResultType(Vector<ValueType> types)
        : m_types(move(types))
    {
    }

    auto const& types() const { return m_types; }

    static ParseResult<ResultType> parse(ConstrainedStream& stream);

private:
    Vector<ValueType> m_types;
};

// https://webassembly.github.io/spec/core/bikeshed/#function-types%E2%91%A4
class FunctionType {
public:
    FunctionType(Vector<ValueType> parameters, Vector<ValueType> results)
        : m_parameters(move(parameters))
        , m_results(move(results))
    {
    }

    auto& parameters() const { return m_parameters; }
    auto& results() const { return m_results; }

    static ParseResult<FunctionType> parse(ConstrainedStream& stream);

private:
    Vector<ValueType> m_parameters;
    Vector<ValueType> m_results;
};

// https://webassembly.github.io/spec/core/bikeshed/#composite-types%E2%91%A0
class FieldType {
public:
    FieldType(bool is_mutable, ValueType type_)
        : m_is_mutable(is_mutable)
        , m_type(type_)
    {
    }

    auto& type() const { return m_type; }
    auto is_mutable() const { return m_is_mutable; }

    static ParseResult<FieldType> parse(ConstrainedStream& stream);

private:
    bool m_is_mutable { false };
    ValueType m_type;
};

// https://webassembly.github.io/spec/core/bikeshed/#composite-types%E2%91%A0
class StructType {
public:
    StructType(Vector<FieldType> fields)
        : m_fields(move(fields))
    {
    }

    auto& fields() const { return m_fields; }

    static ParseResult<StructType> parse(ConstrainedStream& stream);

private:
    Vector<FieldType> m_fields;
};

// https://webassembly.github.io/spec/core/bikeshed/#composite-types%E2%91%A0
class ArrayType {
public:
    ArrayType(FieldType type)
        : m_type(type)
    {
    }

    auto& type() const { return m_type; }

    static ParseResult<ArrayType> parse(ConstrainedStream& stream);

private:
    FieldType m_type;
};

// https://webassembly.github.io/memory64/core/bikeshed/#address-type%E2%91%A0
enum class AddressType : u8 {
    I32,
    I64,
};

// https://webassembly.github.io/spec/core/bikeshed/#limits%E2%91%A5
class Limits {
public:
    explicit Limits(AddressType address_type, u64 min, Optional<u64> max = {})
        : m_address_type(address_type)
        , m_min(min)
        , m_max(move(max))
    {
    }

    ValueType address_value_type() const
    {
        return m_address_type == AddressType::I32 ? ValueType(ValueType::I32) : ValueType(ValueType::I64);
    }
    auto address_type() const { return m_address_type; }
    auto min() const { return m_min; }
    auto& max() const { return m_max; }
    bool is_subset_of(Limits other) const
    {
        return m_min >= other.min()
            && (!other.max().has_value() || (m_max.has_value() && *m_max <= *other.max()))
            && m_address_type == other.m_address_type;
    }

    static ParseResult<Limits> parse(ConstrainedStream& stream);

private:
    AddressType m_address_type { AddressType::I32 };
    u64 m_min { 0 };
    Optional<u64> m_max;
};

// https://webassembly.github.io/spec/core/bikeshed/#memory-types%E2%91%A4
class MemoryType {
public:
    explicit MemoryType(Limits limits)
        : m_limits(move(limits))
    {
    }

    auto& limits() const { return m_limits; }

    static ParseResult<MemoryType> parse(ConstrainedStream& stream);

private:
    Limits m_limits;
};

// https://webassembly.github.io/spec/core/bikeshed/#table-types%E2%91%A4
class TableType {
public:
    explicit TableType(ValueType element_type, Limits limits)
        : m_element_type(element_type)
        , m_limits(move(limits))
    {
        VERIFY(m_element_type.is_reference());
    }

    auto& limits() const { return m_limits; }
    auto& element_type() const { return m_element_type; }

    static ParseResult<TableType> parse(ConstrainedStream& stream);

private:
    ValueType m_element_type;
    Limits m_limits;
};

// https://webassembly.github.io/spec/core/bikeshed/#global-types%E2%91%A4
class GlobalType {
public:
    GlobalType(ValueType type, bool is_mutable)
        : m_type(type)
        , m_is_mutable(is_mutable)
    {
    }

    auto& type() const { return m_type; }
    auto is_mutable() const { return m_is_mutable; }

    static ParseResult<GlobalType> parse(ConstrainedStream& stream);

private:
    ValueType m_type;
    bool m_is_mutable { false };
};

// https://webassembly.github.io/exception-handling/core/binary/types.html#tag-types
class TagType {
public:
    enum Flags : u8 {
        None = 0
    };

    TagType(TypeIndex type, Flags flags)
        : m_flags(flags)
        , m_type(type)
    {
    }

    auto& type() const { return m_type; }
    auto flags() const { return m_flags; }

    static ParseResult<TagType> parse(ConstrainedStream& stream);

private:
    Flags m_flags { None };
    TypeIndex m_type;
};

// https://webassembly.github.io/spec/core/bikeshed/#binary-blocktype
class BlockType {
public:
    enum Kind {
        Empty,
        Type,
        Index,
    };

    BlockType()
        : m_kind(Empty)
        , m_empty(0)
    {
    }

    explicit BlockType(ValueType type)
        : m_kind(Type)
        , m_value_type(type)
    {
    }

    explicit BlockType(TypeIndex index)
        : m_kind(Index)
        , m_type_index(index)
    {
    }

    auto kind() const { return m_kind; }
    auto& value_type() const
    {
        VERIFY(kind() == Type);
        return m_value_type;
    }
    auto& type_index() const
    {
        VERIFY(kind() == Index);
        return m_type_index;
    }

    static ParseResult<BlockType> parse(ConstrainedStream& stream);

private:
    Kind m_kind { Empty };
    union {
        ValueType m_value_type;
        TypeIndex m_type_index;
        u8 m_empty;
    };
};

// Proposal "exception-handling"
// https://webassembly.github.io/exception-handling/core/binary/instructions.html
class Catch {
public:
    Catch(bool ref, TagIndex index, LabelIndex label) // catch[_ref] x l
        : m_matching_tag_index(index)
        , m_target_label(label)
        , m_is_ref(ref)
    {
    }

    explicit Catch(bool ref, LabelIndex label) // catch_all[_ref] l
        : m_target_label(label)
        , m_is_ref(ref)
    {
    }

    auto& matching_tag_index() const { return m_matching_tag_index; }
    auto& target_label() const { return m_target_label; }
    auto is_ref() const { return m_is_ref; }

    static ParseResult<Catch> parse(ConstrainedStream& stream);

private:
    Optional<TagIndex> m_matching_tag_index; // None for catch_all
    LabelIndex m_target_label;
    bool m_is_ref = false; // true if catch*_ref
};

// https://webassembly.github.io/spec/core/bikeshed/#binary-instr
// https://webassembly.github.io/spec/core/bikeshed/#reference-instructions%E2%91%A6
// https://webassembly.github.io/spec/core/bikeshed/#parametric-instructions%E2%91%A6
// https://webassembly.github.io/spec/core/bikeshed/#variable-instructions%E2%91%A6
// https://webassembly.github.io/spec/core/bikeshed/#table-instructions%E2%91%A6
// https://webassembly.github.io/spec/core/bikeshed/#memory-instructions%E2%91%A6
// https://webassembly.github.io/spec/core/bikeshed/#numeric-instructions%E2%91%A6
class Instruction {
public:
    explicit Instruction(OpCode opcode)
        : m_opcode(opcode)
        , m_arguments(static_cast<u8>(0))
    {
    }

    struct TableElementArgs {
        ElementIndex element_index;
        TableIndex table_index;
    };

    struct TableTableArgs {
        TableIndex lhs;
        TableIndex rhs;
    };

    struct StructuredInstructionArgs {
        BlockType block_type;
        InstructionPointer end_ip; // 'end' instruction IP if there is no 'else'; otherwise IP of instruction after 'end'.
        Optional<InstructionPointer> else_ip;

        struct Meta {
            u32 arity;
            u32 parameter_count;
        };
        mutable Optional<Meta> meta {};
    };

    struct TableBranchArgs {
        Vector<LabelIndex> labels;
        LabelIndex default_;
    };

    struct BranchArgs {
        LabelIndex label;
        mutable bool has_stack_adjustment { false };
    };

    struct IndirectCallArgs {
        TypeIndex type;
        TableIndex table;
    };

    struct MemoryArgument {
        u32 align;
        u64 offset;
        MemoryIndex memory_index { 0 };
    };

    struct MemoryAndLaneArgument {
        MemoryArgument memory;
        u8 lane;
    };

    struct LaneIndex {
        u8 lane;
    };

    // Proposal "multi-memory"
    struct MemoryCopyArgs {
        MemoryIndex src_index;
        MemoryIndex dst_index;
    };

    struct MemoryInitArgs {
        DataIndex data_index;
        MemoryIndex memory_index;
    };

    struct MemoryIndexArgument {
        MemoryIndex memory_index;
    };

    // Proposal "exception-handling"
    struct TryTableArgs {
        StructuredInstructionArgs try_; // "else" unused.
        Vector<Catch> catches;
    };

    struct ShuffleArgument {
        explicit ShuffleArgument(u8 (&lanes)[16])
            : lanes {
                lanes[0], lanes[1], lanes[2], lanes[3], lanes[4], lanes[5], lanes[6], lanes[7],
                lanes[8], lanes[9], lanes[10], lanes[11], lanes[12], lanes[13], lanes[14], lanes[15]
            }
        {
        }

        u8 lanes[16];
    };

    template<typename T>
    explicit Instruction(OpCode opcode, T argument)
        : m_opcode(opcode)
        , m_arguments(move(argument))
    {
    }

    explicit Instruction(OpCode opcode, LocalIndex argument)
        : m_opcode(opcode)
        , m_local_index(argument)
        , m_arguments(static_cast<u8>(0))
    {
    }

    template<typename Arg1>
    explicit Instruction(OpCode opcode, LocalIndex argument0, Arg1&& argument1)
        : m_opcode(opcode)
        , m_local_index(argument0)
        , m_arguments(forward<Arg1>(argument1))
    {
    }

    static ParseResult<Instruction> parse(ConstrainedStream& stream);

    auto& opcode() const { return m_opcode; }
    auto& arguments() const { return m_arguments; }
    auto& arguments() { return m_arguments; }

    LocalIndex local_index() const { return m_local_index; }

    void set_local_index(Badge<Module>, LocalIndex index) { m_local_index = index; }

private:
    OpCode m_opcode { 0 };
    LocalIndex m_local_index;

    Variant<
        BlockType,
        BranchArgs,
        DataIndex,
        ElementIndex,
        FunctionIndex,
        GlobalIndex,
        TagIndex,
        IndirectCallArgs,
        LabelIndex,
        LaneIndex,
        LocalIndex, // Only used by instructions that take more than one local index (currently only fused ops).
        MemoryArgument,
        MemoryAndLaneArgument,
        MemoryCopyArgs,
        MemoryIndexArgument,
        MemoryInitArgs,
        StructuredInstructionArgs,
        ShuffleArgument,
        TableBranchArgs,
        TableElementArgs,
        TableIndex,
        TableTableArgs,
        TryTableArgs,
        ValueType,
        Vector<ValueType>,
        double,
        float,
        i32,
        i64,
        u128,
        u8> // Empty state
        m_arguments;
};

struct Dispatch {
    enum RegisterOrStack : u8 {
        R0,
        R1,
        R2,
        R3,
        R4,
        R5,
        R6,
        R7,
        CountRegisters,
        Stack = CountRegisters,
        CallRecord,
        LastCallRecord = NumericLimits<u8>::max(),
    };

    static_assert(is_power_of_two(to_underlying(Stack)), "Stack marker must be a single bit");

    union {
        OpCode instruction_opcode;
        FlatPtr handler_ptr;
    };
    Instruction const* instruction { nullptr };
};

union SourcesAndDestination {
    struct {
        Dispatch::RegisterOrStack sources[3];
        Dispatch::RegisterOrStack destination;
    };
    u32 sources_and_destination;
};

struct CompiledInstructions {
    Vector<Dispatch> dispatches;
    Vector<SourcesAndDestination> src_dst_mappings;
    Vector<Instruction, 0, FastLastAccess::Yes> extra_instruction_storage;
    bool direct = false; // true if all dispatches contain handler_ptr, otherwise false and all contain instruction_opcode.
    size_t max_call_arg_count = 0;
    size_t max_call_rec_size = 0;
};

template<Enum auto... Vs>
consteval auto as_ordered()
{
    using Type = CommonType<decltype(to_underlying(Vs))...>;
    Array<Type, sizeof...(Vs)> result;
    [&]<Type... Is>(IntegerSequence<Type, Is...>) {
        (void)((result[to_underlying(Vs)] = Is), ...);
    }(MakeIntegerSequence<Type, static_cast<Type>(sizeof...(Vs))>());
    return result;
}

struct SectionId {
public:
    enum class SectionIdKind : u8 {
        Custom,
        Type,
        Import,
        Function,
        Table,
        Memory,
        Global,
        Export,
        Start,
        Element,
        DataCount,
        Code,
        Data,
        Tag,
    };

    constexpr inline static auto section_order = as_ordered<
        SectionIdKind::Type,
        SectionIdKind::Import,
        SectionIdKind::Function,
        SectionIdKind::Table,
        SectionIdKind::Memory,
        SectionIdKind::Tag,
        SectionIdKind::Global,
        SectionIdKind::Export,
        SectionIdKind::Start,
        SectionIdKind::Element,
        SectionIdKind::DataCount,
        SectionIdKind::Code,
        SectionIdKind::Data,
        SectionIdKind::Custom>();

    explicit SectionId(SectionIdKind kind)
        : m_kind(kind)
    {
    }

    bool can_appear_after(SectionIdKind other) const
    {
        if (kind() == SectionIdKind::Custom || other == SectionIdKind::Custom)
            return true;

        auto index = section_order[to_underlying(kind())];
        auto other_index = section_order[to_underlying(other)];
        return index >= other_index;
    }

    SectionIdKind kind() const
    {
        return m_kind;
    }

    static ParseResult<SectionId> parse(Stream& stream);

private:
    SectionIdKind m_kind;
};

class CustomSection {
public:
    CustomSection(ByteString name, ByteBuffer contents)
        : m_name(move(name))
        , m_contents(move(contents))
    {
    }

    auto& name() const { return m_name; }
    auto& contents() const { return m_contents; }

    static ParseResult<CustomSection> parse(ConstrainedStream& stream);

private:
    ByteString m_name;
    ByteBuffer m_contents;
};

class TypeSection {
public:
    class Type {
    private:
        using TypeDesc = Variant<FunctionType, StructType, ArrayType>;

    public:
        Type(TypeDesc type)
            : m_description(type)
        {
        }

        auto& description() const { return m_description; }

        auto& function() const { return m_description.get<FunctionType>(); }
        auto& unsafe_function() const { return m_description.unsafe_get<FunctionType>(); }
        bool is_function() const { return m_description.has<FunctionType>(); }

        auto& struct_() const { return m_description.get<StructType>(); }
        bool is_struct() const { return m_description.has<StructType>(); }

        ByteString name() const
        {
            return m_description.visit(
                [](FunctionType const&) -> ByteString { return "function type"; },
                [](StructType const&) -> ByteString { return "struct type"; },
                [](ArrayType const&) -> ByteString { return "array type"; });
        }

        static ParseResult<Type> parse(ConstrainedStream& stream);

    private:
        TypeDesc m_description;
    };

    TypeSection() = default;

    explicit TypeSection(Vector<Type> types)
        : m_types(move(types))
    {
    }

    auto& types() const { return m_types; }

    static ParseResult<TypeSection> parse(ConstrainedStream& stream);

private:
    Vector<Type> m_types;
};

class ImportSection {
public:
    class Import {
    public:
        using ImportDesc = Variant<TypeIndex, TableType, MemoryType, GlobalType, FunctionType, TagType>;
        Import(ByteString module, ByteString name, ImportDesc description)
            : m_module(move(module))
            , m_name(move(name))
            , m_description(move(description))
        {
        }

        auto& module() const { return m_module; }
        auto& name() const { return m_name; }
        auto& description() const { return m_description; }

        static ParseResult<Import> parse(ConstrainedStream& stream);

    private:
        template<typename T>
        static ParseResult<Import> parse_with_type(auto&& stream, auto&& module, auto&& name)
        {
            auto result = TRY(T::parse(stream));
            return Import { module, name, result };
        }

        ByteString m_module;
        ByteString m_name;
        ImportDesc m_description;
    };

public:
    ImportSection() = default;

    explicit ImportSection(Vector<Import> imports)
        : m_imports(move(imports))
    {
    }

    auto& imports() const { return m_imports; }

    static ParseResult<ImportSection> parse(ConstrainedStream& stream);

private:
    Vector<Import> m_imports;
};

class FunctionSection {
public:
    FunctionSection() = default;

    explicit FunctionSection(Vector<TypeIndex> types)
        : m_types(move(types))
    {
    }

    auto& types() const { return m_types; }

    static ParseResult<FunctionSection> parse(ConstrainedStream& stream);

private:
    Vector<TypeIndex> m_types;
};

class TableSection {
public:
    class Table {
    public:
        explicit Table(TableType type)
            : m_type(move(type))
        {
        }

        auto& type() const { return m_type; }

        static ParseResult<Table> parse(ConstrainedStream& stream);

    private:
        TableType m_type;
    };

public:
    TableSection() = default;

    explicit TableSection(Vector<Table> tables)
        : m_tables(move(tables))
    {
    }

    auto& tables() const { return m_tables; }

    static ParseResult<TableSection> parse(ConstrainedStream& stream);

private:
    Vector<Table> m_tables;
};

class MemorySection {
public:
    class Memory {
    public:
        explicit Memory(MemoryType type)
            : m_type(move(type))
        {
        }

        auto& type() const { return m_type; }

        static ParseResult<Memory> parse(ConstrainedStream& stream);

    private:
        MemoryType m_type;
    };

public:
    MemorySection() = default;

    explicit MemorySection(Vector<Memory> memories)
        : m_memories(move(memories))
    {
    }

    auto& memories() const { return m_memories; }

    static ParseResult<MemorySection> parse(ConstrainedStream& stream);

private:
    Vector<Memory> m_memories;
};

class Expression {
public:
    explicit Expression(Vector<Instruction> instructions)
        : m_instructions(move(instructions))
    {
    }

    auto& instructions() const { return m_instructions; }

    static ParseResult<Expression> parse(ConstrainedStream& stream, Optional<size_t> size_hint = {});

    void set_stack_usage_hint(size_t value) const { m_stack_usage_hint = value; }
    auto stack_usage_hint() const { return m_stack_usage_hint; }
    void set_frame_usage_hint(size_t value) const { m_frame_usage_hint = value; }
    auto frame_usage_hint() const { return m_frame_usage_hint; }

    mutable CompiledInstructions compiled_instructions;

private:
    Vector<Instruction> m_instructions;
    mutable Optional<size_t> m_stack_usage_hint;
    mutable Optional<size_t> m_frame_usage_hint;
};

class GlobalSection {
public:
    class Global {
    public:
        explicit Global(GlobalType type, Expression expression)
            : m_type(move(type))
            , m_expression(move(expression))
        {
        }

        auto& type() const { return m_type; }
        auto& expression() const { return m_expression; }

        static ParseResult<Global> parse(ConstrainedStream& stream);

    private:
        GlobalType m_type;
        Expression m_expression;
    };

public:
    GlobalSection() = default;

    explicit GlobalSection(Vector<Global> entries)
        : m_entries(move(entries))
    {
    }

    auto& entries() const { return m_entries; }

    static ParseResult<GlobalSection> parse(ConstrainedStream& stream);

private:
    Vector<Global> m_entries;
};

class ExportSection {
private:
    using ExportDesc = Variant<FunctionIndex, TableIndex, MemoryIndex, GlobalIndex, TagIndex>;

public:
    class Export {
    public:
        explicit Export(ByteString name, ExportDesc description)
            : m_name(move(name))
            , m_description(move(description))
        {
        }

        auto& name() const { return m_name; }
        auto& description() const { return m_description; }

        static ParseResult<Export> parse(ConstrainedStream& stream);

    private:
        ByteString m_name;
        ExportDesc m_description;
    };

    ExportSection() = default;

    explicit ExportSection(Vector<Export> entries)
        : m_entries(move(entries))
    {
    }

    auto& entries() const { return m_entries; }

    static ParseResult<ExportSection> parse(ConstrainedStream& stream);

private:
    Vector<Export> m_entries;
};

class StartSection {
public:
    class StartFunction {
    public:
        explicit StartFunction(FunctionIndex index)
            : m_index(index)
        {
        }

        auto& index() const { return m_index; }

        static ParseResult<StartFunction> parse(ConstrainedStream& stream);

    private:
        FunctionIndex m_index;
    };

    StartSection() = default;

    explicit StartSection(Optional<StartFunction> func)
        : m_function(move(func))
    {
    }

    auto& function() const { return m_function; }

    static ParseResult<StartSection> parse(ConstrainedStream& stream);

private:
    Optional<StartFunction> m_function;
};

class ElementSection {
public:
    struct Active {
        TableIndex index;
        Expression expression;
    };
    struct Declarative {
    };
    struct Passive {
    };

    struct Element {
        static ParseResult<Element> parse(ConstrainedStream&);

        ValueType type;
        Vector<Expression> init;
        Variant<Active, Passive, Declarative> mode;
    };

    ElementSection() = default;

    explicit ElementSection(Vector<Element> segs)
        : m_segments(move(segs))
    {
    }

    auto& segments() const { return m_segments; }

    static ParseResult<ElementSection> parse(ConstrainedStream& stream);

private:
    Vector<Element> m_segments;
};

class Locals {
public:
    explicit Locals(u32 n, ValueType type)
        : m_n(n)
        , m_type(type)
    {
    }

    // Yikes...
    auto n() const { return m_n; }
    auto& type() const { return m_type; }

    static ParseResult<Locals> parse(ConstrainedStream& stream);

private:
    u32 m_n { 0 };
    ValueType m_type;
};

class CodeSection {
public:
    // https://webassembly.github.io/spec/core/bikeshed/#binary-func
    class Func {
    public:
        explicit Func(Vector<Locals> locals, Expression body)
            : m_locals(move(locals))
            , m_body(move(body))
        {
            for (auto const& local : m_locals)
                m_total_local_count += local.n();
        }

        auto& locals() const { return m_locals; }
        auto& body() const { return m_body; }

        static ParseResult<Func> parse(ConstrainedStream& stream, size_t size_hint);

        auto total_local_count() const { return m_total_local_count; }

    private:
        Vector<Locals> m_locals;
        Expression m_body;
        size_t m_total_local_count { 0 };
    };
    class Code {
    public:
        explicit Code(u32 size, Func func)
            : m_size(size)
            , m_func(move(func))
        {
        }

        auto size() const { return m_size; }
        auto& func() const { return m_func; }

        static ParseResult<Code> parse(ConstrainedStream& stream);

    private:
        u32 m_size { 0 };
        Func m_func;
    };

    CodeSection() = default;

    explicit CodeSection(Vector<Code> funcs)
        : m_functions(move(funcs))
    {
    }

    auto& functions() const { return m_functions; }

    static ParseResult<CodeSection> parse(ConstrainedStream& stream);

private:
    Vector<Code> m_functions;
};

class DataSection {
public:
    class Data {
    public:
        struct Passive {
            Vector<u8> init;
        };
        struct Active {
            Vector<u8> init;
            MemoryIndex index;
            Expression offset;
        };
        using Value = Variant<Passive, Active>;

        explicit Data(Value value)
            : m_value(move(value))
        {
        }

        auto& value() const { return m_value; }

        static ParseResult<Data> parse(ConstrainedStream& stream);

    private:
        Value m_value;
    };

    DataSection() = default;

    explicit DataSection(Vector<Data> data)
        : m_data(move(data))
    {
    }

    auto& data() const { return m_data; }

    static ParseResult<DataSection> parse(ConstrainedStream& stream);

private:
    Vector<Data> m_data;
};

class DataCountSection {
public:
    DataCountSection() = default;

    explicit DataCountSection(Optional<u32> count)
        : m_count(move(count))
    {
    }

    auto& count() const { return m_count; }

    static ParseResult<DataCountSection> parse(ConstrainedStream& stream);

private:
    Optional<u32> m_count;
};

class TagSection {
public:
    TagSection() = default;

    explicit TagSection(Vector<TagType> tags)
        : m_tags(move(tags))
    {
    }

    auto& tags() const { return m_tags; }

    static ParseResult<TagSection> parse(ConstrainedStream& stream);

private:
    Vector<TagType> m_tags;
};

class WASM_API Module : public RefCounted<Module>
    , public Weakable<Module> {
public:
    enum class ValidationStatus {
        Unchecked,
        Invalid,
        Valid,
    };

    static constexpr Array<u8, 4> wasm_magic { 0, 'a', 's', 'm' };
    static constexpr Array<u8, 4> wasm_version { 1, 0, 0, 0 };

    Module() = default;

    auto& custom_sections() { return m_custom_sections; }
    auto& custom_sections() const { return m_custom_sections; }
    auto& type_section() const { return m_type_section; }
    auto& type_section() { return m_type_section; }
    auto& import_section() const { return m_import_section; }
    auto& import_section() { return m_import_section; }
    auto& function_section() { return m_function_section; }
    auto& function_section() const { return m_function_section; }
    auto& table_section() { return m_table_section; }
    auto& table_section() const { return m_table_section; }
    auto& memory_section() { return m_memory_section; }
    auto& memory_section() const { return m_memory_section; }
    auto& global_section() { return m_global_section; }
    auto& global_section() const { return m_global_section; }
    auto& export_section() { return m_export_section; }
    auto& export_section() const { return m_export_section; }
    auto& start_section() { return m_start_section; }
    auto& start_section() const { return m_start_section; }
    auto& element_section() { return m_element_section; }
    auto& element_section() const { return m_element_section; }
    auto& code_section() { return m_code_section; }
    auto& code_section() const { return m_code_section; }
    auto& data_section() { return m_data_section; }
    auto& data_section() const { return m_data_section; }
    auto& data_count_section() { return m_data_count_section; }
    auto& data_count_section() const { return m_data_count_section; }
    auto& tag_section() { return m_tag_section; }
    auto& tag_section() const { return m_tag_section; }

    void set_validation_status(ValidationStatus status, Badge<Validator>) { set_validation_status(status); }
    ValidationStatus validation_status() const { return m_validation_status; }
    StringView validation_error() const LIFETIME_BOUND { return *m_validation_error; }
    void set_validation_error(ByteString error) { m_validation_error = move(error); }

    static ParseResult<NonnullRefPtr<Module>> parse(Stream& stream);

    size_t minimum_call_record_allocation_size() const { return m_minimum_call_record_allocation_size; }
    void set_minimum_call_record_allocation_size(size_t size) { m_minimum_call_record_allocation_size = size; }

private:
    void set_validation_status(ValidationStatus status) { m_validation_status = status; }
    void preprocess();

    Vector<CustomSection> m_custom_sections;
    TypeSection m_type_section;
    ImportSection m_import_section;
    FunctionSection m_function_section;
    TableSection m_table_section;
    MemorySection m_memory_section;
    GlobalSection m_global_section;
    ExportSection m_export_section;
    StartSection m_start_section;
    ElementSection m_element_section;
    CodeSection m_code_section;
    DataSection m_data_section;
    DataCountSection m_data_count_section;
    TagSection m_tag_section;

    ValidationStatus m_validation_status { ValidationStatus::Unchecked };
    Optional<ByteString> m_validation_error;

    size_t m_minimum_call_record_allocation_size { 0 };
};

CompiledInstructions try_compile_instructions(Expression const&, Span<FunctionType const> functions);

}
