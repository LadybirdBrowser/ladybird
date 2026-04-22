/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NumericLimits.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

// Chain of custom property maps with structural sharing.
// Each node stores only the properties declared directly on its element,
// with a parent pointer to the inherited chain.
class WEB_API CustomPropertyData : public RefCounted<CustomPropertyData> {
public:
    static NonnullRefPtr<CustomPropertyData> create(
        OrderedHashMap<FlyString, StyleProperty> own_values,
        RefPtr<CustomPropertyData const> parent);

    StyleProperty const* get(FlyString const& name) const;
    RefPtr<CustomPropertyData const> inheritable(DOM::Document const&) const;

    OrderedHashMap<FlyString, StyleProperty> const& own_values() const { return m_own_values; }

    void for_each_property(Function<void(FlyString const&, StyleProperty const&)> callback) const;

    RefPtr<CustomPropertyData const> parent() const { return m_parent; }

    bool is_empty() const;

private:
    CustomPropertyData(OrderedHashMap<FlyString, StyleProperty> own_values, RefPtr<CustomPropertyData const> parent, u8 ancestor_count);

    OrderedHashMap<FlyString, StyleProperty> m_own_values;
    RefPtr<CustomPropertyData const> m_parent;
    u8 m_ancestor_count { 0 };
    mutable FlatPtr m_cached_inheritable_document_identity { NumericLimits<FlatPtr>::max() };
    mutable size_t m_cached_inheritable_generation { NumericLimits<size_t>::max() };
    mutable RefPtr<CustomPropertyData const> m_cached_inheritable_data;
    mutable bool m_cached_inheritable_is_self { false };
};

}
