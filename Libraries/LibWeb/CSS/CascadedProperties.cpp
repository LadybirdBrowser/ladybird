/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CascadedProperties.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CascadedProperties);

CascadedProperties::CascadedProperties() = default;

CascadedProperties::~CascadedProperties() = default;

void CascadedProperties::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto const& [property_id, entries] : m_properties) {
        for (auto const& entry : entries) {
            visitor.visit(entry.source);
        }
    }
}

void CascadedProperties::revert_property(PropertyID property_id, Important important, CascadeOrigin cascade_origin)
{
    auto it = m_properties.find(property_id);
    if (it == m_properties.end())
        return;
    auto& entries = it->value;
    entries.remove_all_matching([&](auto& entry) {
        return entry.property.property_id == property_id
            && entry.property.important == important
            && cascade_origin == entry.origin;
    });
    if (entries.is_empty())
        m_properties.remove(it);
}

void CascadedProperties::revert_layer_property(PropertyID property_id, Important important, Optional<FlyString> layer_name)
{
    auto it = m_properties.find(property_id);
    if (it == m_properties.end())
        return;
    auto& entries = it->value;
    entries.remove_all_matching([&](auto& entry) {
        return entry.property.property_id == property_id
            && entry.property.important == important
            && layer_name == entry.layer_name;
    });
    if (entries.is_empty())
        m_properties.remove(it);
}

void CascadedProperties::resolve_unresolved_properties(DOM::AbstractElement abstract_element)
{
    for (auto& [property_id, entries] : m_properties) {
        for (auto& entry : entries) {
            if (!entry.property.value->is_unresolved())
                continue;
            entry.property.value = Parser::Parser::resolve_unresolved_style_value(Parser::ParsingParams { abstract_element.document() }, abstract_element, PropertyNameAndID::from_id(property_id), entry.property.value->as_unresolved());
        }
    }
}

void CascadedProperties::set_property(PropertyID property_id, NonnullRefPtr<StyleValue const> const& value, Important important, CascadeOrigin origin, Optional<FlyString> layer_name, GC::Ptr<CSS::CSSStyleDeclaration const> source)
{
    auto& entries = m_properties.ensure(property_id);

    for (auto& entry : entries.in_reverse()) {
        if (entry.origin == origin && entry.layer_name == layer_name) {
            if (entry.property.important == Important::Yes && important == Important::No)
                return;
            entry.property = StyleProperty {
                .important = important,
                .property_id = property_id,
                .value = value,
            };
            return;
        }
    }

    entries.append(Entry {
        .property = StyleProperty {
            .important = important,
            .property_id = property_id,
            .value = value,
        },
        .origin = origin,
        .layer_name = move(layer_name),
        .source = source,
    });
}

void CascadedProperties::set_property_from_presentational_hint(PropertyID property_id, NonnullRefPtr<StyleValue const> const& value)
{
    StyleComputer::for_each_property_expanding_shorthands(property_id, value, [this](PropertyID longhand_property_id, StyleValue const& longhand_value) {
        auto& entries = m_properties.ensure(longhand_property_id);

        entries.append(Entry {
            .property = StyleProperty {
                .important = Important::No,
                .property_id = longhand_property_id,
                .value = longhand_value,
            },
            .origin = CascadeOrigin::Author,
            .layer_name = {},
            .source = nullptr,
        });
    });
}

RefPtr<StyleValue const> CascadedProperties::property(PropertyID property_id) const
{
    auto it = m_properties.find(property_id);
    if (it == m_properties.end())
        return nullptr;

    return it->value.last().property.value;
}

GC::Ptr<CSSStyleDeclaration const> CascadedProperties::property_source(PropertyID property_id) const
{
    auto it = m_properties.find(property_id);
    if (it == m_properties.end())
        return nullptr;

    return it->value.last().source;
}

bool CascadedProperties::is_property_important(PropertyID property_id) const
{
    auto it = m_properties.find(property_id);
    if (it == m_properties.end())
        return false;

    return it->value.last().property.important == Important::Yes;
}

}
