/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

class SVGFEFloodElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFEFloodElement> {
    WEB_PLATFORM_OBJECT(SVGFEFloodElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEFloodElement);

public:
    virtual ~SVGFEFloodElement() override = default;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

    Gfx::Color flood_color() const;
    float flood_opacity() const;

private:
    SVGFEFloodElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
};

}
