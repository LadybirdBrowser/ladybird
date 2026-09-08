/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Matrix4x4.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/FragmentIdentifier.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::SVG {

SVGGradientElement::SVGGradientElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGGradientElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == AttributeNames::gradientUnits) {
        m_gradient_units = parse_units(value.value_or({}));
    } else if (name == AttributeNames::spreadMethod) {
        m_spread_method = parse_spread_method(value.value_or({}));
    } else if (name == AttributeNames::gradientTransform) {
        if (auto transform_list = parse_transform(value.value_or({})); transform_list.has_value()) {
            m_gradient_transform = transform_from_transform_list(*transform_list);
        } else {
            m_gradient_transform = {};
        }
    }
}

ResolvedGradient SVGGradientElement::resolve_gradient() const
{
    ResolvedGradient result;
    result.is_radial = is_radial_gradient();
    GradientAttributes attributes;
    Optional<GradientUnits> units;
    Optional<SpreadMethod> spread;
    Optional<Gfx::AffineTransform> transform;
    bool can_inherit_coordinates = true;
    bool found_stops = false;
    GC::RootHashTable<SVGGradientElement const*> seen;
    for (GC::Ptr<SVGGradientElement const> gradient = this; gradient && seen.set(gradient.ptr()) == AK::HashSetResult::InsertedNewEntry; gradient = gradient->linked_gradient()) {
        if (!units.has_value())
            units = gradient->m_gradient_units;
        if (!spread.has_value())
            spread = gradient->m_spread_method;
        if (!transform.has_value()) {
            // The cascade can override gradientTransform, including with transform:none.
            auto style = gradient->computed_style();
            if (style && (style->has_transformations() || gradient->m_gradient_transform.has_value())) {
                auto matrix = Gfx::FloatMatrix4x4::identity();
                style->for_each_transformation([&](auto const& css_transform) {
                    matrix = matrix * css_transform.to_matrix(nullptr);
                });
                transform = extract_2d_affine_transform(matrix);
            }
        }
        can_inherit_coordinates &= gradient->is_radial_gradient() == result.is_radial;
        if (can_inherit_coordinates)
            gradient->collect_gradient_attributes(attributes);
        if (!found_stops) {
            float largest_offset = 0;
            gradient->for_each_child_of_type<SVGStopElement>([&](auto& stop) {
                found_stops = true;
                auto offset = max(largest_offset, clamp(stop.stop_offset(), 0.0f, 1.0f));
                largest_offset = offset;
                result.stops.append({ offset, stop.stop_color().with_opacity(stop.stop_opacity()) });
                return IterationDecision::Continue;
            });
        }
    }
    result.units = units.value_or(GradientUnits::ObjectBoundingBox);
    result.spread_method = spread.value_or(SpreadMethod::Pad);
    result.transform = transform.value_or(Gfx::AffineTransform {});
    if (auto style = computed_style())
        result.color_space = CSS::to_interpolation_color_space(style->color_interpolation());
    auto zero = NumberPercentage::create_percentage(0);
    auto half = NumberPercentage::create_percentage(50);
    if (result.is_radial) {
        result.end_x = attributes.cx.value_or(half);
        result.end_y = attributes.cy.value_or(half);
        result.end_radius = attributes.r.value_or(half);
        // Defaults apply to the referencing gradient after the template chain is exhausted.
        result.start_x = attributes.fx.value_or(result.end_x);
        result.start_y = attributes.fy.value_or(result.end_y);
        result.start_radius = attributes.fr.value_or(zero);
    } else {
        result.start_x = attributes.x1.value_or(zero);
        result.start_y = attributes.y1.value_or(zero);
        result.end_x = attributes.x2.value_or(NumberPercentage::create_percentage(100));
        result.end_y = attributes.y2.value_or(zero);
    }
    return result;
}

GC::Ptr<SVGGradientElement const> SVGGradientElement::linked_gradient() const
{
    // FIXME: This entire function is an ad-hoc hack!

    auto link = has_attribute(AttributeNames::href) ? get_attribute(AttributeNames::href) : get_attribute(AttributeNames::xlink_href);
    if (auto href = link; href.has_value() && !link->is_empty()) {
        auto url = document().encoding_parse_url(*href);
        if (!url.has_value())
            return {};
        auto id = url->fragment();
        if (!id.has_value() || id->is_empty())
            return {};
        auto id_as_utf16 = decode_fragment_identifier(id.value());
        GC::Ptr<DOM::Element> element;
        if (auto containing_shadow = containing_shadow_root())
            element = containing_shadow->get_element_by_id(id_as_utf16);
        if (!element)
            element = document().get_element_by_id(id_as_utf16);
        if (!element)
            return {};
        if (element == GC::Ref { *this })
            return {};
        if (!is<SVGGradientElement>(*element))
            return {};
        return &as<SVGGradientElement>(*element);
    }
    return {};
}

void SVGGradientElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
}

}
