/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/AffineTransform.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGMaskElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGMaskElement);

SVGMaskElement::SVGMaskElement(DOM::Document& document, DOM::QualifiedName tag_name)
    : SVGGraphicsElement(document, move(tag_name))
{
}

SVGMaskElement::~SVGMaskElement() = default;

RefPtr<Layout::Node> SVGMaskElement::create_layout_node(CSS::LayoutStyle)
{
    // Masks are handled as a special case in the TreeBuilder.
    return nullptr;
}

void SVGMaskElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == AttributeNames::maskUnits) {
        m_mask_units = AttributeParser::parse_units(value.value_or({}));
    } else if (name == AttributeNames::maskContentUnits) {
        m_mask_content_units = AttributeParser::parse_units(value.value_or({}));
    } else if (name == AttributeNames::x) {
        m_x = AttributeParser::parse_number_percentage(value.value_or({}));
    } else if (name == AttributeNames::y) {
        m_y = AttributeParser::parse_number_percentage(value.value_or({}));
    } else if (name == AttributeNames::width) {
        m_width = AttributeParser::parse_number_percentage(value.value_or({}));
    } else if (name == AttributeNames::height) {
        m_height = AttributeParser::parse_number_percentage(value.value_or({}));
    }
}

// https://drafts.csswg.org/css-masking/#element-attrdef-mask-maskcontentunits
MaskContentUnits SVGMaskElement::mask_content_units() const
{
    // If attribute maskContentUnits is not specified, then the effect is as if a value of userSpaceOnUse were specified.
    return m_mask_content_units.value_or(MaskContentUnits::UserSpaceOnUse);
}

// https://drafts.csswg.org/css-masking/#element-attrdef-mask-maskunits
MaskUnits SVGMaskElement::mask_units() const
{
    // If attribute maskUnits is not specified, then the effect is as if a value of objectBoundingBox were specified.
    return m_mask_units.value_or(MaskUnits::ObjectBoundingBox);
}

// https://drafts.csswg.org/css-masking/#element-attrdef-mask-x
NumberPercentage SVGMaskElement::mask_x() const
{
    // The x-axis coordinate of one corner of the rectangle for the largest possible offscreen buffer. If the attribute
    // is not specified but at least one of the attributes y, width or height are specified, the effect is as if a value
    // of '-10%' were specified.
    // AD-HOC: We use the '-10%' default also when none of x, y, width and height are specified; that gives the masking
    //         area from SVG 1.1 — and matches the behavior of other engines.
    return m_x.value_or(NumberPercentage::create_percentage(-10));
}

// https://drafts.csswg.org/css-masking/#element-attrdef-mask-y
NumberPercentage SVGMaskElement::mask_y() const
{
    // The y-axis coordinate of one corner of the rectangle for the largest possible offscreen buffer. If the attribute
    // is not specified but at least one of the attributes x, width or height are specified, the effect is as if a value
    // of '-10%' were specified.
    // AD-HOC: See mask_x().
    return m_y.value_or(NumberPercentage::create_percentage(-10));
}

// https://drafts.csswg.org/css-masking/#element-attrdef-mask-width
NumberPercentage SVGMaskElement::mask_width() const
{
    // The width of the largest possible offscreen buffer. A negative value or a value of zero disables rendering of the
    // element. If the attribute is not specified but at least one of the attributes x, y or height are specified, the
    // effect is as if a value of '120%' were specified.
    // AD-HOC: See mask_x().
    return m_width.value_or(NumberPercentage::create_percentage(120));
}

// https://drafts.csswg.org/css-masking/#element-attrdef-mask-height
NumberPercentage SVGMaskElement::mask_height() const
{
    // The height of the largest possible offscreen buffer. A negative value or a value of zero disables rendering of
    // the element. If the attribute is not specified but at least one of the attributes x, y or width are specified,
    // the effect is as if a value of '120%' were specified.
    // AD-HOC: See mask_x().
    return m_height.value_or(NumberPercentage::create_percentage(120));
}

// https://drafts.csswg.org/css-masking/#element-attrdef-mask-maskunits
CSSPixelRect SVGMaskElement::resolve_masking_area(CSSPixelRect const& target_object_bounding_box, Gfx::FloatSize const& viewport_size, Gfx::AffineTransform const& user_space_to_css_pixels) const
{
    Gfx::FloatRect masking_area;
    if (mask_units() == MaskUnits::ObjectBoundingBox) {
        // objectBoundingBox
        //     x, y, width and height represent fractions or percentages of the object bounding box of the element to
        //     which the mask is applied.
        auto target = target_object_bounding_box.to_type<float>();
        masking_area = {
            target.x() + (mask_x().value() * target.width()),
            target.y() + (mask_y().value() * target.height()),
            mask_width().value() * target.width(),
            mask_height().value() * target.height(),
        };
    } else {
        // userSpaceOnUse
        //     x, y, width and height represent values in the current user coordinate system in place at the time when
        //     the mask element is referenced (i.e., the user coordinate system for the element referencing the mask
        //     element via the mask property).
        // Percentages resolve against the SVG viewport; from https://svgwg.org/svg2-draft/coords.html#Units:
        //     For any x-coordinate value or width value expressed as a percentage of the SVG viewport, the value to use
        //     must be the percentage, in user units, of the width parameter of the 'viewBox' applied to that viewport.
        //     If no 'viewBox' is specified, then the value to use must be the percentage, in user units, of the width
        //     of the SVG viewport.
        // (Correspondingly with the height parameter for y-coordinate and height values.)
        Gfx::FloatRect user_space_masking_area {
            mask_x().resolve_relative_to(viewport_size.width()),
            mask_y().resolve_relative_to(viewport_size.height()),
            mask_width().resolve_relative_to(viewport_size.width()),
            mask_height().resolve_relative_to(viewport_size.height()),
        };
        if (user_space_masking_area.is_empty())
            return {};
        masking_area = user_space_to_css_pixels.map(user_space_masking_area);
    }

    // To convey "a negative value or a value of zero disables rendering of the element" to the caller, we return an
    // empty masking area.
    if (masking_area.is_empty())
        return {};
    return masking_area.to_type<CSSPixels>();
}

}
