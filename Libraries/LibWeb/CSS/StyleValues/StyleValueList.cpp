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
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>

namespace Web::CSS {

bool StyleValueList::Properties::operator==(Properties const& other) const
{
    return separator == other.separator && values.span() == other.values.span();
}

ValueComparingNonnullRefPtr<StyleValue const> StyleValueList::absolutized(ComputationContext const& computation_context) const
{
    for (size_t i = 0; i < m_properties.values.size(); ++i) {
        auto absolutized_value = m_properties.values[i]->absolutized(computation_context);
        if (absolutized_value != m_properties.values[i]) {
            StyleValueVector result;
            result.ensure_capacity(m_properties.values.size());
            for (size_t j = 0; j < i; ++j)
                result.append(m_properties.values[j]);
            result.append(move(absolutized_value));
            for (size_t j = i + 1; j < m_properties.values.size(); ++j)
                result.append(m_properties.values[j]->absolutized(computation_context));
            return StyleValueList::create(move(result), m_properties.separator);
        }
    }
    return *this;
}

void StyleValueList::serialize(StringBuilder& builder, SerializationMode mode) const
{
    if (m_properties.values.is_empty())
        return;

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
    if (all_of(m_properties.values, [&](auto const& property) { return property == first_value; }) && m_properties.separator != Separator::Comma && m_properties.collapsible == Collapsible::Yes) {
        first_value->serialize(builder, mode);
        return;
    }

    for (size_t i = 0; i < m_properties.values.size(); ++i) {
        m_properties.values[i]->serialize(builder, mode);
        if (i != m_properties.values.size() - 1)
            builder.append(separator);
    }
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
static ErrorOr<GC::Ref<CSSStyleValue>> reify_a_transform_list(JS::Realm& realm, StyleValueVector const& values)
{
    GC::RootVector<GC::Ref<CSSTransformComponent>> transform_components { realm.heap() };
    for (auto const& transform : values) {
        // NB: Not all transform functions are reifiable, in which case we give up reifying as a transform list.
        transform_components.append(TRY(transform->as_transformation().reify_a_transform_function(realm)));
    }
    return CSSTransformValue::create(realm, static_cast<Vector<GC::Ref<CSSTransformComponent>>>(move(transform_components)));
}

GC::Ref<CSSStyleValue> StyleValueList::reify(JS::Realm& realm, FlyString const& associated_property) const
{
    // NB: <transform-list> is a StyleValueList that contains TransformStyleValues. If that's what we are, follow the
    //     steps for reifying that.
    if (all_of(m_properties.values, [](auto const& it) { return it->is_transformation(); })) {
        if (auto transform_list = reify_a_transform_list(realm, m_properties.values); !transform_list.is_error())
            return transform_list.release_value();
    }

    // NB: Otherwise, there isn't an equivalent CSSStyleValue for StyleValueList, so just use the default.
    return Base::reify(realm, associated_property);
}

// https://drafts.css-houdini.org/css-typed-om-1/#subdivide-into-iterations
StyleValueVector StyleValueList::subdivide_into_iterations(PropertyNameAndID const& property) const
{
    // To subdivide into iterations a CSS value whole value for a property property, execute the following steps:
    // 1. If property is a single-valued property, return a list containing whole value.
    if (property.is_custom_property() || !property_is_list_valued(property.id()))
        return StyleValueVector { *this };

    // 2. Otherwise, divide whole value into individual iterations, as appropriate for property, and return a list
    //    containing the iterations in order.
    return values();
}

}
