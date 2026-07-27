/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/embedded.html#InterfaceSVGForeignObjectElement
class SVGForeignObjectElement final : public SVGGraphicsElement {
    WEB_PLATFORM_OBJECT(SVGForeignObjectElement, SVGGraphicsElement);
    GC_DECLARE_ALLOCATOR(SVGForeignObjectElement);

public:
    virtual ~SVGForeignObjectElement() override;

    virtual RefPtr<Layout::Node> create_layout_node(NonnullRefPtr<CSS::ComputedValues const>) override;

    // AD-HOC: The spec states that the x, y, width and height IDL attributes reflect the respective computed values and their
    //         corresponding presentation attributes but other browsers reflect the attribute values instead - see
    //         https://github.com/w3c/svgwg/issues/1153

    // https://w3c.github.io/svgwg/svg2-draft/embedded.html#__svg__SVGForeignObjectElement__x
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(x, Horizontal, CSS::NumberStyleValue::create(0));

    // https://w3c.github.io/svgwg/svg2-draft/embedded.html#__svg__SVGForeignObjectElement__y
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(y, Vertical, CSS::NumberStyleValue::create(0));

    // https://w3c.github.io/svgwg/svg2-draft/embedded.html#__svg__SVGForeignObjectElement__width
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(width, Horizontal, CSS::NumberStyleValue::create(0));

    // https://w3c.github.io/svgwg/svg2-draft/embedded.html#__svg__SVGForeignObjectElement__height
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(height, Vertical, CSS::NumberStyleValue::create(0));

private:
    SVGForeignObjectElement(DOM::Document& document, DOM::QualifiedName qualified_name);

    virtual bool is_svg_foreign_object_element() const override { return true; }

    virtual void initialize(JS::Realm&) override;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGForeignObjectElement>() const { return is_svg_foreign_object_element(); }

}
