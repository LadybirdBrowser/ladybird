/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Utf16FlyString.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibGC/WeakContainer.h>
#include <LibJS/Bytecode/ClassBlueprint.h>
#include <LibJS/Bytecode/IdentifierTable.h>
#include <LibJS/Bytecode/Label.h>
#include <LibJS/Bytecode/Operand.h>
#include <LibJS/Bytecode/PropertyKeyTable.h>
#include <LibJS/Bytecode/StringTable.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/LocalVariable.h>
#include <LibJS/Runtime/EnvironmentCoordinate.h>
#include <LibJS/SourceRange.h>

namespace JS::Bytecode {

// Represents one polymorphic inline cache used for property lookups.
struct PropertyLookupCache {
    static constexpr size_t max_number_of_shapes_to_remember = 4;
    struct Entry {
        enum class Type {
            Empty,
            AddOwnProperty,
            ChangeOwnProperty,
            GetOwnProperty,
            ChangePropertyInPrototypeChain,
            GetPropertyInPrototypeChain,
        };
        u32 property_offset { 0 };
        u32 shape_dictionary_generation { 0 };
        GC::RawPtr<Shape> from_shape;
        GC::RawPtr<Shape> shape;
        GC::RawPtr<Object> prototype;
        GC::RawPtr<PrototypeChainValidity> prototype_chain_validity;
    };

    void update(Entry::Type type, auto callback)
    {
        // First, move all entries one step back.
        for (size_t i = entries.size() - 1; i >= 1; --i) {
            types[i] = types[i - 1];
            entries[i] = entries[i - 1];
        }
        types[0] = type;
        entries[0] = {};
        callback(entries[0]);
    }

    AK::Array<Entry::Type, max_number_of_shapes_to_remember> types;
    AK::Array<Entry, max_number_of_shapes_to_remember> entries;
};

// A PropertyLookupCache for use as a static local variable.
// Registers itself for GC sweep since it's not owned by any Executable.
struct StaticPropertyLookupCache : public PropertyLookupCache {
    StaticPropertyLookupCache();
    static void sweep_all();
};

struct GlobalVariableCache : public PropertyLookupCache {
    u64 environment_serial_number { 0 };
    u32 environment_binding_index { 0 };
    bool has_environment_binding_index { false };
    bool in_module_environment { false };
};

// https://tc39.es/ecma262/#sec-gettemplateobject
// Template objects are cached at the call site.
struct TemplateObjectCache {
    GC::Ptr<Array> cached_template_object;
};

// Cache for object literal shapes.
// When an object literal like {a: 1, b: 2} is instantiated, we cache the final shape
// so that subsequent instantiations can allocate the object with the correct shape directly,
// avoiding repeated shape transitions.
// We also cache the property offsets so that subsequent property writes can bypass
// shape lookups and write directly to the correct storage slot.
struct ObjectShapeCache {
    GC::RawPtr<Shape> shape;
    Vector<u32> property_offsets;
};

enum class ObjectPropertyIteratorFastPath : u8 {
    None,
    PlainNamed,
    PackedIndexed,
};

class JS_API ObjectPropertyIteratorCacheData final : public Cell {
    GC_CELL(ObjectPropertyIteratorCacheData, Cell);
    GC_DECLARE_ALLOCATOR(ObjectPropertyIteratorCacheData);

public:
    ObjectPropertyIteratorCacheData(VM&, Vector<PropertyKey>, ObjectPropertyIteratorFastPath, u32 indexed_property_count, bool receiver_has_magical_length_property, GC::Ref<Shape>, GC::Ptr<PrototypeChainValidity> = nullptr);
    virtual ~ObjectPropertyIteratorCacheData() override = default;

    [[nodiscard]] ReadonlySpan<PropertyKey> properties() const { return m_properties.span(); }
    [[nodiscard]] ReadonlySpan<Value> property_values() const { return m_property_values.span(); }
    [[nodiscard]] ObjectPropertyIteratorFastPath fast_path() const { return m_fast_path; }
    [[nodiscard]] u32 indexed_property_count() const { return m_indexed_property_count; }
    [[nodiscard]] bool receiver_has_magical_length_property() const { return m_receiver_has_magical_length_property; }
    [[nodiscard]] GC::Ptr<Shape> shape() const { return m_shape; }
    [[nodiscard]] GC::Ptr<PrototypeChainValidity> prototype_chain_validity() const { return m_prototype_chain_validity; }
    [[nodiscard]] u32 shape_dictionary_generation() const { return m_shape_dictionary_generation; }

private:
    virtual void visit_edges(Visitor&) override;
    virtual size_t external_memory_size() const override;

    Vector<PropertyKey> m_properties;
    Vector<Value> m_property_values;
    GC::Ptr<Shape> m_shape;
    GC::Ptr<PrototypeChainValidity> m_prototype_chain_validity;
    u32 m_indexed_property_count { 0 };
    u32 m_shape_dictionary_generation { 0 };
    bool m_receiver_has_magical_length_property { false };
    ObjectPropertyIteratorFastPath m_fast_path { ObjectPropertyIteratorFastPath::None };
};

struct ObjectPropertyIteratorCache {
    GC::Ptr<ObjectPropertyIteratorCacheData> data;
    GC::Ptr<Object> reusable_property_name_iterator;
};

struct SourceRecord {
    Position start {};
    Position end {};
};

struct SourceMapEntry {
    u32 bytecode_offset {};
    SourceRecord source_record {};
};

class JS_API Executable final
    : public Cell
    , public GC::WeakContainer {
    GC_CELL(Executable, Cell);
    GC_DECLARE_ALLOCATOR(Executable);

public:
    Executable(
        Vector<u8> bytecode,
        NonnullOwnPtr<IdentifierTable>,
        NonnullOwnPtr<PropertyKeyTable>,
        NonnullOwnPtr<StringTable>,
        NonnullOwnPtr<RegexTable>,
        Vector<Value> constants,
        NonnullRefPtr<SourceCode const>,
        size_t number_of_property_lookup_caches,
        size_t number_of_global_variable_caches,
        size_t number_of_template_object_caches,
        size_t number_of_object_shape_caches,
        size_t number_of_object_property_iterator_caches,
        size_t number_of_registers,
        Strict);

    virtual ~Executable() override;

    Utf16FlyString name;
    Vector<u8> bytecode;
    Vector<PropertyLookupCache> property_lookup_caches;
    Vector<GlobalVariableCache> global_variable_caches;
    Vector<TemplateObjectCache> template_object_caches;
    Vector<ObjectShapeCache> object_shape_caches;
    Vector<ObjectPropertyIteratorCache> object_property_iterator_caches;
    NonnullOwnPtr<StringTable> string_table;
    NonnullOwnPtr<IdentifierTable> identifier_table;
    NonnullOwnPtr<PropertyKeyTable> property_key_table;
    NonnullOwnPtr<RegexTable> regex_table;
    Vector<Value> constants;

    Vector<GC::Ptr<SharedFunctionInstanceData>> shared_function_data;
    Vector<ClassBlueprint> class_blueprints;

    NonnullRefPtr<SourceCode const> source_code;
    u32 number_of_registers { 0 };
    u32 number_of_arguments { 0 };
    bool is_strict_mode { false };

    u32 registers_and_locals_count { 0 };
    u32 registers_and_locals_and_constants_count { 0 };
    size_t asm_constants_size { 0 };
    Value const* asm_constants_data { nullptr };

    struct ExceptionHandlers {
        size_t start_offset;
        size_t end_offset;
        size_t handler_offset;
    };

    Vector<ExceptionHandlers> exception_handlers;
    Vector<size_t> basic_block_start_offsets;

    Vector<SourceMapEntry> source_map;

    Vector<LocalVariable> local_variable_names;
    u32 local_index_base { 0 };
    u32 argument_index_base { 0 };

    Optional<PropertyKeyTableIndex> length_identifier;

    Utf16String const& get_string(StringTableIndex index) const { return string_table->get(index); }
    Utf16FlyString const& get_identifier(IdentifierTableIndex index) const { return identifier_table->get(index); }
    PropertyKey const& get_property_key(PropertyKeyTableIndex index) const { return property_key_table->get(index); }

    Optional<Utf16FlyString const&> get_identifier(Optional<IdentifierTableIndex> const& index) const
    {
        if (!index.has_value())
            return {};
        return get_identifier(*index);
    }

    [[nodiscard]] COLD Optional<ExceptionHandlers const&> exception_handlers_for_offset(size_t offset) const;

    [[nodiscard]] Optional<SourceRange> source_range_at(size_t offset) const;

    [[nodiscard]] SourceRange const& get_source_range(u32 program_counter);

    void fixup_cache_pointers();

    void dump() const;
    [[nodiscard]] String dump_to_string() const;

    [[nodiscard]] Operand original_operand_from_raw(u32) const;

    virtual void remove_dead_cells(Badge<GC::Heap>) override;

private:
    virtual void visit_edges(Visitor&) override;
    virtual size_t external_memory_size() const override;

    HashMap<u32, SourceRange> m_source_range_cache;
};

}
