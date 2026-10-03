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
    WEB_WRAPPABLE(SVGTextElement, SVGTextPositioningElement);
    GC_DECLARE_ALLOCATOR(SVGTextElement);

public:
    virtual CSS::ElementBoxKind box_kind() const override;

protected:
    SVGTextElement(DOM::Document&, DOM::QualifiedName);

private:
    virtual bool is_svg_text_element() const final { return true; }
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGTextElement>() const { return is_svg_text_element(); }

}
