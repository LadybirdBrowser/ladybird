/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWasm/Types.h>

namespace Wasm {

class Reference;
class Value;

ByteString instruction_name(OpCode const& opcode);
Optional<OpCode> instruction_from_name(StringView name);

struct Printer {
    explicit Printer(Stream& stream, size_t initial_indent = 0)
        : m_stream(stream)
        , m_indent(initial_indent)
    {
    }

    void print(Wasm::BlockType const&);
    void print(Wasm::CodeSection const&);
    void print(Wasm::CodeSection::Code const&);
    void print(Wasm::CodeSection::Func const&);
    void print(Wasm::CustomSection const&);
    void print(Wasm::DataCountSection const&);
    void print(Wasm::DataSection const&);
    void print(Wasm::DataSection::Data const&);
    void print(Wasm::ElementSection const&);
    void print(Wasm::ElementSection::Element const&);
    void print(Wasm::ExportSection const&);
    void print(Wasm::ExportSection::Export const&);
    void print(Wasm::Expression const&);
    void print(Wasm::FunctionSection const&);
    void print(Wasm::FunctionType const&);
    void print(Wasm::GlobalSection const&);
    void print(Wasm::GlobalSection::Global const&);
    void print(Wasm::GlobalType const&);
    void print(Wasm::ImportSection const&);
    void print(Wasm::ImportSection::Import const&);
    void print(Wasm::Instruction const&);
    void print(Wasm::Limits const&);
    void print(Wasm::Locals const&);
    void print(Wasm::MemorySection const&);
    void print(Wasm::MemorySection::Memory const&);
    void print(Wasm::MemoryType const&);
    void print(Wasm::Module const&);
    void print(Wasm::Reference const&);
    void print(Wasm::StartSection const&);
    void print(Wasm::StartSection::StartFunction const&);
    void print(Wasm::TableSection const&);
    void print(Wasm::TableSection::Table const&);
    void print(Wasm::TableType const&);
    void print(Wasm::TypeSection const&);
    void print(Wasm::ValueType const&);
    void print(Wasm::Value const&);
    void print(Wasm::Value const&, ValueType const&);

private:
    void print_indent();
    template<typename... Args>
    void print(CheckedFormatString<Args...> fmt, Args&&... args)
    {
        StringBuilder builder;
        builder.appendff(fmt.view(), forward<Args>(args)...);
        m_stream.write_until_depleted(builder.string_view().bytes()).release_value_but_fixme_should_propagate_errors();
    }

    Stream& m_stream;
    size_t m_indent { 0 };
};

}
