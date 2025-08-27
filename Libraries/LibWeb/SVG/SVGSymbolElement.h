/*
 * Copyright (c) 2023, Preston Taylor <95388976+PrestonLTaylor@users.noreply.github.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGFitToViewBox.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::SVG {

class SVGSymbolElement final : public SVGGraphicsElement
    , public SVGFitToViewBox {
    WEB_PLATFORM_OBJECT(SVGSymbolElement, SVGGraphicsElement);
    GC_DECLARE_ALLOCATOR(SVGSymbolElement);

public:
    virtual ~SVGSymbolElement() override = default;

    virtual bool is_presentational_hint(FlyString const&) const override;
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const override;

private:
    SVGSymbolElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

    bool is_direct_child_of_use_shadow_tree() const;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;
};

}
