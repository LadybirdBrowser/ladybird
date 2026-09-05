/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/HashFunctions.h>
#include <AK/NeverDestroyed.h>
#include <AK/NumericLimits.h>
#include <AK/StdLibExtras.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapBlock.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/Op.h>
#include <LibJS/Bytecode/RegexTable.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/RustIntegration.h>
#include <LibJS/SourceCode.h>

namespace JS::Bytecode {

GC_DEFINE_ALLOCATOR(Executable);
GC_DEFINE_ALLOCATOR(TemplateObjectCache);
GC_DEFINE_ALLOCATOR(ObjectPropertyIteratorCacheData);

InstructionStream::InstructionStream(Vector<u8> bytecode)
    : m_storage(MUST(Core::ImmutableBytes::copy_to_readonly_mapping(bytecode.span())))
{
    // Keep each fresh instruction stream in its own read-only mapping. This deliberately costs one VMA
    // and page-rounded allocation per Executable. Packing streams into a shared arena would prevent us
    // from sealing each stream immediately because memory protection is page-granular.
    VERIFY(bytecode.is_empty() || m_storage.is_readonly_mapped());
    update_view_from_storage();
}

InstructionStream::InstructionStream(Core::ImmutableBytes bytecode, size_t offset, size_t size)
    : m_storage(move(bytecode))
{
    update_view_from_storage(offset, size);
}

void InstructionStream::update_view_from_storage(size_t offset, Optional<size_t> size)
{
    auto bytes = m_storage.bytes();

    VERIFY(offset <= bytes.size());
    m_size = size.value_or(bytes.size() - offset);
    VERIFY(m_size <= bytes.size() - offset);
    m_data = bytes.is_empty() ? nullptr : bytes.data() + offset;
}

size_t InstructionStream::external_memory_size() const
{
    if (m_storage.is_file_backed())
        return 0;
    return m_size;
}

static_assert(alignof(PropertyLookupCache::MonomorphicData) > PropertyLookupCache::cache_data_tag_mask);
static_assert(alignof(PropertyLookupCache::PolymorphicData) > PropertyLookupCache::cache_data_tag_mask);
static_assert(alignof(PropertyLookupCache::MegamorphicData) > PropertyLookupCache::cache_data_tag_mask);
static_assert(offsetof(PropertyLookupCache::MonomorphicData, entry) == 0);
static_assert(offsetof(PropertyLookupCache::PolymorphicData, entries) == 0);
static_assert(offsetof(PropertyLookupCache::MegamorphicData, entry) == 0);
static_assert((PropertyLookupCache::megamorphic_primary_cache_size & (PropertyLookupCache::megamorphic_primary_cache_size - 1)) == 0);
static_assert((PropertyLookupCache::megamorphic_secondary_cache_size & (PropertyLookupCache::megamorphic_secondary_cache_size - 1)) == 0);

PropertyLookupCache::PropertyLookupCache(PropertyLookupCache&& other)
    : m_data(exchange(other.m_data, 0))
{
}

PropertyLookupCache& PropertyLookupCache::operator=(PropertyLookupCache&& other)
{
    if (this != &other) {
        clear();
        m_data = exchange(other.m_data, 0);
    }
    return *this;
}

PropertyLookupCache::~PropertyLookupCache()
{
    clear();
}

PropertyLookupCache::MonomorphicData* PropertyLookupCache::monomorphic_data()
{
    if (!m_data || (m_data & cache_data_tag_mask))
        return nullptr;
    return reinterpret_cast<MonomorphicData*>(m_data);
}

PropertyLookupCache::MonomorphicData const* PropertyLookupCache::monomorphic_data() const
{
    if (!m_data || (m_data & cache_data_tag_mask))
        return nullptr;
    return reinterpret_cast<MonomorphicData const*>(m_data);
}

PropertyLookupCache::PolymorphicData* PropertyLookupCache::polymorphic_data()
{
    if ((m_data & cache_data_tag_mask) != polymorphic_data_tag)
        return nullptr;
    return reinterpret_cast<PolymorphicData*>(m_data & ~cache_data_tag_mask);
}

PropertyLookupCache::PolymorphicData const* PropertyLookupCache::polymorphic_data() const
{
    if ((m_data & cache_data_tag_mask) != polymorphic_data_tag)
        return nullptr;
    return reinterpret_cast<PolymorphicData const*>(m_data & ~cache_data_tag_mask);
}

PropertyLookupCache::MegamorphicData* PropertyLookupCache::megamorphic_data()
{
    if ((m_data & cache_data_tag_mask) != megamorphic_data_tag)
        return nullptr;
    return reinterpret_cast<MegamorphicData*>(m_data & ~cache_data_tag_mask);
}

PropertyLookupCache::MegamorphicData const* PropertyLookupCache::megamorphic_data() const
{
    if ((m_data & cache_data_tag_mask) != megamorphic_data_tag)
        return nullptr;
    return reinterpret_cast<MegamorphicData const*>(m_data & ~cache_data_tag_mask);
}

void PropertyLookupCache::set_monomorphic_data(MonomorphicData* data)
{
    VERIFY(data);
    VERIFY(!(reinterpret_cast<FlatPtr>(data) & cache_data_tag_mask));
    m_data = reinterpret_cast<FlatPtr>(data);
}

void PropertyLookupCache::set_polymorphic_data(PolymorphicData* data)
{
    VERIFY(data);
    VERIFY(!(reinterpret_cast<FlatPtr>(data) & cache_data_tag_mask));
    m_data = reinterpret_cast<FlatPtr>(data) | polymorphic_data_tag;
}

void PropertyLookupCache::set_megamorphic_data(MegamorphicData* data)
{
    VERIFY(data);
    VERIFY(!(reinterpret_cast<FlatPtr>(data) & cache_data_tag_mask));
    m_data = reinterpret_cast<FlatPtr>(data) | megamorphic_data_tag;
}

PropertyLookupCache::Entry* PropertyLookupCache::first_entry()
{
    if (auto* data = monomorphic_data())
        return &data->entry;
    if (auto* data = polymorphic_data())
        return &data->entries[0];
    if (auto* data = megamorphic_data())
        return &data->entry;
    return nullptr;
}

PropertyLookupCache::Entry const* PropertyLookupCache::first_entry() const
{
    if (auto* data = monomorphic_data())
        return &data->entry;
    if (auto* data = polymorphic_data())
        return &data->entries[0];
    if (auto* data = megamorphic_data())
        return &data->entry;
    return nullptr;
}

Span<PropertyLookupCache::Entry> PropertyLookupCache::entries()
{
    if (auto* data = monomorphic_data())
        return { &data->entry, 1 };
    if (auto* data = polymorphic_data())
        return data->entries.span();
    if (auto* data = megamorphic_data())
        return { &data->entry, 1 };
    return {};
}

ReadonlySpan<PropertyLookupCache::Entry> PropertyLookupCache::entries() const
{
    if (auto* data = monomorphic_data())
        return { &data->entry, 1 };
    if (auto* data = polymorphic_data())
        return data->entries.span();
    if (auto* data = megamorphic_data())
        return { &data->entry, 1 };
    return {};
}

Span<PropertyLookupCache::Entry> PropertyLookupCache::entries_for_shape(Shape const& shape)
{
    auto* data = megamorphic_data();
    if (!data)
        return entries();

    auto const find_entry = [&](auto& entries, size_t index) -> Entry* {
        auto& entry = entries[index];
        if (entry_lookup_shape(entry).ptr() != &shape)
            return nullptr;
        return &entry;
    };

    auto* entry = find_entry(data->primary_entries, megamorphic_primary_index(shape));
    if (!entry)
        entry = find_entry(data->secondary_entries, megamorphic_secondary_index(shape));
    if (!entry)
        return {};

    data->entry = *entry;
    return { &data->entry, 1 };
}

size_t PropertyLookupCache::external_memory_size() const
{
    if (monomorphic_data())
        return sizeof(MonomorphicData);
    if (polymorphic_data())
        return sizeof(PolymorphicData);
    if (megamorphic_data())
        return sizeof(MegamorphicData);
    return 0;
}

void PropertyLookupCache::copy_from(PropertyLookupCache const& other)
{
    clear();
    if (auto* data = other.monomorphic_data()) {
        set_monomorphic_data(new MonomorphicData(*data));
        return;
    }
    if (auto* data = other.polymorphic_data()) {
        set_polymorphic_data(new PolymorphicData(*data));
        return;
    }
    if (auto* data = other.megamorphic_data())
        set_megamorphic_data(new MegamorphicData(*data));
}

void PropertyLookupCache::clear()
{
    if (auto* data = monomorphic_data()) {
        delete data;
        m_data = 0;
        return;
    }
    if (auto* data = polymorphic_data()) {
        delete data;
        m_data = 0;
        return;
    }
    if (auto* data = megamorphic_data()) {
        delete data;
        m_data = 0;
    }
}

bool PropertyLookupCache::entries_have_same_cache_key(Entry const& a, Entry const& b)
{
    if (a.type == Entry::Type::Empty || b.type == Entry::Type::Empty)
        return false;
    if (a.type != b.type)
        return false;

    switch (a.type) {
    case Entry::Type::AddOwnProperty:
        return a.from_shape == b.from_shape && a.shape == b.shape;
    case Entry::Type::ChangeOwnProperty:
    case Entry::Type::GetOwnProperty:
    case Entry::Type::GetMissingProperty:
        return a.shape == b.shape;
    case Entry::Type::ChangePropertyInPrototypeChain:
    case Entry::Type::GetPropertyInPrototypeChain:
        return a.shape == b.shape && a.prototype == b.prototype;
    case Entry::Type::Empty:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

ObjectPropertyIteratorCacheData::ObjectPropertyIteratorCacheData(VM& vm, Vector<PropertyKey> properties, ObjectPropertyIteratorFastPath fast_path, u32 indexed_property_count, bool receiver_has_magical_length_property, GC::Ref<Shape> shape, GC::Ptr<PrototypeChainValidity> prototype_chain_validity)
    : m_properties(move(properties))
    , m_shape(shape)
    , m_prototype_chain_validity(prototype_chain_validity)
    , m_indexed_property_count(indexed_property_count)
    , m_receiver_has_magical_length_property(receiver_has_magical_length_property)
    , m_fast_path(fast_path)
{
    // The iterator fast path returns JS Values directly, so materialize the
    // cached key list once up front instead of converting PropertyKeys during
    // every ObjectPropertyIteratorNext.
    m_property_values.ensure_capacity(indexed_property_count + m_properties.size());
    for (u32 i = 0; i < indexed_property_count; ++i)
        m_property_values.append(PropertyKey { i }.to_value(vm));
    for (auto const& key : m_properties)
        m_property_values.append(key.to_value(vm));

    if (m_shape->is_dictionary())
        m_shape_dictionary_generation = m_shape->dictionary_generation();
}

void ObjectPropertyIteratorCacheData::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_shape);
    visitor.visit(m_prototype_chain_validity);
    visitor.visit(m_property_values.span());
    for (auto& key : m_properties)
        key.visit_edges(visitor);
}

size_t ObjectPropertyIteratorCacheData::external_memory_size() const
{
    auto size = vector_external_memory_size(m_properties);
    size = saturating_add_external_memory_size(size, vector_external_memory_size(m_property_values));
    return size;
}

void TemplateObjectCache::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(cached_template_object);
}

Executable::Executable(
    InstructionStream bytecode,
    NonnullOwnPtr<IdentifierTable> identifier_table,
    NonnullOwnPtr<PropertyKeyTable> property_key_table,
    NonnullOwnPtr<StringTable> string_table,
    NonnullOwnPtr<RegexTable> regex_table,
    Vector<Value> constants,
    NonnullRefPtr<SourceCode const> source_code,
    size_t number_of_property_lookup_caches,
    size_t number_of_global_variable_caches,
    size_t number_of_environment_coordinate_caches,
    size_t number_of_template_object_caches,
    size_t number_of_object_shape_caches,
    size_t number_of_object_property_iterator_caches,
    size_t number_of_registers,
    Strict strict)
    : GC::WeakContainer(heap())
    , bytecode(move(bytecode))
    , string_table(move(string_table))
    , identifier_table(move(identifier_table))
    , property_key_table(move(property_key_table))
    , regex_table(move(regex_table))
    , constants(move(constants))
    , source_code(move(source_code))
    , number_of_registers(number_of_registers)
    , is_strict_mode(strict == Strict::Yes)
{
    property_lookup_caches.resize(number_of_property_lookup_caches);
    global_variable_caches.resize(number_of_global_variable_caches);
    environment_coordinate_caches.resize(number_of_environment_coordinate_caches);
    template_object_caches.ensure_capacity(number_of_template_object_caches);
    for (size_t i = 0; i < number_of_template_object_caches; ++i)
        template_object_caches.append(heap().allocate<TemplateObjectCache>());
    object_shape_caches.resize(number_of_object_shape_caches);
    object_property_iterator_caches.resize(number_of_object_property_iterator_caches);
    asm_constants_size = this->constants.size();
    asm_constants_data = this->constants.data();
}

Executable::~Executable() = default;

static SourceMapEntry const* first_real_source_map_entry(Executable const& executable)
{
    SourceMapEntry const* first_entry = nullptr;
    for (auto const& entry : executable.source_map) {
        if (entry.line == 0 && entry.column == 0)
            continue;
        if (!first_entry || entry.line < first_entry->line || (entry.line == first_entry->line && entry.column < first_entry->column))
            first_entry = &entry;
    }
    return first_entry;
}

static void dump_header(StringBuilder& output, Executable const& executable)
{
    auto constexpr white_bold = "\033[37;1m"sv;
    auto constexpr reset = "\033[0m"sv;
    auto const* first_source_map_entry = first_real_source_map_entry(executable);

    u32 hash = 2166136261u; // FNV-1a offset basis
    auto update_hash = [&](u32 value) {
        for (size_t i = 0; i < sizeof(value); ++i) {
            hash ^= (value >> (i * 8)) & 0xFF;
            hash *= 16777619u;
        }
    };
    auto update_hash_with_code_unit = [&](u16 code_unit) {
        hash ^= code_unit & 0xFF;
        hash *= 16777619u;
        hash ^= (code_unit >> 8) & 0xFF;
        hash *= 16777619u;
    };

    auto name_view = executable.name.view();
    for (size_t i = 0; i < name_view.length_in_code_units(); ++i)
        update_hash_with_code_unit(name_view.code_unit_at(i));
    if (first_source_map_entry) {
        update_hash(first_source_map_entry->line);
        update_hash(first_source_map_entry->column);
    }
    update_hash(static_cast<u32>(min(executable.bytecode.size(), static_cast<size_t>(NumericLimits<u32>::max()))));

    if (executable.name.is_empty())
        output.appendff("{}${:08x}{}", white_bold, hash, reset);
    else
        output.appendff("{}{}${:08x}{}", white_bold, executable.name, hash, reset);

    // Show source location if available.
    if (first_source_map_entry) {
        auto filename = executable.source_code->filename().utf16_view();
        if (!filename.is_empty()) {
            // Show just the basename to keep output portable across machines.
            Optional<size_t> last_slash;
            for (size_t i = 0; i < filename.length_in_code_units(); ++i) {
                if (filename.code_unit_at(i) == '/')
                    last_slash = i;
            }
            if (last_slash.has_value())
                filename = filename.substring_view(last_slash.value() + 1);
            output.appendff(" {}:{}:{}", filename, first_source_map_entry->line, first_source_map_entry->column);
        } else {
            output.appendff(" line {}, column {}", first_source_map_entry->line, first_source_map_entry->column);
        }
    }
    output.append('\n');
}

static void dump_metadata(StringBuilder& output, Executable const& executable)
{
    auto constexpr green = "\033[32m"sv;
    auto constexpr yellow = "\033[33m"sv;
    auto constexpr blue = "\033[34m"sv;
    auto constexpr cyan = "\033[36m"sv;
    auto constexpr reset = "\033[0m"sv;

    output.appendff("  {}Registers{}: {}\n", green, reset, executable.number_of_registers);
    output.appendff("  {}Blocks{}:    {}\n", green, reset, RustIntegration::count_bytecode_basic_blocks(executable));

    if (!executable.local_variable_names.is_empty()) {
        output.appendff("  {}Locals{}:    ", green, reset);
        for (size_t i = 0; i < executable.local_variable_names.size(); ++i) {
            if (i != 0)
                output.append(", "sv);
            output.appendff("{}{}~{}{}", blue, executable.local_variable_names[i], i, reset);
        }
        output.append('\n');
    }

    if (!executable.constants.is_empty()) {
        output.appendff("  {}Constants{}:\n", green, reset);
        for (size_t i = 0; i < executable.constants.size(); ++i) {
            auto value = executable.constants[i];
            output.append("    "sv);
            output.appendff("{}[{}]{} = ", yellow, i, reset);
            output.append(cyan);
            if (value.is_special_empty_value())
                output.append("<Empty>"sv);
            else if (value.is_boolean())
                output.appendff("Bool({})", value.as_bool() ? "true"sv : "false"sv);
            else if (value.is_int32())
                output.appendff("Int32({})", value.as_i32());
            else if (value.is_double())
                output.appendff("Double({})", value.as_double());
            else if (value.is_bigint())
                output.appendff("BigInt({})", value.as_bigint().to_utf16_string());
            else if (value.is_string())
                output.appendff("String(\"{}\")", value.as_string().utf16_string_view());
            else if (value.is_undefined())
                output.append("Undefined"sv);
            else if (value.is_null())
                output.append("Null"sv);
            else
                output.appendff("Value({})", value);
            output.append(reset);
            output.append('\n');
        }
    }
}

void Executable::dump() const
{
    StringBuilder output;

    dump_header(output, *this);
    dump_metadata(output, *this);
    output.append('\n');
    RustIntegration::dump_bytecode(output, *this);

    output.append('\n');
    warnln("{}", output.string_view());
}

void Executable::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(constants);
    visitor.visit(template_object_caches);
    for (auto& cache : object_property_iterator_caches)
        visitor.visit(cache.data);
    for (auto& cache : object_property_iterator_caches)
        visitor.visit(cache.reusable_property_name_iterator);
    for (auto& data : shared_function_data)
        visitor.visit(data);
    for (auto& blueprint : class_blueprints) {
        for (auto& element : blueprint.elements) {
            if (element.literal_value.has_value() && element.literal_value->is_cell())
                visitor.visit(element.literal_value->as_cell());
        }
    }
    property_key_table->visit_edges(visitor);
}

void Executable::copy_runtime_caches_from(Executable const& other)
{
    if (this == &other)
        return;

    if (property_lookup_caches.size() == other.property_lookup_caches.size()) {
        for (size_t i = 0; i < property_lookup_caches.size(); ++i)
            property_lookup_caches[i].copy_from(other.property_lookup_caches[i]);
    }

    if (global_variable_caches.size() == other.global_variable_caches.size())
        global_variable_caches = other.global_variable_caches;

    if (environment_coordinate_caches.size() == other.environment_coordinate_caches.size())
        environment_coordinate_caches = other.environment_coordinate_caches;

    if (template_object_caches.size() == other.template_object_caches.size())
        template_object_caches = other.template_object_caches;

    if (object_shape_caches.size() == other.object_shape_caches.size())
        object_shape_caches = other.object_shape_caches;

    if (object_property_iterator_caches.size() == other.object_property_iterator_caches.size()) {
        for (size_t i = 0; i < object_property_iterator_caches.size(); ++i)
            object_property_iterator_caches[i].data = other.object_property_iterator_caches[i].data;
    }
}

size_t Executable::external_memory_size() const
{
    size_t size = bytecode.external_memory_size();
    size = saturating_add_external_memory_size(size, vector_external_memory_size(property_lookup_caches));
    for (auto const& cache : property_lookup_caches)
        size = saturating_add_external_memory_size(size, cache.external_memory_size());
    size = saturating_add_external_memory_size(size, vector_external_memory_size(global_variable_caches));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(environment_coordinate_caches));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(template_object_caches));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(object_shape_caches));
    for (auto const& cache : object_shape_caches)
        size = saturating_add_external_memory_size(size, vector_external_memory_size(cache.property_offsets));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(object_property_iterator_caches));
    size = saturating_add_external_memory_size(size, string_table->external_memory_size());
    size = saturating_add_external_memory_size(size, identifier_table->external_memory_size());
    size = saturating_add_external_memory_size(size, property_key_table->external_memory_size());
    size = saturating_add_external_memory_size(size, regex_table->external_memory_size());
    size = saturating_add_external_memory_size(size, vector_external_memory_size(constants));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(shared_function_data));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(class_blueprints));
    for (auto const& blueprint : class_blueprints)
        size = saturating_add_external_memory_size(size, vector_external_memory_size(blueprint.elements));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(exception_handlers));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(source_map));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(local_variable_names));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(argument_variable_names));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(local_variable_metadata));
    size = saturating_add_external_memory_size(size, hash_map_external_memory_size(m_source_range_cache));
    return size;
}

static Vector<PropertyLookupCache*>& static_property_lookup_caches()
{
    static NeverDestroyed<Vector<PropertyLookupCache*>> caches;
    return *caches;
}

StaticPropertyLookupCache::StaticPropertyLookupCache()
{
    static_property_lookup_caches().append(this);
}

static bool cell_is_dead(Cell const* cell)
{
    auto* block = GC::HeapBlock::from_cell(cell);
    if (!GC::Heap::the().is_live_heap_block(block))
        return true;
    return cell->state() != Cell::State::Live || !cell->is_marked();
}

static void clear_cache_entry_if_dead(PropertyLookupCache::Entry& entry)
{
    if (entry.from_shape && cell_is_dead(entry.from_shape.ptr()))
        entry.from_shape = nullptr;
    if (entry.shape && cell_is_dead(entry.shape.ptr()))
        entry.shape = nullptr;
    if (entry.prototype && cell_is_dead(entry.prototype.ptr()))
        entry.prototype = nullptr;
    if (entry.prototype_chain_validity && cell_is_dead(entry.prototype_chain_validity.ptr()))
        entry.prototype_chain_validity = nullptr;
}

static bool cache_entry_has_dead_cell(PropertyLookupCache::Entry const& entry)
{
    return (entry.from_shape && cell_is_dead(entry.from_shape.ptr()))
        || (entry.shape && cell_is_dead(entry.shape.ptr()))
        || (entry.prototype && cell_is_dead(entry.prototype.ptr()))
        || (entry.prototype_chain_validity && cell_is_dead(entry.prototype_chain_validity.ptr()));
}

void PropertyLookupCache::remove_dead_entries()
{
    if (auto* data = megamorphic_data()) {
        for (auto& entry : data->primary_entries) {
            if (cache_entry_has_dead_cell(entry))
                entry = {};
        }
        for (auto& entry : data->secondary_entries) {
            if (cache_entry_has_dead_cell(entry))
                entry = {};
        }
        if (cache_entry_has_dead_cell(data->entry))
            data->entry = {};
        return;
    }

    for (auto& entry : entries())
        clear_cache_entry_if_dead(entry);
}

void StaticPropertyLookupCache::sweep_all()
{
    for (auto* cache : static_property_lookup_caches())
        cache->remove_dead_entries();
}

KeyedPropertyLookupCache::Entry& KeyedPropertyLookupCache::entry_for(Shape const& shape, Utf16FlyString const& property_name)
{
    auto hash = pair_int_hash(ptr_hash(&shape), property_name.hash());
    return m_entries[hash & (number_of_entries - 1)];
}

void KeyedPropertyLookupCache::remove_dead_entries()
{
    for (auto& entry : m_entries) {
        if (entry.type == PropertyLookupCache::Entry::Type::Empty)
            continue;
        if ((entry.shape && cell_is_dead(entry.shape.ptr()))
            || (entry.prototype && cell_is_dead(entry.prototype.ptr()))
            || (entry.prototype_chain_validity && cell_is_dead(entry.prototype_chain_validity.ptr()))) {
            entry = {};
        }
    }
}

void Executable::remove_dead_cells(Badge<GC::Heap>)
{
    for (auto& cache : property_lookup_caches)
        cache.remove_dead_entries();
    for (auto& cache : global_variable_caches)
        clear_cache_entry_if_dead(cache.entry);
    for (auto& cache : object_shape_caches) {
        auto* shape = cache.shape.ptr();
        if (shape && cell_is_dead(shape))
            cache.shape = nullptr;
    }
}

Optional<Executable::ExceptionHandlers const&> Executable::exception_handlers_for_offset(size_t offset) const
{
    // NB: exception_handlers is sorted by start_offset.
    auto* entry = binary_search(exception_handlers, offset, nullptr, [](size_t needle, ExceptionHandlers const& entry) -> int {
        if (needle < entry.start_offset)
            return -1;
        if (needle >= entry.end_offset)
            return 1;
        return 0;
    });
    if (!entry)
        return {};
    return *entry;
}

Optional<SourceRange> Executable::source_range_at(size_t offset) const
{
    if (offset >= bytecode.size())
        return {};
    if (source_map.is_empty())
        return {};
    size_t low = 0;
    size_t high = source_map.size();
    while (low < high) {
        auto middle = low + (high - low) / 2;
        if (source_map[middle].bytecode_offset <= offset)
            low = middle + 1;
        else
            high = middle;
    }
    if (low == 0)
        return {};
    auto& entry = source_map[low - 1];
    return SourceRange {
        .code = source_code,
        .start = { .line = entry.line, .column = entry.column },
    };
}

SourceRange const& Executable::get_source_range(u32 program_counter)
{
    return m_source_range_cache.ensure(program_counter, [&] {
        if (auto source_range = source_range_at(program_counter); source_range.has_value())
            return *source_range;
        static NeverDestroyed<SourceRange> dummy { SourceRange { SourceCode::create({}, Utf16String {}), {} } };
        return *dummy;
    });
}

void Executable::add_debugger_breakpoint(u32 bytecode_offset, BreakpointID breakpoint_id)
{
    if (!m_debugger_breakpoint_sites)
        m_debugger_breakpoint_sites = make<HashMap<u32, DebuggerBreakpointSite>>();

    auto& site = m_debugger_breakpoint_sites->ensure(bytecode_offset);
    if (!site.breakpoint_ids.contains_slow(breakpoint_id))
        site.breakpoint_ids.append(breakpoint_id);
}

void Executable::remove_debugger_breakpoint(BreakpointID breakpoint_id)
{
    if (!m_debugger_breakpoint_sites)
        return;

    m_debugger_breakpoint_sites->remove_all_matching([&](auto&, auto& site) {
        site.breakpoint_ids.remove_first_matching([&](auto id) {
            return id == breakpoint_id;
        });
        return site.breakpoint_ids.is_empty();
    });

    if (m_debugger_breakpoint_sites->is_empty())
        m_debugger_breakpoint_sites = nullptr;
}

void Executable::clear_debugger_breakpoints()
{
    m_debugger_breakpoint_sites = nullptr;
}

bool Executable::has_debugger_breakpoint_at(u32 bytecode_offset) const
{
    return m_debugger_breakpoint_sites && m_debugger_breakpoint_sites->contains(bytecode_offset);
}

ReadonlySpan<BreakpointID> Executable::debugger_breakpoints_at(u32 bytecode_offset) const
{
    if (!m_debugger_breakpoint_sites)
        return {};
    auto site = m_debugger_breakpoint_sites->find(bytecode_offset);
    if (site == m_debugger_breakpoint_sites->end())
        return {};
    return site->value.breakpoint_ids;
}

bool Executable::has_debugger_breakpoint(BreakpointID breakpoint_id) const
{
    if (!m_debugger_breakpoint_sites)
        return false;

    for (auto const& entry : *m_debugger_breakpoint_sites) {
        if (entry.value.breakpoint_ids.contains_slow(breakpoint_id))
            return true;
    }
    return false;
}

}
