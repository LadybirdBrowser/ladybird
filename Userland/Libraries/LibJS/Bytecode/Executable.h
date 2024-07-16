/*
 * Copyright (c) 2021-2024, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DeprecatedFlyString.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/WeakPtr.h>
#include <LibJS/Bytecode/IdentifierTable.h>
#include <LibJS/Bytecode/Label.h>
#include <LibJS/Bytecode/StringTable.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/CellAllocator.h>
#include <LibJS/Runtime/EnvironmentCoordinate.h>
#include <LibJS/SourceRange.h>

namespace JS::Bytecode {

struct PropertyLookupCache {
    WeakPtr<Shape> shape;
    Optional<u32> property_offset;
    WeakPtr<Object> prototype;
    WeakPtr<PrototypeChainValidity> prototype_chain_validity;
};

struct GlobalVariableCache : public PropertyLookupCache {
    u64 environment_serial_number { 0 };
    Optional<u32> environment_binding_index;
};

struct SourceRecord {
    u32 source_start_offset {};
    u32 source_end_offset {};
};

class Executable final : public Cell {
    JS_CELL(Executable, Cell);
    JS_DECLARE_ALLOCATOR(Executable);

public:
    Executable(
        Vector<u8> bytecode,
        NonnullOwnPtr<IdentifierTable>,
        NonnullOwnPtr<StringTable>,
        NonnullOwnPtr<RegexTable>,
        Vector<Value> constants,
        NonnullRefPtr<SourceCode const>,
        size_t number_of_property_lookup_caches,
        size_t number_of_global_variable_caches,
        size_t number_of_registers,
        bool is_strict_mode);

    virtual ~Executable() override;

    DeprecatedFlyString name;
    Vector<u8> bytecode;
    Vector<PropertyLookupCache> property_lookup_caches;
    Vector<GlobalVariableCache> global_variable_caches;
    NonnullOwnPtr<StringTable> string_table;
    NonnullOwnPtr<IdentifierTable> identifier_table;
    NonnullOwnPtr<RegexTable> regex_table;
    Vector<Value> constants;

    NonnullRefPtr<SourceCode const> source_code;
    size_t number_of_registers { 0 };
    bool is_strict_mode { false };

    struct ExceptionHandlers {
        size_t start_offset;
        size_t end_offset;
        Optional<size_t> handler_offset;
        Optional<size_t> finalizer_offset;
    };

    Vector<ExceptionHandlers> exception_handlers;
    Vector<size_t> basic_block_start_offsets;

    HashMap<size_t, SourceRecord> source_map;

    Vector<DeprecatedFlyString> local_variable_names;
    size_t local_index_base { 0 };

    Optional<IdentifierTableIndex> length_identifier;

    ByteString const& get_string(StringTableIndex index) const { return string_table->get(index); }
    DeprecatedFlyString const& get_identifier(IdentifierTableIndex index) const { return identifier_table->get(index); }

    Optional<DeprecatedFlyString const&> get_identifier(Optional<IdentifierTableIndex> const& index) const
    {
        if (!index.has_value())
            return {};
        return get_identifier(*index);
    }

    [[nodiscard]] Optional<ExceptionHandlers const&> exception_handlers_for_offset(size_t offset) const;

    [[nodiscard]] UnrealizedSourceRange source_range_at(size_t offset) const;

    void dump() const;

private:
    virtual void visit_edges(Visitor&) override;
};

}
