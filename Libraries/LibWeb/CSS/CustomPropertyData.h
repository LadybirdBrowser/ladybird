/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NumericLimits.h>
#include <AK/QuickSort.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/StyleEngineIdentifiers.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

// Chain of custom property maps with structural sharing.
// Each node stores only the properties declared directly on its element,
// with a parent pointer to the inherited chain.
class WEB_API CustomPropertyData : public RefCounted<CustomPropertyData> {
public:
    enum class AllowParentOwnValueAbsorption : u8 {
        No,
        Yes,
    };

    static NonnullRefPtr<CustomPropertyData> create(
        OrderedHashMap<Utf16FlyString, StyleProperty> own_values,
        RefPtr<CustomPropertyData const> parent,
        AllowParentOwnValueAbsorption allow_parent_own_value_absorption = AllowParentOwnValueAbsorption::Yes);
    ~CustomPropertyData();

    StyleProperty const* get(Utf16FlyString const& name) const;
    RefPtr<CustomPropertyData const> inheritable_impl(RefPtr<CustomPropertyData const> inheritable_parent, AK::Function<Optional<CustomPropertyRegistration const&>(Utf16FlyString const&)> get_custom_property_registration) const;
    RefPtr<CustomPropertyData const> inheritable(DOM::Document const&) const;

    OrderedHashMap<Utf16FlyString, StyleProperty> const& own_values() const { return m_own_values; }

    // How many of the own values this element's own cascade declared. The rest were absorbed from
    // the chain above to keep it short, and are already what that chain resolved them to - so they
    // are neither this element's to declare nor its to resolve again. They sit after the declared
    // ones, because absorption appends.
    [[nodiscard]] size_t declared_count() const { return m_declared_count; }

    // The engine's identities for the names this environment declares, sorted and deduplicated,
    // worked out once for the environment rather than once for each element handed it: a page whose
    // theme declares a thousand names hands the same thousand to every element under it. Atoms are
    // document-local, so the document that asked is part of what the answer is good for.
    template<typename InternName>
    [[nodiscard]] ReadonlySpan<StyleAtomID> declared_name_atoms(FlatPtr document_identity, InternName&& intern) const
    {
        if (m_cached_name_atoms_document_identity == document_identity)
            return m_cached_declared_name_atoms.span();
        m_cached_declared_name_atoms.clear_with_capacity();
        m_cached_declared_name_atoms.ensure_capacity(m_declared_count);
        size_t declared = 0;
        for (auto const& own : m_own_values) {
            if (declared++ >= m_declared_count)
                break;
            m_cached_declared_name_atoms.unchecked_append(intern(own.key));
        }
        quick_sort(m_cached_declared_name_atoms);
        size_t unique = 0;
        for (size_t index = 0; index < m_cached_declared_name_atoms.size(); ++index) {
            if (index == 0 || m_cached_declared_name_atoms[index] != m_cached_declared_name_atoms[unique - 1])
                m_cached_declared_name_atoms[unique++] = m_cached_declared_name_atoms[index];
        }
        m_cached_declared_name_atoms.shrink(unique);
        m_cached_name_atoms_document_identity = document_identity;
        return m_cached_declared_name_atoms.span();
    }

    void for_each_property(Function<void(Utf16FlyString const&, StyleProperty const&)> callback) const;

    RefPtr<CustomPropertyData const> parent() const { return m_parent; }

    bool is_empty() const;

    // This monotonic identity remains unambiguous after the object dies, so retained StyleEngine
    // catalogs can name an environment without retaining or dereferencing the C++ object.
    u64 identity() const { return m_identity; }
    void const* rust_store() const { return m_rust_store; }

    // What this environment resolves to, which is a function of the values it holds and of the
    // environment it inherits from - both of which are its identity. Two elements handed the same
    // environment therefore resolve the same one, and the second of them does no work at all.
    [[nodiscard]] RefPtr<CustomPropertyData const> cached_resolution(FlatPtr document_identity, size_t registration_generation, PreferredColorScheme color_scheme) const
    {
        if (m_cached_resolution_document_identity != document_identity || m_cached_resolution_generation != registration_generation)
            return {};
        // Registered color values can resolve light-dark() against the element's color scheme,
        // so a resolution only answers for elements sharing the scheme it was made under.
        if (m_cached_resolution_color_scheme != color_scheme)
            return {};
        if (m_cached_resolution_is_self)
            return RefPtr<CustomPropertyData const>(this);
        return m_cached_resolution;
    }
    void set_cached_resolution(FlatPtr document_identity, size_t registration_generation, PreferredColorScheme color_scheme, RefPtr<CustomPropertyData const> resolution) const
    {
        m_cached_resolution_document_identity = document_identity;
        m_cached_resolution_generation = registration_generation;
        m_cached_resolution_color_scheme = color_scheme;
        // Storing a reference to itself would keep the object alive forever, so that case is a flag.
        m_cached_resolution_is_self = resolution.ptr() == this;
        m_cached_resolution = m_cached_resolution_is_self ? nullptr : move(resolution);
    }

private:
    CustomPropertyData(OrderedHashMap<Utf16FlyString, StyleProperty> own_values, RefPtr<CustomPropertyData const> parent, u8 ancestor_count, size_t declared_count);

    OrderedHashMap<Utf16FlyString, StyleProperty> m_own_values;
    RefPtr<CustomPropertyData const> m_parent;
    u8 m_ancestor_count { 0 };
    size_t m_declared_count { 0 };
    u64 m_identity { 0 };
    mutable FlatPtr m_cached_name_atoms_document_identity { NumericLimits<FlatPtr>::max() };
    mutable Vector<StyleAtomID> m_cached_declared_name_atoms;
    mutable FlatPtr m_cached_inheritable_document_identity { NumericLimits<FlatPtr>::max() };
    mutable size_t m_cached_inheritable_generation { NumericLimits<size_t>::max() };
    mutable RefPtr<CustomPropertyData const> m_cached_inheritable_data;
    mutable bool m_cached_inheritable_is_self { false };
    mutable FlatPtr m_cached_resolution_document_identity { NumericLimits<FlatPtr>::max() };
    mutable size_t m_cached_resolution_generation { NumericLimits<size_t>::max() };
    mutable PreferredColorScheme m_cached_resolution_color_scheme { PreferredColorScheme::Auto };
    mutable RefPtr<CustomPropertyData const> m_cached_resolution;
    mutable bool m_cached_resolution_is_self { false };
    void const* m_rust_store { nullptr };
};

}
