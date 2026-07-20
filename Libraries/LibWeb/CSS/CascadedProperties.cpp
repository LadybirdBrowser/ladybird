/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/WeakInlines.h>
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

RefPtr<StyleValue const> CascadedProperties::property(PropertyID property_id) const
{
    return static_cast<StyleValue const*>(ComputedValuesFFI::rust_cascaded_properties_property(m_store, to_underlying(property_id)));
}

GC::Ptr<DOM::ShadowRoot const> CascadedProperties::property_source_shadow_root(PropertyID property_id) const
{
    auto slot = ComputedValuesFFI::rust_cascaded_properties_source_slot(m_store, to_underlying(property_id));
    if (slot < 0)
        return nullptr;
    return m_source_slots[slot].source_shadow_root.ptr();
}

}
