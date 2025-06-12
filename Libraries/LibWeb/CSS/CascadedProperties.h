/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/CascadeOrigin.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleProperty.h>

namespace Web::CSS {

class CascadedProperties final : public JS::Cell {
    GC_CELL(CascadedProperties, JS::Cell);
    GC_DECLARE_ALLOCATOR(CascadedProperties);

public:
    virtual ~CascadedProperties() override;

    [[nodiscard]] RefPtr<CSSStyleValue const> property(PropertyID) const;
    [[nodiscard]] GC::Ptr<CSSStyleDeclaration const> property_source(PropertyID) const;
    [[nodiscard]] bool is_property_important(PropertyID) const;

    void set_property(PropertyID, NonnullRefPtr<CSSStyleValue const>, Important, CascadeOrigin, Optional<FlyString> layer_name, GC::Ptr<CSS::CSSStyleDeclaration const> source);
    void set_property_from_presentational_hint(PropertyID, NonnullRefPtr<CSSStyleValue const>);

    void revert_property(PropertyID, Important, CascadeOrigin);
    void revert_layer_property(PropertyID, Important, Optional<FlyString> layer_name);

    void resolve_unresolved_properties(GC::Ref<DOM::Element>, Optional<PseudoElement>);

private:
    CascadedProperties();

    virtual void visit_edges(Visitor&) override;

    struct Entry {
        StyleProperty property;
        CascadeOrigin origin;
        Optional<FlyString> layer_name;
        GC::Ptr<CSS::CSSStyleDeclaration const> source;
    };
    HashMap<PropertyID, Vector<Entry>> m_properties;
};

}
