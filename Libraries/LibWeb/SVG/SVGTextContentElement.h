/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Geometry/DOMPoint.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/text.html#InterfaceSVGTextContentElement
class SVGTextContentElement : public SVGGraphicsElement {
    WEB_WRAPPABLE(SVGTextContentElement, SVGGraphicsElement);

public:
    WebIDL::ExceptionOr<WebIDL::Long> get_number_of_chars();
    WebIDL::ExceptionOr<float> get_computed_text_length();
    WebIDL::ExceptionOr<float> get_sub_string_length(WebIDL::UnsignedLong charnum, WebIDL::UnsignedLong nchars);
    GC::Ref<Geometry::DOMPoint> get_start_position_of_char(WebIDL::UnsignedLong charnum);
    WebIDL::ExceptionOr<GC::Ref<Geometry::DOMRect>> get_extent_of_char(WebIDL::UnsignedLong charnum);

protected:
    SVGTextContentElement(DOM::Document&, DOM::QualifiedName);

private:
    Vector<Layout::RustFFI::FfiSvgTextCharacterCell> character_cells();
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGTextContentElement>() const { return is_svg_text_content_element(); }

}
