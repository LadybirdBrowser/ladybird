/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Matrix4x4.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGPatternElementPrototype.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/SVGPatternBox.h>
#include <LibWeb/Layout/SVGSVGBox.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGPatternElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGPatternElement);

SVGPatternElement::SVGPatternElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGPatternElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGPatternElement);
    Base::initialize(realm);
    SVGFitToViewBox::initialize(realm);
}

void SVGPatternElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
    SVGFitToViewBox::visit_edges(visitor);
}

void SVGPatternElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    SVGFitToViewBox::attribute_changed(*this, name, value);

    if (name == AttributeNames::patternUnits) {
        m_pattern_units = AttributeParser::parse_units(value.value_or(String {}));
    } else if (name == AttributeNames::patternContentUnits) {
        m_pattern_content_units = AttributeParser::parse_units(value.value_or(String {}));
    } else if (name == AttributeNames::patternTransform) {
        if (auto transform_list = AttributeParser::parse_transform(value.value_or(String {})); transform_list.has_value()) {
            m_pattern_transform = transform_from_transform_list(*transform_list);
        } else {
            m_pattern_transform = {};
        }
    } else if (name == AttributeNames::x) {
        m_x = AttributeParser::parse_number_percentage(value.value_or(String {}));
    } else if (name == AttributeNames::y) {
        m_y = AttributeParser::parse_number_percentage(value.value_or(String {}));
    } else if (name == AttributeNames::width) {
        m_width = AttributeParser::parse_number_percentage(value.value_or(String {}));
    } else if (name == AttributeNames::height) {
        m_height = AttributeParser::parse_number_percentage(value.value_or(String {}));
    }
}

GC::Ptr<SVGPatternElement const> SVGPatternElement::linked_pattern(HashTable<SVGPatternElement const*>& seen_patterns) const
{
    // FIXME: This can only resolve same-document references. The spec allows cross-document references.
    auto link = has_attribute(AttributeNames::href) ? get_attribute(AttributeNames::href) : get_attribute("xlink:href"_fly_string);
    if (!link.has_value() || link->is_empty())
        return {};

    auto url = document().encoding_parse_url(*link);
    if (!url.has_value())
        return {};

    auto id = url->fragment();
    if (!id.has_value() || id->is_empty())
        return {};

    auto element = document().get_element_by_id(id.value());
    if (!element)
        return {};

    if (element == this)
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
    HashTable<SVGPatternElement const*> seen_patterns;
    return pattern_content_element_impl(seen_patterns);
}

GC::Ptr<SVGPatternElement const> SVGPatternElement::pattern_content_element_impl(HashTable<SVGPatternElement const*>& seen_patterns) const
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
    HashTable<SVGPatternElement const*> seen_patterns;
    return pattern_units_impl(seen_patterns);
}

SVGUnits SVGPatternElement::pattern_units_impl(HashTable<SVGPatternElement const*>& seen_patterns) const
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
    HashTable<SVGPatternElement const*> seen_patterns;
    return pattern_content_units_impl(seen_patterns);
}

SVGUnits SVGPatternElement::pattern_content_units_impl(HashTable<SVGPatternElement const*>& seen_patterns) const
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
    HashTable<SVGPatternElement const*> seen_patterns;
    return pattern_transform_impl(seen_patterns);
}

Optional<Gfx::AffineTransform> SVGPatternElement::pattern_transform_impl(HashTable<SVGPatternElement const*>& seen_patterns) const
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
    HashTable<SVGPatternElement const*> seen_patterns;
    return pattern_x_impl(seen_patterns);
}

NumberPercentage SVGPatternElement::pattern_x_impl(HashTable<SVGPatternElement const*>& seen_patterns) const
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
    HashTable<SVGPatternElement const*> seen_patterns;
    return pattern_y_impl(seen_patterns);
}

NumberPercentage SVGPatternElement::pattern_y_impl(HashTable<SVGPatternElement const*>& seen_patterns) const
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
    HashTable<SVGPatternElement const*> seen_patterns;
    return pattern_width_impl(seen_patterns);
}

NumberPercentage SVGPatternElement::pattern_width_impl(HashTable<SVGPatternElement const*>& seen_patterns) const
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
    HashTable<SVGPatternElement const*> seen_patterns;
    return pattern_height_impl(seen_patterns);
}

NumberPercentage SVGPatternElement::pattern_height_impl(HashTable<SVGPatternElement const*>& seen_patterns) const
{
    if (m_height.has_value())
        return *m_height;
    if (auto pattern = linked_pattern(seen_patterns))
        return pattern->pattern_height_impl(seen_patterns);
    return NumberPercentage::create_number(0);
}

Optional<Painting::PaintStyle> SVGPatternElement::to_gfx_paint_style(SVGPaintContext const& paint_context, DisplayListRecordingContext& recording_context, Layout::Node const& target_layout_node) const
{
    auto content_element = pattern_content_element();
    if (!content_element)
        return {};

    Layout::SVGPatternBox const* pattern_box = nullptr;
    target_layout_node.for_each_child_of_type<Layout::SVGPatternBox>([&](auto const& candidate) {
        if (&candidate.dom_node() == content_element.ptr()) {
            pattern_box = &candidate;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (!pattern_box)
        return {};

    auto* pattern_paintable = pattern_box->paintable_box();
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

    auto const* svg_node = target_layout_node.first_ancestor_of_type<Layout::SVGSVGBox>();
    if (!svg_node || !svg_node->paintable_box())
        return {};
    auto svg_element_rect = svg_node->paintable_box()->absolute_rect();
    auto svg_offset = recording_context.rounded_device_point(svg_element_rect.location()).to_type<int>().to_type<float>();
    tile_rect.translate_by(svg_offset);

    auto display_list = Painting::DisplayList::create();
    Painting::DisplayListRecorder display_list_recorder(*display_list);
    auto content_origin = paint_context.paint_transform.map(Gfx::FloatPoint { 0, 0 }) + svg_offset;
    display_list_recorder.translate(-Gfx::IntPoint(content_origin.to_type<int>()));
    auto paint_context_copy = recording_context.clone(display_list_recorder);

    Gfx::AffineTransform target_svg_transform;
    if (auto const* svg_graphics_paintable = as_if<Painting::SVGGraphicsPaintable>(*target_layout_node.first_paintable()))
        target_svg_transform = svg_graphics_paintable->computed_transforms().svg_transform();
    paint_context_copy.set_svg_transform(target_svg_transform);

    // Pattern content paintables are in an unconnected subtree, so the global resolve_paint_only_properties pass won't
    // reach them.
    const_cast<Painting::PaintableBox&>(*pattern_paintable).for_each_in_inclusive_subtree([](Painting::Paintable& paintable) {
        paintable.resolve_paint_properties();
        return TraversalDecision::Continue;
    });

    Painting::StackingContext::paint_svg(paint_context_copy, *pattern_paintable, Painting::PaintPhase::Foreground);

    Optional<Gfx::AffineTransform> user_space_pattern_transform;
    auto css_transformations = computed_properties()->transformations();
    if (!css_transformations.is_empty()) {
        auto matrix = Gfx::FloatMatrix4x4::identity();
        bool transform_valid = true;
        for (auto const& css_transform : css_transformations) {
            auto result = css_transform->to_matrix(*pattern_paintable);
            if (result.is_error()) {
                transform_valid = false;
                break;
            }
            matrix = matrix * result.release_value();
        }
        if (transform_valid)
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

    return Painting::SVGPatternPaintStyle::create(display_list, tile_rect, device_pattern_transform);
}

// https://svgwg.org/svg2-draft/pservers.html#PatternElementXAttribute
GC::Ref<SVGAnimatedLength> SVGPatternElement::x() const
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto base_length = SVGLength::create(realm(), 0, m_x.value_or(NumberPercentage::create_number(0)).value(), SVGLength::ReadOnly::No);
    auto anim_length = SVGLength::create(realm(), 0, m_x.value_or(NumberPercentage::create_number(0)).value(), SVGLength::ReadOnly::Yes);
    return SVGAnimatedLength::create(realm(), base_length, anim_length);
}

// https://svgwg.org/svg2-draft/pservers.html#PatternElementYAttribute
GC::Ref<SVGAnimatedLength> SVGPatternElement::y() const
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto base_length = SVGLength::create(realm(), 0, m_y.value_or(NumberPercentage::create_number(0)).value(), SVGLength::ReadOnly::No);
    auto anim_length = SVGLength::create(realm(), 0, m_y.value_or(NumberPercentage::create_number(0)).value(), SVGLength::ReadOnly::Yes);
    return SVGAnimatedLength::create(realm(), base_length, anim_length);
}

// https://svgwg.org/svg2-draft/pservers.html#PatternElementWidthAttribute
GC::Ref<SVGAnimatedLength> SVGPatternElement::width() const
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto base_length = SVGLength::create(realm(), 0, m_width.value_or(NumberPercentage::create_number(0)).value(), SVGLength::ReadOnly::No);
    auto anim_length = SVGLength::create(realm(), 0, m_width.value_or(NumberPercentage::create_number(0)).value(), SVGLength::ReadOnly::Yes);
    return SVGAnimatedLength::create(realm(), base_length, anim_length);
}

// https://svgwg.org/svg2-draft/pservers.html#PatternElementHeightAttribute
GC::Ref<SVGAnimatedLength> SVGPatternElement::height() const
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto base_length = SVGLength::create(realm(), 0, m_height.value_or(NumberPercentage::create_number(0)).value(), SVGLength::ReadOnly::No);
    auto anim_length = SVGLength::create(realm(), 0, m_height.value_or(NumberPercentage::create_number(0)).value(), SVGLength::ReadOnly::Yes);
    return SVGAnimatedLength::create(realm(), base_length, anim_length);
}

}
