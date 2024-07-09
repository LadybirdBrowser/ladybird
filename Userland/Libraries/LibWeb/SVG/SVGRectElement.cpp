/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGRectElementPrototype.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGLength.h>
#include <LibWeb/SVG/SVGRectElement.h>

namespace Web::SVG {

JS_DEFINE_ALLOCATOR(SVGRectElement);

SVGRectElement::SVGRectElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGeometryElement(document, qualified_name)
{
}

void SVGRectElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGRectElement);
}

void SVGRectElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value)
{
    SVGGeometryElement::attribute_changed(name, old_value, value);

    if (name == SVG::AttributeNames::x) {
        m_x = AttributeParser::parse_coordinate(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::y) {
        m_y = AttributeParser::parse_coordinate(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::width) {
        m_width = AttributeParser::parse_positive_length(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::height) {
        m_height = AttributeParser::parse_positive_length(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::rx) {
        m_radius_x = AttributeParser::parse_length(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::ry) {
        m_radius_y = AttributeParser::parse_length(value.value_or(String {}));
    }
}

Gfx::Path SVGRectElement::get_path(CSSPixelSize)
{
    float width = m_width.value_or(0);
    float height = m_height.value_or(0);
    float x = m_x.value_or(0);
    float y = m_y.value_or(0);

    Gfx::Path path;
    // If width or height is zero, rendering is disabled.
    if (width == 0 || height == 0)
        return path;

    auto corner_radii = calculate_used_corner_radius_values();
    float rx = corner_radii.width();
    float ry = corner_radii.height();

    // 1. perform an absolute moveto operation to location (x+rx,y);
    path.move_to({ x + rx, y });

    // 2, perform an absolute horizontal lineto with parameter x+width-rx;
    path.horizontal_line_to(x + width - rx);

    // 3. if both rx and ry are greater than zero,
    //    perform an absolute elliptical arc operation to coordinate (x+width,y+ry),
    //    where rx and ry are used as the equivalent parameters to the elliptical arc command,
    //    the x-axis-rotation and large-arc-flag are set to zero,
    //    the sweep-flag is set to one;
    double x_axis_rotation = 0;
    bool large_arc_flag = false;
    bool sweep_flag = true;
    if (rx > 0 && ry > 0)
        path.elliptical_arc_to({ x + width, y + ry }, corner_radii, x_axis_rotation, large_arc_flag, sweep_flag);

    // 4. perform an absolute vertical lineto parameter y+height-ry;
    path.vertical_line_to(y + height - ry);

    // 5. if both rx and ry are greater than zero,
    //    perform an absolute elliptical arc operation to coordinate (x+width-rx,y+height),
    //    using the same parameters as previously;
    if (rx > 0 && ry > 0)
        path.elliptical_arc_to({ x + width - rx, y + height }, corner_radii, x_axis_rotation, large_arc_flag, sweep_flag);

    // 6. perform an absolute horizontal lineto parameter x+rx;
    path.horizontal_line_to(x + rx);

    // 7. if both rx and ry are greater than zero,
    //    perform an absolute elliptical arc operation to coordinate (x,y+height-ry),
    //    using the same parameters as previously;
    if (rx > 0 && ry > 0)
        path.elliptical_arc_to({ x, y + height - ry }, corner_radii, x_axis_rotation, large_arc_flag, sweep_flag);

    // 8. perform an absolute vertical lineto parameter y+ry
    path.vertical_line_to(y + ry);

    // 9. if both rx and ry are greater than zero,
    //    perform an absolute elliptical arc operation with a segment-completing close path operation,
    //    using the same parameters as previously.
    if (rx > 0 && ry > 0)
        path.elliptical_arc_to({ x + rx, y }, corner_radii, x_axis_rotation, large_arc_flag, sweep_flag);

    return path;
}

Gfx::FloatSize SVGRectElement::calculate_used_corner_radius_values() const
{
    // 1. Let rx and ry be length values.
    float rx = 0;
    float ry = 0;

    // 2. If neither ‘rx’ nor ‘ry’ are properly specified, then set both rx and ry to 0. (This will result in square corners.)
    if (!m_radius_x.has_value() && !m_radius_y.has_value()) {
        rx = 0;
        ry = 0;
    }
    // 3. Otherwise, if a properly specified value is provided for ‘rx’, but not for ‘ry’, then set both rx and ry to the value of ‘rx’.
    else if (m_radius_x.has_value() && !m_radius_y.has_value()) {
        rx = m_radius_x.value();
        ry = m_radius_x.value();
    }
    // 4. Otherwise, if a properly specified value is provided for ‘ry’, but not for ‘rx’, then set both rx and ry to the value of ‘ry’.
    else if (m_radius_y.has_value() && !m_radius_x.has_value()) {
        rx = m_radius_y.value();
        ry = m_radius_y.value();
    }
    // 5. Otherwise, both ‘rx’ and ‘ry’ were specified properly. Set rx to the value of ‘rx’ and ry to the value of ‘ry’.
    else {
        rx = m_radius_x.value();
        ry = m_radius_y.value();
    }

    // 6. If rx is greater than half of ‘width’, then set rx to half of ‘width’.
    auto half_width = m_width.value_or(0) / 2;
    if (rx > half_width)
        rx = half_width;

    // 7. If ry is greater than half of ‘height’, then set ry to half of ‘height’.
    auto half_height = m_height.value_or(0) / 2;
    if (ry > half_height)
        ry = half_height;

    // 8. The effective values of ‘rx’ and ‘ry’ are rx and ry, respectively.
    return { rx, ry };
}

// https://www.w3.org/TR/SVG11/shapes.html#RectElementXAttribute
JS::NonnullGCPtr<SVGAnimatedLength> SVGRectElement::x() const
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto base_length = SVGLength::create(realm(), 0, m_x.value_or(0));
    auto anim_length = SVGLength::create(realm(), 0, m_x.value_or(0));
    return SVGAnimatedLength::create(realm(), move(base_length), move(anim_length));
}

// https://www.w3.org/TR/SVG11/shapes.html#RectElementYAttribute
JS::NonnullGCPtr<SVGAnimatedLength> SVGRectElement::y() const
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto base_length = SVGLength::create(realm(), 0, m_y.value_or(0));
    auto anim_length = SVGLength::create(realm(), 0, m_y.value_or(0));
    return SVGAnimatedLength::create(realm(), move(base_length), move(anim_length));
}

// https://www.w3.org/TR/SVG11/shapes.html#RectElementWidthAttribute
JS::NonnullGCPtr<SVGAnimatedLength> SVGRectElement::width() const
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto base_length = SVGLength::create(realm(), 0, m_width.value_or(0));
    auto anim_length = SVGLength::create(realm(), 0, m_width.value_or(0));
    return SVGAnimatedLength::create(realm(), move(base_length), move(anim_length));
}

// https://www.w3.org/TR/SVG11/shapes.html#RectElementHeightAttribute
JS::NonnullGCPtr<SVGAnimatedLength> SVGRectElement::height() const
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto base_length = SVGLength::create(realm(), 0, m_height.value_or(0));
    auto anim_length = SVGLength::create(realm(), 0, m_height.value_or(0));
    return SVGAnimatedLength::create(realm(), move(base_length), move(anim_length));
}

// https://www.w3.org/TR/SVG11/shapes.html#RectElementRXAttribute
JS::NonnullGCPtr<SVGAnimatedLength> SVGRectElement::rx() const
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto base_length = SVGLength::create(realm(), 0, m_radius_x.value_or(0));
    auto anim_length = SVGLength::create(realm(), 0, m_radius_x.value_or(0));
    return SVGAnimatedLength::create(realm(), move(base_length), move(anim_length));
}

// https://www.w3.org/TR/SVG11/shapes.html#RectElementRYAttribute
JS::NonnullGCPtr<SVGAnimatedLength> SVGRectElement::ry() const
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto base_length = SVGLength::create(realm(), 0, m_radius_y.value_or(0));
    auto anim_length = SVGLength::create(realm(), 0, m_radius_y.value_or(0));
    return SVGAnimatedLength::create(realm(), move(base_length), move(anim_length));
}

}
