/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGTextPositioningElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/text.html#InterfaceSVGTSpanElement
class SVGTSpanElement : public SVGTextPositioningElement {
    WEB_PLATFORM_OBJECT(SVGTSpanElement, SVGTextPositioningElement);
    JS_DECLARE_ALLOCATOR(SVGTSpanElement);

public:
    virtual JS::GCPtr<Layout::Node> create_layout_node(CSS::StyleProperties) override;

protected:
    SVGTSpanElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
