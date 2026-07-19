/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibWeb/CSS/CascadeOrigin.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// A thin shell over the Rust cascaded property store. The store owns the
// entries (values, importance, origin, layer, cascade order); this shell keeps
// the GC-weak declaration sources the store cannot hold, one pair per
// store-assigned slot.
class CascadedProperties final : public RefCounted<CascadedProperties> {
public:
    static NonnullRefPtr<CascadedProperties> create();

    ~CascadedProperties();

    [[nodiscard]] RefPtr<StyleValue const> property(PropertyID) const;
    [[nodiscard]] PropertyID property_with_higher_priority(PropertyID, PropertyID) const;
    [[nodiscard]] GC::Ptr<CSSStyleDeclaration const> property_source(PropertyID) const;
    [[nodiscard]] GC::Ptr<DOM::ShadowRoot const> property_source_shadow_root(PropertyID) const;
    [[nodiscard]] Optional<StyleProperty> style_property(PropertyID) const;

    void set_property(PropertyID, NonnullRefPtr<StyleValue const>, Important, CascadeOrigin, Optional<Utf16FlyString> layer_name, GC::Ptr<CSS::CSSStyleDeclaration const> source, GC::Ptr<DOM::ShadowRoot const> source_shadow_root);

    void revert_property(PropertyID, Important, CascadeOrigin);
    void revert_layer_property(PropertyID, Important, CascadeOrigin, Optional<Utf16FlyString> layer_name, GC::Ptr<DOM::ShadowRoot const> source_shadow_root);

    // For the Rust-driven cascade application: the underlying store, and assignment of the
    // GC-weak declaration source pair for a slot the store handed out.
    ComputedValuesFFI::CascadedPropertyStore* rust_store() { return m_store; }
    void assign_source_slot(u32 slot, GC::Ptr<CSS::CSSStyleDeclaration const> source, GC::Ptr<DOM::ShadowRoot const> source_shadow_root);

private:
    CascadedProperties();

    struct SourcePair {
        GC::Weak<CSS::CSSStyleDeclaration const> source;
        GC::Weak<DOM::ShadowRoot const> source_shadow_root;
    };

    ComputedValuesFFI::CascadedPropertyStore* m_store { nullptr };
    Vector<SourcePair> m_source_slots;
};

}
