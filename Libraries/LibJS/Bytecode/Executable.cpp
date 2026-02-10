/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <LibJS/Bytecode/BasicBlock.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/RegexTable.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/SourceCode.h>

namespace JS::Bytecode {

GC_DEFINE_ALLOCATOR(Executable);

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
    size_t number_of_registers,
    Strict strict)
    : bytecode(move(bytecode))
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
}

Executable::~Executable() = default;

void Executable::dump() const
{
    warnln("\033[37;1mJS bytecode executable\033[0m \"{}\"", name);
    InstructionStreamIterator it(bytecode, this);

    size_t basic_block_offset_index = 0;

    while (!it.at_end()) {
        bool print_basic_block_marker = false;
        if (basic_block_offset_index < basic_block_start_offsets.size()
            && it.offset() == basic_block_start_offsets[basic_block_offset_index]) {
            ++basic_block_offset_index;
            print_basic_block_marker = true;
        }

        StringBuilder builder;
        builder.appendff("[{:4x}] ", it.offset());
        if (print_basic_block_marker)
            builder.appendff("{:4}: ", basic_block_offset_index - 1);
        else
            builder.append("      "sv);
        builder.append((*it).to_byte_string(*this));

        warnln("{}", builder.string_view());

        ++it;
    }

    if (!exception_handlers.is_empty()) {
        warnln("");
        warnln("Exception handlers:");
        for (auto& handlers : exception_handlers) {
            warnln("    from {:4x} to {:4x} handler {:4x}",
                handlers.start_offset,
                handlers.end_offset,
                handlers.handler_offset);
        }
    }

    warnln("");
}

void Executable::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(constants);
    for (auto& cache : template_object_caches)
        visitor.visit(cache.cached_template_object);
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

UnrealizedSourceRange Executable::source_range_at(size_t offset) const
{
    if (offset >= bytecode.size())
        return {};
    auto it = InstructionStreamIterator(bytecode.span().slice(offset), this);
    VERIFY(!it.at_end());
    auto* entry = binary_search(source_map, offset, nullptr, [](size_t needle, SourceMapEntry const& entry) -> int {
        if (needle < entry.bytecode_offset)
            return -1;
        if (needle > entry.bytecode_offset)
            return 1;
        return 0;
    });
    if (!entry)
        return {};
    return UnrealizedSourceRange {
        .source_code = source_code,
        .start_offset = entry->source_record.source_start_offset,
        .end_offset = entry->source_record.source_end_offset,
    };
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
