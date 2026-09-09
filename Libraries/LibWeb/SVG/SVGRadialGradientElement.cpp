/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGRadialGradientElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGRadialGradientElement);

SVGRadialGradientElement::SVGRadialGradientElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGradientElement(document, qualified_name)
{
}

void SVGRadialGradientElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    // FIXME: These are <length> or <coordinate> in the spec, but all examples seem to allow percentages
    // and unitless values.
    if (name == SVG::AttributeNames::cx) {
        m_cx = parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::cy) {
        m_cy = parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::fx) {
        m_fx = parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::fy) {
        m_fy = parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::fr) {
        m_fr = parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::r) {
        m_r = parse_number_percentage(value.value_or({}));
    }
}

// https://svgwg.org/svg2-draft/pservers.html#RadialGradientElementFXAttribute
NumberPercentage SVGRadialGradientElement::start_circle_x() const
{
    GC::RootHashTable<SVGGradientElement const*> seen_gradients;
    return start_circle_x_impl(seen_gradients);
}

NumberPercentage SVGRadialGradientElement::start_circle_x_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const
{
    if (m_fx.has_value())
        return *m_fx;
    // If the element references an element that specifies a value for 'fx', then the value of 'fx' is
    // inherited from the referenced element.
    if (auto gradient = linked_radial_gradient(seen_gradients))
        return gradient->start_circle_x_impl(seen_gradients);
    // If attribute ‘fx’ is not specified, ‘fx’ will coincide with the presentational value of ‘cx’ for
    // the element whether the value for 'cx' was inherited or not.
    return end_circle_x();
}

// https://svgwg.org/svg2-draft/pservers.html#RadialGradientElementFYAttribute
NumberPercentage SVGRadialGradientElement::start_circle_y() const
{
    GC::RootHashTable<SVGGradientElement const*> seen_gradients;
    return start_circle_y_impl(seen_gradients);
}

NumberPercentage SVGRadialGradientElement::start_circle_y_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const
{
    if (m_fy.has_value())
        return *m_fy;
    // If the element references an element that specifies a value for 'fy', then the value of 'fy' is
    // inherited from the referenced element.
    if (auto gradient = linked_radial_gradient(seen_gradients))
        return gradient->start_circle_y_impl(seen_gradients);
    // If attribute ‘fy’ is not specified, ‘fy’ will coincide with the presentational value of ‘cy’ for
    // the element whether the value for 'cy' was inherited or not.
    return end_circle_y();
}

// https://svgwg.org/svg2-draft/pservers.html#RadialGradientElementFRAttribute
NumberPercentage SVGRadialGradientElement::start_circle_radius() const
{
    GC::RootHashTable<SVGGradientElement const*> seen_gradients;
    return start_circle_radius_impl(seen_gradients);
}

NumberPercentage SVGRadialGradientElement::start_circle_radius_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const
{
    // Note: A negative value is an error.
    if (m_fr.has_value() && m_fr->value() >= 0)
        return *m_fr;
    // if the element references an element that specifies a value for 'fr', then the value of
    // 'fr' is inherited from the referenced element.
    if (auto gradient = linked_radial_gradient(seen_gradients))
        return gradient->start_circle_radius_impl(seen_gradients);
    // If the attribute is not specified, the effect is as if a value of '0%' were specified.
    return NumberPercentage::create_percentage(0);
}

// https://svgwg.org/svg2-draft/pservers.html#RadialGradientElementCXAttribute
NumberPercentage SVGRadialGradientElement::end_circle_x() const
{
    GC::RootHashTable<SVGGradientElement const*> seen_gradients;
    return end_circle_x_impl(seen_gradients);
}

NumberPercentage SVGRadialGradientElement::end_circle_x_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const
{
    if (m_cx.has_value())
        return *m_cx;
    if (auto gradient = linked_radial_gradient(seen_gradients))
        return gradient->end_circle_x_impl(seen_gradients);
    return NumberPercentage::create_percentage(50);
}

// https://svgwg.org/svg2-draft/pservers.html#RadialGradientElementCYAttribute
NumberPercentage SVGRadialGradientElement::end_circle_y() const
{
    GC::RootHashTable<SVGGradientElement const*> seen_gradients;
    return end_circle_y_impl(seen_gradients);
}

NumberPercentage SVGRadialGradientElement::end_circle_y_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const
{
    if (m_cy.has_value())
        return *m_cy;
    if (auto gradient = linked_radial_gradient(seen_gradients))
        return gradient->end_circle_y_impl(seen_gradients);
    return NumberPercentage::create_percentage(50);
}

// https://svgwg.org/svg2-draft/pservers.html#RadialGradientElementRAttribute
NumberPercentage SVGRadialGradientElement::end_circle_radius() const
{
    GC::RootHashTable<SVGGradientElement const*> seen_gradients;
    return end_circle_radius_impl(seen_gradients);
}

NumberPercentage SVGRadialGradientElement::end_circle_radius_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const
{
    // Note: A negative value is an error.
    if (m_r.has_value() && m_r->value() >= 0)
        return *m_r;
    if (auto gradient = linked_radial_gradient(seen_gradients))
        return gradient->end_circle_radius_impl(seen_gradients);
    return NumberPercentage::create_percentage(50);
}

void SVGRadialGradientElement::push_paint_server_description(void* sink) const
{
    auto description = base_paint_server_description(Layout::RustFFI::FfiSvgGradientKind::Radial);
    description.cx = Layout::to_ffi_number_percentage(end_circle_x());
    description.cy = Layout::to_ffi_number_percentage(end_circle_y());
    description.r = Layout::to_ffi_number_percentage(end_circle_radius());
    description.fx = Layout::to_ffi_number_percentage(start_circle_x());
    description.fy = Layout::to_ffi_number_percentage(start_circle_y());
    description.fr = Layout::to_ffi_number_percentage(start_circle_radius());
    Layout::RustFFI::layout_arena_svg_paint_resources_push_gradient(sink, &description);
    push_color_stops(sink);
}

}
