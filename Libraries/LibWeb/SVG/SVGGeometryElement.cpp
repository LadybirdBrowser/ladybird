/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibGC/Heap.h>
#include <LibWeb/Bindings/DOMPointReadOnly.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/CSS/RustDeclarationBlock.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/SVG/SVGGeometryElement.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::SVG {

SVGGeometryElement::SVGGeometryElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

void SVGGeometryElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_path_length);
}

CSS::ElementBoxKind SVGGeometryElement::box_kind() const
{
    return CSS::ElementBoxKind::SvgGeometry;
}

// The style of an element outside the document, where no rule reaches it: the style engine cascades its own
// presentation attributes and inline style over the initial values. The record comes back pinned for the caller, or
// zero where the engine leaves the computation to C++.
static CSS::StyleRecordID declared_only_style_record(Layout::BegunRead const& read, CSS::StyleComputer& style_computer, DOM::Document const& document, SVGGeometryElement& element)
{
    auto hints = CSS::StyleComputer::collect_presentational_hint_properties({ element });
    Vector<CSS::Parser::ValueParserFFI::FfiDeclaredProperty> declarations;
    declarations.ensure_capacity(hints.size());
    for (auto const& hint : hints) {
        declarations.unchecked_append({
            .property_id = to_underlying(hint.property_id),
            .important = hint.important == CSS::Important::Yes,
            .value = hint.value->rust_style_value_data(),
            .name = {},
        });
    }
    auto inline_style = element.inline_style();
    return CSS::StyleRecordID { CSS::StyleEngineFFI::style_engine_declared_only_record(
        style_computer.style_engine().host(),
        &read, document.style_node_id().value(),
        CSS::element_box_type_adjustment_facts(element),
        CSS::StyleEngineFFI::FfiElementDeclarationKind::SvgPresentationAttribute,
        declarations.data(),
        declarations.size(),
        inline_style ? inline_style->declaration_block().handle() : nullptr) };
}

// The geometry of the element in user units, which the geometry queries such as getTotalLength() measure. It is empty
// for an element without style.
bool SVGGeometryElement::with_computed_values(Function<void(CSSPixelSize viewport_size, CSS::ComputedValues const&)> const& callback)
{
    // NB: Update layout so that the viewport size is resolved correctly
    Layout::ForcedReadScope read { document() };
    document().update_layout(DOM::UpdateLayoutReason::SVGPathLength);

    auto viewport_size = viewport_size_for_percentage_resolution(read);

    // NB: Update style for the element so that the correct computed values are used to generate the path - this is done
    //     separately from the layout update above since it may have been skipped if the element was display: none.
    document().update_style_for_element(*this);

    if (auto computed_values = computed_style()) {
        callback({ viewport_size.width(), viewport_size.height() }, *computed_values);
        return true;
    }

    // NB: An element with no style is either in a subtree that is not rendered, which the style engine answers
    //     without installing anything, or outside the document, where no rule reaches it. The engine of the window's
    //     document computes the latter, as an element's own document may never have been styled, like the one
    //     holding a template's contents.
    auto const has_no_style_node = style_node_id() == CSS::StyleNodeID {};
    auto& style_document = has_no_style_node ? HTML::relevant_window(*this).associated_document() : document();
    auto& style_computer = style_document.style_computer();
    // The style comes from the engine of the document that computes it, as that document's read.
    Layout::ForcedReadScope style_read { style_document };
    auto record = has_no_style_node
        ? declared_only_style_record(style_read, style_computer, style_document, *this)
        : CSS::StyleRecordID { style_computer.style_engine().answer_record_demand(style_read, style_node_id(), CSS::StyleEngine::RecordDemand::ElementRead).record.style_record };
    ScopeGuard unpin_record = [&] {
        if (has_no_style_node && !!record)
            style_computer.unpin_style_record(record);
    };
    auto view = style_computer.computed_style_record_view(style_read, record);
    if (!view)
        return false;
    callback({ viewport_size.width(), viewport_size.height() }, *view);
    return true;
}

// The geometry the element draws, built by the same shape builders layout uses, from what the element parses and from
// the given computed values.
static Gfx::Path geometry_path(SVGGeometryElement const& element, CSSPixelSize viewport_size, CSS::ComputedValues const& computed_values)
{
    Array<void const*, to_underlying(CSS::StyleGroupIndex::Count)> payloads;
    for (size_t index = 0; index < payloads.size(); ++index)
        payloads[index] = computed_values.style_group_payload(static_cast<CSS::StyleGroupIndex>(index));
    auto points = Layout::svg_shape_points(element);
    static_assert(sizeof(Gfx::FloatPoint) == sizeof(Layout::RustFFI::FfiFloatPoint));
    Vector<u8> path_bytes;
    Layout::RustFFI::svg_geometry_element_encode_path(
        Layout::svg_geometry_facts(element),
        payloads.data(),
        payloads.size(),
        reinterpret_cast<Layout::RustFFI::FfiFloatPoint const*>(points.data()),
        points.size(),
        viewport_size,
        &path_bytes,
        [](void* context, u8 const* data, size_t length) { static_cast<Vector<u8>*>(context)->append(data, length); });
    return Gfx::Path::from_serialized_bytes(path_bytes);
}

Gfx::Path SVGGeometryElement::computed_path()
{
    Gfx::Path path;
    with_computed_values([&](CSSPixelSize viewport_size, CSS::ComputedValues const& computed_values) {
        path = geometry_path(*this, viewport_size, computed_values);
    });
    return path;
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGGeometryElement__isPointInFill
WebIDL::ExceptionOr<bool> SVGGeometryElement::is_point_in_fill(Bindings::DOMPointInit const& point)
{
    // The isPointInFill method, when invoked, must return true if the point given by point passed to the method, in
    // the coordinate space of an element, is inside the intended path as determined by the winding rule indicated by
    // the fill-rule property of an element; and must return false otherwise. Open subpaths must be implicitly closed
    // when computing the area inside the path, without affecting the actual subpaths. Points on the path itself must be
    // considered to be inside the path.
    // NB: Gfx::Path::contains() closes open subpaths and counts points on the path as inside.

    // The returned value is independent of any visual CSS property but fill-rule
    // If either of the x or y properties on point are infinite or NaN, then the method must return false.
    if (!isfinite(point.x) || !isfinite(point.y))
        return false;

    // If current element is a non-rendered element, and the UA is not able to compute the geometry of the element,
    // then throw an InvalidStateError.
    bool is_point_in_fill = false;
    auto has_geometry = with_computed_values([&](CSSPixelSize viewport_size, CSS::ComputedValues const& computed_values) {
        auto winding_rule = computed_values.fill_rule() == CSS::FillRule::Evenodd ? Gfx::WindingRule::EvenOdd : Gfx::WindingRule::Nonzero;
        is_point_in_fill = geometry_path(*this, viewport_size, computed_values).contains({ point.x, point.y }, winding_rule);
    });
    if (!has_geometry)
        return WebIDL::InvalidStateError::create(HTML::relevant_realm(*this), "The element's geometry cannot be computed"_utf16);
    return is_point_in_fill;
}

// https://w3c.github.io/svgwg/svg2-draft/types.html#__svg__SVGGeometryElement__getTotalLength
WebIDL::ExceptionOr<float> SVGGeometryElement::get_total_length()
{
    // When getTotalLength() is called, the user agent's computed value for the total length of the path, in user units,
    // is returned.
    return computed_path().length();
}

// https://w3c.github.io/svgwg/svg2-draft/types.html#__svg__SVGGeometryElement__getPointAtLength
WebIDL::ExceptionOr<GC::Ref<Geometry::DOMPoint>> SVGGeometryElement::get_point_at_length(float distance)
{
    // When getPointAtLength(distance) is called, the following steps are run:
    auto path = computed_path();

    // 1. If current element is a non-rendered element, and the UA is not able to compute the total length of the path,
    //    then throw an InvalidStateError.
    // NB: Like other engines, throw for an empty path whether or not the element is rendered.
    auto point_along_path = path.point_at_length(distance);
    if (!point_along_path.has_value())
        return WebIDL::InvalidStateError::create(HTML::relevant_realm(*this), "The element's path is empty"_utf16);

    // 2. Let length be the user agent's computed value for the total length of the path, in user units.
    // 3. Clamp distance to [0, length].
    // 4. Let (x, y) be the point on the path at distance distance.
    // NB: Gfx::Path::point_at_length() clamps the distance to the ends of the path.

    // 5. Return a newly created, detached DOMPoint object representing the point (x, y).
    return Geometry::DOMPoint::create(point_along_path->point.x(), point_along_path->point.y(), 0, 1);
}

GC::Ref<SVGAnimatedNumber> SVGGeometryElement::path_length()
{
    if (!m_path_length)
        m_path_length = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::pathLength, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_path_length;
}

}
