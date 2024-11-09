/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGTextPositioningElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/text.html#InterfaceSVGTextElement
class SVGTextElement : public SVGTextPositioningElement {
    WEB_PLATFORM_OBJECT(SVGTextElement, SVGTextPositioningElement);
    JS_DECLARE_ALLOCATOR(SVGTextElement);

public:
    virtual JS::GCPtr<Layout::Node> create_layout_node(CSS::StyleProperties) override;

protected:
    SVGTextElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
