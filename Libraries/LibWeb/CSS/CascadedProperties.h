/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/CSS/CascadeOrigin.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class CascadedProperties final : public JS::Cell {
    GC_CELL(CascadedProperties, JS::Cell);
    GC_DECLARE_ALLOCATOR(CascadedProperties);

public:
    virtual ~CascadedProperties() override;

    [[nodiscard]] RefPtr<StyleValue const> property(PropertyNameAndID const&) const;
    [[nodiscard]] GC::Ptr<CSSStyleDeclaration const> property_source(PropertyNameAndID const&) const;
    [[nodiscard]] bool is_property_important(PropertyNameAndID const&) const;

    void set_property(PropertyNameAndID const&, NonnullRefPtr<StyleValue const>, Important, CascadeOrigin, Optional<FlyString> layer_name, GC::Ptr<CSSStyleDeclaration const> source);
    void set_property_from_presentational_hint(PropertyID, NonnullRefPtr<StyleValue const>);

    void revert_property(PropertyNameAndID const&, Important, CascadeOrigin);
    void revert_layer_property(PropertyNameAndID const&, Important, Optional<FlyString> layer_name);

    struct Entry {
        StyleProperty property;
        CascadeOrigin origin;
        Optional<FlyString> layer_name;
        GC::Ptr<CSSStyleDeclaration const> source;
    };
    OrderedHashMap<PropertyID, Entry> const& unresolved_shorthands() const { return m_unresolved_shorthands; }
    void set_unresolved_shorthand(PropertyID, NonnullRefPtr<StyleValue const>, Important, CascadeOrigin, Optional<FlyString> layer_name, GC::Ptr<CSSStyleDeclaration const> source);

    OrderedHashMap<FlyString, StyleProperty> custom_properties() const;

private:
    CascadedProperties();

    virtual void visit_edges(Visitor&) override;

    Optional<Vector<Entry>&> get_entries(PropertyNameAndID const&);
    Optional<Vector<Entry> const&> get_entries(PropertyNameAndID const&) const;
    Vector<Entry>& ensure_entry(PropertyNameAndID const&);
    void remove_entry(PropertyNameAndID const&);

    HashMap<PropertyID, Vector<Entry>> m_properties;
    OrderedHashMap<FlyString, Vector<Entry>> m_custom_properties;
    OrderedHashMap<PropertyID, Entry> m_unresolved_shorthands;
};

}
