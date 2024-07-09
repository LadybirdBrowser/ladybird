/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGLinearGradientElementPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGLinearGradientElement.h>
#include <LibWeb/SVG/SVGStopElement.h>

namespace Web::SVG {

JS_DEFINE_ALLOCATOR(SVGLinearGradientElement);

SVGLinearGradientElement::SVGLinearGradientElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGradientElement(document, qualified_name)
{
}

void SVGLinearGradientElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGLinearGradientElement);
}

void SVGLinearGradientElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value)
{
    SVGGradientElement::attribute_changed(name, old_value, value);

    // FIXME: Should allow for `<number-percentage> | <length>` for x1, x2, y1, y2
    if (name == SVG::AttributeNames::x1) {
        m_x1 = AttributeParser::parse_number_percentage(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::y1) {
        m_y1 = AttributeParser::parse_number_percentage(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::x2) {
        m_x2 = AttributeParser::parse_number_percentage(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::y2) {
        m_y2 = AttributeParser::parse_number_percentage(value.value_or(String {}));
    }
}

// https://www.w3.org/TR/SVG11/pservers.html#LinearGradientElementX1Attribute
NumberPercentage SVGLinearGradientElement::start_x() const
{
    HashTable<SVGGradientElement const*> seen_gradients;
    return start_x_impl(seen_gradients);
}

NumberPercentage SVGLinearGradientElement::start_x_impl(HashTable<SVGGradientElement const*>& seen_gradients) const
{
    if (m_x1.has_value())
        return *m_x1;
    if (auto gradient = linked_linear_gradient(seen_gradients))
        return gradient->start_x_impl(seen_gradients);
    // If the attribute is not specified, the effect is as if a value of '0%' were specified.
    return NumberPercentage::create_percentage(0);
}

// https://www.w3.org/TR/SVG11/pservers.html#LinearGradientElementY1Attribute
NumberPercentage SVGLinearGradientElement::start_y() const
{
    HashTable<SVGGradientElement const*> seen_gradients;
    return start_y_impl(seen_gradients);
}

NumberPercentage SVGLinearGradientElement::start_y_impl(HashTable<SVGGradientElement const*>& seen_gradients) const
{
    if (m_y1.has_value())
        return *m_y1;
    if (auto gradient = linked_linear_gradient(seen_gradients))
        return gradient->start_y_impl(seen_gradients);
    // If the attribute is not specified, the effect is as if a value of '0%' were specified.
    return NumberPercentage::create_percentage(0);
}

// https://www.w3.org/TR/SVG11/pservers.html#LinearGradientElementX2Attribute
NumberPercentage SVGLinearGradientElement::end_x() const
{
    HashTable<SVGGradientElement const*> seen_gradients;
    return end_x_impl(seen_gradients);
}

NumberPercentage SVGLinearGradientElement::end_x_impl(HashTable<SVGGradientElement const*>& seen_gradients) const
{
    if (m_x2.has_value())
        return *m_x2;
    if (auto gradient = linked_linear_gradient(seen_gradients))
        return gradient->end_x_impl(seen_gradients);
    // If the attribute is not specified, the effect is as if a value of '100%' were specified.
    return NumberPercentage::create_percentage(100);
}

// https://www.w3.org/TR/SVG11/pservers.html#LinearGradientElementY2Attribute
NumberPercentage SVGLinearGradientElement::end_y() const
{
    HashTable<SVGGradientElement const*> seen_gradients;
    return end_y_impl(seen_gradients);
}

NumberPercentage SVGLinearGradientElement::end_y_impl(HashTable<SVGGradientElement const*>& seen_gradients) const
{
    if (m_y2.has_value())
        return *m_y2;
    if (auto gradient = linked_linear_gradient(seen_gradients))
        return gradient->end_y_impl(seen_gradients);
    // If the attribute is not specified, the effect is as if a value of '0%' were specified.
    return NumberPercentage::create_percentage(0);
}

Optional<Painting::PaintStyle> SVGLinearGradientElement::to_gfx_paint_style(SVGPaintContext const& paint_context) const
{
    auto units = gradient_units();
    // FIXME: Resolve percentages properly
    Gfx::FloatPoint start_point {};
    Gfx::FloatPoint end_point {};
    // https://svgwg.org/svg2-draft/pservers.html#LinearGradientElementGradientUnitsAttribute
    if (units == GradientUnits::ObjectBoundingBox) {
        // If gradientUnits="objectBoundingBox", the user coordinate system for attributes ‘x1’, ‘y1’, ‘x2’ and ‘y2’
        // is established using the bounding box of the element to which the gradient is applied (see Object bounding
        // box units) and then applying the transform specified by attribute ‘gradientTransform’. Percentages represent
        // values relative to the bounding box for the object.
        // Note: For gradientUnits="objectBoundingBox" both "100%" and "1" are treated the same.
        start_point = { start_x().value(), start_y().value() };
        end_point = { end_x().value(), end_y().value() };
    } else {
        // GradientUnits::UserSpaceOnUse
        // If gradientUnits="userSpaceOnUse", ‘x1’, ‘y1’, ‘x2’, and ‘y2’ represent values in the coordinate system
        // that results from taking the current user coordinate system in place at the time when the gradient element
        // is referenced (i.e., the user coordinate system for the element referencing the gradient element via a
        // fill or stroke property) and then applying the transform specified by attribute ‘gradientTransform’.
        // Percentages represent values relative to the current SVG viewport.
        start_point = Gfx::FloatPoint {
            start_x().resolve_relative_to(paint_context.viewport.width()),
            start_y().resolve_relative_to(paint_context.viewport.height()),
        };
        end_point = Gfx::FloatPoint {
            end_x().resolve_relative_to(paint_context.viewport.width()),
            end_y().resolve_relative_to(paint_context.viewport.height()),
        };
    }

    if (!m_paint_style) {
        m_paint_style = Painting::SVGLinearGradientPaintStyle::create(start_point, end_point);
        // FIXME: Update stops in DOM changes:
        add_color_stops(*m_paint_style);
    } else {
        m_paint_style->set_start_point(start_point);
        m_paint_style->set_end_point(end_point);
    }

    m_paint_style->set_gradient_transform(gradient_paint_transform(paint_context));
    m_paint_style->set_spread_method(to_painting_spread_method(spread_method()));
    return *m_paint_style;
}

JS::NonnullGCPtr<SVGAnimatedLength> SVGLinearGradientElement::x1() const
{
    // FIXME: Implement this properly.
    return SVGAnimatedLength::create(realm(), SVGLength::create(realm(), 0, 0), SVGLength::create(realm(), 0, 0));
}

JS::NonnullGCPtr<SVGAnimatedLength> SVGLinearGradientElement::y1() const
{
    // FIXME: Implement this properly.
    return SVGAnimatedLength::create(realm(), SVGLength::create(realm(), 0, 0), SVGLength::create(realm(), 0, 0));
}

JS::NonnullGCPtr<SVGAnimatedLength> SVGLinearGradientElement::x2() const
{
    // FIXME: Implement this properly.
    return SVGAnimatedLength::create(realm(), SVGLength::create(realm(), 0, 0), SVGLength::create(realm(), 0, 0));
}

JS::NonnullGCPtr<SVGAnimatedLength> SVGLinearGradientElement::y2() const
{
    // FIXME: Implement this properly.
    return SVGAnimatedLength::create(realm(), SVGLength::create(realm(), 0, 0), SVGLength::create(realm(), 0, 0));
}

}
