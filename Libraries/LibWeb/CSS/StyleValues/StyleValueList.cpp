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

ValueComparingNonnullRefPtr<StyleValue const> StyleValueList::absolutized(ComputationContext const& computation_context) const
{
    auto values = this->values();
    for (size_t i = 0; i < values.size(); ++i) {
        auto absolutized_value = values[i]->absolutized(computation_context);
        if (absolutized_value != values[i]) {
            StyleValueVector result;
            result.ensure_capacity(values.size());
            for (size_t j = 0; j < i; ++j)
                result.append(values[j]);
            result.append(move(absolutized_value));
            for (size_t j = i + 1; j < values.size(); ++j)
                result.append(values[j]->absolutized(computation_context));
            return StyleValueList::create(move(result), separator(), collapsible());
        }
    }
    return *this;
}

void StyleValueList::serialize(StringBuilder& builder, SerializationMode mode) const
{
    auto values = this->values();
    if (values.is_empty())
        return;

    auto separator_string = ""sv;
    switch (separator()) {
    case Separator::Space:
        separator_string = " "sv;
        break;
    case Separator::Comma:
        separator_string = ", "sv;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    auto first_value = values.first();
    if (all_of(values, [&](auto const& property) { return property == first_value; }) && separator() != Separator::Comma && collapsible() == Collapsible::Yes && !first_value->is_empty_optional()) {
        first_value->serialize(builder, mode);
        return;
    }

    bool first = true;

    for (size_t i = 0; i < values.size(); ++i) {
        if (values[i]->is_empty_optional())
            continue;

        if (!first)
            builder.append(separator_string);

        first = false;
        values[i]->serialize(builder, mode);
    }
}

void StyleValueList::set_style_sheet(GC::Ptr<CSSStyleSheet> style_sheet)
{
    Base::set_style_sheet(style_sheet);
    for (auto& value : values())
        const_cast<StyleValue&>(*value).set_style_sheet(style_sheet);
}

Vector<Parser::ComponentValue> StyleValueList::tokenize() const
{
    Vector<Parser::ComponentValue> component_values;
    bool first = true;
    for (auto const& value : values()) {
        if (value->is_empty_optional())
            continue;
        if (first) {
            first = false;
        } else {
            if (separator() == Separator::Comma)
                component_values.empend(Parser::Token::create(Parser::Token::Type::Comma));
            component_values.empend(Parser::Token::create_whitespace(" "_string));
        }
        component_values.extend(value->tokenize());
    }

    return component_values;
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-transform-list
static GC::Ptr<CSSStyleValue> reify_a_transform_list(JS::Realm& realm, ReadonlySpan<ValueComparingNonnullRefPtr<StyleValue const>> values)
{
    GC::RootVector<GC::Ref<CSSTransformComponent>> transform_components;
    for (auto const& transform : values) {
        auto reified_transform = transform->as_transformation().reify_a_transform_function(realm);

        if (!reified_transform)
            return nullptr;

        // NB: Not all transform functions are reifiable, in which case we give up reifying as a transform list.
        transform_components.append(reified_transform.as_nonnull());
    }
    return CSSTransformValue::create(realm, move(transform_components));
}

GC::Ref<CSSStyleValue> StyleValueList::reify(JS::Realm& realm, Utf16FlyString const& associated_property) const
{
    auto values = this->values();

    // NB: <transform-list> is a StyleValueList that contains TransformStyleValues. If that's what we are, follow the
    //     steps for reifying that.
    if (all_of(values, [](auto const& it) { return it->is_transformation(); })) {
        if (auto transform_list = reify_a_transform_list(realm, values))
            return transform_list.as_nonnull();
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
    return StyleValueVector { values() };
}

}
