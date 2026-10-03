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

namespace Web::CSS {

static Atomic<u64> s_next_custom_property_data_identity { 1 };
static constexpr u8 max_ancestor_count = 32;
static constexpr size_t absorb_threshold = 8;

static ComputedValuesFFI::FfiUtf16View ffi_utf16_view(Utf16View view)
{
    return {
        .ascii = view.has_ascii_storage() ? reinterpret_cast<u8 const*>(view.ascii_span().data()) : nullptr,
        .utf16 = view.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(view.utf16_span().data()),
        .length = view.length_in_code_units(),
    };
}

CustomPropertyData::CustomPropertyData(OrderedHashMap<Utf16FlyString, StyleProperty> own_values, RefPtr<CustomPropertyData const> parent, RefPtr<CustomPropertyData const> inheritance_parent, u8 ancestor_count, size_t declared_count, void const* prebuilt_rust_store, u64 identity)
    : m_own_values(move(own_values))
    , m_parent(move(parent))
    , m_ancestor_count(ancestor_count)
    , m_declared_count(declared_count)
    , m_identity(identity ? identity : s_next_custom_property_data_identity.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
    , m_rust_store(prebuilt_rust_store)
{
    if (m_rust_store)
        return;
    Vector<ComputedValuesFFI::FfiCustomPropertyStoreEntry> entries;
    entries.ensure_capacity(m_own_values.size());
    for (auto const& [name, property] : m_own_values) {
        entries.unchecked_append({
            .name_raw = name.to_raw_leaked(),
            .name = ffi_utf16_view(name),
            .important = property.important == Important::Yes,
            .data = StyleValueFFI::rust_style_value_retain(property.value->rust_style_value_data()),
        });
    }
    m_rust_store = ComputedValuesFFI::rust_custom_property_store_create(
        entries.data(), entries.size(), m_declared_count, m_parent ? m_parent->rust_store() : nullptr,
        inheritance_parent ? inheritance_parent->rust_store() : nullptr);
}

CustomPropertyData::~CustomPropertyData()
{
    ComputedValuesFFI::rust_custom_property_store_destroy(m_rust_store);
}

NonnullRefPtr<CustomPropertyData> CustomPropertyData::create(
    OrderedHashMap<Utf16FlyString, StyleProperty> own_values,
    RefPtr<CustomPropertyData const> parent,
    void const* prebuilt_rust_store,
    u64 identity)
{
    auto declared_count = own_values.size();
    if (!parent)
        return adopt_ref(*new CustomPropertyData(move(own_values), nullptr, nullptr, 0, declared_count, prebuilt_rust_store, identity));

    auto inheritance_parent = parent;

    // If parent chain is too deep, flatten by copying all ancestor values into own.
    if (parent->m_ancestor_count >= max_ancestor_count - 1) {
        parent->for_each_property([&](Utf16FlyString const& name, StyleProperty const& property) {
            own_values.ensure(name, [&] { return property; });
        });
        return adopt_ref(*new CustomPropertyData(move(own_values), nullptr, move(inheritance_parent), 0, declared_count, prebuilt_rust_store, identity));
    }

    // If parent has few own values, absorb them to shorten the chain.
    if (parent->m_own_values.size() <= absorb_threshold) {
        for (auto const& [name, property] : parent->m_own_values)
            own_values.ensure(name, [&] { return property; });
        auto grandparent = parent->m_parent;
        u8 ancestor_count = grandparent ? grandparent->m_ancestor_count + 1 : 0;
        return adopt_ref(*new CustomPropertyData(move(own_values), move(grandparent), move(inheritance_parent), ancestor_count, declared_count, prebuilt_rust_store, identity));
    }

    u8 ancestor_count = parent->m_ancestor_count + 1;
    return adopt_ref(*new CustomPropertyData(move(own_values), move(parent), move(inheritance_parent), ancestor_count, declared_count, prebuilt_rust_store, identity));
}

NonnullRefPtr<CustomPropertyData> CustomPropertyData::create_animation_overlay(
    OrderedHashMap<Utf16FlyString, StyleProperty> animated_values,
    RefPtr<CustomPropertyData const> base, DOM::AbstractElement const& owner)
{
    Vector<ComputedValuesFFI::FfiCustomPropertyStoreEntry> entries;
    entries.ensure_capacity(animated_values.size());
    for (auto const& [name, property] : animated_values) {
        entries.unchecked_append({
            .name_raw = name.to_raw_leaked(),
            .name = ffi_utf16_view(name),
            .important = property.important == Important::Yes,
            .data = StyleValueFFI::rust_style_value_retain(property.value->rust_style_value_data()),
        });
    }
    auto const* rust_store = ComputedValuesFFI::rust_custom_property_store_create_animation_overlay(
        entries.data(), entries.size(), base ? base->rust_store() : nullptr);
    auto declared_count = animated_values.size();
    u8 ancestor_count = base ? base->m_ancestor_count + 1 : 0;
    auto inheritance_parent = base;
    auto data = adopt_ref(*new CustomPropertyData(move(animated_values), move(base), move(inheritance_parent), ancestor_count, declared_count, rust_store));
    data->m_animation_owner = AnimationOwner { owner.element().unique_id(), owner.pseudo_element() };
    return data;
}

bool CustomPropertyData::is_animation_overlay_for(DOM::AbstractElement const& element) const
{
    // Compare with the id the element already has rather than ask it for one, which would give an id to every element
    // that inherits an overlay: the owner was given one when the overlay was made.
    return m_animation_owner.has_value()
        && m_animation_owner->pseudo_element == element.pseudo_element()
        && element.element().unique_id_if_assigned() == m_animation_owner->element;
}

StyleProperty const* CustomPropertyData::get(Utf16FlyString const& name) const
{
    if (auto it = m_own_values.find(name); it != m_own_values.end())
        return &it->value;
    if (m_parent)
        return m_parent->get(name);
    return nullptr;
}

RefPtr<CustomPropertyData const> CustomPropertyData::inheritable_impl(RefPtr<CustomPropertyData const> inheritable_parent, AK::Function<Optional<CustomPropertyRegistration const&>(Utf16FlyString const&)> get_custom_property_registration) const
{
    OrderedHashMap<Utf16FlyString, StyleProperty> inheritable_own_values;

    for (auto const& [name, property] : m_own_values) {
        auto registration = get_custom_property_registration(name);

        if (registration.has_value() && !registration->inherit)
            continue;

        inheritable_own_values.set(name, property);
    }

    // Filtering only non-inherited properties leaves the parent's environment unchanged.
    // Preserve its identity instead of inserting an empty layer into the chain.
    if (inheritable_own_values.is_empty())
        return inheritable_parent;

    if (inheritable_own_values.size() == m_own_values.size() && inheritable_parent.ptr() == m_parent.ptr())
        return this;

    return CustomPropertyData::create(move(inheritable_own_values), move(inheritable_parent));
}

bool CustomPropertyData::declares_same_names(CustomPropertyData const& other) const
{
    if (m_declared_count != other.m_declared_count)
        return false;
    auto own = m_own_values.begin();
    auto other_own = other.m_own_values.begin();
    for (size_t index = 0; index < m_declared_count; ++index, ++own, ++other_own) {
        if (own->key != other_own->key)
            return false;
    }
    return true;
}

RefPtr<CustomPropertyData const> CustomPropertyData::inheritable(Layout::BegunRead const& read, DOM::Document const& document) const
{
    auto document_identity = reinterpret_cast<FlatPtr>(&document);
    auto generation = document.custom_property_registration_generation();
    if (m_cached_inheritable_document_identity == document_identity && m_cached_inheritable_generation == generation) {
        if (m_cached_inheritable_is_self)
            return RefPtr<CustomPropertyData const>(this);
        return m_cached_inheritable_data;
    }

    RefPtr<CustomPropertyData const> inheritable_parent;
    if (m_parent)
        inheritable_parent = m_parent->inheritable(read, document);

    // NB: What an environment the style engine resolved hands down is the engine's to name, under the registrations
    //     as they are now: the environments its records resolve over the element's are built over that one.
    RefPtr<CustomPropertyData const> inheritable;
    if (StyleEngine::is_engine_custom_property_environment(m_identity)) {
        auto const& style_computer = document.style_computer();
        auto inheritable_identity = style_computer.style_engine().inheritable_custom_property_environment(read, m_identity);
        if (inheritable_identity == m_identity)
            inheritable = this;
        else if (inheritable_identity == (inheritable_parent ? inheritable_parent->identity() : 0))
            inheritable = inheritable_parent;
        else
            inheritable = style_computer.engine_custom_property_environment(read, inheritable_identity, inheritable_parent);
    }
    if (!inheritable) {
        inheritable = inheritable_impl(
            inheritable_parent,
            [&](Utf16FlyString const& name) { return document.get_registered_custom_property(name); });
    }

    set_inheritable(document, inheritable);
    return inheritable;
}

void CustomPropertyData::set_inheritable(DOM::Document const& document, RefPtr<CustomPropertyData const> inheritable) const
{
    // Registration generations are local to each document, so the cache has to include the destination document
    // identity as well. Otherwise a subtree adopted into another document can incorrectly reuse a filtered result
    // that was computed under a different registration set.
    m_cached_inheritable_document_identity = reinterpret_cast<FlatPtr>(&document);
    m_cached_inheritable_generation = document.custom_property_registration_generation();

    // We can't store a RefPtr to `this` in m_cached_inheritable_data since it would create a reference cycle so we store
    // that case as a special boolean flag instead.
    m_cached_inheritable_is_self = inheritable.ptr() == this;
    m_cached_inheritable_data = m_cached_inheritable_is_self ? nullptr : move(inheritable);
}

void CustomPropertyData::for_each_property(Function<void(Utf16FlyString const&, StyleProperty const&)> callback) const
{
    HashTable<Utf16FlyString> seen;
    for (auto const* node = this; node; node = node->m_parent.ptr()) {
        for (auto const& [name, property] : node->m_own_values) {
            if (seen.set(name) == HashSetResult::KeptExistingEntry)
                continue;
            callback(name, property);
        }
    }
}

bool CustomPropertyData::is_empty() const
{
    return m_own_values.is_empty() && (!m_parent || m_parent->is_empty());
}

}

// The style engine keeps the custom-property environment each element holds, with a reference of its own.
extern "C" void web_css_custom_property_data_reference(void const* data)
{
    static_cast<Web::CSS::CustomPropertyData const*>(data)->ref();
}

extern "C" void web_css_custom_property_data_unreference(void const* data)
{
    static_cast<Web::CSS::CustomPropertyData const*>(data)->unref();
}
