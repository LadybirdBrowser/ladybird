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
    JS_DECLARE_ALLOCATOR(SVGForeignObjectElement);

public:
    virtual ~SVGForeignObjectElement() override;

    virtual JS::GCPtr<Layout::Node> create_layout_node(NonnullRefPtr<CSS::StyleProperties>) override;

    JS::NonnullGCPtr<SVG::SVGAnimatedLength> x();
    JS::NonnullGCPtr<SVG::SVGAnimatedLength> y();
    JS::NonnullGCPtr<SVG::SVGAnimatedLength> width();
    JS::NonnullGCPtr<SVG::SVGAnimatedLength> height();

private:
    SVGForeignObjectElement(DOM::Document& document, DOM::QualifiedName qualified_name);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void apply_presentational_hints(CSS::StyleProperties&) const override;

    JS::GCPtr<SVG::SVGAnimatedLength> m_x;
    JS::GCPtr<SVG::SVGAnimatedLength> m_y;
    JS::GCPtr<SVG::SVGAnimatedLength> m_width;
    JS::GCPtr<SVG::SVGAnimatedLength> m_height;
};

}
