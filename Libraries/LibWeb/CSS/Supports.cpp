/*
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Supports.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

bool Supports::matches() const
{
    auto result = Parser::ValueParserFFI::css_query_evaluate_supports(m_rust_query_handle.data());
    VERIFY(result <= 2);
    return result == 1;
}

Utf16String Supports::to_string() const
{
    Utf16String serialized;
    auto set_serialized_query = [](void* context, u16 const* code_units, size_t length) {
        *static_cast<Utf16String*>(context) = Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(code_units), length });
    };
    VERIFY(Parser::ValueParserFFI::css_query_serialize_condition(m_rust_query_handle.data(), &serialized, set_serialized_query));
    return serialized;
}

void Supports::dump(StringBuilder& builder, int indent_levels) const
{
    dump_indent(builder, indent_levels);
    builder.appendff("Supports condition: `{}` (matches = {})\n", to_string(), matches());
}

}
