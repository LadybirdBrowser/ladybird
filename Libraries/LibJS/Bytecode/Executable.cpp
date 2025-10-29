/*
 * Copyright (c) 2021-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Bytecode/BasicBlock.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/RegexTable.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/SourceCode.h>

namespace JS::Bytecode {

GC_DEFINE_ALLOCATOR(Executable);

Executable::Executable(
    Vector<u8> bytecode,
    NonnullOwnPtr<IdentifierTable> identifier_table,
    NonnullOwnPtr<StringTable> string_table,
    NonnullOwnPtr<RegexTable> regex_table,
    Vector<Value> constants,
    NonnullRefPtr<SourceCode const> source_code,
    size_t number_of_property_lookup_caches,
    size_t number_of_global_variable_caches,
    size_t number_of_registers,
    Strict strict)
    : bytecode(move(bytecode))
    , string_table(move(string_table))
    , identifier_table(move(identifier_table))
    , regex_table(move(regex_table))
    , constants(move(constants))
    , source_code(move(source_code))
    , number_of_registers(number_of_registers)
    , is_strict_mode(strict == Strict::Yes)
{
    property_lookup_caches.resize(number_of_property_lookup_caches);
    global_variable_caches.resize(number_of_global_variable_caches);
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
            warnln("    from {:4x} to {:4x} handler {:4x} finalizer {:4x}",
                handlers.start_offset,
                handlers.end_offset,
                handlers.handler_offset.value_or(0),
                handlers.finalizer_offset.value_or(0));
        }
    }

    warnln("");
}

void Executable::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(constants);
}

Optional<Executable::ExceptionHandlers const&> Executable::exception_handlers_for_offset(size_t offset) const
{
    for (auto& handlers : exception_handlers) {
        if (handlers.start_offset <= offset && offset < handlers.end_offset)
            return handlers;
    }
    return {};
}

UnrealizedSourceRange Executable::source_range_at(size_t offset) const
{
    if (offset >= bytecode.size())
        return {};
    auto it = InstructionStreamIterator(bytecode.span().slice(offset), this);
    VERIFY(!it.at_end());
    auto mapping = source_map.get(offset);
    if (!mapping.has_value())
        return {};
    return UnrealizedSourceRange {
        .source_code = source_code,
        .start_offset = mapping->source_start_offset,
        .end_offset = mapping->source_end_offset,
    };
}

}
