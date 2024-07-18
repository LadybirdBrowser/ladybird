/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/COWVector.h>
#include <AK/Debug.h>
#include <AK/RedBlackTree.h>
#include <AK/SourceLocation.h>
#include <AK/Tuple.h>
#include <AK/Vector.h>
#include <LibWasm/Forward.h>
#include <LibWasm/Types.h>

namespace Wasm {

struct Context {
    struct RefRBTree : RefCounted<RefRBTree> {
        RedBlackTree<size_t, FunctionIndex> tree;
    };

    COWVector<FunctionType> types;
    COWVector<FunctionType> functions;
    COWVector<TableType> tables;
    COWVector<MemoryType> memories;
    COWVector<GlobalType> globals;
    COWVector<ValueType> elements;
    COWVector<bool> datas;
    COWVector<ValueType> locals;
    Optional<u32> data_count;
    RefPtr<RefRBTree> references { make_ref_counted<RefRBTree>() };
    size_t imported_function_count { 0 };
};

struct ValidationError : public Error {
    ValidationError(ByteString error)
        : Error(Error::from_string_view(error.view()))
        , error_string(move(error))
    {
    }

    ByteString error_string;
};

class Validator {
    AK_MAKE_NONCOPYABLE(Validator);
    AK_MAKE_NONMOVABLE(Validator);

public:
    Validator() = default;

    [[nodiscard]] Validator fork() const
    {
        return Validator { m_context };
    }

    // Module
    ErrorOr<void, ValidationError> validate(Module&);
    ErrorOr<void, ValidationError> validate(ImportSection const&);
    ErrorOr<void, ValidationError> validate(ExportSection const&);
    ErrorOr<void, ValidationError> validate(StartSection const&);
    ErrorOr<void, ValidationError> validate(DataSection const&);
    ErrorOr<void, ValidationError> validate(ElementSection const&);
    ErrorOr<void, ValidationError> validate(GlobalSection const&);
    ErrorOr<void, ValidationError> validate(MemorySection const&);
    ErrorOr<void, ValidationError> validate(TableSection const&);
    ErrorOr<void, ValidationError> validate(CodeSection const&);
    ErrorOr<void, ValidationError> validate(FunctionSection const&) { return {}; }
    ErrorOr<void, ValidationError> validate(DataCountSection const&) { return {}; }
    ErrorOr<void, ValidationError> validate(TypeSection const&) { return {}; }
    ErrorOr<void, ValidationError> validate(CustomSection const&) { return {}; }

    ErrorOr<void, ValidationError> validate(TypeIndex index) const
    {
        if (index.value() < m_context.types.size())
            return {};
        return Errors::invalid("TypeIndex"sv);
    }

    ErrorOr<void, ValidationError> validate(FunctionIndex index) const
    {
        if (index.value() < m_context.functions.size())
            return {};
        return Errors::invalid("FunctionIndex"sv);
    }

    ErrorOr<void, ValidationError> validate(MemoryIndex index) const
    {
        if (index.value() < m_context.memories.size())
            return {};
        return Errors::invalid("MemoryIndex"sv);
    }

    ErrorOr<void, ValidationError> validate(ElementIndex index) const
    {
        if (index.value() < m_context.elements.size())
            return {};
        return Errors::invalid("ElementIndex"sv);
    }

    ErrorOr<void, ValidationError> validate(DataIndex index) const
    {
        if (index.value() < m_context.datas.size())
            return {};
        return Errors::invalid("DataIndex"sv);
    }

    ErrorOr<void, ValidationError> validate(GlobalIndex index) const
    {
        if (index.value() < m_context.globals.size())
            return {};
        return Errors::invalid("GlobalIndex"sv);
    }

    ErrorOr<void, ValidationError> validate(LabelIndex index) const
    {
        if (index.value() < m_frames.size())
            return {};
        return Errors::invalid("LabelIndex"sv);
    }

    ErrorOr<void, ValidationError> validate(LocalIndex index) const
    {
        if (index.value() < m_context.locals.size())
            return {};
        return Errors::invalid("LocalIndex"sv);
    }

    ErrorOr<void, ValidationError> validate(TableIndex index) const
    {
        if (index.value() < m_context.tables.size())
            return {};
        return Errors::invalid("TableIndex"sv);
    }

    enum class FrameKind {
        Block,
        Loop,
        If,
        Else,
        Function,
    };

    struct Frame {
        FunctionType type;
        FrameKind kind;
        size_t initial_size;
        // Stack polymorphism is handled with this field
        bool unreachable { false };

        Vector<ValueType> const& labels() const
        {
            return kind != FrameKind::Loop ? type.results() : type.parameters();
        }
    };

    // Instructions
    struct StackEntry {
        StackEntry(ValueType type)
            : concrete_type(type)
            , is_known(true)
        {
        }

        explicit StackEntry()
            : concrete_type(ValueType::I32)
            , is_known(false)
        {
        }

        bool is_of_kind(ValueType::Kind kind) const
        {
            if (is_known)
                return concrete_type.kind() == kind;
            return true;
        }

        bool is_numeric() const { return !is_known || concrete_type.is_numeric(); }
        bool is_reference() const { return !is_known || concrete_type.is_reference(); }

        bool operator==(ValueType const& other) const
        {
            if (is_known)
                return concrete_type == other;
            return true;
        }

        bool operator==(StackEntry const& other) const
        {
            if (is_known && other.is_known)
                return other.concrete_type == concrete_type;
            return true;
        }

        ValueType concrete_type;
        bool is_known { true };
    };

    // This is a wrapper that can model "polymorphic" stacks,
    // by treating unknown stack entries as a potentially infinite number of entries
    class Stack : private Vector<StackEntry> {
        template<typename, typename>
        friend struct AK::Formatter;

    public:
        Stack(Vector<Frame> const& frames)
            : m_frames(frames)
        {
        }

        using Vector<StackEntry>::is_empty;
        using Vector<StackEntry>::last;
        using Vector<StackEntry>::at;
        using Vector<StackEntry>::size;
        using Vector<StackEntry>::resize;

        ErrorOr<StackEntry, ValidationError> take_last()
        {
            if (size() == m_frames.last().initial_size && m_frames.last().unreachable)
                return StackEntry();
            if (size() == m_frames.last().initial_size)
                return Errors::invalid("stack state"sv, "<any>"sv, "<nothing>"sv);
            return Vector<StackEntry>::take_last();
        }
        void append(StackEntry entry)
        {
            Vector<StackEntry>::append(entry);
        }

        ErrorOr<StackEntry, ValidationError> take(ValueType type, SourceLocation location = SourceLocation::current())
        {
            auto type_on_stack = TRY(take_last());
            if (type_on_stack != type)
                return Errors::invalid("stack state"sv, type, type_on_stack, location);

            return type_on_stack;
        }

        template<auto... kinds>
        ErrorOr<void, ValidationError> take(SourceLocation location = SourceLocation::current())
        {
            for (auto kind : { kinds... })
                TRY(take(Wasm::ValueType(kind), location));
            return {};
        }
        template<auto... kinds>
        ErrorOr<void, ValidationError> take_and_put(Wasm::ValueType::Kind kind, SourceLocation location = SourceLocation::current())
        {
            TRY(take<kinds...>(location));
            append(Wasm::ValueType(kind));
            return {};
        }

        Vector<StackEntry> release_vector() { return exchange(static_cast<Vector<StackEntry>&>(*this), Vector<StackEntry> {}); }

    private:
        Vector<Frame> const& m_frames;
    };

    struct ExpressionTypeResult {
        Vector<StackEntry> result_types;
        bool is_constant { false };
    };
    ErrorOr<ExpressionTypeResult, ValidationError> validate(Expression const&, Vector<ValueType> const&);
    ErrorOr<void, ValidationError> validate(Instruction const& instruction, Stack& stack, bool& is_constant);
    template<u64 opcode>
    ErrorOr<void, ValidationError> validate_instruction(Instruction const&, Stack& stack, bool& is_constant);

    // Types
    ErrorOr<void, ValidationError> validate(Limits const&, u64 bound); // n <= bound && m? <= bound
    ErrorOr<FunctionType, ValidationError> validate(BlockType const&);
    ErrorOr<void, ValidationError> validate(FunctionType const&) { return {}; }
    ErrorOr<void, ValidationError> validate(TableType const&);
    ErrorOr<void, ValidationError> validate(MemoryType const&);
    ErrorOr<void, ValidationError> validate(GlobalType const&) { return {}; }

private:
    explicit Validator(Context context)
        : m_context(move(context))
    {
    }

    struct Errors {
        static ValidationError invalid(StringView name) { return ByteString::formatted("Invalid {}", name); }

        template<typename Expected, typename Given>
        static ValidationError invalid(StringView name, Expected expected, Given given, SourceLocation location = SourceLocation::current())
        {
            if constexpr (WASM_VALIDATOR_DEBUG)
                return ByteString::formatted("Invalid {} in {}, expected {} but got {}", name, find_instruction_name(location), expected, given);
            else
                return ByteString::formatted("Invalid {}, expected {} but got {}", name, expected, given);
        }

        template<typename... Args>
        static ValidationError non_conforming_types(StringView name, Args... args)
        {
            return ByteString::formatted("Non-conforming types for {}: {}", name, Vector { args... });
        }

        static ValidationError duplicate_export_name(StringView name) { return ByteString::formatted("Duplicate exported name '{}'", name); }
        static ValidationError multiple_start_sections() { return ByteString("Found multiple start sections"sv); }
        static ValidationError stack_height_mismatch(Stack const& stack, size_t expected_height) { return ByteString::formatted("Stack height mismatch, got {} but expected length {}", stack, expected_height); }

        template<typename T, typename U, typename V>
        static ValidationError out_of_bounds(StringView name, V value, T min, U max) { return ByteString::formatted("Value {} for {} is out of bounds ({},{})", value, name, min, max); }

        template<typename... Expected>
        static ValidationError invalid_stack_state(Stack const& stack, Tuple<Expected...> expected, SourceLocation location = SourceLocation::current())
        {
            constexpr size_t count = expected.size();
            StringBuilder builder;
            if constexpr (WASM_VALIDATOR_DEBUG)
                builder.appendff("Invalid stack state in {}: ", find_instruction_name(location));
            else
                builder.appendff("Invalid stack state in <unknown>: ");

            builder.append("Expected [ "sv);

            expected.apply_as_args([&]<typename... Ts>(Ts const&... args) {
                (builder.appendff("{} ", args), ...);
            });

            builder.append("], but found [ "sv);

            auto actual_size = stack.size();
            for (size_t i = 1; i <= min(count, actual_size); ++i) {
                auto& entry = stack.at(actual_size - i);
                if (entry.is_known) {
                    builder.appendff("{} ", entry.concrete_type);
                } else {
                    builder.appendff("<polymorphic stack>");
                    break;
                }
            }
            builder.append(']');
            return { builder.to_byte_string() };
        }

    private:
        static ByteString find_instruction_name(SourceLocation const&);
    };

    Context m_context;
    Vector<Frame> m_frames;
    COWVector<GlobalType> m_globals_without_internal_globals;
};

}

template<>
struct AK::Formatter<Wasm::Validator::StackEntry> : public AK::Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Wasm::Validator::StackEntry const& value)
    {
        if (value.is_known)
            return Formatter<StringView>::format(builder, Wasm::ValueType::kind_name(value.concrete_type.kind()));

        return Formatter<StringView>::format(builder, "<unknown>"sv);
    }
};

template<>
struct AK::Formatter<Wasm::Validator::Stack> : public AK::Formatter<Vector<Wasm::Validator::StackEntry>> {
    ErrorOr<void> format(FormatBuilder& builder, Wasm::Validator::Stack const& value)
    {
        return Formatter<Vector<Wasm::Validator::StackEntry>>::format(builder, static_cast<Vector<Wasm::Validator::StackEntry> const&>(value));
    }
};

template<>
struct AK::Formatter<Wasm::ValueType> : public AK::Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Wasm::ValueType const& value)
    {
        return Formatter<StringView>::format(builder, Wasm::ValueType::kind_name(value.kind()));
    }
};

template<>
struct AK::Formatter<Wasm::ValidationError> : public AK::Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Wasm::ValidationError const& error)
    {
        return Formatter<StringView>::format(builder, error.error_string);
    }
};
