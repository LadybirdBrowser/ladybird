/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/COWVector.h>
#include <AK/Debug.h>
#include <AK/HashTable.h>
#include <AK/SourceLocation.h>
#include <AK/Tuple.h>
#include <AK/Vector.h>
#include <LibWasm/Forward.h>
#include <LibWasm/Types.h>

namespace Wasm {

struct Context {
    COWVector<FunctionType> types;
    COWVector<FunctionType> functions;
    COWVector<TableType> tables;
    COWVector<MemoryType> memories;
    COWVector<GlobalType> globals;
    COWVector<ValueType> elements;
    COWVector<bool> datas;
    COWVector<ValueType> locals;
    COWVector<ResultType> labels;
    Optional<u32> data_count;
    Optional<ResultType> return_;
    AK::HashTable<FunctionIndex> references;
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
        if (index.value() < m_context.labels.size())
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
        // The unknown entry will never be popped off, so we can safely use the original `is_empty`.
        using Vector<StackEntry>::is_empty;
        using Vector<StackEntry>::last;
        using Vector<StackEntry>::at;

        StackEntry take_last()
        {
            if (last().is_known)
                return Vector<StackEntry>::take_last();
            return last();
        }
        void append(StackEntry entry)
        {
            if (!entry.is_known)
                m_did_insert_unknown_entry = true;
            Vector<StackEntry>::append(entry);
        }

        ErrorOr<void, ValidationError> take(ValueType type, SourceLocation location = SourceLocation::current())
        {
            if (is_empty())
                return Errors::invalid("stack state"sv, type, "<nothing>"sv, location);

            auto type_on_stack = take_last();
            if (type_on_stack != type)
                return Errors::invalid("stack state"sv, type, type_on_stack, location);

            return {};
        }

        template<auto... kinds>
        ErrorOr<void, ValidationError> take(SourceLocation location = SourceLocation::current())
        {
            ErrorOr<void, ValidationError> result;
            if (((result = take(Wasm::ValueType(kinds), location)).is_error(), ...)) {
                return result;
            }
            return result;
        }

        template<auto... kinds>
        ErrorOr<void, ValidationError> take_and_put(Wasm::ValueType::Kind kind, SourceLocation location = SourceLocation::current())
        {
            ErrorOr<void, ValidationError> result;
            if (((result = take(Wasm::ValueType(kinds), location)).is_error(), ...)) {
                return result;
            }
            append(Wasm::ValueType(kind));
            return result;
        }

        size_t actual_size() const { return Vector<StackEntry>::size(); }
        size_t size() const { return m_did_insert_unknown_entry ? static_cast<size_t>(-1) : actual_size(); }

        Vector<StackEntry> release_vector() { return exchange(static_cast<Vector<StackEntry>&>(*this), Vector<StackEntry> {}); }

        bool operator==(Stack const& other) const;

    private:
        bool m_did_insert_unknown_entry { false };
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

            auto actual_size = stack.actual_size();
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

    enum class ChildScopeKind {
        Block,
        IfWithoutElse,
        IfWithElse,
        Else,
    };

    struct BlockDetails {
        size_t initial_stack_size { 0 };
        struct IfDetails {
            Stack initial_stack;
        };
        Variant<IfDetails, Empty> details;
    };

    Context m_context;
    Vector<Context> m_parent_contexts;
    Vector<ChildScopeKind> m_entered_scopes;
    Vector<BlockDetails> m_block_details;
    Vector<FunctionType> m_entered_blocks;
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
