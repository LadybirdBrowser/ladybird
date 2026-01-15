/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FontVariantAlternatesFunctionStyleValue.h"
#include <LibWeb/CSS/FontFeatureData.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

void FontVariantAlternatesFunctionStyleValue::serialize(StringBuilder& builder, SerializationMode) const
{
    switch (m_function_type) {
    case FontFeatureValueType::Stylistic:
        builder.append("stylistic("sv);
        break;
    case FontFeatureValueType::Styleset:
        builder.append("styleset("sv);
        break;
    case FontFeatureValueType::CharacterVariant:
        builder.append("character-variant("sv);
        break;
    case FontFeatureValueType::Swash:
        builder.append("swash("sv);
        break;
    case FontFeatureValueType::Ornaments:
        builder.append("ornaments("sv);
        break;
    case FontFeatureValueType::Annotation:
        builder.append("annotation("sv);
        break;
    }

    serialize_a_comma_separated_list(builder, m_names, [](StringBuilder& builder, NonnullRefPtr<StyleValue const> const& name) {
        name->serialize(builder, SerializationMode::Normal);
    });

    builder.append(')');
}

ValueComparingNonnullRefPtr<StyleValue const> FontVariantAlternatesFunctionStyleValue::absolutized(ComputationContext const& context) const
{
    StyleValueVector absolutized_names;

    bool any_name_changed = false;

    for (auto const& name : m_names) {
        auto absolutized_name = name->absolutized(context);

        if (absolutized_name != name)
            any_name_changed = true;

        absolutized_names.append(absolutized_name);
    }

    if (!any_name_changed)
        return *this;

    return FontVariantAlternatesFunctionStyleValue::create(m_function_type, move(absolutized_names));
}

}
