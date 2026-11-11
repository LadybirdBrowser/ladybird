/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLMapElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/SVG/SVGFilterElement.h>

namespace Web::Painting {

static bool g_paint_viewport_scrollbars = true;

void set_paint_viewport_scrollbars(bool enabled)
{
    g_paint_viewport_scrollbars = enabled;
}

bool should_paint_viewport_scrollbars()
{
    return g_paint_viewport_scrollbars;
}

GC::Ptr<SVG::SVGFilterElement> resolve_svg_filter_reference(CSS::ComputedValuesFFI::ComputedStyleValueHandle const& url_value, Layout::NodeWithStyle const& layout_node)
{
    auto fragment = CSS::ComputedFilterView::url_fragment(url_value);
    auto referenced_element = fragment.is_empty() ? nullptr : layout_node.document().get_element_by_id(fragment);
    return referenced_element ? as_if<SVG::SVGFilterElement>(*referenced_element) : nullptr;
}

Compositing::RustFFI::NodeSlotId committed_row_slot(Layout::Node const& node)
{
    return Layout::Node::slot_id(&node);
}

Compositing::RustFFI::NodeSlotId viewport_row_slot(Layout::BegunRead const& read, DOM::Document const& document)
{
    return Layout::Node::slot_id(document.unsafe_layout_node(read));
}

Optional<Layout::RustFFI::PaintableData> committed_row(Layout::Node const& node)
{
    Layout::RustFFI::PaintableData row;
    if (!Layout::RustFFI::render_state_paintable_row(node.document_host(), committed_row_slot(node), &row))
        return {};
    return row;
}

bool has_committed_box(Layout::Node const& node)
{
    return Layout::RustFFI::render_state_has_paintable_row(node.document_host(), committed_row_slot(node));
}

Layout::Node* layout_node_for_committed_slot(Layout::BegunRead const& read, Layout::NodeArena& arena, Compositing::RustFFI::NodeSlotId slot)
{
    return static_cast<Layout::Node*>(Layout::RustFFI::layout_row_paintable_layout_node_shell(arena.host(), &read, slot));
}

static PixelBox pixel_box_from_ffi(Layout::RustFFI::FfiPixelBox const& box)
{
    return { box.top, box.right, box.bottom, box.left };
}

CSSPixelRect absolute_rect(Layout::Node const& node)
{
    CSSPixelRect rect;
    Layout::RustFFI::layout_script_paintable_absolute_rect(node.document_host(), committed_row_slot(node), &rect);
    return rect;
}

CSSPixelRect absolute_padding_box_rect(Layout::Node const& node)
{
    CSSPixelRect rect;
    Layout::RustFFI::layout_script_paintable_absolute_padding_box_rect(node.document_host(), committed_row_slot(node), &rect);
    return rect;
}

CSSPixelRect absolute_border_box_rect(Layout::Node const& node)
{
    CSSPixelRect rect;
    Layout::RustFFI::layout_script_paintable_absolute_border_box_rect(node.document_host(), committed_row_slot(node), &rect);
    return rect;
}

CSSPixelRect border_box_rect_of_fragments(Layout::Node const& node)
{
    CSSPixelRect rect;
    Layout::RustFFI::layout_script_paintable_border_box_rect_of_fragments(node.document_host(), committed_row_slot(node), &rect);
    return rect;
}

size_t box_fragment_count(Layout::Node const& node)
{
    return Layout::RustFFI::layout_script_paintable_box_fragment_count(node.document_host(), committed_row_slot(node));
}

CSSPixelSize unbroken_border_box_size(Layout::Node const& node)
{
    CSSPixelSize size;
    Layout::RustFFI::layout_script_paintable_unbroken_border_box_size(node.document_host(), committed_row_slot(node), &size);
    return size;
}

CSSPixelPoint absolute_position(Layout::Node const& node)
{
    return absolute_rect(node).location();
}

CSSPixelSize content_size(Layout::Node const& node)
{
    CSSPixelSize size;
    Layout::RustFFI::layout_script_paintable_content_size(node.document_host(), committed_row_slot(node), &size);
    return size;
}

CSSPixels content_width(Layout::Node const& node)
{
    return content_size(node).width();
}

CSSPixels content_height(Layout::Node const& node)
{
    return content_size(node).height();
}

BoxModelMetrics box_model(Layout::Node const& node)
{
    Layout::RustFFI::FfiBoxModelMetrics metrics;
    Layout::RustFFI::layout_script_paintable_box_model(node.document_host(), committed_row_slot(node), &metrics);
    return {
        .margin = pixel_box_from_ffi(metrics.margin),
        .padding = pixel_box_from_ffi(metrics.padding),
        .border = pixel_box_from_ffi(metrics.border),
        .inset = pixel_box_from_ffi(metrics.inset),
    };
}

Optional<CSSPixels> anchor_resolved_size(Layout::Node const& node, CSS::PropertyID property_id)
{
    auto property = [&] {
        switch (property_id) {
        case CSS::PropertyID::MinWidth:
            return Layout::RustFFI::FfiAnchorSizedProperty::MinWidth;
        case CSS::PropertyID::MinHeight:
            return Layout::RustFFI::FfiAnchorSizedProperty::MinHeight;
        case CSS::PropertyID::MaxWidth:
            return Layout::RustFFI::FfiAnchorSizedProperty::MaxWidth;
        case CSS::PropertyID::MaxHeight:
            return Layout::RustFFI::FfiAnchorSizedProperty::MaxHeight;
        default:
            VERIFY_NOT_REACHED();
        }
    }();
    CSSPixels size;
    if (!Layout::RustFFI::layout_script_anchor_resolved_size(node.document_host(), committed_row_slot(node), property, &size))
        return {};
    return size;
}

CSSPixels border_box_width(Layout::Node const& node)
{
    auto border_box = box_model(node).border_box();
    return content_width(node) + border_box.left + border_box.right;
}

CSSPixels border_box_height(Layout::Node const& node)
{
    auto border_box = box_model(node).border_box();
    return content_height(node) + border_box.top + border_box.bottom;
}

bool has_scrollable_overflow(Layout::Node const& node)
{
    Layout::RustFFI::FfiOptionalOverflowData overflow;
    Layout::RustFFI::render_state_paintable_scrollable_overflow(node.document_host(), committed_row_slot(node), &overflow);
    return overflow.has_value && overflow.value.has_scrollable_overflow;
}

Optional<CSSPixelRect> scrollable_overflow_rect(Layout::Node const& node)
{
    Layout::RustFFI::FfiOptionalOverflowData overflow;
    Layout::RustFFI::render_state_paintable_scrollable_overflow(node.document_host(), committed_row_slot(node), &overflow);
    if (!overflow.has_value)
        return {};
    return overflow.value.rect;
}

static bool layout_node_is_visible(Layout::NodeWithStyle const& layout_node)
{
    return layout_node.visibility() == CSS::Visibility::Visible && layout_node.opacity() != 0;
}

bool is_visible(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return false;
    return layout_node_is_visible(as<Layout::NodeWithStyle>(node));
}

bool visible_for_hit_testing(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return false;
    if (auto dom_node = node.dom_node(); dom_node && dom_node->is_inert())
        return false;
    return as<Layout::NodeWithStyle>(node).pointer_events() != CSS::PointerEvents::None;
}

bool has_stacking_context(Layout::Node const& node)
{
    auto row = committed_row(node);
    return row.has_value() && row->establishes_stacking_context;
}

CSS::Display display(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return {};
    return as<Layout::NodeWithStyle>(node).display();
}

bool is_positioned(Layout::Node const& node)
{
    return Layout::RustFFI::layout_row_paintable_is_positioned(node.document_host(), committed_row_slot(node));
}

CSS::StyleRecordID style_record_identity(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return {};
    return as<Layout::NodeWithStyle>(node).style_record_identity();
}

bool is_navigable_container_viewport_paintable(Layout::Node const& node)
{
    return has_committed_box(node) && node.kind() == Layout::RustFFI::NodeKind::NavigableContainerViewport;
}

bool is_viewport_paintable(Layout::Node const& node)
{
    return has_committed_box(node) && node.kind() == Layout::RustFFI::NodeKind::Viewport;
}

bool is_paintable_with_lines(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return false;
    switch (node.kind()) {
    case Layout::RustFFI::NodeKind::Viewport:
    case Layout::RustFFI::NodeKind::BlockContainer:
    case Layout::RustFFI::NodeKind::LegendBox:
    case Layout::RustFFI::NodeKind::TableWrapper:
    case Layout::RustFFI::NodeKind::TextAreaBox:
    case Layout::RustFFI::NodeKind::TextInputBox:
    case Layout::RustFFI::NodeKind::RangeInputBox:
    case Layout::RustFFI::NodeKind::ListItemMarkerBox:
    case Layout::RustFFI::NodeKind::SVGForeignObjectBox:
        return true;
    case Layout::RustFFI::NodeKind::ListItemBox:
        return !node.is_fragmented_inline();
    default:
        return false;
    }
}

static bool is_inline_paintable(Layout::Node const& node)
{
    return has_committed_box(node) && node.is_fragmented_inline();
}

bool has_accumulated_visual_context(Layout::Node const& node)
{
    auto row = committed_row(node);
    return row.has_value() && row->has_accumulated_visual_context;
}

Compositing::ContextRef accumulated_visual_context(Layout::Node const& node)
{
    auto row = committed_row(node);
    return row.has_value() ? row->accumulated_visual_context : Compositing::ContextRef {};
}

Compositing::ContextRef accumulated_visual_context_for_descendants(Layout::Node const& node)
{
    auto row = committed_row(node);
    return row.has_value() ? row->accumulated_visual_context_for_descendants : Compositing::ContextRef {};
}

Compositing::SpatialNodeIndex own_scroll_node_index(Layout::Node const& node)
{
    auto row = committed_row(node);
    return row.has_value() ? row->own_scroll_node_index : Compositing::VISUAL_VIEWPORT_NODE_INDEX;
}

Optional<Layout::RustFFI::SvgGeometry> svg_geometry(Layout::Node const& node)
{
    Layout::RustFFI::SvgGeometry geometry;
    if (!Layout::RustFFI::render_state_svg_geometry(node.document_host(), committed_row_slot(node), &geometry))
        return {};
    return geometry;
}

// The glyph cells a text content element's box committed for its own characters, one per code unit, copied out
// before any later commit can replace them.
Vector<Layout::RustFFI::FfiSvgTextCharacterCell> svg_text_character_cells(Layout::Node const& node)
{
    auto result = Layout::RustFFI::render_state_paintable_svg_text_character_cells(node.document_host(), committed_row_slot(node));
    Vector<Layout::RustFFI::FfiSvgTextCharacterCell> cells;
    cells.ensure_capacity(result.count);
    for (size_t index = 0; index < result.count; ++index)
        cells.unchecked_append(result.cells[index]);
    return cells;
}

Optional<Gfx::AffineTransform> svg_viewport_transform(Layout::Node const& node)
{
    auto result = Layout::RustFFI::render_state_paintable_svg_viewport_transform(node.document_host(), committed_row_slot(node));
    if (!result.has_value)
        return {};
    auto const& transform = result.transform;
    return Gfx::AffineTransform { transform.a, transform.b, transform.c, transform.d, transform.e, transform.f };
}

Gfx::AffineTransform svg_element_transform(Layout::Node const& node)
{
    auto transform = Layout::RustFFI::render_state_paintable_svg_element_transform(node.document_host(), committed_row_slot(node));
    return { transform.a, transform.b, transform.c, transform.d, transform.e, transform.f };
}

CSS::RustStyleValueHandle used_value_for_grid_template(Layout::Node const& node, CSS::PropertyID property)
{
    VERIFY(property == CSS::PropertyID::GridTemplateColumns || property == CSS::PropertyID::GridTemplateRows);
    auto* value = Layout::RustFFI::render_state_paintable_used_grid_tracks(node.document_host(), committed_row_slot(node), property == CSS::PropertyID::GridTemplateColumns);
    if (!value)
        return {};
    return CSS::RustStyleValueHandle { static_cast<CSS::StyleValueFFI::StyleValueData const*>(value) };
}

CSSPixelPoint box_type_agnostic_position(Layout::Node const& node)
{
    auto row = committed_row(node);
    if (!row.has_value())
        return {};
    if (is_inline_paintable(node)) {
        Layout::RustFFI::FfiOptionalCssPixelPoint result;
        Layout::RustFFI::render_state_inline_paintable_first_piece_position(node.document_host(), committed_row_slot(node), &result);
        if (result.has_value)
            return { result.x, result.y };
    }
    return absolute_position(node);
}

Optional<Layout::RustFFI::FfiCaretPosition> layout_caret_position(Layout::BegunRead const& read, DOM::Node const& node, size_t offset, TextAffinity affinity)
{
    auto const* layout_node = node.unsafe_layout_node(read);
    if (!layout_node)
        return {};
    auto slot_of = [&](DOM::Node const* child) {
        auto const* child_layout_node = child ? child->unsafe_layout_node(read) : nullptr;
        return child_layout_node ? Layout::Node::slot_id(child_layout_node) : Compositing::RustFFI::NodeSlotId { Compositing::RustFFI::INVALID_NODE_SLOT_INDEX };
    };
    auto const* child_before = offset > 0 ? node.child_at_index(offset - 1) : nullptr;
    return Layout::RustFFI::FfiCaretPosition {
        .node = Layout::Node::slot_id(layout_node),
        .offset = offset,
        .downstream = affinity == TextAffinity::Downstream,
        .child_before = slot_of(child_before),
        .child_before_length = child_before ? child_before->length() : 0,
        .child_after = slot_of(node.child_at_index(offset)),
    };
}

Layout::RustFFI::FfiCaret caret_at_position(Layout::BegunRead const& read, DOM::Position const& position, Layout::RustFFI::FfiViewportMapping viewport_mapping)
{
    Layout::RustFFI::FfiCaret caret {};
    auto ffi_position = layout_caret_position(read, *position.node(), position.offset(), position.affinity());
    if (!ffi_position.has_value())
        return caret;
    Layout::RustFFI::layout_script_caret(position.node()->unsafe_layout_node(read)->document_host(), *ffi_position, viewport_mapping, &caret);
    return caret;
}

Layout::RustFFI::FfiCaretPaint resolve_document_caret_paint(Layout::BegunRead const& read, DOM::Document& document)
{
    Layout::RustFFI::FfiCaretPaint caret_paint {};
    Compositing::RustFFI::NodeSlotId const no_slot { Compositing::RustFFI::INVALID_NODE_SLOT_INDEX };
    caret_paint.kind = Layout::RustFFI::FfiCaretPaintKind::None;
    caret_paint.block = no_slot;
    caret_paint.owner = no_slot;

    auto cursor_position = document.cursor_position();
    if (!cursor_position)
        return caret_paint;
    // The caret paints only while the cursor node is editable (or a mutable text control has focus), and the recorder
    // paints it only while the window has focus.
    bool cursor_is_editable = false;
    if (auto const* text_control = as_if<HTML::FormAssociatedTextControlElement>(document.focused_area().ptr()); text_control && text_control->text_control_to_html_element().is_mutable())
        cursor_is_editable = true;
    else
        cursor_is_editable = cursor_position->node()->is_editable_or_editing_host();
    if (!cursor_is_editable)
        return caret_paint;

    auto caret = caret_at_position(read, *cursor_position, {});
    if (!caret.found)
        return caret_paint;
    auto const* style_source = static_cast<Layout::NodeWithStyle const*>(document.layout_node_arena().node_if_live(read, caret.style_source));
    if (!style_source || !layout_node_is_visible(*style_source))
        return caret_paint;
    caret_paint.kind = caret.paint;
    caret_paint.block = caret.block;
    caret_paint.owner = caret.owner;
    caret_paint.rect = caret.rect;
    caret_paint.color = style_source->caret_color();
    caret_paint.blink_cycle_start_time_ns = document.cursor_blink_cycle_start_time_ns();
    caret_paint.should_blink = !HTML::Window::in_test_mode();
    return caret_paint;
}

Layout::RustFFI::FfiFocusedTextControlSelection resolve_focused_text_control_selection(Layout::BegunRead const& read, DOM::Document const& document)
{
    Layout::RustFFI::FfiFocusedTextControlSelection selection {};
    auto const* text_control = as_if<HTML::FormAssociatedTextControlElement>(document.focused_area().ptr());
    if (!text_control)
        return selection;
    auto text_node = text_control->form_associated_element_to_text_node();
    if (!text_node)
        return selection;
    auto selection_start = text_control->selection_start();
    auto selection_end = text_control->selection_end();
    if (selection_start == selection_end)
        return selection;
    auto const* text_layout_node = text_node->unsafe_layout_node(read);
    if (!text_layout_node)
        return selection;
    selection.text_node = Layout::Node::slot_id(text_layout_node);
    selection.start = selection_start;
    selection.end = selection_end;
    return selection;
}

Layout::RustFFI::FfiFocusedAreaOutline resolve_focused_area_outline(Layout::BegunRead const& read, DOM::Document const& document)
{
    // https://html.spec.whatwg.org/multipage/interaction.html#focusable-area
    // The shapes of area elements in an image map associated with an img element that is being rendered and is not
    // inert. Focused area elements have no box of their own, so the image whose rendering makes the area's shape a
    // focusable area paints the focus outline along that shape.
    Layout::RustFFI::FfiFocusedAreaOutline outline {};
    auto const* area_element = as_if<HTML::HTMLAreaElement>(document.focused_area().ptr());
    if (!area_element)
        return outline;
    auto const* map_element = area_element->first_ancestor_of_type<HTML::HTMLMapElement>();
    if (!map_element)
        return outline;
    auto image_element = map_element->first_painted_image_with_focusable_shapes(read);
    if (!image_element)
        return outline;
    auto const* layout_node = image_element->layout_node(read);
    if (!layout_node || !has_committed_box(*layout_node))
        return outline;
    auto area_computed_values = area_element->computed_style();
    if (!area_computed_values || area_computed_values->outline_style() != CSS::OutlineStyle::Auto)
        return outline;
    outline.image = committed_row_slot(*layout_node);
    outline.area = DOM::NodeIdentity::of(*area_element).style_node().value();
    return outline;
}

CSSPixelRect transform_reference_box(Layout::Node const& node)
{
    CSSPixelRect rect;
    Layout::RustFFI::render_state_paintable_transform_reference_box(node.document_host(), committed_row_slot(node), &rect);
    return rect;
}

Layout::RustFFI::FfiViewportMapping viewport_mapping(DOM::Document const& document)
{
    if (!document.has_committed_viewport_box())
        return {};
    // NB: The document's tree outlives the handle made here: the render state holds it, and so does the scroll state,
    //     whose world transforms retain the tree they were built from.
    return viewport_mapping(document, document.visual_context_tree());
}

Layout::RustFFI::FfiViewportMapping viewport_mapping(DOM::Document const& document, Compositing::AccumulatedVisualContextTree const& visual_context_tree)
{
    if (!document.has_committed_viewport_box())
        return {};
    return document.paint_state().viewport_mapping(visual_context_tree, document.page().client().device_pixels_per_css_pixel());
}

// NB: Taking the document's visual context tree brings it up to date, which can compact it and move a box's visual
//     context to other nodes. The functions below read a box's visual context only once they have the tree.
CSSPixelRect rect_to_viewport(Layout::Node const& node, CSSPixelRect const& rect, Layout::RustFFI::ViewportSpace space)
{
    if (!has_committed_box(node))
        return {};
    auto mapping = viewport_mapping(node.document());
    CSSPixelRect result;
    Layout::RustFFI::viewport_mapping_rect_in_space_to_viewport(mapping, accumulated_visual_context(node).spatial, rect, space, &result);
    return result;
}

CSSPixelRect visible_rect_in_containing_blocks(Layout::Node const& node, CSSPixelRect const& rect, Layout::RustFFI::ViewportSpace space)
{
    auto mapping = viewport_mapping(node.document());
    CSSPixelRect visible;
    Layout::RustFFI::render_state_visible_rect_in_containing_blocks(node.document_host(), committed_row_slot(node), mapping, rect, space, &visible);
    return visible;
}

CSSPixelPoint point_to_local(Layout::Node const& node, CSSPixelPoint point, Layout::RustFFI::ViewportSpace space)
{
    if (!has_committed_box(node))
        return {};
    auto mapping = viewport_mapping(node.document());
    CSSPixelPoint local;
    if (!Layout::RustFFI::viewport_mapping_point_to_local(mapping, accumulated_visual_context(node), point, space, &local))
        return point;
    return local;
}

// https://drafts.csswg.org/cssom-view/#dom-mouseevent-offsetx
CSSPixelPoint mouse_event_offset(Layout::Node const& node, CSSPixelPoint viewport_position)
{
    // The position where the event occurred, ignoring the transforms that apply to the element and its ancestors,
    // relative to the origin of the padding edge of the target node.
    return point_to_local(node, viewport_position, Layout::RustFFI::ViewportSpace::LayoutViewport) - box_type_agnostic_position(node);
}

CSSPixelPoint presented_scroll_offset(Layout::Node const& node, CSSPixelPoint scroll_offset)
{
    auto const& document = node.document();
    if (!has_committed_box(node) || !document.has_committed_viewport_box())
        return scroll_offset;
    auto visual_context_tree = document.visual_context_tree();
    if (!visual_context_tree.translations_snap_to_device_pixels(accumulated_visual_context(node).spatial))
        return scroll_offset;
    auto pixel_ratio = document.page().client().device_pixels_per_css_pixel();
    auto snap = [&](CSSPixels value) { return CSSPixels::nearest_value_for(round(value.to_double() * pixel_ratio) / pixel_ratio); };
    return { snap(scroll_offset.x()), snap(scroll_offset.y()) };
}

Optional<String> grid_layout_json(Layout::Node const& node, UniqueNodeID container_node_id)
{
    Optional<String> result;
    Layout::RustFFI::render_state_paintable_grid_layout_json(node.document_host(), committed_row_slot(node), container_node_id.value(), &result,
        [](void* context, u8 const* bytes, size_t length) {
            *static_cast<Optional<String>*>(context) = MUST(String::from_utf8(StringView { bytes, length }));
        });
    return result;
}

// The devtools protocol names a flex item by its DOM node's unique id, while the layout row that
// recorded the item names it by its style node.
static i64 devtools_node_id_for_style_node(void const* context, u32 style_node)
{
    auto const& document = *static_cast<DOM::Document const*>(context);
    auto dom_node = document.style_computer().node_for_style_node(CSS::StyleNodeID { style_node });
    return dom_node ? dom_node->unique_id().value() : -1;
}

Optional<String> flex_layout_json(Layout::Node const& node, UniqueNodeID container_node_id)
{
    Optional<String> result;
    Layout::RustFFI::render_state_paintable_flex_layout_json(
        node.document_host(), committed_row_slot(node), container_node_id.value(), &result,
        [](void* context, u8 const* bytes, size_t length) {
            *static_cast<Optional<String>*>(context) = MUST(String::from_utf8(StringView { bytes, length }));
        },
        &node.document(), devtools_node_id_for_style_node);
    return result;
}

DOM::NodeIdentity node_identity_of(Layout::RustFFI::FfiNodeIdentity identity)
{
    if (identity.is_document)
        return DOM::NodeIdentity::of_document();
    return DOM::NodeIdentity::of_style_node(CSS::StyleNodeID { identity.style_node });
}

void push_highlight_pseudo_styles(DOM::Element const& element)
{
    auto* arena = const_cast<DOM::Document&>(element.document()).layout_node_arena_if_created();
    if (!arena)
        return;
    Layout::RustFFI::render_state_sync_highlight_pseudo_styles(arena->host(), element.style_node_id().value(), element.style_record_identity(CSS::PseudoElement::Selection).value(), element.style_record_identity(CSS::PseudoElement::SearchText).value(), element.style_record_identity(CSS::PseudoElement::SearchTextCurrent).value(), element.style_record_identity(CSS::PseudoElement::SpellingError).value());
    Vector<u32, 4> names;
    Vector<u64, 4> style_records;
    if (auto const* styles = element.custom_highlight_styles()) {
        for (auto const& [name, style] : *styles) {
            names.append(name.value());
            style_records.append(style.record().value());
        }
    }
    Layout::RustFFI::render_state_sync_custom_highlight_pseudo_styles(arena->host(), element.style_node_id().value(), names.span(), style_records.span());
}

class BoxViewRepaintAccess {
public:
    static void set_document_needs_repaint(DOM::Document& document, InvalidateDisplayList should_invalidate_display_list)
    {
        document.set_needs_repaint(Badge<BoxViewRepaintAccess> {}, should_invalidate_display_list);
    }
};

void mark_box(Layout::Node const& node, Layout::RustFFI::FfiBoxMarks marks)
{
    Layout::RustFFI::render_state_mark_row_box(node.document_host(), Layout::Node::slot_id(&node), marks);
    BoxViewRepaintAccess::set_document_needs_repaint(const_cast<DOM::Document&>(node.document()), display_list_invalidation_of(marks));
}

Layout::RustFFI::FfiBoxMarks repaint_marks(InvalidateDisplayList should_invalidate_display_list)
{
    Layout::RustFFI::FfiBoxMarks marks {};
    marks.repaint = should_invalidate_display_list != InvalidateDisplayList::No;
    marks.repaint_hit_testing = should_invalidate_display_list == InvalidateDisplayList::PaintCommandsAndHitTestList;
    return marks;
}

InvalidateDisplayList display_list_invalidation_of(Layout::RustFFI::FfiBoxMarks marks)
{
    if (marks.repaint_hit_testing || marks.repaint_subtree || marks.has_dom_paint_facts)
        return InvalidateDisplayList::PaintCommandsAndHitTestList;
    return marks.repaint ? InvalidateDisplayList::PaintCommands : InvalidateDisplayList::No;
}

void set_needs_repaint(Layout::Node const& node, InvalidateDisplayList should_invalidate_display_list)
{
    if (has_committed_box(node))
        mark_box(node, repaint_marks(should_invalidate_display_list));
}

void request_document_repaint(DOM::Document const& document, InvalidateDisplayList should_invalidate_display_list)
{
    BoxViewRepaintAccess::set_document_needs_repaint(const_cast<DOM::Document&>(document), should_invalidate_display_list);
}

void set_needs_repaint_in_subtree(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return;
    Layout::RustFFI::FfiBoxMarks marks {};
    marks.repaint_subtree = true;
    mark_box(node, marks);
}

static void invalidate_propagated_text_decoration_caches(Layout::Node const& node)
{
    Layout::RustFFI::FfiBoxMarks marks {};
    marks.propagated_text_decorations = true;
    mark_box(node, marks);
}

void repaint_after_style_change(Layout::Node const& node, CSS::RequiredInvalidationAfterStyleChange const& invalidation)
{
    if (invalidation.needs_repaint())
        set_needs_repaint(node, invalidation.invalidates_hit_test_display_list() ? InvalidateDisplayList::PaintCommandsAndHitTestList : InvalidateDisplayList::PaintCommands);
    if (invalidation.repaint_propagated_text_decorations)
        invalidate_propagated_text_decoration_caches(node);
    if (invalidation.needs_stacking_context_tree_rebuild())
        const_cast<DOM::Document&>(node.document()).schedule_accumulated_visual_context_update(node, DOM::Document::AccumulatedVisualContextUpdateScope::Structure);
}

Vector<CSSPixelRect> client_rects(Layout::Node const& node, Layout::RustFFI::FfiViewportMapping const& viewport_mapping)
{
    Vector<CSSPixelRect> rects;
    Layout::RustFFI::layout_script_client_rects(
        node.document_host(), committed_row_slot(node), viewport_mapping, &rects,
        [](void* context, CSSPixelRect rect) {
            static_cast<Vector<CSSPixelRect>*>(context)->append(rect);
        });
    return rects;
}

bool has_sticky_positioned_inclusive_ancestor(Layout::Node const& node)
{
    return Layout::RustFFI::layout_script_has_sticky_positioned_inclusive_ancestor(node.document_host(), committed_row_slot(node));
}

CSSPixelPoint accumulated_sticky_offset(Layout::Node const& node)
{
    CSSPixelPoint offset;
    Layout::RustFFI::layout_script_accumulated_sticky_offset(node.document_host(), committed_row_slot(node), viewport_mapping(node.document()), &offset);
    return offset;
}

CSSPixelRect bounding_client_rect(Layout::Node const& node, Layout::RustFFI::FfiViewportMapping const& viewport_mapping)
{
    CSSPixelRect rect;
    Layout::RustFFI::layout_script_bounding_client_rect(node.document_host(), committed_row_slot(node), viewport_mapping, &rect);
    return rect;
}

CSSPixelPoint offset_to_viewport_ignoring_transforms(Layout::Node const& node)
{
    CSSPixelPoint offset;
    Layout::RustFFI::layout_script_offset_to_viewport_ignoring_transforms(node.document_host(), committed_row_slot(node), viewport_mapping(node.document()), &offset);
    return offset;
}

}
