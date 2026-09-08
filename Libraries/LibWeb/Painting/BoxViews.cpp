/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLMapElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TextNode.h>
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

static bool body_background_is_propagated_to_root(Layout::NodeWithStyle const& layout_node)
{
    if (!layout_node.is_body())
        return false;
    // Reachable at invalidation time, when the root element's layout node may already be detached.
    auto const* html_element = layout_node.document().html_element();
    return html_element && html_element->unsafe_layout_node() && html_element->should_use_body_background_properties();
}

GC::Ptr<SVG::SVGFilterElement> resolve_svg_filter_reference(CSS::ComputedValuesFFI::ComputedStyleValueHandle const& url_value, Layout::NodeWithStyle const& layout_node)
{
    auto fragment = CSS::ComputedFilterView::url_fragment(url_value);
    auto referenced_element = fragment.is_empty() ? nullptr : layout_node.document().get_element_by_id(fragment);
    return referenced_element ? as_if<SVG::SVGFilterElement>(*referenced_element) : nullptr;
}

Layout::RustFFI::NodeSlotId committed_row_slot(Layout::Node const& node)
{
    return Layout::Node::slot_id(&node);
}

Layout::RustFFI::NodeSlotId viewport_row_slot(DOM::Document const& document)
{
    return Layout::Node::slot_id(document.unsafe_layout_node());
}

Layout::RustFFI::PaintableData const* committed_row(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_row(node.arena_handle(), committed_row_slot(node));
}

bool has_committed_box(Layout::Node const& node)
{
    return committed_row(node) != nullptr;
}

Layout::Node* layout_node_for_committed_slot(Layout::NodeArena& arena, Layout::RustFFI::NodeSlotId slot)
{
    return static_cast<Layout::Node*>(Layout::RustFFI::layout_arena_paintable_layout_node_shell(arena.handle(), slot));
}

static PixelBox pixel_box_from_ffi(Layout::RustFFI::FfiPixelBox const& box)
{
    return { box.top, box.right, box.bottom, box.left };
}

CSSPixelRect absolute_rect(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_absolute_rect(node.arena_handle(), committed_row_slot(node));
}

CSSPixelRect absolute_padding_box_rect(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_absolute_padding_box_rect(node.arena_handle(), committed_row_slot(node));
}

CSSPixelRect absolute_border_box_rect(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_absolute_border_box_rect(node.arena_handle(), committed_row_slot(node));
}

CSSPixelPoint absolute_position(Layout::Node const& node)
{
    return absolute_rect(node).location();
}

CSSPixelPoint offset(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_offset(node.arena_handle(), committed_row_slot(node));
}

CSSPixelSize content_size(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_content_size(node.arena_handle(), committed_row_slot(node));
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
    auto metrics = Layout::RustFFI::layout_arena_paintable_box_model(node.arena_handle(), committed_row_slot(node));
    return {
        .margin = pixel_box_from_ffi(metrics.margin),
        .padding = pixel_box_from_ffi(metrics.padding),
        .border = pixel_box_from_ffi(metrics.border),
        .inset = pixel_box_from_ffi(metrics.inset),
    };
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

Optional<OverflowData> overflow_data(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    if (!row || !row->overflow_measured_this_commit)
        return {};
    return OverflowData { row->overflow_relative_to_padding_box.rect, row->overflow_relative_to_padding_box.has_scrollable_overflow };
}

static bool overflow_is_valid(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_overflow_is_valid(node.arena_handle(), committed_row_slot(node));
}

static void measure_scrollable_overflow_if_missing(Layout::Node const& node)
{
    if (overflow_is_valid(node))
        return;
    if (auto const* box = as_if<Layout::Box>(node))
        rust_measure_scrollable_overflow(*box);
}

bool has_scrollable_overflow(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    if (!row)
        return false;
    measure_scrollable_overflow_if_missing(node);
    return overflow_is_valid(node) && row->overflow_relative_to_padding_box.has_scrollable_overflow;
}

Optional<CSSPixelRect> scrollable_overflow_rect(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    if (!row)
        return {};
    measure_scrollable_overflow_if_missing(node);
    if (!overflow_is_valid(node))
        return {};
    auto rect = row->overflow_relative_to_padding_box.rect;
    rect.translate_by(absolute_padding_box_rect(node).location());
    return rect;
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
    auto const* row = committed_row(node);
    return row && row->establishes_stacking_context;
}

CSS::Display display(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return {};
    return as<Layout::NodeWithStyle>(node).display();
}

bool is_positioned(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_is_positioned(node.arena_handle(), committed_row_slot(node));
}

bool is_fixed_position(Layout::Node const& node)
{
    return has_committed_box(node) && as<Layout::NodeWithStyle>(node).is_fixed_position();
}

SelectionState selection_state(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    if (!row)
        return {};
    return static_cast<SelectionState>(row->selection_state);
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

bool is_inline_paintable(Layout::Node const& node)
{
    return has_committed_box(node) && node.is_fragmented_inline();
}

bool is_svg_paintable(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return false;
    switch (node.kind()) {
    case Layout::RustFFI::NodeKind::SVGGraphicsBox:
    case Layout::RustFFI::NodeKind::SVGGeometryBox:
    case Layout::RustFFI::NodeKind::SVGTextBox:
    case Layout::RustFFI::NodeKind::SVGTextPathBox:
    case Layout::RustFFI::NodeKind::SVGImageBox:
    case Layout::RustFFI::NodeKind::SVGMaskBox:
    case Layout::RustFFI::NodeKind::SVGClipBox:
    case Layout::RustFFI::NodeKind::SVGPatternBox:
        return true;
    default:
        return false;
    }
}

bool is_svg_svg_paintable(Layout::Node const& node)
{
    return has_committed_box(node) && node.kind() == Layout::RustFFI::NodeKind::SVGSVGBox;
}

bool is_svg_path_paintable(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return false;
    switch (node.kind()) {
    case Layout::RustFFI::NodeKind::SVGGeometryBox:
    case Layout::RustFFI::NodeKind::SVGTextBox:
    case Layout::RustFFI::NodeKind::SVGTextPathBox:
        return true;
    default:
        return false;
    }
}

bool has_accumulated_visual_context(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    return row && row->has_accumulated_visual_context;
}

ContextRef accumulated_visual_context(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    return row ? row->accumulated_visual_context : ContextRef {};
}

ContextRef accumulated_visual_context_for_descendants(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    return row ? row->accumulated_visual_context_for_descendants : ContextRef {};
}

SpatialNodeIndex enclosing_scroll_node_index(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    return row ? row->enclosing_scroll_node_index : VISUAL_VIEWPORT_NODE_INDEX;
}

SpatialNodeIndex own_scroll_node_index(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    return row ? row->own_scroll_node_index : VISUAL_VIEWPORT_NODE_INDEX;
}

Gfx::Path const* committed_svg_path(Layout::Node const& node)
{
    return static_cast<Gfx::Path const*>(Layout::RustFFI::layout_arena_paintable_computed_svg_path(node.arena_handle(), committed_row_slot(node)));
}

CSSPixelSize svg_viewport_size(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_svg_viewport_size(node.arena_handle(), committed_row_slot(node));
}

Optional<Gfx::AffineTransform> svg_viewport_transform(Layout::Node const& node)
{
    auto result = Layout::RustFFI::layout_arena_paintable_svg_viewport_transform(node.arena_handle(), committed_row_slot(node));
    if (!result.has_value)
        return {};
    auto const& transform = result.transform;
    return Gfx::AffineTransform { transform.a, transform.b, transform.c, transform.d, transform.e, transform.f };
}

CSS::RustStyleValueHandle used_value_for_grid_template(Layout::Node const& node, CSS::PropertyID property)
{
    VERIFY(property == CSS::PropertyID::GridTemplateColumns || property == CSS::PropertyID::GridTemplateRows);
    auto* value = Layout::RustFFI::layout_arena_paintable_used_grid_tracks(node.arena_handle(), committed_row_slot(node), property == CSS::PropertyID::GridTemplateColumns);
    if (!value)
        return {};
    return CSS::RustStyleValueHandle { static_cast<CSS::StyleValueFFI::StyleValueData const*>(value) };
}

CSSPixelPoint box_type_agnostic_position(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    if (!row)
        return {};
    if (is_inline_paintable(node)) {
        auto result = Layout::RustFFI::layout_arena_inline_paintable_first_piece_position(node.arena_handle(), committed_row_slot(node));
        if (result.has_value)
            return { result.x, result.y };
    }
    return absolute_position(node);
}

static bool has_content(Layout::Node const& node)
{
    // Interrupting block-in-inline children produce only placeholder pieces, so any child
    // paintable also counts as content.
    return Layout::RustFFI::layout_arena_inline_paintable_has_content_pieces(node.arena_handle(), committed_row_slot(node))
        || Layout::RustFFI::layout_arena_paintable_has_child_paintables(node.arena_handle(), committed_row_slot(node));
}

static CSSPixelRect caret_rect_for_empty_line(Layout::NodeWithStyle const& node, CSSPixelPoint position)
{
    // NB: Match the font-height caret used by text fragments, centered in the empty line's line-height box.
    auto const& font_metrics = node.first_available_font().pixel_metrics();
    auto line_height = node.line_height();
    auto caret_height = min(line_height, CSSPixels::nearest_value_for(font_metrics.ascent + font_metrics.descent));
    return { position.x(), position.y() + (line_height - caret_height) / 2, 1, caret_height };
}

// Caret rect for a cursor parked on this paintable's DOM node at the given child offset, e.g. on an empty line
// rendered by a <br> child or in an empty editable element.
CSSPixelRect caret_rect_for_child_offset(Layout::Node const& block, size_t offset)
{
    if (!has_committed_box(block))
        return {};
    auto const& styled_block = as<Layout::NodeWithStyle>(block);

    auto content_box = absolute_padding_box_rect(block);
    auto line_height = styled_block.line_height();
    auto rect = caret_rect_for_empty_line(styled_block, content_box.location());
    auto caret_offset_in_line = rect.y() - content_box.y();

    auto dom_node = block.dom_node();
    if (!dom_node)
        return rect;

    // NB: A boundary beside a text child has the same geometry as the corresponding text offset.
    //     Editors can leave the selection on the parent after inserting their first character.
    //     Use the text fragment's position and font metrics instead of the empty-block fallback.
    auto caret_rect_in_text = [&](DOM::Node const* node, size_t text_offset) -> Optional<CSSPixelRect> {
        auto const* text = as_if<DOM::Text>(node);
        auto const* layout_node = text ? text->unsafe_layout_node() : nullptr;
        if (!layout_node)
            return {};
        auto result = Layout::RustFFI::layout_arena_text_caret_rect_for_position(
            block.arena_handle(), Layout::Node::slot_id(layout_node), text_offset, true);
        if (result.found)
            return result.rect;
        return {};
    };
    if (offset > 0) {
        auto const* previous_child = dom_node->child_at_index(offset - 1);
        if (auto text_rect = caret_rect_in_text(previous_child, previous_child ? previous_child->length() : 0); text_rect.has_value())
            return *text_rect;
    }
    if (auto text_rect = caret_rect_in_text(dom_node->child_at_index(offset), 0); text_rect.has_value())
        return *text_rect;

    // A boundary immediately after an atomic inline element paints after that element. Atomic inline elements have
    if (offset > 0) {
        auto* previous_child = dom_node->child_at_index(offset - 1);
        auto const* previous_layout_node = previous_child ? previous_child->unsafe_layout_node() : nullptr;
        if (previous_layout_node && previous_layout_node->is_atomic_inline()) {
            auto result = Layout::RustFFI::layout_arena_paintable_first_fragment_rect_for_node(block.arena_handle(), committed_row_slot(block), Layout::Node::slot_id(previous_layout_node));
            if (result.has_value) {
                auto fragment_rect = result.rect;
                if (styled_block.writing_mode() == CSS::WritingMode::HorizontalTb)
                    rect.set_x(styled_block.inline_axis_is_reverse() ? fragment_rect.left() : fragment_rect.right());
                else
                    rect.set_y(styled_block.inline_axis_is_reverse() ? fragment_rect.top() : fragment_rect.bottom());
                return rect;
            }
        }
    }

    auto* child = dom_node->child_at_index(offset);
    if (!child || !is<HTML::HTMLBRElement>(*child))
        return rect;

    // A caret parked before a <br> sits on the line below the content preceding the <br>. Layout produces no
    // fragments for <br>, so start below the fragments of any preceding content, and add one line height for each
    // empty line rendered by earlier <br>s.
    struct PrecedingContentContext {
        GC::Ref<DOM::Node> child;
        Optional<CSSPixels> preceding_content_bottom;
    } preceding_context { const_cast<DOM::Node&>(*child), {} };
    Layout::RustFFI::layout_arena_for_each_subtree_fragment_rect(
        block.arena_handle(), committed_row_slot(block), &preceding_context,
        [](void* context_pointer, void* fragment_layout_node_shell, CSSPixelRect rect) {
            auto& context = *static_cast<PrecedingContentContext*>(context_pointer);
            auto const* fragment_layout_node = static_cast<Layout::Node const*>(fragment_layout_node_shell);
            auto* fragment_dom_node = fragment_layout_node ? const_cast<DOM::Node*>(fragment_layout_node->dom_node()) : nullptr;
            if (!fragment_dom_node || !(context.child->compare_document_position(fragment_dom_node) & DOM::Node::DOCUMENT_POSITION_PRECEDING))
                return;
            auto bottom = rect.bottom();
            if (!context.preceding_content_bottom.has_value() || bottom > *context.preceding_content_bottom)
                context.preceding_content_bottom = bottom;
        });
    auto& preceding_content_bottom = preceding_context.preceding_content_bottom;

    size_t preceding_empty_lines = 0;
    dom_node->for_each_in_subtree_of_type<HTML::HTMLBRElement>([&](auto& br) {
        if (&br == child)
            return TraversalDecision::Break;
        if (br.represents_empty_line())
            ++preceding_empty_lines;
        return TraversalDecision::Continue;
    });

    rect.set_y(preceding_content_bottom.value_or(content_box.y()) + line_height * preceding_empty_lines + caret_offset_in_line);
    return rect;
}

Layout::RustFFI::FfiCaretPaint resolve_document_caret_paint(DOM::Document& document)
{
    Layout::RustFFI::FfiCaretPaint caret {};
    Layout::RustFFI::NodeSlotId const no_slot { Layout::RustFFI::INVALID_NODE_SLOT_INDEX };
    caret.kind = Layout::RustFFI::FfiCaretPaintKind::None;
    caret.block = no_slot;
    caret.owner = no_slot;

    auto cursor_position = document.cursor_position();
    if (!cursor_position)
        return caret;
    // The caret paints only while the window has focus and the cursor node is editable (or a mutable text
    // control has focus); every candidate box below has a committed box.
    auto navigable = document.navigable();
    if (!navigable || !navigable->is_focused())
        return caret;
    auto const* cursor_node = cursor_position->node().ptr();
    if (!cursor_node)
        return caret;
    bool cursor_is_editable = false;
    if (auto const* text_control = as_if<HTML::FormAssociatedTextControlElement>(document.focused_area().ptr()); text_control && text_control->text_control_to_html_element().is_mutable())
        cursor_is_editable = true;
    else
        cursor_is_editable = cursor_node->is_editable_or_editing_host();
    if (!cursor_is_editable)
        return caret;

    auto fill = [&](Layout::RustFFI::FfiCaretPaintKind kind, Layout::RustFFI::NodeSlotId block, Layout::RustFFI::NodeSlotId owner, CSSPixelRect rect, Color color) {
        caret.kind = kind;
        caret.block = block;
        caret.owner = owner;
        caret.rect = rect;
        caret.color = color;
        caret.blink_cycle_start_time_ns = document.cursor_blink_cycle_start_time_ns();
        caret.should_blink = !HTML::Window::in_test_mode();
    };

    if (auto const* text = as_if<DOM::Text>(cursor_node)) {
        auto const* text_layout_node = text->unsafe_layout_node();
        if (!text_layout_node)
            return caret;
        auto* arena = text_layout_node->arena_handle();
        auto result = Layout::RustFFI::layout_arena_text_caret_rect_for_position(
            arena, Layout::Node::slot_id(text_layout_node), cursor_position->offset(),
            cursor_position->affinity() == TextAffinity::Downstream);
        if (result.found) {
            auto const* style_source = static_cast<Layout::NodeWithStyle const*>(result.style_source);
            if (style_source && layout_node_is_visible(*style_source))
                fill(Layout::RustFFI::FfiCaretPaintKind::InBlock, result.owner_paintable, result.nearest_self_painting_inline, result.rect, style_source->caret_color());
            return caret;
        }
        // No fragment holds the position: the caret sits on an empty line of the block that lays the text out.
        for (auto const* block = text_layout_node->parent(); block; block = block->parent()) {
            if (!has_committed_box(*block) || !is_visible(*block))
                continue;
            auto empty_line = Layout::RustFFI::layout_arena_paintable_empty_line_caret_rect(
                arena, committed_row_slot(*block), Layout::Node::slot_id(text_layout_node), cursor_position->offset());
            if (!empty_line.has_value)
                continue;
            auto const* style_source = static_cast<Layout::NodeWithStyle const*>(empty_line.style_source);
            if (!style_source)
                return caret;
            auto empty_line_rect = empty_line.rect;
            fill(Layout::RustFFI::FfiCaretPaintKind::InBlock, committed_row_slot(*block), no_slot, CSSPixelRect { empty_line_rect.x(), empty_line_rect.y(), 1, empty_line_rect.height() }, style_source->caret_color());
            return caret;
        }
        return caret;
    }

    // The cursor is parked on an element: its own box paints the caret at the child offset, or, for an
    // empty editable inline, at the box's position.
    auto const* layout_node = cursor_node->layout_node();
    if (!layout_node || !has_committed_box(*layout_node))
        return caret;
    auto const& styled_node = as<Layout::NodeWithStyle>(*layout_node);
    if (is_inline_paintable(*layout_node)) {
        if (has_content(*layout_node))
            return caret;
        auto position = box_type_agnostic_position(*layout_node);
        fill(Layout::RustFFI::FfiCaretPaintKind::EmptyInline, committed_row_slot(*layout_node), no_slot, caret_rect_for_empty_line(styled_node, position), styled_node.caret_color());
        return caret;
    }
    if (!is_visible(*layout_node))
        return caret;
    fill(Layout::RustFFI::FfiCaretPaintKind::InBlock, committed_row_slot(*layout_node), no_slot, caret_rect_for_child_offset(*layout_node, cursor_position->offset()), styled_node.caret_color());
    return caret;
}

Layout::RustFFI::FfiFocusedTextControlSelection resolve_focused_text_control_selection(DOM::Document const& document)
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
    auto const* text_layout_node = text_node->unsafe_layout_node();
    if (!text_layout_node)
        return selection;
    selection.text_node = Layout::Node::slot_id(text_layout_node);
    selection.start = selection_start;
    selection.end = selection_end;
    return selection;
}

Layout::RustFFI::FfiFocusedAreaOutline resolve_focused_area_outline(DOM::Document const& document, Vector<u8>& path_bytes)
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
    auto image_element = map_element->first_painted_image_with_focusable_shapes();
    if (!image_element)
        return outline;
    auto const* layout_node = image_element->layout_node();
    if (!layout_node || !has_committed_box(*layout_node))
        return outline;
    auto area_computed_values = area_element->computed_style();
    if (!area_computed_values || area_computed_values->outline_style() != CSS::OutlineStyle::Auto)
        return outline;
    auto outline_data = Painting::outline_data(*layout_node, *area_computed_values);
    if (!outline_data.has_value())
        return outline;
    auto path = area_element->shape_path(absolute_rect(*layout_node).size());
    if (!path.has_value())
        return outline;
    path_bytes = path->serialize_to_bytes();
    outline.image = committed_row_slot(*layout_node);
    outline.path_bytes = path_bytes.data();
    outline.path_byte_count = path_bytes.size();
    outline.color = outline_data->color;
    outline.width = outline_data->width;
    return outline;
}

static Optional<CSS::BorderData> border_data_for_outline(Layout::Node const& layout_node, Color outline_color, CSS::OutlineStyle outline_style, CSSPixels outline_width)
{
    CSS::LineStyle line_style;
    if (outline_style == CSS::OutlineStyle::Auto) {
        line_style = CSS::LineStyle::Solid;
        outline_color = CSS::KeywordStyleValue::create(CSS::Keyword::Accentcolor)->to_color(CSS::ColorResolutionContext::for_layout_node_with_style(*static_cast<Layout::NodeWithStyle const*>(&layout_node))).value();
        outline_width = 2;
    } else {
        line_style = CSS::keyword_to_line_style(CSS::to_keyword(outline_style)).value_or(CSS::LineStyle::None);
    }

    if (outline_color.alpha() == 0 || line_style == CSS::LineStyle::None || outline_width == 0)
        return {};

    return CSS::BorderData {
        .color = outline_color,
        .line_style = line_style,
        .width = outline_width,
    };
}

Optional<CSS::BorderData> outline_data(Layout::Node const& node, CSS::ComputedValues const& computed_values)
{
    if (!has_committed_box(node))
        return {};

    // The `auto` outline is the UA focus ring; like native controls, it is only shown while the window has focus.
    auto navigable = node.document().navigable();
    if (computed_values.outline_style() == CSS::OutlineStyle::Auto && (!navigable || !navigable->is_focused()))
        return {};

    return border_data_for_outline(node, computed_values.outline_color(), computed_values.outline_style(), computed_values.outline_width());
}

CSSPixels outline_offset(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return {};
    return as<Layout::NodeWithStyle>(node).outline_offset();
}

CSSPixelRect transform_reference_box(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_transform_reference_box(node.arena_handle(), committed_row_slot(node));
}

CSSPixelRect transform_rect_to_viewport(Layout::Node const& node, CSSPixelRect const& rect, AccumulatedVisualContextTree::IncludeVisualViewportTransform include_visual_viewport_transform)
{
    auto const* row = committed_row(node);
    if (!row)
        return {};
    auto const& document = node.document();
    if (!document.layout_node() || !has_committed_box(*document.layout_node()))
        return rect;
    auto pixel_ratio = static_cast<float>(document.page().client().device_pixels_per_css_pixel());
    auto result = document.visual_context_tree().transform_rect_to_viewport(
        row->accumulated_visual_context.spatial, rect.to_type<float>() * pixel_ratio,
        document.scroll_state_snapshot(), include_visual_viewport_transform);
    return (result * (1.f / pixel_ratio)).to_type<CSSPixels>();
}

Optional<CSSPixelPoint> transform_point_to_local(Layout::Node const& node, CSSPixelPoint position)
{
    auto const* row = committed_row(node);
    if (!row)
        return {};
    auto const& document = node.document();
    if (!document.layout_node() || !has_committed_box(*document.layout_node()))
        return position;
    auto pixel_ratio = static_cast<float>(document.page().client().device_pixels_per_css_pixel());
    auto result = document.visual_context_tree().transform_point_for_hit_test(
        row->accumulated_visual_context, position.to_type<float>() * pixel_ratio,
        document.scroll_state_snapshot());
    if (!result.has_value())
        return {};
    return (*result / pixel_ratio).to_type<CSSPixels>();
}

CSSPixelPoint inverse_transform_point(Layout::Node const& node, CSSPixelPoint position)
{
    auto const* row = committed_row(node);
    if (!row)
        return {};
    auto const& document = node.document();
    if (!document.layout_node() || !has_committed_box(*document.layout_node()))
        return position;
    auto pixel_ratio = static_cast<float>(document.page().client().device_pixels_per_css_pixel());
    auto result = document.visual_context_tree().inverse_transform_point(row->accumulated_visual_context.spatial, position.to_type<float>() * pixel_ratio);
    return (result / pixel_ratio).to_type<CSSPixels>();
}

CSSPixelPoint transform_to_local_coordinates(Layout::Node const& node, CSSPixelPoint position)
{
    if (!has_committed_box(node))
        return {};
    return transform_point_to_local(node, position).value_or(position);
}

Optional<String> grid_layout_json(Layout::Node const& node, UniqueNodeID container_node_id)
{
    Optional<String> result;
    Layout::RustFFI::layout_arena_paintable_grid_layout_json(node.arena_handle(), committed_row_slot(node), container_node_id.value(), &result,
        [](void* context, u8 const* bytes, size_t length) {
            *static_cast<Optional<String>*>(context) = MUST(String::from_utf8(StringView { bytes, length }));
        });
    return result;
}

Optional<String> flex_layout_json(Layout::Node const& node, UniqueNodeID container_node_id)
{
    Optional<String> result;
    Layout::RustFFI::layout_arena_paintable_flex_layout_json(node.arena_handle(), committed_row_slot(node), container_node_id.value(), &result,
        [](void* context, u8 const* bytes, size_t length) {
            *static_cast<Optional<String>*>(context) = MUST(String::from_utf8(StringView { bytes, length }));
        });
    return result;
}

struct SelectionPseudoStyleFacts {
    bool has_styling { false };
    Layout::RustFFI::FfiSelectionStyleFacts facts {};
    Vector<Layout::RustFFI::FfiSelectionShadowLayer> shadows;
};

static SelectionPseudoStyleFacts selection_pseudo_style_facts_of_element(DOM::Element const& element)
{
    auto computed_selection_style = element.computed_style(CSS::PseudoElement::Selection);
    if (!computed_selection_style)
        return {};

    SelectionPseudoStyleFacts result;
    auto& facts = result.facts;

    // https://drafts.csswg.org/css-pseudo-4/#paired-defaults
    // Paired default highlight colors must only be used when neither 'color' nor 'background-color' yield a
    // cascaded value from the author origin (or inherit their value from the author origin).
    facts.colors_authored = computed_selection_style->highlight_colors_authored();
    if (facts.colors_authored) {
        facts.background_color = computed_selection_style->background_color();
        // https://drafts.csswg.org/css-pseudo-4/#highlight-text
        // currentColor on a highlight pseudo-element's 'color' property represents the color of the next active
        // highlight pseudo-element layer below, falling back finally to the colors that would otherwise have been
        // used.
        if (!computed_selection_style->highlight_color_is_current_color())
            facts.text_color = computed_selection_style->color();
    }

    auto const& shadows = computed_selection_style->text_shadow();
    if (!shadows.is_empty()) {
        facts.has_text_shadow = true;
        for (auto const& shadow : shadows)
            result.shadows.append({ .color = shadow.color, .offset_x = shadow.offset_x, .offset_y = shadow.offset_y, .blur_radius = shadow.blur_radius });
    }

    auto lines = computed_selection_style->text_decoration_line();
    if (!lines.is_empty()) {
        facts.has_text_decoration = true;
        facts.text_decoration_line_count = min(lines.size(), array_size(facts.text_decoration_lines));
        for (size_t i = 0; i < facts.text_decoration_line_count; ++i)
            facts.text_decoration_lines[i] = to_underlying(lines[i]);
        facts.text_decoration_style = to_underlying(computed_selection_style->text_decoration_style());
        facts.text_decoration_color = computed_selection_style->text_decoration_color();
    }

    result.has_styling = facts.colors_authored || facts.has_text_shadow || facts.has_text_decoration;
    return result;
}

static void push_selection_pseudo_style_onto(Layout::Node const& layout_node, SelectionPseudoStyleFacts const& style)
{
    Layout::RustFFI::layout_arena_set_node_selection_pseudo_style(layout_node.arena_handle(), Layout::Node::slot_id(&layout_node), style.has_styling, style.facts, style.shadows.data(), style.shadows.size());
}

void push_selection_pseudo_style(DOM::Element const& element)
{
    auto style = selection_pseudo_style_facts_of_element(element);
    if (auto const* layout_node = element.unsafe_layout_node()) {
        push_selection_pseudo_style_onto(*layout_node, style);
        return;
    }
    for (auto const* child = element.first_child(); child; child = child->next_sibling()) {
        if (!is<DOM::Text>(*child))
            continue;
        if (auto const* text_layout_node = child->unsafe_layout_node())
            push_selection_pseudo_style_onto(*text_layout_node, style);
    }
}

void push_selection_pseudo_style_of_parent(Layout::TextNode& text_layout_node)
{
    auto const* text = text_layout_node.dom_text();
    auto const* parent_element = text ? text->parent_element().ptr() : nullptr;
    if (!parent_element || parent_element->unsafe_layout_node())
        return;
    if (!parent_element->computed_style(CSS::PseudoElement::Selection))
        return;
    push_selection_pseudo_style_onto(text_layout_node, selection_pseudo_style_facts_of_element(*parent_element));
}

class BoxViewRepaintAccess {
public:
    static void set_document_needs_repaint(DOM::Document& document, InvalidateDisplayList should_invalidate_display_list)
    {
        document.set_needs_repaint(Badge<BoxViewRepaintAccess> {}, should_invalidate_display_list);
    }
};

void set_needs_repaint(Layout::Node const& node, InvalidateDisplayList should_invalidate_display_list)
{
    if (!has_committed_box(node))
        return;

    auto& document = const_cast<DOM::Document&>(node.document());
    if (should_invalidate_display_list == InvalidateDisplayList::Yes) {
        Layout::RustFFI::layout_arena_paintable_invalidate_for_repaint(node.arena_handle(), committed_row_slot(node));

        // The root element paints the body's propagated background, so a body repaint must also refresh the
        // root's cached background. A root repaint can conversely flip whether propagation applies, changing
        // what the body itself paints.
        if (body_background_is_propagated_to_root(as<Layout::NodeWithStyle>(node))) {
            if (auto const* document_element = document.document_element()) {
                if (auto const* document_element_layout_node = document_element->unsafe_layout_node())
                    invalidate_paint_cache(*document_element_layout_node);
            }
        } else if (node.is_root_element()) {
            if (auto const* body = document.body()) {
                if (auto const* body_layout_node = body->unsafe_layout_node())
                    invalidate_paint_cache(*body_layout_node);
            }
        }
    }
    BoxViewRepaintAccess::set_document_needs_repaint(document, should_invalidate_display_list);
}

void set_needs_repaint_in_subtree(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return;
    Layout::RustFFI::layout_arena_paintable_invalidate_subtree_for_repaint(node.arena_handle(), committed_row_slot(node));
    set_needs_repaint(node);
}

void invalidate_paint_cache(Layout::Node const& node)
{
    mirror_rust_invalidate_paint_cache(node);
}

void repaint_after_style_change(Layout::Node const& node, CSS::RequiredInvalidationAfterStyleChange const& invalidation)
{
    if (invalidation.needs_repaint())
        set_needs_repaint(node);
    if (invalidation.repaint_propagated_text_decorations)
        rust_invalidate_propagated_text_decoration_caches(node);
    if (invalidation.needs_stacking_context_tree_rebuild()) {
        auto& document = const_cast<DOM::Document&>(node.document());
        document.schedule_accumulated_visual_context_update(node, DOM::Document::AccumulatedVisualContextUpdateScope::Structure);
        auto const* table_wrapper_carrying_the_moved_table_properties
            = display(node).is_table_inside() && node.parent() && node.parent()->is_table_wrapper() ? node.parent() : nullptr;
        if (table_wrapper_carrying_the_moved_table_properties)
            document.schedule_accumulated_visual_context_update(*table_wrapper_carrying_the_moved_table_properties, DOM::Document::AccumulatedVisualContextUpdateScope::Structure);
    }
}

void clear_overflow_data(Layout::Node const& node)
{
    Layout::RustFFI::layout_arena_paintable_clear_overflow_data(node.arena_handle(), committed_row_slot(node));
}

void clear_cached_overflow_data(Layout::Node const& node)
{
    Layout::RustFFI::layout_arena_paintable_clear_cached_overflow_data(node.arena_handle(), committed_row_slot(node));
}

Layout::RustFFI::FfiRectToViewportTransform identity_rect_to_viewport_transform()
{
    return {};
}

Layout::RustFFI::FfiRectToViewportTransform rect_to_viewport_transform(DOM::Document const& document, AccumulatedVisualContextTree const& visual_context_tree)
{
    if (!document.has_committed_viewport_box())
        return identity_rect_to_viewport_transform();
    auto scroll_offsets = document.scroll_state_snapshot().device_offsets();
    return {
        .visual_context_tree = visual_context_tree.rust_handle(),
        .scroll_offsets = scroll_offsets.data(),
        .scroll_offsets_len = scroll_offsets.size(),
        .device_pixels_per_css_pixel = static_cast<float>(document.page().client().device_pixels_per_css_pixel()),
    };
}

Vector<CSSPixelRect> client_rects(Layout::Node const& node, Layout::RustFFI::FfiRectToViewportTransform const& rect_to_viewport_transform)
{
    Vector<CSSPixelRect> rects;
    Layout::RustFFI::layout_arena_client_rects(
        node.arena_handle(), committed_row_slot(node), rect_to_viewport_transform, &rects,
        [](void* context, CSSPixelRect rect) {
            static_cast<Vector<CSSPixelRect>*>(context)->append(rect);
        });
    return rects;
}

CSSPixelRect bounding_client_rect(Layout::Node const& node, Layout::RustFFI::FfiRectToViewportTransform const& rect_to_viewport_transform)
{
    return Layout::RustFFI::layout_arena_bounding_client_rect(node.arena_handle(), committed_row_slot(node), rect_to_viewport_transform);
}

CSSPixelPoint cumulative_scroll_compensation(Layout::Node const& node)
{
    auto index = enclosing_scroll_node_index(node);
    if (index == VISUAL_VIEWPORT_NODE_INDEX)
        return {};
    auto const& document = node.document();
    if (!document.layout_node() || !has_committed_box(*document.layout_node()))
        return {};
    auto pixel_ratio = static_cast<float>(document.page().client().device_pixels_per_css_pixel());
    auto device_offset = document.visual_context_tree().cumulative_scroll_chain_offset(index, document.scroll_state_snapshot());
    return { CSSPixels::nearest_value_for(device_offset.x() / pixel_ratio), CSSPixels::nearest_value_for(device_offset.y() / pixel_ratio) };
}

}
