/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/ComponentValue.h>

namespace Web::CSS::Parser {

ComponentValue::ComponentValue(Token token)
    : m_value(move(token))
{
}
ComponentValue::ComponentValue(Function&& function)
    : m_value(move(function))
{
}
ComponentValue::ComponentValue(SimpleBlock&& block)
    : m_value(move(block))
{
}
ComponentValue::ComponentValue(GuaranteedInvalidValue&& invalid)
    : m_value(move(invalid))
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
        },
        [](GuaranteedInvalidValue const&) {
            return "Guaranteed-invalid value"_string;
        });
}

String ComponentValue::original_source_text() const
{
    return m_value.visit([](auto const& it) { return it.original_source_text(); });
}

bool ComponentValue::contains_guaranteed_invalid_value() const
{
    return m_value.visit(
        [](Token const&) {
            return false;
        },
        [](SimpleBlock const& block) {
            return block.value
                .first_matching([](auto const& it) { return it.contains_guaranteed_invalid_value(); })
                .has_value();
        },
        [](Function const& function) {
            return function.value
                .first_matching([](auto const& it) { return it.contains_guaranteed_invalid_value(); })
                .has_value();
        },
        [](GuaranteedInvalidValue const&) {
            return true;
        });
}

}
