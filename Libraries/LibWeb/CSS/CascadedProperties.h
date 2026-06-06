/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedBitmap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibWeb/CSS/CascadeOrigin.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class CascadedProperties final : public RefCounted<CascadedProperties> {
public:
    static NonnullRefPtr<CascadedProperties> create();

    ~CascadedProperties();

    [[nodiscard]] RefPtr<StyleValue const> property(PropertyID) const;
    [[nodiscard]] PropertyID property_with_higher_priority(PropertyID, PropertyID) const;
    [[nodiscard]] GC::Ptr<CSSStyleDeclaration const> property_source(PropertyID) const;
    [[nodiscard]] GC::Ptr<DOM::ShadowRoot const> property_source_shadow_root(PropertyID) const;
    [[nodiscard]] Optional<StyleProperty> style_property(PropertyID) const;

    void set_property(PropertyID, NonnullRefPtr<StyleValue const>, Important, CascadeOrigin, Optional<FlyString> layer_name, GC::Ptr<CSS::CSSStyleDeclaration const> source, GC::Ptr<DOM::ShadowRoot const> source_shadow_root);

    void revert_property(PropertyID, Important, CascadeOrigin);
    void revert_layer_property(PropertyID, Important, CascadeOrigin, Optional<FlyString> layer_name, GC::Ptr<DOM::ShadowRoot const> source_shadow_root);

private:
    CascadedProperties();

    struct Entry {
        StyleProperty property;
        size_t cascade_index { 0 };
        CascadeOrigin origin;
        Optional<FlyString> layer_name;
        GC::Weak<CSS::CSSStyleDeclaration const> source;
        GC::Weak<DOM::ShadowRoot const> source_shadow_root;
    };
    HashMap<PropertyID, Vector<Entry>> m_properties;
    size_t m_next_cascade_index { 0 };
    AK::FixedBitmap<to_underlying(last_longhand_property_id) + 1> m_contained_properties_cache { false };
};

}
