/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Geometry/DOMPoint.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/text.html#InterfaceSVGTextContentElement
class SVGTextContentElement : public SVGGraphicsElement {
    WEB_PLATFORM_OBJECT(SVGTextContentElement, SVGGraphicsElement);

public:
    WebIDL::ExceptionOr<WebIDL::Long> get_number_of_chars() const;

    Optional<TextAnchor> text_anchor() const;

    ByteString text_contents() const;

    JS::NonnullGCPtr<Geometry::DOMPoint> get_start_position_of_char(WebIDL::UnsignedLong charnum);

protected:
    SVGTextContentElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
