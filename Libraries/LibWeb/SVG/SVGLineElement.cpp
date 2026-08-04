/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Path.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGLineElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGLineElement);

SVGLineElement::SVGLineElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGeometryElement(document, qualified_name)
{
}

void SVGLineElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == SVG::AttributeNames::x1) {
        m_x1 = AttributeParser::parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::y1) {
        m_y1 = AttributeParser::parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::x2) {
        m_x2 = AttributeParser::parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::y2) {
        m_y2 = AttributeParser::parse_number_percentage(value.value_or({}));
    }

    if (name.is_one_of(SVG::AttributeNames::x1, SVG::AttributeNames::y1, SVG::AttributeNames::x2, SVG::AttributeNames::y2))
        set_needs_layout_update(DOM::SetNeedsLayoutReason::StyleChange);
}

Gfx::Path SVGLineElement::get_path(CSSPixelSize viewport_size)
{
    auto const viewport_width = viewport_size.width().to_float();
    auto const viewport_height = viewport_size.height().to_float();

    Gfx::Path path;
    float const x1 = m_x1.value_or({ 0, false }).resolve_relative_to(viewport_width);
    float const y1 = m_y1.value_or({ 0, false }).resolve_relative_to(viewport_height);
    float const x2 = m_x2.value_or({ 0, false }).resolve_relative_to(viewport_width);
    float const y2 = m_y2.value_or({ 0, false }).resolve_relative_to(viewport_height);

    // 1. perform an absolute moveto operation to absolute location (x1,y1)
    path.move_to({ x1, y1 });

    // 2. perform an absolute lineto operation to absolute location (x2,y2)
    path.line_to({ x2, y2 });

    return path;
}

}
