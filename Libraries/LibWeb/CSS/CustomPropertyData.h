/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NumericLimits.h>
#include <AK/QuickSort.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/CSS/StyleEngineIdentifiers.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

// Chain of custom property maps with structural sharing.
// Each node stores only the properties declared directly on its element,
// with a parent pointer to the inherited chain.
// NB: The style engine references the data elements hold, and may take and give up those references on whichever
//     thread it runs on.
class WEB_API CustomPropertyData : public AtomicRefCounted<CustomPropertyData> {
public:
    static NonnullRefPtr<CustomPropertyData> create(
        OrderedHashMap<Utf16FlyString, StyleProperty> own_values,
        RefPtr<CustomPropertyData const> parent,
        // Transfers one strong Rust store reference when non-null.
        void const* prebuilt_rust_store = nullptr,
        // The identity the style engine minted for an environment it resolved; zero mints one here.
        u64 identity = 0);
    // The values `owner`'s animations sample over `base`, the environment it holds beneath them.
    static NonnullRefPtr<CustomPropertyData> create_animation_overlay(
        OrderedHashMap<Utf16FlyString, StyleProperty> animated_values,
        RefPtr<CustomPropertyData const> base, DOM::AbstractElement const& owner);
    ~CustomPropertyData();

    // Whether these are the values the element's own animations sample. A child that inherits all of its parent's
    // custom properties holds its parent's overlay itself, which is not an animation of the child's.
    bool is_animation_overlay_for(DOM::AbstractElement const&) const;

    StyleProperty const* get(Utf16FlyString const& name) const;
    RefPtr<CustomPropertyData const> inheritable_impl(RefPtr<CustomPropertyData const> inheritable_parent, AK::Function<Optional<CustomPropertyRegistration const&>(Utf16FlyString const&)> get_custom_property_registration) const;
    RefPtr<CustomPropertyData const> inheritable(Layout::BegunRead const& read, DOM::Document const&) const;
    // What a child inherits of an environment the style engine resolved, which the engine decided.
    void set_inheritable(DOM::Document const&, RefPtr<CustomPropertyData const>) const;

    OrderedHashMap<Utf16FlyString, StyleProperty> const& own_values() const { return m_own_values; }

    // How many of the own values this element's own cascade declared. The rest were absorbed from
    // the chain above to keep it short, and are already what that chain resolved them to - so they
    // are neither this element's to declare nor its to resolve again. They sit after the declared
    // ones, because absorption appends.
    [[nodiscard]] size_t declared_count() const { return m_declared_count; }

    // Whether this environment declares the same names, in the same order, as another: what its
    // declared name atoms are made of, whatever the values.
    [[nodiscard]] bool declares_same_names(CustomPropertyData const&) const;

    // The engine's identities for the names this environment declares, sorted and deduplicated,
    // worked out once for the environment rather than once for each element handed it: a page whose
    // theme declares a thousand names hands the same thousand to every element under it. Atom ids
    // are process-global, but each document must acquire its own references, so the document that
    // asked is part of what the answer is good for.
    template<typename InternName>
    [[nodiscard]] ReadonlySpan<StyleAtomID> declared_name_atoms(FlatPtr document_identity, u64 atom_generation, InternName&& intern) const
    {
        if (m_cached_name_atoms_document_identity == document_identity && m_cached_name_atoms_generation == atom_generation)
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
        m_cached_name_atoms_generation = atom_generation;
        return m_cached_declared_name_atoms.span();
    }

    void for_each_property(Function<void(Utf16FlyString const&, StyleProperty const&)> callback) const;

    RefPtr<CustomPropertyData const> parent() const { return m_parent; }

    bool is_empty() const;

    // This monotonic identity remains unambiguous after the object dies, so retained StyleEngine
    // catalogs can name an environment without retaining or dereferencing the C++ object.
    u64 identity() const { return m_identity; }
    void const* rust_store() const { return m_rust_store; }

private:
    CustomPropertyData(OrderedHashMap<Utf16FlyString, StyleProperty> own_values, RefPtr<CustomPropertyData const> parent, RefPtr<CustomPropertyData const> inheritance_parent, u8 ancestor_count, size_t declared_count, void const* prebuilt_rust_store, u64 identity = 0);

    OrderedHashMap<Utf16FlyString, StyleProperty> m_own_values;
    RefPtr<CustomPropertyData const> m_parent;
    u8 m_ancestor_count { 0 };
    size_t m_declared_count { 0 };
    u64 m_identity { 0 };
    mutable FlatPtr m_cached_name_atoms_document_identity { NumericLimits<FlatPtr>::max() };
    mutable u64 m_cached_name_atoms_generation { 0 };
    mutable Vector<StyleAtomID> m_cached_declared_name_atoms;
    mutable FlatPtr m_cached_inheritable_document_identity { NumericLimits<FlatPtr>::max() };
    mutable size_t m_cached_inheritable_generation { NumericLimits<size_t>::max() };
    mutable RefPtr<CustomPropertyData const> m_cached_inheritable_data;
    mutable bool m_cached_inheritable_is_self { false };
    struct AnimationOwner {
        UniqueNodeID element;
        Optional<PseudoElement> pseudo_element;
    };
    Optional<AnimationOwner> m_animation_owner;
    void const* m_rust_store { nullptr };
};

}

// The style engine keeps the custom-property environment each element holds, with a reference of its own.
extern "C" WEB_API void web_css_custom_property_data_reference(void const*);
extern "C" WEB_API void web_css_custom_property_data_unreference(void const*);
