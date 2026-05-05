/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <LibJS/Bytecode/BasicBlock.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/FormatOperand.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/Op.h>
#include <LibJS/Bytecode/RegexTable.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/SourceCode.h>

namespace JS::Bytecode {

GC_DEFINE_ALLOCATOR(Executable);
GC_DEFINE_ALLOCATOR(ObjectPropertyIteratorCacheData);

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

Executable::Executable(
    Vector<u8> bytecode,
    NonnullOwnPtr<IdentifierTable> identifier_table,
    NonnullOwnPtr<PropertyKeyTable> property_key_table,
    NonnullOwnPtr<StringTable> string_table,
    NonnullOwnPtr<RegexTable> regex_table,
    Vector<Value> constants,
    NonnullRefPtr<SourceCode const> source_code,
    size_t number_of_property_lookup_caches,
    size_t number_of_global_variable_caches,
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
    template_object_caches.resize(number_of_template_object_caches);
    object_shape_caches.resize(number_of_object_shape_caches);
    object_property_iterator_caches.resize(number_of_object_property_iterator_caches);
    asm_constants_size = this->constants.size();
    asm_constants_data = this->constants.data();
}

Executable::~Executable() = default;

void Executable::fixup_cache_pointers()
{
    for (auto it = InstructionStreamIterator(bytecode); !it.at_end(); ++it) {
        fixup_instruction_cache(
            const_cast<Instruction&>(*it),
            property_lookup_caches.span(),
            global_variable_caches.span(),
            template_object_caches.span(),
            object_shape_caches.span(),
            object_property_iterator_caches.span());
    }
}

static void dump_header(StringBuilder& output, Executable const& executable, bool use_color)
{
    auto const white_bold = use_color ? "\033[37;1m"sv : ""sv;
    auto const reset = use_color ? "\033[0m"sv : ""sv;

    // Generate a stable hash from the source text for identification.
    // We hash source code rather than bytecode so the ID is stable
    // across changes to bytecode generation.
    // Find the overall source range covered by this executable.
    u32 source_start = NumericLimits<u32>::max();
    u32 source_end = 0;
    Optional<Position> first_position;
    for (auto const& entry : executable.source_map) {
        if (entry.source_record.start.offset < entry.source_record.end.offset) {
            source_start = min(source_start, entry.source_record.start.offset);
            source_end = max(source_end, entry.source_record.end.offset);
            if (!first_position.has_value() || entry.source_record.start.offset < first_position->offset)
                first_position = entry.source_record.start;
        }
    }

    u32 hash = 2166136261u; // FNV-1a offset basis
    auto code_view = executable.source_code->code_view();
    for (auto i = source_start; i < source_end && i < code_view.length_in_code_units(); ++i) {
        auto code_unit = code_view.code_unit_at(i);
        hash ^= code_unit & 0xFF;
        hash *= 16777619u;
        hash ^= (code_unit >> 8) & 0xFF;
        hash *= 16777619u;
    }

    if (executable.name.is_empty())
        output.appendff("{}${:08x}{}", white_bold, hash, reset);
    else
        output.appendff("{}{}${:08x}{}", white_bold, executable.name, hash, reset);

    // Show source location if available.
    if (source_start < source_end && first_position.has_value()) {
        auto filename = executable.source_code->filename();
        if (!filename.is_empty()) {
            // Show just the basename to keep output portable across machines.
            auto last_slash = filename.bytes_as_string_view().find_last('/');
            if (last_slash.has_value())
                filename = MUST(filename.substring_from_byte_offset(last_slash.value() + 1));
            output.appendff(" {}:{}:{}", filename, first_position->line, first_position->column);
        } else {
            output.appendff(" line {}, column {}", first_position->line, first_position->column);
        }
    }
    output.append('\n');
}

static void dump_metadata(StringBuilder& output, Executable const& executable, bool use_color)
{
    auto const green = use_color ? "\033[32m"sv : ""sv;
    auto const yellow = use_color ? "\033[33m"sv : ""sv;
    auto const blue = use_color ? "\033[34m"sv : ""sv;
    auto const cyan = use_color ? "\033[36m"sv : ""sv;
    auto const reset = use_color ? "\033[0m"sv : ""sv;

    output.appendff("  {}Registers{}: {}\n", green, reset, executable.number_of_registers);
    output.appendff("  {}Blocks{}:    {}\n", green, reset, executable.basic_block_start_offsets.size());

    if (!executable.local_variable_names.is_empty()) {
        output.appendff("  {}Locals{}:    ", green, reset);
        for (size_t i = 0; i < executable.local_variable_names.size(); ++i) {
            if (i != 0)
                output.append(", "sv);
            output.appendff("{}{}~{}{}", blue, executable.local_variable_names[i].name, i, reset);
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
                output.appendff("BigInt({})", MUST(value.as_bigint().to_string()));
            else if (value.is_string())
                output.appendff("String(\"{}\")", value.as_string().utf8_string_view());
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

static void dump_bytecode(StringBuilder& output, Executable const& executable, bool use_color)
{
    auto const magenta = use_color ? "\033[35;1m"sv : ""sv;
    auto const reset = use_color ? "\033[0m"sv : ""sv;

    InstructionStreamIterator it(executable.bytecode, &executable);

    size_t basic_block_offset_index = 0;

    while (!it.at_end()) {
        if (basic_block_offset_index < executable.basic_block_start_offsets.size()
            && it.offset() == executable.basic_block_start_offsets[basic_block_offset_index]) {
            if (basic_block_offset_index > 0)
                output.append('\n');
            output.appendff("{}block{}{}:\n", magenta, basic_block_offset_index, reset);
            ++basic_block_offset_index;
        }

        output.appendff("  [{:4x}] {}\n", it.offset(), (*it).to_byte_string(executable));

        ++it;
    }
}

void Executable::dump() const
{
    StringBuilder output;

    dump_header(output, *this, true);
    dump_metadata(output, *this, true);
    output.append('\n');
    dump_bytecode(output, *this, true);

    if (!exception_handlers.is_empty()) {
        output.append("\nException handlers:\n"sv);
        for (auto const& handler : exception_handlers) {
            output.appendff("  [{:4x} .. {:4x}] => handler ", handler.start_offset, handler.end_offset);
            Label handler_label(static_cast<u32>(handler.handler_offset));
            output.appendff("{}\n", format_label(""sv, handler_label, *this));
        }
    }

    output.append('\n');
    warnln("{}", output.string_view());
}

String Executable::dump_to_string() const
{
    StringBuilder output;
    dump_header(output, *this, false);
    dump_metadata(output, *this, false);
    output.append('\n');
    dump_bytecode(output, *this, false);

    if (!exception_handlers.is_empty()) {
        output.append("\nException handlers:\n"sv);
        for (auto const& handler : exception_handlers) {
            output.appendff("  [{:4x} .. {:4x}] => handler ", handler.start_offset, handler.end_offset);
            Label handler_label(static_cast<u32>(handler.handler_offset));
            output.appendff("{}\n", format_label(""sv, handler_label, *this));
        }
    }

    return output.to_string_without_validation();
}

void Executable::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(constants);
    for (auto& cache : template_object_caches)
        visitor.visit(cache.cached_template_object);
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

size_t Executable::external_memory_size() const
{
    size_t size = vector_external_memory_size(bytecode);
    size = saturating_add_external_memory_size(size, vector_external_memory_size(property_lookup_caches));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(global_variable_caches));
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
    size = saturating_add_external_memory_size(size, vector_external_memory_size(basic_block_start_offsets));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(source_map));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(local_variable_names));
    size = saturating_add_external_memory_size(size, hash_map_external_memory_size(m_source_range_cache));
    return size;
}

static Vector<PropertyLookupCache*>& static_property_lookup_caches()
{
    static Vector<PropertyLookupCache*> caches;
    return caches;
}

StaticPropertyLookupCache::StaticPropertyLookupCache()
{
    static_property_lookup_caches().append(this);
}

static void clear_cache_entry_if_dead(PropertyLookupCache::Entry& entry)
{
    if (entry.from_shape && entry.from_shape->state() != Cell::State::Live)
        entry.from_shape = nullptr;
    if (entry.shape && entry.shape->state() != Cell::State::Live)
        entry.shape = nullptr;
    if (entry.prototype && entry.prototype->state() != Cell::State::Live)
        entry.prototype = nullptr;
    if (entry.prototype_chain_validity && entry.prototype_chain_validity->state() != Cell::State::Live)
        entry.prototype_chain_validity = nullptr;
}

void StaticPropertyLookupCache::sweep_all()
{
    for (auto* cache : static_property_lookup_caches()) {
        for (auto& entry : cache->entries)
            clear_cache_entry_if_dead(entry);
    }
}

void Executable::remove_dead_cells(Badge<GC::Heap>)
{
    for (auto& cache : property_lookup_caches) {
        for (auto& entry : cache.entries)
            clear_cache_entry_if_dead(entry);
    }
    for (auto& cache : global_variable_caches) {
        for (auto& entry : cache.entries)
            clear_cache_entry_if_dead(entry);
    }
    for (auto& cache : object_shape_caches) {
        if (cache.shape && cache.shape->state() != Cell::State::Live)
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
    auto* entry = binary_search(source_map, offset, nullptr, [](size_t needle, SourceMapEntry const& entry) -> int {
        if (needle < entry.bytecode_offset)
            return -1;
        if (needle > entry.bytecode_offset)
            return 1;
        return 0;
    });
    if (!entry)
        return {};
    return SourceRange {
        .code = source_code,
        .start = entry->source_record.start,
        .end = entry->source_record.end,
    };
}

SourceRange const& Executable::get_source_range(u32 program_counter)
{
    return m_source_range_cache.ensure(program_counter, [&] {
        if (auto source_range = source_range_at(program_counter); source_range.has_value())
            return *source_range;
        static SourceRange dummy { SourceCode::create({}, {}), {}, {} };
        return dummy;
    });
}

Operand Executable::original_operand_from_raw(u32 raw) const
{
    // NB: Layout is [registers | locals | constants | arguments]
    if (raw < number_of_registers)
        return Operand { Operand::Type::Register, raw };
    if (raw < registers_and_locals_count)
        return Operand { Operand::Type::Local, raw - local_index_base };
    if (raw < argument_index_base)
        return Operand { Operand::Type::Constant, raw - registers_and_locals_count };
    return Operand { Operand::Type::Argument, raw - argument_index_base };
}

}
