/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StyleValueList.h"
#include <LibGC/RootVector.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/CSS/CSSTransformComponent.h>
#include <LibWeb/CSS/CSSTransformValue.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>

namespace Web::CSS {

bool StyleValueList::Properties::operator==(Properties const& other) const
{
    return separator == other.separator && values.span() == other.values.span();
}

ValueComparingNonnullRefPtr<StyleValue const> StyleValueList::absolutized(ComputationContext const& computation_context) const
{
    StyleValueVector absolutized_style_values;
    absolutized_style_values.ensure_capacity(m_properties.values.size());

    for (auto const& value : m_properties.values)
        absolutized_style_values.append(value->absolutized(computation_context));

    return StyleValueList::create(move(absolutized_style_values), m_properties.separator);
}

String StyleValueList::to_string(SerializationMode mode) const
{
    if (m_properties.values.is_empty())
        return {};

    auto separator = ""sv;
    switch (m_properties.separator) {
    case Separator::Space:
        separator = " "sv;
        break;
    case Separator::Comma:
        separator = ", "sv;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    auto first_value = m_properties.values.first();
    if (all_of(m_properties.values, [&](auto const& property) { return property == first_value; }))
        return first_value->to_string(mode);

    StringBuilder builder;
    for (size_t i = 0; i < m_properties.values.size(); ++i) {
        builder.append(m_properties.values[i]->to_string(mode));
        if (i != m_properties.values.size() - 1)
            builder.append(separator);
    }
    return MUST(builder.to_string());
}

void StyleValueList::set_style_sheet(GC::Ptr<CSSStyleSheet> style_sheet)
{
    Base::set_style_sheet(style_sheet);
    for (auto& value : m_properties.values)
        const_cast<StyleValue&>(*value).set_style_sheet(style_sheet);
}

Vector<Parser::ComponentValue> StyleValueList::tokenize() const
{
    Vector<Parser::ComponentValue> component_values;
    bool first = true;
    for (auto const& value : m_properties.values) {
        if (first) {
            first = false;
        } else {
            if (m_properties.separator == Separator::Comma)
                component_values.empend(Parser::Token::create(Parser::Token::Type::Comma));
            component_values.empend(Parser::Token::create_whitespace(" "_string));
        }
        component_values.extend(value->tokenize());
    }

    return component_values;
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-transform-list
static GC::Ref<CSSStyleValue> reify_a_transform_list(JS::Realm& realm, StyleValueVector const& values)
{
    GC::RootVector<GC::Ref<CSSTransformComponent>> transform_components { realm.heap() };
    for (auto const& transform : values) {
        transform_components.append(transform->as_transformation().reify_a_transform_function(realm));
    }
    return CSSTransformValue::create(realm, static_cast<Vector<GC::Ref<CSSTransformComponent>>>(move(transform_components)));
}

GC::Ref<CSSStyleValue> StyleValueList::reify(JS::Realm& realm, FlyString const& associated_property) const
{
    // NB: <transform-list> is a StyleValueList that contains TransformStyleValues. If that's what we are, follow the
    //     steps for reifying that.
    if (all_of(m_properties.values, [](auto const& it) { return it->is_transformation(); })) {
        return reify_a_transform_list(realm, m_properties.values);
    }

    // NB: Otherwise, there isn't an equivalent CSSStyleValue for StyleValueList, so just use the default.
    return Base::reify(realm, associated_property);
}

}
