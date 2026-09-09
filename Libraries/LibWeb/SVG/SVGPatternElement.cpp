/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Matrix4x4.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParsing.h>
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
        m_pattern_units = parse_units(value.value_or({}));
    } else if (name == AttributeNames::patternContentUnits) {
        m_pattern_content_units = parse_units(value.value_or({}));
    } else if (name == AttributeNames::patternTransform) {
        if (auto transform_list = parse_transform(value.value_or({})); transform_list.has_value()) {
            m_pattern_transform = transform_from_transform_list(*transform_list);
        } else {
            m_pattern_transform = {};
        }
    } else if (name == AttributeNames::x) {
        m_x = parse_number_percentage(value.value_or({}));
    } else if (name == AttributeNames::y) {
        m_y = parse_number_percentage(value.value_or({}));
    } else if (name == AttributeNames::width) {
        m_width = parse_number_percentage(value.value_or({}));
    } else if (name == AttributeNames::height) {
        m_height = parse_number_percentage(value.value_or({}));
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

void SVGPatternElement::push_paint_server_description(void* sink, Layout::Node const& target_layout_node) const
{
    auto content_element = pattern_content_element();
    if (!content_element)
        return;

    Layout::Box const* pattern_box = nullptr;
    target_layout_node.for_each_child_of_type<Layout::Box>([&](auto const& candidate) {
        if (candidate.is_svg_pattern_box() && candidate.dom_node() == content_element.ptr()) {
            pattern_box = &candidate;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (!pattern_box)
        return;

    Layout::RustFFI::FfiSvgPatternDescription description {};
    description.pattern_box = Layout::Node::slot_id(pattern_box);
    description.units_are_object_bounding_box = pattern_units() == SVGUnits::ObjectBoundingBox;
    description.content_units_are_object_bounding_box = pattern_content_units() == SVGUnits::ObjectBoundingBox;
    description.has_view_box = view_box().has_value();
    description.x = Layout::to_ffi_number_percentage(pattern_x());
    description.y = Layout::to_ffi_number_percentage(pattern_y());
    description.width = Layout::to_ffi_number_percentage(pattern_width());
    description.height = Layout::to_ffi_number_percentage(pattern_height());
    description.pattern_transform_attribute = pattern_transform();
    auto const* transform_values = style_group<CSS::ComputedValues::TransformValues>();
    auto const* css_transform_entries = transform_values ? transform_values->resolved_transforms.pointer : nullptr;
    auto css_transform_count = transform_values ? transform_values->resolved_transforms.length : 0;
    Layout::RustFFI::layout_arena_svg_paint_resources_push_pattern(sink, &description, css_transform_entries, css_transform_count);
}

// Reflected length accessors are generated by SVGElement's reflection macro.

}
