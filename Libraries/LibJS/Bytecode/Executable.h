/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Utf16FlyString.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibCore/ImmutableBytes.h>
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
#include <LibJS/Runtime/EnvironmentCoordinate.h>
#include <LibJS/SourceRange.h>

namespace JS::Bytecode {

class InstructionStream {
public:
    explicit InstructionStream(Vector<u8>);
    InstructionStream(Core::ImmutableBytes, size_t offset, size_t size);

    [[nodiscard]] ReadonlyBytes span() const LIFETIME_BOUND { return { m_data, m_size }; }
    [[nodiscard]] u8 const* data() const LIFETIME_BOUND { return m_data; }
    [[nodiscard]] size_t size() const { return m_size; }
    [[nodiscard]] size_t external_memory_size() const;
    [[nodiscard]] u8 operator[](size_t index) const { return m_data[index]; }

    operator ReadonlyBytes() const LIFETIME_BOUND { return span(); }

    static constexpr size_t data_member_offset() { return offsetof(InstructionStream, m_data); }

private:
    void update_view_from_storage(size_t offset = 0, Optional<size_t> size = {});

    Variant<Vector<u8>, Core::ImmutableBytes> m_storage;
    u8 const* m_data { nullptr };
    size_t m_size { 0 };
};

// Represents one tiered inline cache used for property lookups.
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
        Type type { Type::Empty };
        u32 property_offset { 0 };
        u32 shape_dictionary_generation { 0 };
        GC::RawPtr<Shape> from_shape;
        GC::RawPtr<Shape> shape;
        GC::RawPtr<Object> prototype;
        GC::RawPtr<PrototypeChainValidity> prototype_chain_validity;
    };

    struct MonomorphicData {
        Entry entry;
    };

    struct PolymorphicData {
        AK::Array<Entry, max_number_of_shapes_to_remember> entries;
    };

    PropertyLookupCache() = default;
    PropertyLookupCache(PropertyLookupCache const&) = delete;
    PropertyLookupCache& operator=(PropertyLookupCache const&) = delete;
    PropertyLookupCache(PropertyLookupCache&&);
    PropertyLookupCache& operator=(PropertyLookupCache&&);
    ~PropertyLookupCache();

    [[nodiscard]] Entry* first_entry();
    [[nodiscard]] Entry const* first_entry() const;
    [[nodiscard]] Span<Entry> entries();
    [[nodiscard]] ReadonlySpan<Entry> entries() const;
    [[nodiscard]] size_t external_memory_size() const;
    void copy_from(PropertyLookupCache const&);

    void update(Entry::Type type, auto callback)
    {
        Entry new_entry;
        new_entry.type = type;
        callback(new_entry);

        if (!m_data) {
            auto data = make<MonomorphicData>();
            data->entry = new_entry;
            set_monomorphic_data(data.leak_ptr());
            return;
        }

        if (auto* data = monomorphic_data()) {
            if (entries_have_same_cache_key(data->entry, new_entry)) {
                data->entry = new_entry;
                return;
            }

            auto old_entry = data->entry;
            auto new_data = make<PolymorphicData>();
            new_data->entries[0] = new_entry;
            new_data->entries[1] = old_entry;
            clear();
            set_polymorphic_data(new_data.leak_ptr());
            return;
        }

        auto& entries = polymorphic_data()->entries;
        size_t insertion_index = entries.size() - 1;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries_have_same_cache_key(entries[i], new_entry)) {
                insertion_index = i;
                break;
            }
        }

        for (size_t i = insertion_index; i > 0; --i)
            entries[i] = entries[i - 1];
        entries[0] = new_entry;
    }

    void clear();

    static constexpr FlatPtr polymorphic_data_tag = 1;
    FlatPtr m_data { 0 };

private:
    [[nodiscard]] MonomorphicData* monomorphic_data();
    [[nodiscard]] MonomorphicData const* monomorphic_data() const;
    [[nodiscard]] PolymorphicData* polymorphic_data();
    [[nodiscard]] PolymorphicData const* polymorphic_data() const;
    void set_monomorphic_data(MonomorphicData*);
    void set_polymorphic_data(PolymorphicData*);
    static bool entries_have_same_cache_key(Entry const&, Entry const&);
};

// A PropertyLookupCache for use as a static local variable.
// Registers itself for GC sweep since it's not owned by any Executable.
struct StaticPropertyLookupCache : public PropertyLookupCache {
    StaticPropertyLookupCache();
    static void sweep_all();
};

struct GlobalVariableCache {
    PropertyLookupCache::Entry* first_entry()
    {
        if (entry.type == PropertyLookupCache::Entry::Type::Empty)
            return nullptr;
        return &entry;
    }

    PropertyLookupCache::Entry const* first_entry() const
    {
        if (entry.type == PropertyLookupCache::Entry::Type::Empty)
            return nullptr;
        return &entry;
    }

    void update(PropertyLookupCache::Entry::Type type, auto callback)
    {
        entry = {};
        entry.type = type;
        callback(entry);
    }

    PropertyLookupCache::Entry entry;
    u64 environment_serial_number { 0 };
    u32 environment_binding_index { 0 };
    bool has_environment_binding_index { false };
    bool in_module_environment { false };
};

// https://tc39.es/ecma262/#sec-gettemplateobject
// Template objects are cached at the call site.
class JS_API TemplateObjectCache final : public Cell {
    GC_CELL(TemplateObjectCache, Cell);
    GC_DECLARE_ALLOCATOR(TemplateObjectCache);

public:
    virtual ~TemplateObjectCache() override = default;

    GC::Ptr<Array> cached_template_object;

private:
    virtual void visit_edges(Visitor&) override;
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

struct SourceMapEntry {
    u32 bytecode_offset {};
    u32 line {};
    u32 column {};
};

class JS_API Executable final
    : public Cell
    , public GC::WeakContainer {
    GC_CELL(Executable, Cell);
    GC_DECLARE_ALLOCATOR(Executable);

public:
    Executable(
        InstructionStream bytecode,
        NonnullOwnPtr<IdentifierTable>,
        NonnullOwnPtr<PropertyKeyTable>,
        NonnullOwnPtr<StringTable>,
        NonnullOwnPtr<RegexTable>,
        Vector<Value> constants,
        NonnullRefPtr<SourceCode const>,
        size_t number_of_property_lookup_caches,
        size_t number_of_global_variable_caches,
        size_t number_of_environment_coordinate_caches,
        size_t number_of_template_object_caches,
        size_t number_of_object_shape_caches,
        size_t number_of_object_property_iterator_caches,
        size_t number_of_registers,
        Strict);

    virtual ~Executable() override;

    Utf16FlyString name;
    InstructionStream bytecode;
    Vector<PropertyLookupCache> property_lookup_caches;
    Vector<GlobalVariableCache> global_variable_caches;
    Vector<EnvironmentCoordinate> environment_coordinate_caches;
    Vector<GC::Ref<TemplateObjectCache>> template_object_caches;
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

    Vector<SourceMapEntry> source_map;

    Vector<Utf16FlyString> local_variable_names;
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

    void copy_runtime_caches_from(Executable const&);
    [[nodiscard]] COLD Optional<ExceptionHandlers const&> exception_handlers_for_offset(size_t offset) const;

    [[nodiscard]] Optional<SourceRange> source_range_at(size_t offset) const;

    [[nodiscard]] SourceRange const& get_source_range(u32 program_counter);

    void dump() const;

    virtual Cell const& owner_cell(Badge<GC::Heap>) const override { return *this; }
    virtual void remove_dead_cells(Badge<GC::Heap>) override;

private:
    virtual void visit_edges(Visitor&) override;
    virtual size_t external_memory_size() const override;

    HashMap<u32, SourceRange> m_source_range_cache;
};

}
