/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>

// NB: The style engine's store, not the host, allocates an environment.
template<>
inline constexpr bool AllocatedWithCustomAllocator<Web::CSS::CustomPropertyData> = true;

namespace Web::CSS {

static constexpr u8 max_ancestor_count = 32;
static constexpr size_t absorb_threshold = 8;

// NB: An environment is its store, so a reference to one is a pointer.
static_assert(sizeof(RefPtr<CustomPropertyData const>) == sizeof(void*));

static NonnullRefPtr<CustomPropertyData> adopt_store(void const* rust_store)
{
    VERIFY(rust_store);
    return adopt_ref(*const_cast<CustomPropertyData*>(static_cast<CustomPropertyData const*>(rust_store)));
}

static void const* create_store(OrderedHashMap<Utf16FlyString, StyleProperty> const& own_values, size_t declared_count, CustomPropertyData const* parent, CustomPropertyData const* inheritance_parent)
{
    Vector<ComputedValuesFFI::FfiCustomPropertyStoreEntry> entries;
    entries.ensure_capacity(own_values.size());
    for (auto const& [name, property] : own_values) {
        entries.unchecked_append({
            .name_raw = name.to_raw_leaked(),
            .important = property.important == Important::Yes,
            .data = StyleValueFFI::rust_style_value_retain(property.value->rust_style_value_data()),
        });
    }
    return ComputedValuesFFI::rust_custom_property_store_create(
        entries.data(), entries.size(), declared_count, parent ? parent->rust_store() : nullptr,
        inheritance_parent ? inheritance_parent->rust_store() : nullptr);
}

NonnullRefPtr<CustomPropertyData const> CustomPropertyData::of_store(void const* rust_store)
{
    return *static_cast<CustomPropertyData const*>(rust_store);
}

void CustomPropertyData::ref() const
{
    ComputedValuesFFI::rust_custom_property_store_retain(rust_store());
}

void CustomPropertyData::unref() const
{
    ComputedValuesFFI::rust_custom_property_store_destroy(rust_store());
}

size_t CustomPropertyData::ref_count() const
{
    return ComputedValuesFFI::rust_custom_property_store_reference_count(rust_store());
}

u8 CustomPropertyData::ancestor_count() const
{
    return ComputedValuesFFI::rust_custom_property_store_ancestor_count(rust_store());
}

RefPtr<CustomPropertyData const> CustomPropertyData::parent() const
{
    auto const* parent = ComputedValuesFFI::rust_custom_property_store_chain_parent(rust_store());
    if (!parent)
        return nullptr;
    return of_store(parent);
}

NonnullRefPtr<CustomPropertyData> CustomPropertyData::create(
    OrderedHashMap<Utf16FlyString, StyleProperty> own_values,
    RefPtr<CustomPropertyData const> parent,
    void const* prebuilt_rust_store)
{
    // NB: A store the style engine built holds its values already, and lies over its own chain, which compacts itself.
    if (prebuilt_rust_store)
        return adopt_store(prebuilt_rust_store);

    auto declared_count = own_values.size();
    if (!parent)
        return adopt_store(create_store(own_values, declared_count, nullptr, nullptr));

    auto const& inheritance_parent = parent;

    // If parent chain is too deep, flatten by copying all ancestor values into own.
    if (parent->ancestor_count() >= max_ancestor_count - 1) {
        parent->for_each_property([&](Utf16FlyString const& name, StyleProperty const& property) {
            own_values.ensure(name, [&] { return property; });
        });
        return adopt_store(create_store(own_values, declared_count, nullptr, inheritance_parent));
    }

    // If parent has few own values, absorb them to shorten the chain.
    if (parent->own_value_count() <= absorb_threshold) {
        parent->for_each_own_value([&](Utf16FlyString const& name, StyleProperty const& property) {
            own_values.ensure(name, [&] { return property; });
        });
        auto grandparent = parent->parent();
        return adopt_store(create_store(own_values, declared_count, grandparent, inheritance_parent));
    }

    return adopt_store(create_store(own_values, declared_count, parent, inheritance_parent));
}

NonnullRefPtr<CustomPropertyData> CustomPropertyData::create_animation_overlay(
    OrderedHashMap<Utf16FlyString, StyleProperty> animated_values,
    RefPtr<CustomPropertyData const> base)
{
    Vector<ComputedValuesFFI::FfiCustomPropertyStoreEntry> entries;
    entries.ensure_capacity(animated_values.size());
    for (auto const& [name, property] : animated_values) {
        entries.unchecked_append({
            .name_raw = name.to_raw_leaked(),
            .important = property.important == Important::Yes,
            .data = StyleValueFFI::rust_style_value_retain(property.value->rust_style_value_data()),
        });
    }
    return adopt_store(ComputedValuesFFI::rust_custom_property_store_create_animation_overlay(
        entries.data(), entries.size(), base ? base->rust_store() : nullptr));
}

static StyleProperty property_of_store_value(bool important, void const* data)
{
    return StyleProperty {
        .important = important ? Important::Yes : Important::No,
        .property_id = PropertyID::Custom,
        .value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data))),
    };
}

using StoreValueCallback = Function<void(Utf16FlyString const&, StyleProperty const&)>;

static void call_with_store_value(void* context, size_t name_raw, bool important, void const* data)
{
    (*static_cast<StoreValueCallback const*>(context))(Utf16FlyString::from_raw(name_raw), property_of_store_value(important, data));
}

Optional<StyleProperty> CustomPropertyData::get(Utf16FlyString const& name) const
{
    bool important = false;
    auto const* data = ComputedValuesFFI::rust_custom_property_store_get(rust_store(), name.raw_identity(), &important);
    if (!data)
        return {};
    return property_of_store_value(important, data);
}

void CustomPropertyData::for_each_own_value(StoreValueCallback const& callback) const
{
    ComputedValuesFFI::rust_custom_property_store_for_each_own_value(rust_store(), const_cast<StoreValueCallback*>(&callback), call_with_store_value);
}

void CustomPropertyData::for_each_declared_value(StoreValueCallback const& callback) const
{
    ComputedValuesFFI::rust_custom_property_store_for_each_own_entry(rust_store(), const_cast<StoreValueCallback*>(&callback), call_with_store_value);
}

size_t CustomPropertyData::declared_count() const
{
    return ComputedValuesFFI::rust_custom_property_store_declared_count(rust_store());
}

size_t CustomPropertyData::own_value_count() const
{
    return ComputedValuesFFI::rust_custom_property_store_own_count(rust_store());
}

bool CustomPropertyData::declares_value(Utf16FlyString const& name, StyleValue const& value) const
{
    return ComputedValuesFFI::rust_custom_property_store_declares_value(rust_store(), name.raw_identity(), value.rust_style_value_data());
}

RefPtr<CustomPropertyData const> CustomPropertyData::inheritable_impl(RefPtr<CustomPropertyData const> inheritable_parent, AK::Function<Optional<CustomPropertyRegistration const&>(Utf16FlyString const&)> get_custom_property_registration) const
{
    OrderedHashMap<Utf16FlyString, StyleProperty> inheritable_own_values;
    size_t own_value_count = 0;
    for_each_own_value([&](Utf16FlyString const& name, StyleProperty const& property) {
        ++own_value_count;
        auto registration = get_custom_property_registration(name);

        if (registration.has_value() && !registration->inherit)
            return;

        inheritable_own_values.set(name, property);
    });

    // Filtering only non-inherited properties leaves the parent's environment unchanged.
    // Preserve its identity instead of inserting an empty layer into the chain.
    if (inheritable_own_values.is_empty())
        return inheritable_parent;

    if (inheritable_own_values.size() == own_value_count && inheritable_parent.ptr() == parent().ptr())
        return this;

    return CustomPropertyData::create(move(inheritable_own_values), move(inheritable_parent));
}

RefPtr<CustomPropertyData const> CustomPropertyData::inheritable(Layout::BegunRead const& read, DOM::Document const& document) const
{
    return document.style_computer().inheritable_custom_property_environment(read, *this);
}

void CustomPropertyData::for_each_property(Function<void(Utf16FlyString const&, StyleProperty const&)> callback) const
{
    ComputedValuesFFI::rust_custom_property_store_for_each_entry(rust_store(), &callback, call_with_store_value);
}

bool CustomPropertyData::is_empty() const
{
    return ComputedValuesFFI::rust_custom_property_store_is_empty(rust_store());
}

}
