/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/ComponentValue.h>

namespace Web::CSS::Parser {

ComponentValue::ComponentValue(Token token)
    : m_value(token)
{
}
ComponentValue::ComponentValue(Function&& function)
    : m_value(function)
{
}
ComponentValue::ComponentValue(SimpleBlock&& block)
    : m_value(block)
{
}

ComponentValue::~ComponentValue() = default;

bool ComponentValue::is_function(StringView name) const
{
    return is_function() && function().name.equals_ignoring_ascii_case(name);
}

bool ComponentValue::is_ident(StringView ident) const
{
    return is(Token::Type::Ident) && token().ident().equals_ignoring_ascii_case(ident);
}

String ComponentValue::to_string() const
{
    return m_value.visit([](auto const& it) { return it.to_string(); });
}

String ComponentValue::to_debug_string() const
{
    return m_value.visit(
        [](Token const& token) {
            return MUST(String::formatted("Token: {}", token.to_debug_string()));
        },
        [](SimpleBlock const& block) {
            return MUST(String::formatted("Block: {}", block.to_string()));
        },
        [](Function const& function) {
            return MUST(String::formatted("Function: {}", function.to_string()));
        });
}

String ComponentValue::original_source_text() const
{
    return m_value.visit([](auto const& it) { return it.original_source_text(); });
}

}
