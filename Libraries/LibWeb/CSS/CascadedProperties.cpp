/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CascadedProperties.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleComputer.h>

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
    for (auto const& [property_id, entries] : m_custom_properties) {
        for (auto const& entry : entries) {
            visitor.visit(entry.source);
        }
    }
}

void CascadedProperties::revert_property(PropertyNameAndID const& property, Important important, CascadeOrigin cascade_origin)
{
    auto entries = get_entries(property);
    if (!entries.has_value())
        return;
    entries->remove_all_matching([&](auto& entry) {
        return entry.property.name_and_id == property
            && entry.property.important == important
            && cascade_origin == entry.origin;
    });
    if (entries->is_empty())
        remove_entry(property);
}

void CascadedProperties::revert_layer_property(PropertyNameAndID const& property, Important important, Optional<FlyString> layer_name)
{
    auto entries = get_entries(property);
    if (!entries.has_value())
        return;
    entries->remove_all_matching([&](auto& entry) {
        return entry.property.name_and_id == property
            && entry.property.important == important
            && layer_name == entry.layer_name;
    });
    if (entries->is_empty())
        remove_entry(property);
}

void CascadedProperties::set_property(PropertyNameAndID const& property, NonnullRefPtr<StyleValue const> value, Important important, CascadeOrigin origin, Optional<FlyString> layer_name, GC::Ptr<CSSStyleDeclaration const> source)
{
    auto& entries = ensure_entry(property);
    auto custom_name = property.is_custom_property() ? property.name() : ""_fly_string;

    for (auto& entry : entries.in_reverse()) {
        if (entry.origin == origin && entry.layer_name == layer_name) {
            if (entry.property.important == Important::Yes && important == Important::No)
                return;
            entry.property = StyleProperty {
                .name_and_id = property,
                .value = value,
                .important = important,
            };
            return;
        }
    }

    entries.append(Entry {
        .property = StyleProperty {
            .name_and_id = property,
            .value = value,
            .important = important,
        },
        .origin = origin,
        .layer_name = move(layer_name),
        .source = source,
    });
}

void CascadedProperties::set_unresolved_shorthand(PropertyID property_id, NonnullRefPtr<StyleValue const> value, Important important, CascadeOrigin origin, Optional<FlyString> layer_name, GC::Ptr<CSSStyleDeclaration const> source)
{
    m_unresolved_shorthands.set(property_id,
        Entry {
            .property = StyleProperty {
                .name_and_id = PropertyNameAndID::from_id(property_id),
                .value = value,
                .important = important,
            },
            .origin = origin,
            .layer_name = move(layer_name),
            .source = source,
        });
}

void CascadedProperties::set_property_from_presentational_hint(PropertyID property_id, NonnullRefPtr<StyleValue const> value)
{
    StyleComputer::for_each_property_expanding_shorthands(property_id, value, [this](PropertyID longhand_property_id, StyleValue const& longhand_value) {
        auto& entries = m_properties.ensure(longhand_property_id);

        entries.append(Entry {
            .property = StyleProperty {
                .name_and_id = PropertyNameAndID::from_id(longhand_property_id),
                .value = longhand_value,
                .important = Important::No,
            },
            .origin = CascadeOrigin::Author,
            .layer_name = {},
            .source = nullptr,
        });
    });
}

RefPtr<StyleValue const> CascadedProperties::property(PropertyNameAndID const& property) const
{
    if (auto entries = get_entries(property); entries.has_value())
        return entries->last().property.value;
    return nullptr;
}

GC::Ptr<CSSStyleDeclaration const> CascadedProperties::property_source(PropertyNameAndID const& property) const
{
    if (auto entries = get_entries(property); entries.has_value())
        return entries->last().source;
    return nullptr;
}

bool CascadedProperties::is_property_important(PropertyNameAndID const& property) const
{
    if (auto entries = get_entries(property); entries.has_value())
        return entries->last().property.important == Important::Yes;
    return false;
}

OrderedHashMap<FlyString, StyleProperty> CascadedProperties::custom_properties() const
{
    OrderedHashMap<FlyString, StyleProperty> results;
    results.ensure_capacity(m_custom_properties.size());
    for (auto const& [name, entry] : m_custom_properties) {
        results.set(name, entry.last().property);
    }
    return results;
}

Optional<Vector<CascadedProperties::Entry>&> CascadedProperties::get_entries(PropertyNameAndID const& property)
{
    if (property.is_custom_property())
        return m_custom_properties.get(property.name());
    return m_properties.get(property.id());
}

Optional<Vector<CascadedProperties::Entry> const&> CascadedProperties::get_entries(PropertyNameAndID const& property) const
{
    if (property.is_custom_property())
        return m_custom_properties.get(property.name());
    return m_properties.get(property.id());
}

Vector<CascadedProperties::Entry>& CascadedProperties::ensure_entry(PropertyNameAndID const& property)
{
    if (property.is_custom_property())
        return m_custom_properties.ensure(property.name());
    return m_properties.ensure(property.id());
}

void CascadedProperties::remove_entry(PropertyNameAndID const& property)
{
    if (property.is_custom_property())
        m_custom_properties.remove(property.name());
    else
        m_properties.remove(property.id());
}

}
