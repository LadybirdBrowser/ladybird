/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Matrix4x4.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/FragmentIdentifier.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGPatternElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGPatternElement);

SVGPatternElement::SVGPatternElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGPatternElement::initialize_element()
{
    SVGFitToViewBox::initialize_fit_to_view_box();
}

void SVGPatternElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
    SVGFitToViewBox::visit_edges(visitor);
}

void SVGPatternElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    SVGFitToViewBox::attribute_changed(*this, name, value);

    if (name == AttributeNames::patternUnits) {
        m_pattern_units = AttributeParser::parse_units(value.value_or({}));
    } else if (name == AttributeNames::patternContentUnits) {
        m_pattern_content_units = AttributeParser::parse_units(value.value_or({}));
    } else if (name == AttributeNames::patternTransform) {
        if (auto transform_list = AttributeParser::parse_transform(value.value_or({})); transform_list.has_value()) {
            m_pattern_transform = transform_from_transform_list(*transform_list);
        } else {
            m_pattern_transform = {};
        }
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

GC::Ptr<SVGPatternElement const> SVGPatternElement::linked_pattern(GC::RootHashTable<SVGPatternElement const*>& seen_patterns) const
{
    // FIXME: This can only resolve same-document references. The spec allows cross-document references.
    auto link = has_attribute(AttributeNames::href) ? get_attribute(AttributeNames::href) : get_attribute(AttributeNames::xlink_href);
    if (!link.has_value() || link->is_empty())
        return {};

    auto url = document().encoding_parse_url(*link);
    if (!url.has_value())
        return {};

    auto id = url->fragment();
    if (!id.has_value() || id->is_empty())
        return {};

    auto element = document().get_element_by_id(decode_fragment_identifier(id.value()));
    if (!element)
        return {};

    if (element == GC::Ref { *this })
        return {};
    auto* pattern = as_if<SVGPatternElement>(*element);
    if (!pattern)
        return {};

    // Detect circular references in the template chain.
    if (seen_patterns.set(pattern) != AK::HashSetResult::InsertedNewEntry)
        return {};

    return pattern;
}

GC::Ptr<SVGPatternElement const> SVGPatternElement::pattern_content_element() const
{
    GC::RootHashTable<SVGPatternElement const*> seen_patterns;
    return pattern_content_element_impl(seen_patterns);
}

GC::Ptr<SVGPatternElement const> SVGPatternElement::pattern_content_element_impl(GC::RootHashTable<SVGPatternElement const*>& seen_patterns) const
{
    if (child_element_count() > 0)
        return this;
    if (auto pattern = linked_pattern(seen_patterns))
        return pattern->pattern_content_element_impl(seen_patterns);
    return {};
}

// https://svgwg.org/svg2-draft/pservers.html#PatternElementPatternUnitsAttribute
SVGUnits SVGPatternElement::pattern_units() const
{
    GC::RootHashTable<SVGPatternElement const*> seen_patterns;
    return pattern_units_impl(seen_patterns);
}

SVGUnits SVGPatternElement::pattern_units_impl(GC::RootHashTable<SVGPatternElement const*>& seen_patterns) const
{
    if (m_pattern_units.has_value())
        return *m_pattern_units;
    if (auto pattern = linked_pattern(seen_patterns))
        return pattern->pattern_units_impl(seen_patterns);
    // Initial value: objectBoundingBox
    return SVGUnits::ObjectBoundingBox;
}

// https://svgwg.org/svg2-draft/pservers.html#PatternElementPatternContentUnitsAttribute
SVGUnits SVGPatternElement::pattern_content_units() const
{
    GC::RootHashTable<SVGPatternElement const*> seen_patterns;
    return pattern_content_units_impl(seen_patterns);
}

SVGUnits SVGPatternElement::pattern_content_units_impl(GC::RootHashTable<SVGPatternElement const*>& seen_patterns) const
{
    if (m_pattern_content_units.has_value())
        return *m_pattern_content_units;
    if (auto pattern = linked_pattern(seen_patterns))
        return pattern->pattern_content_units_impl(seen_patterns);
    // Initial value: userSpaceOnUse
    return SVGUnits::UserSpaceOnUse;
}

// https://svgwg.org/svg2-draft/pservers.html#PatternElementPatternTransformAttribute
Optional<Gfx::AffineTransform> SVGPatternElement::pattern_transform() const
{
    GC::RootHashTable<SVGPatternElement const*> seen_patterns;
    return pattern_transform_impl(seen_patterns);
}

Optional<Gfx::AffineTransform> SVGPatternElement::pattern_transform_impl(GC::RootHashTable<SVGPatternElement const*>& seen_patterns) const
{
    if (m_pattern_transform.has_value())
        return m_pattern_transform;
    if (auto pattern = linked_pattern(seen_patterns))
        return pattern->pattern_transform_impl(seen_patterns);
    return {};
}

// https://svgwg.org/svg2-draft/pservers.html#PatternElementXAttribute
NumberPercentage SVGPatternElement::pattern_x() const
{
    GC::RootHashTable<SVGPatternElement const*> seen_patterns;
    return pattern_x_impl(seen_patterns);
}

NumberPercentage SVGPatternElement::pattern_x_impl(GC::RootHashTable<SVGPatternElement const*>& seen_patterns) const
{
    if (m_x.has_value())
        return *m_x;
    if (auto pattern = linked_pattern(seen_patterns))
        return pattern->pattern_x_impl(seen_patterns);
    return NumberPercentage::create_number(0);
}

// https://svgwg.org/svg2-draft/pservers.html#PatternElementYAttribute
NumberPercentage SVGPatternElement::pattern_y() const
{
    GC::RootHashTable<SVGPatternElement const*> seen_patterns;
    return pattern_y_impl(seen_patterns);
}

NumberPercentage SVGPatternElement::pattern_y_impl(GC::RootHashTable<SVGPatternElement const*>& seen_patterns) const
{
    if (m_y.has_value())
        return *m_y;
    if (auto pattern = linked_pattern(seen_patterns))
        return pattern->pattern_y_impl(seen_patterns);
    return NumberPercentage::create_number(0);
}

// https://svgwg.org/svg2-draft/pservers.html#PatternElementWidthAttribute
NumberPercentage SVGPatternElement::pattern_width() const
{
    GC::RootHashTable<SVGPatternElement const*> seen_patterns;
    return pattern_width_impl(seen_patterns);
}

NumberPercentage SVGPatternElement::pattern_width_impl(GC::RootHashTable<SVGPatternElement const*>& seen_patterns) const
{
    if (m_width.has_value())
        return *m_width;
    if (auto pattern = linked_pattern(seen_patterns))
        return pattern->pattern_width_impl(seen_patterns);
    return NumberPercentage::create_number(0);
}

// https://svgwg.org/svg2-draft/pservers.html#PatternElementHeightAttribute
NumberPercentage SVGPatternElement::pattern_height() const
{
    GC::RootHashTable<SVGPatternElement const*> seen_patterns;
    return pattern_height_impl(seen_patterns);
}

NumberPercentage SVGPatternElement::pattern_height_impl(GC::RootHashTable<SVGPatternElement const*>& seen_patterns) const
{
    if (m_height.has_value())
        return *m_height;
    if (auto pattern = linked_pattern(seen_patterns))
        return pattern->pattern_height_impl(seen_patterns);
    return NumberPercentage::create_number(0);
}

Optional<SVGPatternElement::PaintGeometry> SVGPatternElement::resolve_paint_geometry(SVGPaintContext const& paint_context, double device_pixels_per_css_pixel, Layout::Node const& target_layout_node) const
{
    auto content_element = pattern_content_element();
    if (!content_element)
        return {};

    Layout::Box const* pattern_box = nullptr;
    target_layout_node.for_each_child_of_type<Layout::Box>([&](auto const& candidate) {
        if (candidate.is_svg_pattern_box() && candidate.dom_node() == content_element.ptr()) {
            pattern_box = &candidate;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (!pattern_box)
        return {};

    auto pattern_paintable = pattern_box->paintable_box();
    if (!pattern_paintable)
        return {};

    float tile_x = 0;
    float tile_y = 0;
    float tile_width = 0;
    float tile_height = 0;
    if (pattern_units() == SVGUnits::ObjectBoundingBox) {
        // For objectBoundingBox, values are fractions of the bounding box.
        // NumberPercentage::value() already normalizes percentages to 0-1 range.
        auto const& bbox = paint_context.path_bounding_box;
        tile_x = pattern_x().value() * bbox.width() + bbox.x();
        tile_y = pattern_y().value() * bbox.height() + bbox.y();
        tile_width = pattern_width().value() * bbox.width();
        tile_height = pattern_height().value() * bbox.height();
    } else {
        // For userSpaceOnUse, resolve percentages relative to the viewport.
        auto const& viewport = paint_context.viewport;
        tile_x = pattern_x().resolve_relative_to(viewport.width());
        tile_y = pattern_y().resolve_relative_to(viewport.height());
        tile_width = pattern_width().resolve_relative_to(viewport.width());
        tile_height = pattern_height().resolve_relative_to(viewport.height());
    }

    if (tile_width <= 0 || tile_height <= 0)
        return {};

    auto tile_rect = paint_context.paint_transform.map(Gfx::FloatRect { tile_x, tile_y, tile_width, tile_height });

    if (tile_rect.is_empty())
        return {};

    auto content_scale = paint_context.content_scale;
    if (!(content_scale.width() > 0 && content_scale.height() > 0))
        content_scale = { 1, 1 };

    // Pattern content records in the pattern's own units scaled by the device pixel ratio; the
    // nested tree root maps it into the tile surface, including the objectBoundingBox content
    // scaling when patternContentUnits asks for it. A pattern viewBox maps content into
    // tile-local coordinates through the pattern's viewport transform node instead, and per
    // https://svgwg.org/svg2-draft/pservers.html#PatternElementViewBoxAttribute it overrides
    // patternContentUnits — so the root then only applies the surface resolution scale.
    auto device_scale = static_cast<float>(device_pixels_per_css_pixel);
    auto recorded_to_surface = Gfx::AffineTransform {}.scale({ content_scale.width(), content_scale.height() });
    if (!view_box().has_value()) {
        auto content_to_tile_transform = Gfx::AffineTransform {};
        if (pattern_content_units() == SVGUnits::ObjectBoundingBox) {
            auto const& bounding_box = paint_context.path_bounding_box;
            content_to_tile_transform = Gfx::AffineTransform {}
                                            .translate({ bounding_box.x() * device_scale, bounding_box.y() * device_scale })
                                            .scale({ bounding_box.width(), bounding_box.height() });
        }
        recorded_to_surface = recorded_to_surface
                                  .translate(-tile_rect.location())
                                  .multiply(content_to_tile_transform);
    }
    Painting::TransformData tile_content_transform { recorded_to_surface.to_matrix(), {} };

    Optional<Gfx::AffineTransform> user_space_pattern_transform;
    auto style = computed_style();
    VERIFY(style);
    if (style->has_transformations()) {
        auto matrix = Gfx::FloatMatrix4x4::identity();
        style->for_each_transformation([&](auto const& css_transform) {
            matrix = matrix * css_transform.to_matrix(*pattern_paintable);
        });

        user_space_pattern_transform = extract_2d_affine_transform(matrix);
    } else {
        user_space_pattern_transform = pattern_transform();
    }

    Optional<Gfx::AffineTransform> device_pattern_transform;
    if (user_space_pattern_transform.has_value()) {
        if (!user_space_pattern_transform->inverse().has_value())
            return {};
        // patternTransform is defined in user space, but the tile rect and shader operate in device pixel space.
        // Convert by conjugating with paint_transform.
        if (auto inv = paint_context.paint_transform.inverse(); inv.has_value()) {
            auto transform = paint_context.paint_transform;
            device_pattern_transform = transform.multiply(*user_space_pattern_transform).multiply(*inv);
        }
    }

    return PaintGeometry {
        .pattern_paintable = pattern_paintable,
        .tile_rect = tile_rect,
        .content_scale = content_scale,
        .tile_content_transform = tile_content_transform,
        .device_pattern_transform = device_pattern_transform,
    };
}

// Reflected length accessors are generated by SVGElement's reflection macro.

}
