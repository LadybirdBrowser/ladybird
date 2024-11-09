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
#include <LibWasm/Forward.h>
#include <LibWasm/Opcode.h>

namespace Wasm {

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

ByteString parse_error_to_byte_string(ParseError);

template<typename T>
using ParseResult = ErrorOr<T, ParseError>;

AK_TYPEDEF_DISTINCT_ORDERED_ID(size_t, TypeIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(size_t, FunctionIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(size_t, TableIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(size_t, ElementIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(size_t, MemoryIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(size_t, LocalIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(size_t, GlobalIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(size_t, LabelIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(size_t, DataIndex);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u64, InstructionPointer, Arithmetic, Comparison, Flags, Increment);

ParseError with_eof_check(Stream const& stream, ParseError error_if_not_eof);

template<typename T>
struct GenericIndexParser {
    static ParseResult<T> parse(Stream& stream)
    {
        auto value_or_error = stream.read_value<LEB128<u32>>();
        if (value_or_error.is_error())
            return with_eof_check(stream, ParseError::ExpectedIndex);
        size_t value = value_or_error.release_value();
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
    };

    explicit ValueType(Kind kind)
        : m_kind(kind)
    {
    }

    bool operator==(ValueType const&) const = default;

    auto is_reference() const { return m_kind == ExternReference || m_kind == FunctionReference; }
    auto is_vector() const { return m_kind == V128; }
    auto is_numeric() const { return !is_reference() && !is_vector(); }
    auto kind() const { return m_kind; }

    static ParseResult<ValueType> parse(Stream& stream);

    static ByteString kind_name(Kind kind)
    {
        switch (kind) {
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
        }
        VERIFY_NOT_REACHED();
    }

private:
    Kind m_kind;
};

// https://webassembly.github.io/spec/core/bikeshed/#result-types%E2%91%A2
class ResultType {
public:
    explicit ResultType(Vector<ValueType> types)
        : m_types(move(types))
    {
    }

    auto const& types() const { return m_types; }

    static ParseResult<ResultType> parse(Stream& stream);

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

    static ParseResult<FunctionType> parse(Stream& stream);

private:
    Vector<ValueType> m_parameters;
    Vector<ValueType> m_results;
};

// https://webassembly.github.io/spec/core/bikeshed/#limits%E2%91%A5
class Limits {
public:
    explicit Limits(u32 min, Optional<u32> max = {})
        : m_min(min)
        , m_max(move(max))
    {
    }

    auto min() const { return m_min; }
    auto& max() const { return m_max; }
    bool is_subset_of(Limits other) const
    {
        return m_min >= other.min()
            && (!other.max().has_value() || (m_max.has_value() && *m_max <= *other.max()));
    }

    static ParseResult<Limits> parse(Stream& stream);

private:
    u32 m_min { 0 };
    Optional<u32> m_max;
};

// https://webassembly.github.io/spec/core/bikeshed/#memory-types%E2%91%A4
class MemoryType {
public:
    explicit MemoryType(Limits limits)
        : m_limits(move(limits))
    {
    }

    auto& limits() const { return m_limits; }

    static ParseResult<MemoryType> parse(Stream& stream);

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

    static ParseResult<TableType> parse(Stream& stream);

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

    static ParseResult<GlobalType> parse(Stream& stream);

private:
    ValueType m_type;
    bool m_is_mutable { false };
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

    static ParseResult<BlockType> parse(Stream& stream);

private:
    Kind m_kind { Empty };
    union {
        ValueType m_value_type;
        TypeIndex m_type_index;
        u8 m_empty;
    };
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
        InstructionPointer end_ip;
        Optional<InstructionPointer> else_ip;
    };

    struct TableBranchArgs {
        Vector<LabelIndex> labels;
        LabelIndex default_;
    };

    struct IndirectCallArgs {
        TypeIndex type;
        TableIndex table;
    };

    struct MemoryArgument {
        u32 align;
        u32 offset;
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

    static ParseResult<Instruction> parse(Stream& stream);

    auto& opcode() const { return m_opcode; }
    auto& arguments() const { return m_arguments; }
    auto& arguments() { return m_arguments; }

private:
    OpCode m_opcode { 0 };
    Variant<
        BlockType,
        DataIndex,
        ElementIndex,
        FunctionIndex,
        GlobalIndex,
        IndirectCallArgs,
        LabelIndex,
        LaneIndex,
        LocalIndex,
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
    };

    explicit SectionId(SectionIdKind kind)
        : m_kind(kind)
    {
    }

    SectionIdKind kind() const { return m_kind; }

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

    static ParseResult<CustomSection> parse(Stream& stream);

private:
    ByteString m_name;
    ByteBuffer m_contents;
};

class TypeSection {
public:
    TypeSection() = default;

    explicit TypeSection(Vector<FunctionType> types)
        : m_types(move(types))
    {
    }

    auto& types() const { return m_types; }

    static ParseResult<TypeSection> parse(Stream& stream);

private:
    Vector<FunctionType> m_types;
};

class ImportSection {
public:
    class Import {
    public:
        using ImportDesc = Variant<TypeIndex, TableType, MemoryType, GlobalType, FunctionType>;
        Import(ByteString module, ByteString name, ImportDesc description)
            : m_module(move(module))
            , m_name(move(name))
            , m_description(move(description))
        {
        }

        auto& module() const { return m_module; }
        auto& name() const { return m_name; }
        auto& description() const { return m_description; }

        static ParseResult<Import> parse(Stream& stream);

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

    static ParseResult<ImportSection> parse(Stream& stream);

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

    static ParseResult<FunctionSection> parse(Stream& stream);

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

        static ParseResult<Table> parse(Stream& stream);

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

    static ParseResult<TableSection> parse(Stream& stream);

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

        static ParseResult<Memory> parse(Stream& stream);

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

    static ParseResult<MemorySection> parse(Stream& stream);

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

    static ParseResult<Expression> parse(Stream& stream, Optional<size_t> size_hint = {});

private:
    Vector<Instruction> m_instructions;
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

        static ParseResult<Global> parse(Stream& stream);

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

    static ParseResult<GlobalSection> parse(Stream& stream);

private:
    Vector<Global> m_entries;
};

class ExportSection {
private:
    using ExportDesc = Variant<FunctionIndex, TableIndex, MemoryIndex, GlobalIndex>;

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

        static ParseResult<Export> parse(Stream& stream);

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

    static ParseResult<ExportSection> parse(Stream& stream);

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

        static ParseResult<StartFunction> parse(Stream& stream);

    private:
        FunctionIndex m_index;
    };

    StartSection() = default;

    explicit StartSection(Optional<StartFunction> func)
        : m_function(move(func))
    {
    }

    auto& function() const { return m_function; }

    static ParseResult<StartSection> parse(Stream& stream);

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
        static ParseResult<Element> parse(Stream&);

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

    static ParseResult<ElementSection> parse(Stream& stream);

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

    static ParseResult<Locals> parse(Stream& stream);

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
        }

        auto& locals() const { return m_locals; }
        auto& body() const { return m_body; }

        static ParseResult<Func> parse(Stream& stream, size_t size_hint);

    private:
        Vector<Locals> m_locals;
        Expression m_body;
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

        static ParseResult<Code> parse(Stream& stream);

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

    static ParseResult<CodeSection> parse(Stream& stream);

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

        static ParseResult<Data> parse(Stream& stream);

    private:
        Value m_value;
    };

    DataSection() = default;

    explicit DataSection(Vector<Data> data)
        : m_data(move(data))
    {
    }

    auto& data() const { return m_data; }

    static ParseResult<DataSection> parse(Stream& stream);

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

    static ParseResult<DataCountSection> parse(Stream& stream);

private:
    Optional<u32> m_count;
};

class Module : public RefCounted<Module>
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

    void set_validation_status(ValidationStatus status, Badge<Validator>) { set_validation_status(status); }
    ValidationStatus validation_status() const { return m_validation_status; }
    StringView validation_error() const { return *m_validation_error; }
    void set_validation_error(ByteString error) { m_validation_error = move(error); }

    static ParseResult<NonnullRefPtr<Module>> parse(Stream& stream);

private:
    void set_validation_status(ValidationStatus status) { m_validation_status = status; }

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

    ValidationStatus m_validation_status { ValidationStatus::Unchecked };
    Optional<ByteString> m_validation_error;
};
}
