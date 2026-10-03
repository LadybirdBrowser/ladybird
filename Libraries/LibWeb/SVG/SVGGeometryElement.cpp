/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibGC/Heap.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/CSS/RustDeclarationBlock.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/SVG/SVGGeometryElement.h>

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

// https://w3c.github.io/svgwg/svg2-draft/types.html#__svg__SVGGeometryElement__getTotalLength
WebIDL::ExceptionOr<float> SVGGeometryElement::get_total_length()
{
    // When getTotalLength() is called, the user agent's computed value for the total length of the path, in user units,
    // is returned.

    // NB: Update layout so that the viewport size is resolved correctly
    Layout::ForcedReadScope read { document(), true };
    document().update_layout(DOM::UpdateLayoutReason::SVGPathLength);

    auto viewport_size = viewport_size_for_percentage_resolution(read);

    // NB: Update style for the element so that the correct computed values are used to generate the path - this is done
    //     separately from the layout update above since it may have been skipped if the element was display: none.
    document().update_style_for_element(*this);

    // FIXME: Layout builds each shape's geometry in Rust (svg_geometry_path_of() in svg_formatting_context.rs), while
    //        this reads the get_path() implementations, which duplicate it. Answer from the Rust builders instead, so
    //        a fix to one copy cannot miss the other.
    if (auto computed_values = computed_style())
        return get_path({ viewport_size.width(), viewport_size.height() }, *computed_values).length();

    // NB: An element with no style is either in a subtree that is not rendered, which the style engine answers
    //     without installing anything, or outside the document, where no rule reaches it. The engine of the window's
    //     document computes the latter, as an element's own document may never have been styled, like the one
    //     holding a template's contents.
    auto const has_no_style_node = style_node_id() == CSS::StyleNodeID {};
    auto& style_document = has_no_style_node ? HTML::relevant_window(*this).associated_document() : document();
    auto& style_computer = style_document.style_computer();
    // The style comes from the engine of the document that computes it, as that document's read.
    Layout::ForcedReadScope style_read { style_document, true };
    auto record = has_no_style_node
        ? declared_only_style_record(style_read, style_computer, style_document, *this)
        : CSS::StyleRecordID { style_computer.style_engine().answer_record_demand(style_read, style_node_id(), CSS::StyleEngine::RecordDemand::ElementRead).record.style_record };
    ScopeGuard unpin_record = [&] {
        if (has_no_style_node && !!record)
            style_computer.unpin_style_record(record);
    };
    auto view = style_computer.computed_style_record_view(style_read, record);
    if (!view)
        return 0;
    return get_path({ viewport_size.width(), viewport_size.height() }, *view).length();
}

GC::Ref<Geometry::DOMPoint> SVGGeometryElement::get_point_at_length(float distance)
{
    (void)distance;
    return Geometry::DOMPoint::create(0, 0, 0, 0);
}

GC::Ref<SVGAnimatedNumber> SVGGeometryElement::path_length()
{
    if (!m_path_length)
        m_path_length = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::pathLength, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_path_length;
}

}
