/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGTextContentElement.h>
#include <LibWeb/SVG/SVGURIReference.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/text.html#TextPathElement
class SVGTextPathElement
    : public SVGTextContentElement
    , public SVGURIReferenceMixin<SupportsXLinkHref::Yes> {
    WEB_WRAPPABLE(SVGTextPathElement, SVGTextContentElement);
    GC_DECLARE_ALLOCATOR(SVGTextPathElement);

public:
    virtual CSS::ElementBoxKind box_kind() const override;

    // The `href`/`xlink:href` this element names a shape with, and the parsed `startOffset`, as the element publishes
    // them to layout.
    Optional<Utf16String> href_attribute_value() const;
    Optional<NumberPercentage> const& parsed_start_offset() const { return m_start_offset; }

    // https://w3c.github.io/svgwg/svg2-draft/text.html#__svg__SVGTextPathElement__startOffset
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE_WITH_GETTER(startOffset, start_offset, Horizontal, SVGLengthValue::number(0));

protected:
    SVGTextPathElement(DOM::Document&, DOM::QualifiedName);
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

private:
    virtual bool is_svg_text_path_element() const final { return true; }

    Optional<NumberPercentage> m_start_offset;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGTextPathElement>() const { return is_svg_text_path_element(); }

}
