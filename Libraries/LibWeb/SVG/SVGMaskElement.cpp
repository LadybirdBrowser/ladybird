/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/AffineTransform.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGMaskElement.h>
#include <LibWeb/SVG/SVGUnitTypes.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGMaskElement);

SVGMaskElement::SVGMaskElement(DOM::Document& document, DOM::QualifiedName tag_name)
    : SVGGraphicsElement(document, move(tag_name))
{
}

SVGMaskElement::~SVGMaskElement() = default;

CSS::ElementBoxKind SVGMaskElement::box_kind() const
{
    // Masks are handled as a special case in the TreeBuilder.
    return CSS::ElementBoxKind::NoBox;
}

void SVGMaskElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == AttributeNames::maskUnits) {
        m_mask_units = parse_units(value.value_or({}));
    } else if (name == AttributeNames::maskContentUnits) {
        m_mask_content_units = parse_units(value.value_or({}));
    } else if (name == AttributeNames::x) {
        m_x = parse_number_percentage(value.value_or({}));
    } else if (name == AttributeNames::y) {
        m_y = parse_number_percentage(value.value_or({}));
    } else if (name == AttributeNames::width) {
        m_width = parse_number_percentage(value.value_or({}));
    } else if (name == AttributeNames::height) {
        m_height = parse_number_percentage(value.value_or({}));
    } else {
        return;
    }
    mark_resource_box_referencing_elements_for_layout_update();
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

// https://drafts.fxtf.org/css-masking/#dom-svgmaskelement-maskunits
GC::Ref<SVGAnimatedEnumeration> SVGMaskElement::mask_units_for_bindings()
{
    return svg_animated_enumeration_for_attribute(AttributeNames::maskUnits, svg_unit_type_keywords, to_underlying(SVGUnitType::ObjectBoundingBox));
}

// https://drafts.fxtf.org/css-masking/#dom-svgmaskelement-maskcontentunits
GC::Ref<SVGAnimatedEnumeration> SVGMaskElement::mask_content_units_for_bindings()
{
    return svg_animated_enumeration_for_attribute(AttributeNames::maskContentUnits, svg_unit_type_keywords, to_underlying(SVGUnitType::UserSpaceOnUse));
}

}
