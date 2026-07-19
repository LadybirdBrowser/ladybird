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

static ComputedValuesFFI::CascadeOrigin to_ffi_cascade_origin(CascadeOrigin origin)
{
    return static_cast<ComputedValuesFFI::CascadeOrigin>(to_underlying(origin));
}

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

void CascadedProperties::revert_property(PropertyID property_id, Important important, CascadeOrigin cascade_origin)
{
    ComputedValuesFFI::rust_cascaded_properties_revert_property(m_store, to_underlying(property_id), important == Important::Yes, to_ffi_cascade_origin(cascade_origin));
}

void CascadedProperties::revert_layer_property(PropertyID property_id, Important important, CascadeOrigin cascade_origin, Optional<Utf16FlyString> layer_name, GC::Ptr<DOM::ShadowRoot const> source_shadow_root)
{
    FlatPtr layer_name_raw = layer_name.has_value() ? layer_name->to_raw_leaked() : 0;
    ComputedValuesFFI::rust_cascaded_properties_revert_layer_property(
        m_store,
        to_underlying(property_id),
        important == Important::Yes,
        to_ffi_cascade_origin(cascade_origin),
        layer_name.has_value(),
        layer_name_raw,
        bit_cast<FlatPtr>(source_shadow_root.ptr()));
    if (layer_name.has_value())
        Utf16FlyString::unref_raw(layer_name_raw);
}

void CascadedProperties::set_property(PropertyID property_id, NonnullRefPtr<StyleValue const> value, Important important, CascadeOrigin origin, Optional<Utf16FlyString> layer_name, GC::Ptr<CSS::CSSStyleDeclaration const> source, GC::Ptr<DOM::ShadowRoot const> source_shadow_root)
{
    auto slot = ComputedValuesFFI::rust_cascaded_properties_set_property(
        m_store,
        to_underlying(property_id),
        retain_style_value_for_rust(value.ptr()),
        value->rust_style_value_data(),
        important == Important::Yes,
        to_ffi_cascade_origin(origin),
        layer_name.has_value(),
        layer_name.has_value() ? layer_name->to_raw_leaked() : 0,
        bit_cast<FlatPtr>(source_shadow_root.ptr()));

    // A negative slot means the declaration lost to an existing important entry.
    if (slot < 0)
        return;

    assign_source_slot(slot, source, source_shadow_root);
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

PropertyID CascadedProperties::property_with_higher_priority(PropertyID first_property_id, PropertyID second_property_id) const
{
    return static_cast<PropertyID>(ComputedValuesFFI::rust_cascaded_properties_property_with_higher_priority(m_store, to_underlying(first_property_id), to_underlying(second_property_id)));
}

GC::Ptr<CSSStyleDeclaration const> CascadedProperties::property_source(PropertyID property_id) const
{
    auto slot = ComputedValuesFFI::rust_cascaded_properties_source_slot(m_store, to_underlying(property_id));
    if (slot < 0)
        return nullptr;
    return m_source_slots[slot].source.ptr();
}

GC::Ptr<DOM::ShadowRoot const> CascadedProperties::property_source_shadow_root(PropertyID property_id) const
{
    auto slot = ComputedValuesFFI::rust_cascaded_properties_source_slot(m_store, to_underlying(property_id));
    if (slot < 0)
        return nullptr;
    return m_source_slots[slot].source_shadow_root.ptr();
}

Optional<StyleProperty> CascadedProperties::style_property(PropertyID property_id) const
{
    bool important = false;
    auto* value = static_cast<StyleValue const*>(ComputedValuesFFI::rust_cascaded_properties_style_property(m_store, to_underlying(property_id), &important));
    if (!value)
        return {};

    return StyleProperty {
        .important = important ? Important::Yes : Important::No,
        .property_id = property_id,
        .value = *value,
    };
}

}
