/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<UnresolvedStyleValue const> UnresolvedStyleValue::create(Vector<Parser::ComponentValue>&& values, Optional<Parser::SubstitutionFunctionsPresence> substitution_presence, Optional<String> original_source_text)
{
    if (!substitution_presence.has_value()) {
        substitution_presence = Parser::SubstitutionFunctionsPresence {};
        for (auto const& value : values) {
            if (value.is_function())
                value.function().contains_arbitrary_substitution_function(*substitution_presence);
            if (value.is_block())
                value.block().contains_arbitrary_substitution_function(*substitution_presence);
        }
    }

    return adopt_ref(*new (nothrow) UnresolvedStyleValue(move(values), *substitution_presence, move(original_source_text)));
}

UnresolvedStyleValue::UnresolvedStyleValue(Vector<Parser::ComponentValue>&& values, Parser::SubstitutionFunctionsPresence substitution_presence, Optional<String> original_source_text)
    : CSSStyleValue(Type::Unresolved)
    , m_values(move(values))
    , m_substitution_functions_presence(substitution_presence)
    , m_original_source_text(move(original_source_text))
{
}

String UnresolvedStyleValue::to_string(SerializationMode) const
{
    if (m_original_source_text.has_value())
        return *m_original_source_text;

    return serialize_a_series_of_component_values(m_values, InsertWhitespace::Yes);
}

bool UnresolvedStyleValue::equals(CSSStyleValue const& other) const
{
    if (type() != other.type())
        return false;
    return values() == other.as_unresolved().values();
}

}
