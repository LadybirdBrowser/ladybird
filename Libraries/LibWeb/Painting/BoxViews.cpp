/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
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
#include <LibWebCommon/CSS/SystemColor.h>

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
    return Layout::RustFFI::layout_script_paintable_absolute_rect(node.document_host(), committed_row_slot(node));
}

CSSPixelRect absolute_padding_box_rect(Layout::Node const& node)
{
    return Layout::RustFFI::layout_script_paintable_absolute_padding_box_rect(node.document_host(), committed_row_slot(node));
}

CSSPixelRect absolute_border_box_rect(Layout::Node const& node)
{
    return Layout::RustFFI::layout_script_paintable_absolute_border_box_rect(node.document_host(), committed_row_slot(node));
}

CSSPixelPoint absolute_position(Layout::Node const& node)
{
    return absolute_rect(node).location();
}

CSSPixelSize content_size(Layout::Node const& node)
{
    return Layout::RustFFI::layout_script_paintable_content_size(node.document_host(), committed_row_slot(node));
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
    auto metrics = Layout::RustFFI::layout_script_paintable_box_model(node.document_host(), committed_row_slot(node));
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

bool has_scrollable_overflow(Layout::Node const& node)
{
    auto overflow = Layout::RustFFI::render_state_paintable_scrollable_overflow(node.document_host(), committed_row_slot(node));
    return overflow.has_value && overflow.value.has_scrollable_overflow;
}

Optional<CSSPixelRect> scrollable_overflow_rect(Layout::Node const& node)
{
    auto overflow = Layout::RustFFI::render_state_paintable_scrollable_overflow(node.document_host(), committed_row_slot(node));
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

bool is_inline_paintable(Layout::Node const& node)
{
    return has_committed_box(node) && node.is_fragmented_inline();
}

bool is_svg_svg_paintable(Layout::Node const& node)
{
    return has_committed_box(node) && node.kind() == Layout::RustFFI::NodeKind::SVGSVGBox;
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

Compositing::SpatialNodeIndex enclosing_scroll_node_index(Layout::Node const& node)
{
    auto row = committed_row(node);
    return row.has_value() ? row->enclosing_scroll_node_index : Compositing::VISUAL_VIEWPORT_NODE_INDEX;
}

Compositing::SpatialNodeIndex own_scroll_node_index(Layout::Node const& node)
{
    auto row = committed_row(node);
    return row.has_value() ? row->own_scroll_node_index : Compositing::VISUAL_VIEWPORT_NODE_INDEX;
}

Gfx::Path const* committed_svg_path(Layout::Node const& node)
{
    return static_cast<Gfx::Path const*>(Layout::RustFFI::render_state_paintable_computed_svg_path(node.document_host(), committed_row_slot(node)));
}

CSSPixelSize svg_viewport_size(Layout::Node const& node)
{
    return Layout::RustFFI::render_state_paintable_svg_viewport_size(node.document_host(), committed_row_slot(node));
}

Optional<Gfx::AffineTransform> svg_viewport_transform(Layout::Node const& node)
{
    auto result = Layout::RustFFI::render_state_paintable_svg_viewport_transform(node.document_host(), committed_row_slot(node));
    if (!result.has_value)
        return {};
    auto const& transform = result.transform;
    return Gfx::AffineTransform { transform.a, transform.b, transform.c, transform.d, transform.e, transform.f };
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
        auto result = Layout::RustFFI::render_state_inline_paintable_first_piece_position(node.document_host(), committed_row_slot(node));
        if (result.has_value)
            return { result.x, result.y };
    }
    return absolute_position(node);
}

static bool has_content(Layout::Node const& node)
{
    // Interrupting block-in-inline children produce only placeholder pieces, so any child
    // paintable also counts as content.
    return Layout::RustFFI::render_state_inline_paintable_has_content_pieces(node.document_host(), committed_row_slot(node))
        || Layout::RustFFI::layout_row_paintable_has_child_paintables(node.document_host(), committed_row_slot(node));
}

static CSSPixelRect caret_rect_for_empty_line(Layout::NodeWithStyle const& node, CSSPixelPoint position)
{
    // NB: Match the font-height caret used by text fragments, centered in the empty line's line-height box.
    auto const& font_metrics = node.first_available_font().pixel_metrics();
    auto line_height = node.line_height();
    auto caret_height = min(line_height, CSSPixels::nearest_value_for(font_metrics.ascent + font_metrics.descent));
    return { position.x(), position.y() + (line_height - caret_height) / 2, 1, caret_height };
}

static Optional<Layout::RustFFI::FfiCaretRectResult> caret_at_atomic_child(Layout::Node const& layout_node, size_t offset)
{
    auto const& read = layout_node.held_read();
    auto* node = layout_node.dom_node();
    if (!node)
        return {};
    auto resolve = [&](size_t child_offset) -> Optional<Layout::RustFFI::FfiCaretRectResult> {
        auto const* child = node->child_at_index(child_offset);
        auto* child_layout_node = child ? child->unsafe_layout_node(read) : nullptr;
        if (!child_layout_node || !child_layout_node->is_atomic_inline())
            return {};
        auto result = Layout::RustFFI::layout_script_atomic_inline_caret_rect_for_position(
            layout_node.document_host(), Layout::Node::slot_id(child_layout_node), child_offset < offset);
        if (!result.found)
            return {};
        return result;
    };
    if (offset > 0) {
        if (auto result = resolve(offset - 1); result.has_value())
            return result;
    }
    return resolve(offset);
}

// Caret rect for a cursor parked on this paintable's DOM node at the given child offset, e.g. on an empty line
// rendered by a <br> child or in an empty editable element.
CSSPixelRect caret_rect_for_child_offset(Layout::BegunRead const& read, Layout::Node const& block, size_t offset)
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

    if (auto atomic_caret = caret_at_atomic_child(block, offset); atomic_caret.has_value())
        return atomic_caret->rect;

    // NB: A boundary beside a text child has the same geometry as the corresponding text offset.
    //     Editors can leave the selection on the parent after inserting their first character.
    //     Use the text fragment's position and font metrics instead of the empty-block fallback.
    auto caret_rect_in_text = [&](DOM::Node const* node, size_t text_offset) -> Optional<CSSPixelRect> {
        auto const* text = as_if<DOM::Text>(node);
        auto const* layout_node = text ? text->unsafe_layout_node(read) : nullptr;
        if (!layout_node)
            return {};
        auto result = Layout::RustFFI::layout_script_text_caret_rect_for_position(
            block.document_host(), Layout::Node::slot_id(layout_node), text_offset, true);
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

    auto* child = dom_node->child_at_index(offset);
    if (!child || !is<HTML::HTMLBRElement>(*child))
        return rect;

    // A caret parked before a <br> sits on the line below the content preceding the <br>. Layout produces no
    // fragments for <br>, so start below the fragments of any preceding content, and add one line height for each
    // empty line rendered by earlier <br>s.
    struct PrecedingContentContext {
        GC::Ref<DOM::Node> child;
        Layout::BegunRead const& read;
        Optional<CSSPixels> preceding_content_bottom;
    } preceding_context {
        .child = const_cast<DOM::Node&>(*child),
        .read = read,
        .preceding_content_bottom = {},
    };
    Layout::RustFFI::render_state_for_each_subtree_fragment_rect(
        block.document_host(), committed_row_slot(block), &preceding_context,
        [](void* context_pointer, Compositing::RustFFI::NodeSlotId fragment_slot, CSSPixelRect rect) {
            auto& context = *static_cast<PrecedingContentContext*>(context_pointer);
            auto const* fragment_layout_node = context.child->document().layout_node_arena().node_if_live(context.read, fragment_slot);
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
        if (br.represents_empty_line(read))
            ++preceding_empty_lines;
        return TraversalDecision::Continue;
    });

    rect.set_y(preceding_content_bottom.value_or(content_box.y()) + line_height * preceding_empty_lines + caret_offset_in_line);
    return rect;
}

Layout::RustFFI::FfiCaretPaint resolve_document_caret_paint(Layout::BegunRead const& read, DOM::Document& document)
{
    Layout::RustFFI::FfiCaretPaint caret {};
    Compositing::RustFFI::NodeSlotId const no_slot { Compositing::RustFFI::INVALID_NODE_SLOT_INDEX };
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

    auto fill = [&](Layout::RustFFI::FfiCaretPaintKind kind, Compositing::RustFFI::NodeSlotId block, Compositing::RustFFI::NodeSlotId owner, CSSPixelRect rect, Color color) {
        caret.kind = kind;
        caret.block = block;
        caret.owner = owner;
        caret.rect = rect;
        caret.color = color;
        caret.blink_cycle_start_time_ns = document.cursor_blink_cycle_start_time_ns();
        caret.should_blink = !HTML::Window::in_test_mode();
    };

    if (auto const* text = as_if<DOM::Text>(cursor_node)) {
        auto const* text_layout_node = text->unsafe_layout_node(read);
        if (!text_layout_node)
            return caret;
        auto* host = text_layout_node->document_host();
        auto result = Layout::RustFFI::layout_script_text_caret_rect_for_position(
            host, Layout::Node::slot_id(text_layout_node), cursor_position->offset(),
            cursor_position->affinity() == TextAffinity::Downstream);
        if (result.found) {
            auto const* style_source = static_cast<Layout::NodeWithStyle const*>(text_layout_node->node_arena().node_if_live(read, result.style_source));
            if (style_source && layout_node_is_visible(*style_source))
                fill(Layout::RustFFI::FfiCaretPaintKind::InBlock, result.owner_paintable, result.nearest_self_painting_inline, result.rect, style_source->caret_color());
            return caret;
        }
        // No fragment holds the position: the caret sits on an empty line of the block that lays the text out.
        for (auto const* block = text_layout_node->parent(); block; block = block->parent()) {
            if (!has_committed_box(*block) || !is_visible(*block))
                continue;
            auto empty_line = Layout::RustFFI::layout_script_paintable_empty_line_caret_rect(
                host, committed_row_slot(*block), Layout::Node::slot_id(text_layout_node), cursor_position->offset());
            if (!empty_line.has_value)
                continue;
            auto const* style_source = static_cast<Layout::NodeWithStyle const*>(text_layout_node->node_arena().node_if_live(read, empty_line.style_source));
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
    auto const* layout_node = cursor_node->layout_node(read);
    if (!layout_node || !has_committed_box(*layout_node))
        return caret;
    auto const& styled_node = as<Layout::NodeWithStyle>(*layout_node);
    if (is_inline_paintable(*layout_node)) {
        if (auto atomic_caret = caret_at_atomic_child(*layout_node, cursor_position->offset()); atomic_caret.has_value()) {
            if (layout_node_is_visible(styled_node))
                fill(Layout::RustFFI::FfiCaretPaintKind::InBlock, atomic_caret->owner_paintable, atomic_caret->nearest_self_painting_inline, atomic_caret->rect, styled_node.caret_color());
            return caret;
        }
        if (has_content(*layout_node))
            return caret;
        auto position = box_type_agnostic_position(*layout_node);
        fill(Layout::RustFFI::FfiCaretPaintKind::EmptyInline, committed_row_slot(*layout_node), no_slot, caret_rect_for_empty_line(styled_node, position), styled_node.caret_color());
        return caret;
    }
    if (!is_visible(*layout_node))
        return caret;
    fill(Layout::RustFFI::FfiCaretPaintKind::InBlock, committed_row_slot(*layout_node), no_slot, caret_rect_for_child_offset(read, *layout_node, cursor_position->offset()), styled_node.caret_color());
    return caret;
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

Layout::RustFFI::FfiFocusedAreaOutline resolve_focused_area_outline(Layout::BegunRead const& read, DOM::Document const& document, Vector<u8>& path_bytes)
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

CSSPixelRect transform_reference_box(Layout::Node const& node)
{
    return Layout::RustFFI::render_state_paintable_transform_reference_box(node.document_host(), committed_row_slot(node));
}

CSSPixelRect transform_rect_to_viewport(Layout::Node const& node, CSSPixelRect const& rect, Compositing::AccumulatedVisualContextTree::IncludeVisualViewportTransform include_visual_viewport_transform)
{
    auto row = committed_row(node);
    if (!row.has_value())
        return {};
    auto const& document = node.document();
    if (!document.has_committed_viewport_box())
        return rect;
    auto pixel_ratio = static_cast<float>(document.page().client().device_pixels_per_css_pixel());
    auto result = document.visual_context_tree().transform_rect_to_viewport(
        row->accumulated_visual_context.spatial, rect.to_type<float>() * pixel_ratio,
        document.scroll_state_snapshot(), include_visual_viewport_transform);
    return (result * (1.f / pixel_ratio)).to_type<CSSPixels>();
}

Optional<CSSPixelPoint> transform_point_to_local(Layout::Node const& node, CSSPixelPoint position)
{
    auto row = committed_row(node);
    if (!row.has_value())
        return {};
    auto const& document = node.document();
    if (!document.has_committed_viewport_box())
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
    auto row = committed_row(node);
    if (!row.has_value())
        return {};
    auto const& document = node.document();
    if (!document.has_committed_viewport_box())
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
    if (auto* arena = const_cast<DOM::Document&>(element.document()).layout_node_arena_if_created())
        Layout::RustFFI::render_state_sync_highlight_pseudo_styles(arena->host(), element.style_node_id().value(), element.style_record_identity(CSS::PseudoElement::Selection).value(), element.style_record_identity(CSS::PseudoElement::SearchText).value(), element.style_record_identity(CSS::PseudoElement::SearchTextCurrent).value());
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

void invalidate_propagated_text_decoration_caches(Layout::Node const& node)
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
    if (invalidation.needs_stacking_context_tree_rebuild()) {
        auto& document = const_cast<DOM::Document&>(node.document());
        document.schedule_accumulated_visual_context_update(node, DOM::Document::AccumulatedVisualContextUpdateScope::Structure);
        auto const* table_wrapper_carrying_the_moved_table_properties
            = display(node).is_table_inside() && node.parent() && node.parent()->is_table_wrapper() ? node.parent() : nullptr;
        if (table_wrapper_carrying_the_moved_table_properties)
            document.schedule_accumulated_visual_context_update(*table_wrapper_carrying_the_moved_table_properties, DOM::Document::AccumulatedVisualContextUpdateScope::Structure);
    }
}

Layout::RustFFI::FfiRectToViewportTransform identity_rect_to_viewport_transform()
{
    return {};
}

Layout::RustFFI::FfiRectToViewportTransform rect_to_viewport_transform(DOM::Document const& document, Compositing::AccumulatedVisualContextTree const& visual_context_tree)
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
    Layout::RustFFI::layout_script_client_rects(
        node.document_host(), committed_row_slot(node), rect_to_viewport_transform, &rects,
        [](void* context, CSSPixelRect rect) {
            static_cast<Vector<CSSPixelRect>*>(context)->append(rect);
        });
    return rects;
}

CSSPixelRect bounding_client_rect(Layout::Node const& node, Layout::RustFFI::FfiRectToViewportTransform const& rect_to_viewport_transform)
{
    return Layout::RustFFI::layout_script_bounding_client_rect(node.document_host(), committed_row_slot(node), rect_to_viewport_transform);
}

CSSPixelPoint cumulative_scroll_compensation(Layout::Node const& node)
{
    auto index = enclosing_scroll_node_index(node);
    if (index == Compositing::VISUAL_VIEWPORT_NODE_INDEX)
        return {};
    auto const& document = node.document();
    if (!document.has_committed_viewport_box())
        return {};
    auto pixel_ratio = static_cast<float>(document.page().client().device_pixels_per_css_pixel());
    auto device_offset = document.visual_context_tree().cumulative_scroll_chain_offset(index, document.scroll_state_snapshot());
    return { CSSPixels::nearest_value_for(device_offset.x() / pixel_ratio), CSSPixels::nearest_value_for(device_offset.y() / pixel_ratio) };
}

}
