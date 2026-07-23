/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/WeakInlines.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CascadedProperties.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>

namespace Web::CSS {

// The Rust store mirrors the CascadeOrigin discriminants; pin them.
static_assert(to_underlying(CascadeOrigin::Author) == to_underlying(ComputedValuesFFI::CascadeOrigin::Author));
static_assert(to_underlying(CascadeOrigin::AuthorPresentationalHint) == to_underlying(ComputedValuesFFI::CascadeOrigin::AuthorPresentationalHint));
static_assert(to_underlying(CascadeOrigin::User) == to_underlying(ComputedValuesFFI::CascadeOrigin::User));
static_assert(to_underlying(CascadeOrigin::UserAgent) == to_underlying(ComputedValuesFFI::CascadeOrigin::UserAgent));
static_assert(to_underlying(CascadeOrigin::Animation) == to_underlying(ComputedValuesFFI::CascadeOrigin::Animation));
static_assert(to_underlying(CascadeOrigin::Transition) == to_underlying(ComputedValuesFFI::CascadeOrigin::Transition));

CascadedProperties::CascadedProperties()
    : m_store(ComputedValuesFFI::rust_cascaded_properties_create())
{
}

CascadedProperties::~CascadedProperties()
{
    ComputedValuesFFI::rust_cascaded_properties_destroy(m_store);
}

NonnullRefPtr<CascadedProperties> CascadedProperties::create()
{
    return adopt_ref(*new CascadedProperties);
}

void CascadedProperties::assign_source_slot(u32 slot, GC::Ptr<CSSStyleDeclaration const> source, GC::Ptr<DOM::ShadowRoot const> source_shadow_root)
{
    if (slot >= m_source_slots.size())
        m_source_slots.resize(slot + 1);
    m_source_slots[slot] = SourcePair { source.ptr(), source_shadow_root.ptr() };
}

GC::Ptr<CSSStyleDeclaration const> CascadedProperties::source_for_slot(u32 slot) const
{
    if (slot >= m_source_slots.size())
        return nullptr;
    return m_source_slots[slot].source.ptr();
}

RefPtr<StyleValue const> CascadedProperties::property(PropertyID property_id) const
{
    auto const* data = static_cast<StyleValueFFI::StyleValueData const*>(ComputedValuesFFI::rust_cascaded_properties_property(m_store, to_underlying(property_id)));
    if (!data) {
        m_property_cache.remove(property_id);
        return nullptr;
    }
    if (auto it = m_property_cache.find(property_id); it != m_property_cache.end() && it->value->rust_style_value_data() == data)
        return it->value;
    auto value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(data));
    auto source_slot = ComputedValuesFFI::rust_cascaded_properties_source_slot(m_store, to_underlying(property_id));
    if (source_slot >= 0 && ComputedValuesFFI::rust_cascaded_properties_has_style_sheet_context(m_store, to_underlying(property_id))) {
        if (auto source = source_for_slot(static_cast<u32>(source_slot)); source && source->parent_rule())
            const_cast<StyleValue&>(*value).set_style_sheet(source->parent_rule()->parent_style_sheet());
    }
    m_property_cache.set(property_id, value);
    return value;
}

GC::Ptr<DOM::ShadowRoot const> CascadedProperties::property_source_shadow_root(PropertyID property_id) const
{
    auto slot = ComputedValuesFFI::rust_cascaded_properties_source_slot(m_store, to_underlying(property_id));
    if (slot < 0)
        return nullptr;
    return m_source_slots[slot].source_shadow_root.ptr();
}

}
