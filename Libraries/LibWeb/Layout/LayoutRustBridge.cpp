/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Debug.h>
#include <AK/GenericShorthands.h>
#include <AK/Math.h>
#include <AK/NeverDestroyed.h>
#include <AK/NumericLimits.h>
#include <AK/Variant.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Display.h>
#include <LibWeb/CSS/LengthBox.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/ValueType.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/CommitMessages.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLTableCellElement.h>
#include <LibWeb/HTML/HTMLTableColElement.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/ImageProvider.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/PaintableTypes.h>
#include <LibWeb/Painting/ScrollSnap.h>
#include <LibWeb/SVG/FragmentIdentifier.h>
#include <LibWeb/SVG/SVGCircleElement.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGEllipseElement.h>
#include <LibWeb/SVG/SVGImageElement.h>
#include <LibWeb/SVG/SVGLineElement.h>
#include <LibWeb/SVG/SVGMaskElement.h>
#include <LibWeb/SVG/SVGPathElement.h>
#include <LibWeb/SVG/SVGPatternElement.h>
#include <LibWeb/SVG/SVGPolygonElement.h>
#include <LibWeb/SVG/SVGPolylineElement.h>
#include <LibWeb/SVG/SVGRectElement.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/SVG/SVGSymbolElement.h>
#include <LibWeb/SVG/SVGTextElement.h>
#include <LibWeb/SVG/SVGTextPathElement.h>
#include <LibWeb/SVG/SVGTextPositioningElement.h>
#include <LibWeb/SVG/SVGUseElement.h>

namespace Web::Layout {

static_assert(to_underlying(CSS::StyleGroupIndex::Count) == RustFFI::STYLE_GROUP_COUNT);
static_assert(to_underlying(CSS::StyleGroupIndex::GridValues) == RustFFI::STYLE_GROUP_INDEX_GRID);
static_assert(to_underlying(CSS::StyleGroupIndex::ContentValues) == RustFFI::STYLE_GROUP_INDEX_CONTENT);
static_assert(to_underlying(CSS::StyleGroupIndex::AnchorValues) == RustFFI::STYLE_GROUP_INDEX_ANCHOR);
static_assert(to_underlying(CSS::StyleGroupIndex::InheritedTableValues) == RustFFI::STYLE_GROUP_INDEX_INHERITED_TABLE);
static_assert(to_underlying(CSS::StyleGroupIndex::InheritedTextValues) == RustFFI::STYLE_GROUP_INDEX_INHERITED_TEXT);
static_assert(to_underlying(CSS::StyleGroupIndex::InheritedBoxValues) == RustFFI::STYLE_GROUP_INDEX_INHERITED_BOX);
static_assert(to_underlying(CSS::StyleGroupIndex::FontValues) == RustFFI::STYLE_GROUP_INDEX_FONT);
static_assert(to_underlying(CSS::StyleGroupIndex::SVGResetValues) == RustFFI::STYLE_GROUP_INDEX_SVG_RESET);
static_assert(to_underlying(CSS::StyleGroupIndex::BorderValues) == RustFFI::STYLE_GROUP_INDEX_BORDER);
static_assert(to_underlying(CSS::StyleGroupIndex::AlignmentValues) == RustFFI::STYLE_GROUP_INDEX_ALIGNMENT);
static_assert(to_underlying(CSS::StyleGroupIndex::SizingValues) == RustFFI::STYLE_GROUP_INDEX_SIZING);
static_assert(to_underlying(CSS::StyleGroupIndex::SurroundValues) == RustFFI::STYLE_GROUP_INDEX_SURROUND);
static_assert(to_underlying(CSS::StyleGroupIndex::BoxValues) == RustFFI::STYLE_GROUP_INDEX_BOX);

static RustFFI::FfiSvgViewBox to_ffi_svg_view_box(SVG::ViewBox const& view_box)
{
    return {
        .min_x = view_box.min_x,
        .min_y = view_box.min_y,
        .width = view_box.width,
        .height = view_box.height,
    };
}

static RustFFI::FfiSvgLengthValue to_ffi_svg_length_value(Optional<SVG::SVGLengthValue> const& value)
{
    if (!value.has_value())
        return { .value = 0, .kind = RustFFI::SVG_LENGTH_KIND_NONE, .unit = 0 };
    switch (value->kind()) {
    case SVG::SVGLengthValue::Kind::Number:
        return { .value = value->value(), .kind = RustFFI::SVG_LENGTH_KIND_NUMBER, .unit = 0 };
    case SVG::SVGLengthValue::Kind::Length:
        return { .value = value->value(), .kind = RustFFI::SVG_LENGTH_KIND_LENGTH, .unit = static_cast<u8>(to_underlying(value->unit())) };
    case SVG::SVGLengthValue::Kind::Percentage:
        return { .value = value->value(), .kind = RustFFI::SVG_LENGTH_KIND_PERCENTAGE, .unit = 0 };
    }
    VERIFY_NOT_REACHED();
}

// The element an SVG reference names, as the style mirror's id index can answer for it: the URL's decoded fragment,
// interned as the atom the element's id is indexed under. Parsing a URL is document work rather than layout work, so a
// reference travels as an atom and the pass resolves it through the index instead of asking the document for the
// element.
// FIXME: A same-document fragment is all this carries, which is all SVG resolves today.
static CSS::StyleAtomID svg_reference_fragment_atom(DOM::Element& element, Optional<Utf16String> const& url_string)
{
    if (!url_string.has_value())
        return {};
    auto url = element.document().encoding_parse_url(*url_string);
    if (!url.has_value() || !url->fragment().has_value())
        return {};
    auto fragment = SVG::decode_fragment_identifier(*url->fragment());
    // `#` alone names no element, as no element answers to an empty id.
    if (fragment.is_empty())
        return {};
    return element.document().style_computer().style_engine().intern_atom(Utf16FlyString::from_utf16(fragment.utf16_view()));
}

// The same, for a reference a graphics element's style carries rather than its `href`.
// `SVGGraphicsElement::resolve_url_to_element(CSS::URL const&)` takes the text after the first `#` of the URL as
// written, with no base-URL resolution, so `url(other.svg#shape)` names `#shape` in this document. Reproduced here as
// written.
// FIXME: Complete and use the entire URL, not just the fragment.
static CSS::StyleAtomID svg_style_reference_fragment_atom(DOM::Element& element, Optional<CSS::URL> const& url)
{
    if (!url.has_value())
        return {};
    auto fragment_offset = Utf16View { url->url() }.find_code_unit_offset('#');
    if (!fragment_offset.has_value())
        return {};
    auto fragment = SVG::decode_fragment_identifier(url->url().substring_view(fragment_offset.value() + 1));
    if (fragment.is_empty())
        return {};
    return element.document().style_computer().style_engine().intern_atom(Utf16FlyString::from_utf16(fragment.utf16_view()));
}

static Optional<CSS::URL> svg_paint_url(Optional<CSS::SVGPaint> const& paint)
{
    if (!paint.has_value() || !paint->is_url())
        return {};
    return paint->as_url();
}

// The four resources `mask`, `clip-path`, `fill` and `stroke` name. Read from the record's group payloads rather than
// through a materialized view: this runs for every SVG graphics element whose style record is replaced, and pinning a
// record to look at four properties is most of the cost of looking at them.
static Array<CSS::StyleAtomID, 4> svg_style_reference_atoms(DOM::Element& element)
{
    if (!is<SVG::SVGGraphicsElement>(element))
        return {};
    auto const* payloads = static_cast<void const* const*>(element.style_record_payloads());
    if (!payloads)
        return {};
    auto const* mask_payload = static_cast<CSS::ComputedValues::MaskValues const*>(payloads[CSS::ComputedValues::MaskValues::style_group_index]);
    auto const* svg_payload = static_cast<CSS::ComputedValues::InheritedSVGValues const*>(payloads[CSS::ComputedValues::InheritedSVGValues::style_group_index]);
    if (!mask_payload || !svg_payload)
        return {};
    auto const& mask = mask_payload->mask_value();
    return {
        svg_style_reference_fragment_atom(element, mask.has_value() ? Optional<CSS::URL> { mask->url() } : OptionalNone {}),
        svg_style_reference_fragment_atom(element, mask_payload->clip_path_value()),
        svg_style_reference_fragment_atom(element, svg_paint_url(svg_payload->fill_value())),
        svg_style_reference_fragment_atom(element, svg_paint_url(svg_payload->stroke_value())),
    };
}

// The SVG attributes an element parses, as the layout stage reads them.
static RustFFI::FfiSvgAttributeFacts build_svg_attribute_facts(DOM::Element& dom_node)
{
    auto const* svg_element = as_if<SVG::SVGElement>(dom_node);
    if (!svg_element)
        return {};
    auto const* fit_to_view_box = svg_element->fit_to_view_box();

    Optional<SVG::ViewBox> active_view_box;
    if (auto const* svg_graphics_element = as_if<SVG::SVGGraphicsElement>(dom_node))
        active_view_box = svg_graphics_element->active_view_box();
    else if (fit_to_view_box)
        active_view_box = fit_to_view_box->view_box();

    SVG::PreserveAspectRatio preserve_aspect_ratio {};
    if (fit_to_view_box)
        preserve_aspect_ratio = fit_to_view_box->preserve_aspect_ratio().value_or(SVG::PreserveAspectRatio {});
    else if (is<SVG::SVGMaskElement>(dom_node) || is<SVG::SVGClipPathElement>(dom_node))
        preserve_aspect_ratio = { SVG::PreserveAspectRatio::Align::None, {} };

    SVG::SVGUnits content_units {};
    SVG::SVGUnits pattern_units {};
    SVG::SVGUnits mask_units {};
    SVG::NumberPercentage mask_x = SVG::NumberPercentage::create_number(0);
    SVG::NumberPercentage mask_y = SVG::NumberPercentage::create_number(0);
    SVG::NumberPercentage mask_width = SVG::NumberPercentage::create_number(0);
    SVG::NumberPercentage mask_height = SVG::NumberPercentage::create_number(0);
    SVG::NumberPercentage pattern_width = SVG::NumberPercentage::create_number(0);
    SVG::NumberPercentage pattern_height = SVG::NumberPercentage::create_number(0);
    if (auto const* mask_element = as_if<SVG::SVGMaskElement>(dom_node)) {
        content_units = mask_element->mask_content_units();
        mask_units = mask_element->mask_units();
        mask_x = mask_element->mask_x();
        mask_y = mask_element->mask_y();
        mask_width = mask_element->mask_width();
        mask_height = mask_element->mask_height();
    } else if (auto const* clip_path_element = as_if<SVG::SVGClipPathElement>(dom_node))
        content_units = clip_path_element->clip_path_units();
    else if (auto const* pattern_element = as_if<SVG::SVGPatternElement>(dom_node)) {
        content_units = pattern_element->pattern_content_units();
        pattern_units = pattern_element->pattern_units();
        pattern_width = pattern_element->pattern_width();
        pattern_height = pattern_element->pattern_height();
    }

    auto geometry_kind = RustFFI::SVG_GEOMETRY_KIND_NONE;
    if (is<SVG::SVGPathElement>(dom_node))
        geometry_kind = RustFFI::SVG_GEOMETRY_KIND_PATH;
    else if (is<SVG::SVGRectElement>(dom_node))
        geometry_kind = RustFFI::SVG_GEOMETRY_KIND_RECT;
    else if (is<SVG::SVGCircleElement>(dom_node))
        geometry_kind = RustFFI::SVG_GEOMETRY_KIND_CIRCLE;
    else if (is<SVG::SVGEllipseElement>(dom_node))
        geometry_kind = RustFFI::SVG_GEOMETRY_KIND_ELLIPSE;
    else if (is<SVG::SVGPolylineElement>(dom_node))
        geometry_kind = RustFFI::SVG_GEOMETRY_KIND_POLYLINE;
    else if (is<SVG::SVGPolygonElement>(dom_node))
        geometry_kind = RustFFI::SVG_GEOMETRY_KIND_POLYGON;

    SVG::NumberPercentage line_x1 = SVG::NumberPercentage::create_number(0);
    SVG::NumberPercentage line_y1 = SVG::NumberPercentage::create_number(0);
    SVG::NumberPercentage line_x2 = SVG::NumberPercentage::create_number(0);
    SVG::NumberPercentage line_y2 = SVG::NumberPercentage::create_number(0);
    if (auto const* line_element = as_if<SVG::SVGLineElement>(dom_node)) {
        geometry_kind = RustFFI::SVG_GEOMETRY_KIND_LINE;
        line_x1 = line_element->x1_value();
        line_y1 = line_element->y1_value();
        line_x2 = line_element->x2_value();
        line_y2 = line_element->y2_value();
    }

    SVG::SVGTextPositioningElement::ParsedTextPositioning text_positioning;
    if (auto const* text_positioning_element = as_if<SVG::SVGTextPositioningElement>(dom_node))
        text_positioning = text_positioning_element->parsed_text_positioning();

    CSS::StyleAtomID reference_fragment;
    SVG::NumberPercentage start_offset = SVG::NumberPercentage::create_number(0);
    if (auto const* text_path_element = as_if<SVG::SVGTextPathElement>(dom_node)) {
        reference_fragment = svg_reference_fragment_atom(dom_node, text_path_element->href_attribute_value());
        start_offset = text_path_element->parsed_start_offset().value_or(start_offset);
    } else if (auto const* pattern_element = as_if<SVG::SVGPatternElement>(dom_node)) {
        // The pattern a <pattern> inherits its content and attributes from. An empty href names nothing, rather than
        // naming the document's own fragment.
        auto link = pattern_element->href_attribute_value();
        if (link.has_value() && !link->is_empty())
            reference_fragment = svg_reference_fragment_atom(dom_node, link);
    }

    // The resources an element's style names. They live with the presentation attributes because both are read as a
    // box is built, but they change with the element's style rather than with an attribute, so they have a
    // republication of their own.
    auto style_references = svg_style_reference_atoms(dom_node);

    // What an <svg>'s natural size is negotiated from: its width and height where they are a <length>, which layout
    // resolves against the box's style, and the aspect ratio its active SVG view or viewBox gives it.
    RustFFI::FfiSvgLengthValue natural_width { .value = 0, .kind = RustFFI::SVG_LENGTH_KIND_NONE, .unit = 0 };
    RustFFI::FfiSvgLengthValue natural_height { .value = 0, .kind = RustFFI::SVG_LENGTH_KIND_NONE, .unit = 0 };
    Optional<CSSPixelFraction> view_box_aspect_ratio;
    if (auto const* svg_element = as_if<SVG::SVGSVGElement>(dom_node)) {
        auto to_ffi_length = [](Optional<CSS::Length> const& length) -> RustFFI::FfiSvgLengthValue {
            if (!length.has_value())
                return { .value = 0, .kind = RustFFI::SVG_LENGTH_KIND_NONE, .unit = 0 };
            return { .value = length->raw_value(), .kind = RustFFI::SVG_LENGTH_KIND_LENGTH, .unit = static_cast<u8>(to_underlying(length->unit())) };
        };
        natural_width = to_ffi_length(svg_element->width_attribute_length());
        natural_height = to_ffi_length(svg_element->height_attribute_length());
        view_box_aspect_ratio = SVG::SVGSVGElement::view_box_natural_aspect_ratio(*svg_element);
    }

    return {
        .is_graphics_element = is<SVG::SVGGraphicsElement>(dom_node),
        .is_use_element = is<SVG::SVGUseElement>(dom_node),
        .is_svg_svg_element = is<SVG::SVGSVGElement>(dom_node),
        .is_symbol_element = is<SVG::SVGSymbolElement>(dom_node),
        .is_text_element = is<SVG::SVGTextElement>(dom_node),
        .is_fit_to_view_box = fit_to_view_box != nullptr,
        .has_active_view_box = active_view_box.has_value(),
        .active_view_box = active_view_box.has_value() ? to_ffi_svg_view_box(*active_view_box) : RustFFI::FfiSvgViewBox {},
        .preserve_aspect_ratio_align = static_cast<u8>(to_underlying(preserve_aspect_ratio.align)),
        .preserve_aspect_ratio_meet_or_slice = static_cast<u8>(to_underlying(preserve_aspect_ratio.meet_or_slice)),
        .content_units = static_cast<u8>(to_underlying(content_units)),
        .pattern_units = static_cast<u8>(to_underlying(pattern_units)),
        .pattern_width = to_ffi_number_percentage(pattern_width),
        .pattern_height = to_ffi_number_percentage(pattern_height),
        .mask_units = static_cast<u8>(to_underlying(mask_units)),
        .mask_x = to_ffi_number_percentage(mask_x),
        .mask_y = to_ffi_number_percentage(mask_y),
        .mask_width = to_ffi_number_percentage(mask_width),
        .mask_height = to_ffi_number_percentage(mask_height),
        .geometry_kind = geometry_kind,
        .line_x1 = to_ffi_number_percentage(line_x1),
        .line_y1 = to_ffi_number_percentage(line_y1),
        .line_x2 = to_ffi_number_percentage(line_x2),
        .line_y2 = to_ffi_number_percentage(line_y2),
        .text_x = to_ffi_svg_length_value(text_positioning.x),
        .text_y = to_ffi_svg_length_value(text_positioning.y),
        .text_dx = to_ffi_svg_length_value(text_positioning.dx),
        .text_dy = to_ffi_svg_length_value(text_positioning.dy),
        .reference_fragment_atom = reference_fragment.value(),
        .mask_reference_atom = style_references[0].value(),
        .clip_path_reference_atom = style_references[1].value(),
        .fill_reference_atom = style_references[2].value(),
        .stroke_reference_atom = style_references[3].value(),
        .text_path_start_offset = to_ffi_number_percentage(start_offset),
        .natural_width = natural_width,
        .natural_height = natural_height,
        .has_view_box_aspect_ratio = view_box_aspect_ratio.has_value(),
        .view_box_aspect_ratio_numerator = view_box_aspect_ratio.has_value() ? view_box_aspect_ratio->numerator() : 0,
        .view_box_aspect_ratio_denominator = view_box_aspect_ratio.has_value() ? view_box_aspect_ratio->denominator() : 0,
    };
}

// The answer moves only when the focused area does, or when a node arrives in the focused control's shadow tree, and a
// node in no user agent shadow tree never holds it, so the published set holds one control's shadow tree at a time.
void publish_is_in_focused_text_control(DOM::Node const& node)
{
    auto identity = Node::style_node_of(&node);
    if (identity.value() == 0)
        return;
    auto shadow_root = node.containing_shadow_root();
    auto value = shadow_root
        && shadow_root->is_user_agent_internal()
        && is<HTML::FormAssociatedTextControlElement>(shadow_root->host())
        && shadow_root->host()->is_focused();
    auto* arena = const_cast<DOM::Document&>(node.document()).layout_node_arena_if_created();
    if (!arena) {
        if (!value)
            return;
        arena = &const_cast<DOM::Document&>(node.document()).layout_node_arena();
    }
    RustFFI::render_state_set_identity_in_focused_text_control(arena->host(), identity.value(), value);
}

void publish_element_scroll_offset(DOM::Element const& element)
{
    if (element.style_node_id().value() == 0)
        return;
    auto& document = const_cast<DOM::Document&>(element.document());
    auto offset = element.scroll_offset({});
    // Nothing has scrolled anything before a layout tree exists, so there is no offset to forget.
    if (!document.layout_node_arena_if_created() && offset.is_zero())
        return;
    RustFFI::render_state_set_element_scroll_offset(document.layout_node_arena().host(), element.style_node_id().value(), offset);
}

void publish_table_spans(DOM::Element const& element)
{
    if (element.style_node_id().value() == 0)
        return;
    u16 column_span = 1;
    u16 row_span = 1;
    u32 raw_column_span = 1;
    if (auto const* cell = as_if<HTML::HTMLTableCellElement>(element)) {
        column_span = static_cast<u16>(cell->col_span());
        row_span = static_cast<u16>(cell->row_span());
    } else if (auto const* column = as_if<HTML::HTMLTableColElement>(element)) {
        column_span = static_cast<u16>(column->span());
        // The raw span keeps the unclamped attribute value; its only consumer is the table formatting context's column
        // handling, so other elements' span attributes stay out of the mirror.
        raw_column_span = column->get_attribute_value(HTML::AttributeNames::span).to_number<u32>().value_or(1);
    } else {
        return;
    }
    CSS::StyleEngineFFI::style_engine_set_element_table_spans(const_cast<DOM::Document&>(element.document()).style_computer().style_engine().host(), element.style_node_id(), column_span, row_span, raw_column_span);
}

// The publication is keyed by the element's style node rather than by a row, because an element that draws nothing
// itself has no row at all, while a mask, a clip or a pattern has one row per referencing element.
void publish_svg_attribute_facts(DOM::Element& element)
{
    VERIFY(element.style_node_id() != 0);
    ReadonlySpan<Gfx::FloatPoint> points;
    if (auto const* polygon = as_if<SVG::SVGPolygonElement>(element))
        points = polygon->points();
    else if (auto const* polyline = as_if<SVG::SVGPolylineElement>(element))
        points = polyline->points();
    static_assert(sizeof(Gfx::FloatPoint) == sizeof(RustFFI::FfiFloatPoint));
    RustFFI::render_state_set_svg_attribute_facts(
        element.document().layout_node_arena().host(),
        element.style_node_id().value(),
        build_svg_attribute_facts(element),
        reinterpret_cast<RustFFI::FfiFloatPoint const*>(points.data()),
        points.size());
}

// Republished on its own when an element's style record is replaced: the presentation attributes it sits beside are
// unchanged, and parsing all of them again for four names would make every style change on every SVG element pay for
// it.
void publish_svg_style_references(DOM::Element& element)
{
    VERIFY(element.style_node_id() != 0);
    auto references = svg_style_reference_atoms(element);
    RustFFI::render_state_set_svg_style_references(
        element.document().layout_node_arena().host(),
        element.style_node_id().value(),
        references[0].value(),
        references[1].value(),
        references[2].value(),
        references[3].value());
}

void register_layout_host(NodeArena& arena, DOM::Document& document)
{
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::None) == 0);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::xMinYMin) == 1);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::xMidYMin) == 2);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::xMaxYMin) == 3);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::xMinYMid) == 4);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::xMidYMid) == 5);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::xMaxYMid) == 6);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::xMinYMax) == 7);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::xMidYMax) == 8);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::xMaxYMax) == 9);
    static_assert(to_underlying(SVG::PreserveAspectRatio::MeetOrSlice::Meet) == 0);
    static_assert(to_underlying(SVG::PreserveAspectRatio::MeetOrSlice::Slice) == 1);
    static_assert(to_underlying(SVG::SVGUnits::ObjectBoundingBox) == 0);
    static_assert(to_underlying(SVG::SVGUnits::UserSpaceOnUse) == 1);
    RustFFI::FfiLayoutHostCallbacks callbacks {
        .context = &document,
        .deliver_commit_messages = [](void* context, BegunRead const* read, RustFFI::FfiCommitMessage const* messages, size_t count) {
            auto& commit_messages = static_cast<DOM::Document*>(context)->commit_messages();
            for (size_t index = 0; index < count; ++index)
                commit_messages.append(*read, messages[index]);
            // The pass that produced them reads back what they change before it ends.
            commit_messages.apply(*read); },
        .container_length_bases = [](void* document_pointer, u32 style_node) -> RustFFI::FfiContainerLengthBases {
            auto& document = *static_cast<DOM::Document*>(document_pointer);
            auto element = document.style_computer().element_for_style_node(style_node);
            VERIFY(element);
            auto context = CSS::Length::ResolutionContext::for_element(DOM::AbstractElement { *element });
            return {
                .width = CSS::Length(100, CSS::LengthUnit::Cqw).to_px_without_rounding(context),
                .height = CSS::Length(100, CSS::LengthUnit::Cqh).to_px_without_rounding(context),
            }; },
        .take_built_scroll_containers = [](void* context, BegunRead const* read, RustFFI::FfiBuiltScrollContainer const* built, size_t count) {
            auto& document = *static_cast<DOM::Document*>(context);
            for (auto const& scroll_container : ReadonlySpan<RustFFI::FfiBuiltScrollContainer> { built, count })
                Painting::take_built_scroll_container(*read, document, scroll_container.slot, scroll_container.is_scroll_snap_container); },
    };
    RustFFI::document_host_set_layout_host_callbacks(arena.host(), callbacks);
    RustFFI::render_state_set_document_is_decoded_svg(arena.host(), document.is_decoded_svg());
}

}

extern "C" WEB_API Web::Layout::RustFFI::FfiCodePointCategoryFacts ladybird_layout_code_point_category_facts(u32 code_point)
{
    static auto const ps = Unicode::general_category_from_string("Ps"sv).value();
    static auto const pd = Unicode::general_category_from_string("Pd"sv).value();
    return {
        .is_space_separator = Unicode::code_point_has_space_separator_general_category(code_point),
        .is_punctuation = Unicode::code_point_has_punctuation_general_category(code_point),
        .is_letter = Unicode::code_point_has_letter_general_category(code_point),
        .is_number = Unicode::code_point_has_number_general_category(code_point),
        .is_symbol = Unicode::code_point_has_symbol_general_category(code_point),
        .is_open_punctuation = Unicode::code_point_has_general_category(code_point, ps),
        .is_dash_punctuation = Unicode::code_point_has_general_category(code_point, pd),
    };
}

extern "C" WEB_API void ladybird_layout_node_shell_destroy(void* shell)
{
    Web::Layout::Node::delete_arena_owned_shell(*static_cast<Web::Layout::Node*>(shell));
}

extern "C" WEB_API void ladybird_layout_owned_image_provider_destroy(void* image_provider)
{
    delete static_cast<Web::Layout::ImageProvider*>(image_provider);
}

extern "C" WEB_API void ladybird_layout_owned_image_provider_notify_detach(void* image_provider)
{
    static_cast<Web::Layout::ImageProvider*>(image_provider)->layout_node_was_detached();
}

extern "C" WEB_API void ladybird_layout_image_observers_destroy(void* image_observers)
{
    delete static_cast<Web::Layout::NodeWithStyle::ImageObserverSlots*>(image_observers);
}
