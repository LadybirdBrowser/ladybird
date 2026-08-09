/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGGeometryElement.h>
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
    virtual RefPtr<Layout::Node> create_layout_node(CSS::LayoutStyle) override;

    GC::Ptr<SVGGeometryElement const> path_or_shape() const;

    float start_offset_for_path_length(float path_length) const;

    // https://w3c.github.io/svgwg/svg2-draft/text.html#__svg__SVGTextPathElement__startOffset
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE_WITH_GETTER(startOffset, start_offset, Horizontal, CSS::NumberStyleValue::create(0));

protected:
    SVGTextPathElement(DOM::Document&, DOM::QualifiedName);
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

private:
    Optional<NumberPercentage> m_start_offset;
};

}
