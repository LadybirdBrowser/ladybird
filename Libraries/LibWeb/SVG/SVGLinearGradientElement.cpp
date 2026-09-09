/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGLinearGradientElement.h>
#include <LibWeb/SVG/SVGStopElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGLinearGradientElement);

SVGLinearGradientElement::SVGLinearGradientElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGradientElement(document, qualified_name)
{
}

void SVGLinearGradientElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    // FIXME: Should allow for `<number-percentage> | <length>` for x1, x2, y1, y2
    if (name == SVG::AttributeNames::x1) {
        m_x1 = parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::y1) {
        m_y1 = parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::x2) {
        m_x2 = parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::y2) {
        m_y2 = parse_number_percentage(value.value_or({}));
    }
}

// https://www.w3.org/TR/SVG11/pservers.html#LinearGradientElementX1Attribute
NumberPercentage SVGLinearGradientElement::start_x() const
{
    GC::RootHashTable<SVGGradientElement const*> seen_gradients;
    return start_x_impl(seen_gradients);
}

NumberPercentage SVGLinearGradientElement::start_x_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const
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
    GC::RootHashTable<SVGGradientElement const*> seen_gradients;
    return start_y_impl(seen_gradients);
}

NumberPercentage SVGLinearGradientElement::start_y_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const
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
    GC::RootHashTable<SVGGradientElement const*> seen_gradients;
    return end_x_impl(seen_gradients);
}

NumberPercentage SVGLinearGradientElement::end_x_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const
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
    GC::RootHashTable<SVGGradientElement const*> seen_gradients;
    return end_y_impl(seen_gradients);
}

NumberPercentage SVGLinearGradientElement::end_y_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const
{
    if (m_y2.has_value())
        return *m_y2;
    if (auto gradient = linked_linear_gradient(seen_gradients))
        return gradient->end_y_impl(seen_gradients);
    // If the attribute is not specified, the effect is as if a value of '0%' were specified.
    return NumberPercentage::create_percentage(0);
}

void SVGLinearGradientElement::push_paint_server_description(void* sink) const
{
    auto description = base_paint_server_description(Layout::RustFFI::FfiSvgGradientKind::Linear);
    description.x1 = Layout::to_ffi_number_percentage(start_x());
    description.y1 = Layout::to_ffi_number_percentage(start_y());
    description.x2 = Layout::to_ffi_number_percentage(end_x());
    description.y2 = Layout::to_ffi_number_percentage(end_y());
    Layout::RustFFI::layout_arena_svg_paint_resources_push_gradient(sink, &description);
    push_color_stops(sink);
}

}
