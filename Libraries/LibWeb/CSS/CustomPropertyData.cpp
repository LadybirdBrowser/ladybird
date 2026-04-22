/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/DOM/Document.h>

namespace Web::CSS {

static constexpr u8 max_ancestor_count = 32;
static constexpr size_t absorb_threshold = 8;

CustomPropertyData::CustomPropertyData(OrderedHashMap<FlyString, StyleProperty> own_values, RefPtr<CustomPropertyData const> parent, u8 ancestor_count)
    : m_own_values(move(own_values))
    , m_parent(move(parent))
    , m_ancestor_count(ancestor_count)
{
}

NonnullRefPtr<CustomPropertyData> CustomPropertyData::create(
    OrderedHashMap<FlyString, StyleProperty> own_values,
    RefPtr<CustomPropertyData const> parent)
{
    if (!parent)
        return adopt_ref(*new CustomPropertyData(move(own_values), nullptr, 0));

    // If parent chain is too deep, flatten by copying all ancestor values into own.
    if (parent->m_ancestor_count >= max_ancestor_count - 1) {
        parent->for_each_property([&](FlyString const& name, StyleProperty const& property) {
            own_values.ensure(name, [&] { return property; });
        });
        return adopt_ref(*new CustomPropertyData(move(own_values), nullptr, 0));
    }

    // If parent has few own values, absorb them to shorten the chain.
    if (parent->m_own_values.size() <= absorb_threshold) {
        for (auto const& [name, property] : parent->m_own_values)
            own_values.ensure(name, [&] { return property; });
        auto grandparent = parent->m_parent;
        u8 ancestor_count = grandparent ? grandparent->m_ancestor_count + 1 : 0;
        return adopt_ref(*new CustomPropertyData(move(own_values), move(grandparent), ancestor_count));
    }

    u8 ancestor_count = parent->m_ancestor_count + 1;
    return adopt_ref(*new CustomPropertyData(move(own_values), move(parent), ancestor_count));
}

StyleProperty const* CustomPropertyData::get(FlyString const& name) const
{
    if (auto it = m_own_values.find(name); it != m_own_values.end())
        return &it->value;
    if (m_parent)
        return m_parent->get(name);
    return nullptr;
}

RefPtr<CustomPropertyData const> CustomPropertyData::inheritable(DOM::Document const& document) const
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
        inheritable_parent = m_parent->inheritable(document);

    OrderedHashMap<FlyString, StyleProperty> inheritable_own_values;
    bool filtered_any_own_values = false;
    for (auto const& [name, property] : m_own_values) {
        auto registration = document.get_registered_custom_property(name);
        if (registration.has_value() && !registration->inherit) {
            filtered_any_own_values = true;
            continue;
        }
        inheritable_own_values.set(name, property);
    }

    // Registration generations are local to each document, so the cache has to include the destination document
    // identity as well. Otherwise a subtree adopted into another document can incorrectly reuse a filtered result
    // that was computed under a different registration set.
    m_cached_inheritable_document_identity = document_identity;
    m_cached_inheritable_generation = generation;

    if (!filtered_any_own_values && inheritable_parent.ptr() == m_parent.ptr()) {
        m_cached_inheritable_data = nullptr;
        m_cached_inheritable_is_self = true;
        return RefPtr<CustomPropertyData const>(this);
    }

    m_cached_inheritable_is_self = false;
    if (inheritable_own_values.is_empty() && !inheritable_parent) {
        m_cached_inheritable_data = nullptr;
        return nullptr;
    }

    m_cached_inheritable_data = CustomPropertyData::create(move(inheritable_own_values), move(inheritable_parent));
    return m_cached_inheritable_data;
}

void CustomPropertyData::for_each_property(Function<void(FlyString const&, StyleProperty const&)> callback) const
{
    HashTable<FlyString> seen;
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
