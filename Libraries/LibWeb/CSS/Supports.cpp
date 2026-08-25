/*
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Query.h>
#include <LibWeb/CSS/Supports.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

bool supports_condition_matches(RustQueryHandle const& handle)
{
    auto result = Parser::ValueParserFFI::css_query_evaluate_supports(handle.data());
    if (result > to_underlying(MatchResult::Unknown)) {
        dbgln("supports_condition_matches: unexpected query handle kind");
        return false;
    }
    return result == 1;
}

Utf16String serialize_supports_condition(RustQueryHandle const& handle)
{
    Utf16String serialized;
    auto set_serialized_query = [](void* context, u16 const* code_units, size_t length) {
        *static_cast<Utf16String*>(context) = Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(code_units), length });
    };
    VERIFY(Parser::ValueParserFFI::css_query_serialize_condition(handle.data(), &serialized, set_serialized_query));
    return serialized;
}

void dump_supports_condition(StringBuilder& builder, RustQueryHandle const& handle, int indent_levels)
{
    dump_indent(builder, indent_levels);
    builder.appendff("Supports condition: `{}` (matches = {})\n", serialize_supports_condition(handle), supports_condition_matches(handle));
}

}
