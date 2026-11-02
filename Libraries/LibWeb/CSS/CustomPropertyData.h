/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NumericLimits.h>
#include <AK/QuickSort.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/CSS/StyleEngineIdentifiers.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

// Whether the custom-property environment an element or pseudo-element holds is the overlay of what its own
// animations sample, over the environment its style resolves to.
enum class IsAnimationOverlay : bool {
    No,
    Yes,
};

// Chain of custom property environments with structural sharing. Each node is a Rust store, which holds the
// properties declared directly on its element, with those it took in from the chain above it, and lies over the store
// of its parent: a reference to one of these is a reference to the store, counted by the store's own count.
// NB: The style engine references the stores elements hold, and may take and give up those references on whichever
//     thread it runs on.
class WEB_API CustomPropertyData {
    AK_MAKE_NONCOPYABLE(CustomPropertyData);
    AK_MAKE_NONMOVABLE(CustomPropertyData);

public:
    static NonnullRefPtr<CustomPropertyData> create(
        OrderedHashMap<Utf16FlyString, StyleProperty> own_values,
        RefPtr<CustomPropertyData const> parent,
        // Transfers one strong Rust store reference when non-null, which holds the values in place of `own_values`.
        void const* prebuilt_rust_store = nullptr);
    // The values an element's animations sample over `base`, the environment it holds beneath them.
    static NonnullRefPtr<CustomPropertyData> create_animation_overlay(
        OrderedHashMap<Utf16FlyString, StyleProperty> animated_values,
        RefPtr<CustomPropertyData const> base);
    // The environment whose store is `rust_store`, a live store, taking a reference to it.
    static NonnullRefPtr<CustomPropertyData const> of_store(void const* rust_store);

    void ref() const;
    void unref() const;
    [[nodiscard]] size_t ref_count() const;

    Optional<StyleProperty> get(Utf16FlyString const& name) const;
    RefPtr<CustomPropertyData const> inheritable_impl(RefPtr<CustomPropertyData const> inheritable_parent, AK::Function<Optional<CustomPropertyRegistration const&>(Utf16FlyString const&)> get_custom_property_registration) const;
    RefPtr<CustomPropertyData const> inheritable(Layout::BegunRead const& read, DOM::Document const&) const;

    // The values this node holds itself: the ones its own cascade declared, in declaration order, then the ones it took
    // in from the chain above it.
    void for_each_own_value(Function<void(Utf16FlyString const&, StyleProperty const&)> const& callback) const;
    // The values this node's own cascade declared, in declaration order.
    void for_each_declared_value(Function<void(Utf16FlyString const&, StyleProperty const&)> const& callback) const;
    [[nodiscard]] size_t own_value_count() const;
    // Whether this node's own cascade declared `name` with a value equal to `value`.
    [[nodiscard]] bool declares_value(Utf16FlyString const& name, StyleValue const& value) const;

    // How many of the own values this element's own cascade declared. The rest were absorbed from
    // the chain above to keep it short, and are already what that chain resolved them to - so they
    // are neither this element's to declare nor its to resolve again. They sit after the declared
    // ones, because absorption appends.
    [[nodiscard]] size_t declared_count() const;

    void for_each_property(Function<void(Utf16FlyString const&, StyleProperty const&)> callback) const;

    // The node this one lies over in its chain, or, for the values an element's animations sample, the environment they
    // lie over. An environment the style engine resolved names the one it was resolved over as its parent instead,
    // instead.
    RefPtr<CustomPropertyData const> parent() const;

    bool is_empty() const;

    // The environment's identity: the address of its store, which the style engine's catalogs keep alive for as long
    // as they name it, so no other store takes the address while they do.
    u64 identity() const { return reinterpret_cast<FlatPtr>(this); }
    void const* rust_store() const { return this; }

private:
    CustomPropertyData() = delete;
    ~CustomPropertyData() = delete;
    u8 ancestor_count() const;
};

}
