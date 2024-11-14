/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/embedded.html#InterfaceSVGForeignObjectElement
class SVGForeignObjectElement final : public SVGGraphicsElement {
    WEB_PLATFORM_OBJECT(SVGForeignObjectElement, SVGGraphicsElement);
    GC_DECLARE_ALLOCATOR(SVGForeignObjectElement);

public:
    virtual ~SVGForeignObjectElement() override;

    virtual GC::Ptr<Layout::Node> create_layout_node(CSS::StyleProperties) override;

    GC::Ref<SVG::SVGAnimatedLength> x();
    GC::Ref<SVG::SVGAnimatedLength> y();
    GC::Ref<SVG::SVGAnimatedLength> width();
    GC::Ref<SVG::SVGAnimatedLength> height();

private:
    SVGForeignObjectElement(DOM::Document& document, DOM::QualifiedName qualified_name);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void apply_presentational_hints(CSS::StyleProperties&) const override;

    GC::Ptr<SVG::SVGAnimatedLength> m_x;
    GC::Ptr<SVG::SVGAnimatedLength> m_y;
    GC::Ptr<SVG::SVGAnimatedLength> m_width;
    GC::Ptr<SVG::SVGAnimatedLength> m_height;
};

}
