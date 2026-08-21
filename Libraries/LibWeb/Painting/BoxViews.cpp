/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TextOffsetMapping.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::Painting {

bool has_committed_box(Layout::Node const& node)
{
    return node.paintable_ptr();
}

#define FORWARD_BOX_VIEW(return_type, name, zero_value) \
    return_type name(Layout::Node const& node)          \
    {                                                   \
        auto const* paintable = node.paintable_ptr();   \
        if (!paintable)                                 \
            return zero_value;                          \
        return paintable->name();                       \
    }

FORWARD_BOX_VIEW(CSSPixelRect, absolute_rect, {})
FORWARD_BOX_VIEW(CSSPixelRect, absolute_padding_box_rect, {})
FORWARD_BOX_VIEW(CSSPixelRect, absolute_border_box_rect, {})
FORWARD_BOX_VIEW(CSSPixelPoint, absolute_position, {})
FORWARD_BOX_VIEW(CSSPixels, absolute_x, {})
FORWARD_BOX_VIEW(CSSPixels, absolute_y, {})
FORWARD_BOX_VIEW(CSSPixelPoint, offset, {})
FORWARD_BOX_VIEW(CSSPixelSize, content_size, {})
FORWARD_BOX_VIEW(CSSPixels, content_width, {})
FORWARD_BOX_VIEW(CSSPixels, content_height, {})
FORWARD_BOX_VIEW(CSSPixels, border_box_width, {})
FORWARD_BOX_VIEW(CSSPixels, border_box_height, {})
FORWARD_BOX_VIEW(BoxModelMetrics, box_model, {})
FORWARD_BOX_VIEW(CSSPixels, outline_offset, {})
FORWARD_BOX_VIEW(CSSPixelRect, transform_reference_box, {})
FORWARD_BOX_VIEW(Optional<CSSPixelRect>, scrollable_overflow_rect, {})
FORWARD_BOX_VIEW(bool, has_scrollable_overflow, false)
FORWARD_BOX_VIEW(Optional<Paintable::OverflowData>, overflow_data, {})
FORWARD_BOX_VIEW(Optional<Paintable::CachedOverflowData>, cached_overflow_data, {})

FORWARD_BOX_VIEW(bool, is_visible, false)
FORWARD_BOX_VIEW(bool, visible_for_hit_testing, false)
FORWARD_BOX_VIEW(bool, has_stacking_context, false)
FORWARD_BOX_VIEW(Optional<int>, effective_z_index, {})
FORWARD_BOX_VIEW(CSS::Display, display, {})
FORWARD_BOX_VIEW(bool, is_positioned, false)
FORWARD_BOX_VIEW(bool, is_fixed_position, false)
FORWARD_BOX_VIEW(bool, is_sticky_position, false)
FORWARD_BOX_VIEW(bool, is_absolutely_positioned, false)
FORWARD_BOX_VIEW(bool, is_floating, false)
FORWARD_BOX_VIEW(bool, is_inline, false)
FORWARD_BOX_VIEW(bool, has_css_transform, false)
FORWARD_BOX_VIEW(bool, has_non_invertible_css_transform, false)
FORWARD_BOX_VIEW(bool, uses_collapsing_borders_model, false)
FORWARD_BOX_VIEW(SelectionState, selection_state, {})
FORWARD_BOX_VIEW(CSS::StyleRecordID, style_record_identity, {})
FORWARD_BOX_VIEW(StringView, class_name, {})
FORWARD_BOX_VIEW(String, debug_description, {})
FORWARD_BOX_VIEW(bool, is_navigable_container_viewport_paintable, false)
FORWARD_BOX_VIEW(bool, is_viewport_paintable, false)
FORWARD_BOX_VIEW(bool, is_paintable_with_lines, false)
FORWARD_BOX_VIEW(bool, is_inline_paintable, false)
FORWARD_BOX_VIEW(bool, is_svg_paintable, false)
FORWARD_BOX_VIEW(bool, is_svg_svg_paintable, false)
FORWARD_BOX_VIEW(bool, is_svg_path_paintable, false)
FORWARD_BOX_VIEW(bool, is_svg_foreign_object_paintable, false)

FORWARD_BOX_VIEW(bool, has_accumulated_visual_context, false)
FORWARD_BOX_VIEW(VisualContextIndex, accumulated_visual_context_index, {})
FORWARD_BOX_VIEW(VisualContextIndex, accumulated_visual_context_for_descendants_index, {})
FORWARD_BOX_VIEW(Optional<VisualContextIndex>, fixed_background_visual_context, {})
FORWARD_BOX_VIEW(VisualContextIndex, enclosing_scroll_node_index, {})
FORWARD_BOX_VIEW(VisualContextIndex, own_scroll_node_index, {})

FORWARD_BOX_VIEW(Gfx::Path const*, committed_svg_path, nullptr)
FORWARD_BOX_VIEW(CSSPixelSize, svg_viewport_size, {})
FORWARD_BOX_VIEW(Optional<Gfx::AffineTransform>, svg_viewport_transform, {})
FORWARD_BOX_VIEW(Optional<UsedGridTrackList>, used_values_for_grid_template_columns, {})
FORWARD_BOX_VIEW(Optional<UsedGridTrackList>, used_values_for_grid_template_rows, {})

FORWARD_BOX_VIEW(CSSPixelPoint, box_type_agnostic_position, {})
FORWARD_BOX_VIEW(Paintable::SelectionStyle, selection_style, {})
FORWARD_BOX_VIEW(StickyInsets, sticky_insets, {})
FORWARD_BOX_VIEW(bool, has_sticky_insets, false)

#undef FORWARD_BOX_VIEW

bool should_paint_cursor(Layout::Node const& node)
{
    if (!node.paintable_ptr())
        return false;

    auto const& document = node.document();
    if (!document.cursor_blink_state() || !document.navigable()->is_focused())
        return false;

    auto cursor_position = document.cursor_position();
    if (!cursor_position)
        return false;

    if (auto const* text_control = as_if<HTML::FormAssociatedTextControlElement>(document.focused_area().ptr());
        text_control && text_control->text_control_to_html_element().is_mutable()) {
        return true;
    }

    // The editable element may sit anywhere between the cursor and this box (e.g. a
    // contenteditable inline box), so editability is the cursor node's, not this box's.
    auto const* editable_node = cursor_position->node().ptr();
    return editable_node && editable_node->is_editable_or_editing_host();
}

Layout::Node const* nearest_self_painting_inline_box(Layout::Node const& node)
{
    for (auto const* ancestor = node.nearest_fragmented_inline_ancestor(); ancestor; ancestor = ancestor->nearest_fragmented_inline_ancestor()) {
        if (is_inline_paintable(*ancestor) && (has_stacking_context(*ancestor) || is_positioned(*ancestor)))
            return ancestor;
    }
    return nullptr;
}

bool has_content(Layout::Node const& node)
{
    auto const* paintable = node.paintable_ptr();
    if (!paintable)
        return false;

    // Interrupting block-in-inline children produce only placeholder pieces, so any child
    // paintable also counts as content.
    return Layout::RustFFI::layout_arena_inline_paintable_has_content_pieces(paintable->rust_arena().handle(), paintable->rust_slot())
        || Layout::RustFFI::layout_arena_paintable_has_child_paintables(paintable->rust_arena().handle(), paintable->rust_slot());
}

// Caret rect for a cursor parked on this paintable's DOM node at the given child offset, e.g. on an empty line
// rendered by a <br> child or in an empty editable element.
CSSPixelRect caret_rect_for_child_offset(Layout::Node const& block, size_t offset)
{
    auto const* paintable = block.paintable_ptr();
    if (!paintable)
        return {};
    auto const& styled_block = as<Layout::NodeWithStyle>(block);

    auto content_box = absolute_padding_box_rect(block);
    auto line_height = styled_block.line_height();
    CSSPixelRect rect { content_box.x(), content_box.y(), 1, line_height };

    auto dom_node = block.dom_node();
    if (!dom_node)
        return rect;

    // A boundary immediately after an atomic inline element paints after that element. Atomic inline elements have
    if (offset > 0) {
        auto* previous_child = dom_node->child_at_index(offset - 1);
        auto const* previous_layout_node = previous_child ? previous_child->unsafe_layout_node() : nullptr;
        if (previous_layout_node && previous_layout_node->is_atomic_inline()) {
            auto result = Layout::RustFFI::layout_arena_paintable_first_fragment_rect_for_node(paintable->rust_arena().handle(), paintable->rust_slot(), Layout::Node::slot_id(previous_layout_node));
            if (result.has_value) {
                auto fragment_rect = from_ffi_css_pixel_rect(result.rect);
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
        paintable->rust_arena().handle(), paintable->rust_slot(), &preceding_context,
        [](void* context_pointer, void* fragment_layout_node_shell, Layout::RustFFI::FfiCssPixelRect rect) {
            auto& context = *static_cast<PrecedingContentContext*>(context_pointer);
            auto const* fragment_layout_node = static_cast<Layout::Node const*>(fragment_layout_node_shell);
            auto* fragment_dom_node = fragment_layout_node ? const_cast<DOM::Node*>(fragment_layout_node->dom_node()) : nullptr;
            if (!fragment_dom_node || !(context.child->compare_document_position(fragment_dom_node) & DOM::Node::DOCUMENT_POSITION_PRECEDING))
                return;
            auto bottom = CSSPixels::from_raw(rect.y) + CSSPixels::from_raw(rect.height);
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

    rect.set_y(preceding_content_bottom.value_or(content_box.y()) + line_height * preceding_empty_lines);
    return rect;
}

static bool layout_node_is_visible(Layout::NodeWithStyle const& layout_node)
{
    return layout_node.visibility() == CSS::Visibility::Visible && layout_node.opacity() != 0;
}

Optional<CaretPaint> resolve_caret_paint(Layout::Node const& block, Layout::Node const* owner_inline)
{
    auto const* paintable = block.paintable_ptr();
    if (!paintable || !should_paint_cursor(block))
        return {};
    auto const& styled_block = as<Layout::NodeWithStyle>(block);

    auto cursor_position = block.document().cursor_position();
    VERIFY(cursor_position);

    auto const* dom_node = block.dom_node();

    Vector<Layout::RustFFI::NodeSlotId, 2> text_slots;
    if (auto const* text = as_if<DOM::Text>(cursor_position->node().ptr()))
        text_slots = Layout::TextOffsetMapping { *text }.slot_ids();

    if (!text_slots.is_empty()) {
        auto result = Layout::RustFFI::layout_arena_text_caret_rect_for_position(
            paintable->rust_arena().handle(), text_slots.data(), text_slots.size(), cursor_position->offset(),
            cursor_position->affinity() == TextAffinity::Downstream);
        if (result.found && result.owner_paintable.index == paintable->rust_slot().index) {
            auto const* owner_paintable = owner_inline ? owner_inline->paintable_ptr() : nullptr;
            auto owner_slot = owner_paintable ? owner_paintable->rust_slot()
                                              : Layout::RustFFI::PaintableSlotId { Layout::RustFFI::INVALID_PAINTABLE_SLOT_INDEX };
            if (result.nearest_self_painting_inline.index != owner_slot.index)
                return {};
            auto const* style_source = static_cast<Layout::NodeWithStyle const*>(result.style_source);
            if (!style_source || !layout_node_is_visible(*style_source))
                return {};
            return CaretPaint { from_ffi_css_pixel_rect(result.rect), style_source->caret_color() };
        }
        if (result.found) {
            return {};
        }
    }

    if (owner_inline) {
        // Blank lines and empty editable elements are handled by the block / the box itself.
        return {};
    }
    if (!is_visible(block)) {
        // Blank-line and empty-element carets belong to this block itself.
        return {};
    }

    if (!text_slots.is_empty()) {
        auto empty_line = Layout::RustFFI::layout_arena_paintable_empty_line_caret_rect(
            paintable->rust_arena().handle(), paintable->rust_slot(), text_slots.data(), text_slots.size(), cursor_position->offset());
        if (empty_line.has_value) {
            auto const* style_source = static_cast<Layout::NodeWithStyle const*>(empty_line.style_source);
            if (!style_source)
                return {};
            auto empty_line_rect = from_ffi_css_pixel_rect(empty_line.rect);
            CSSPixelRect cursor_rect { empty_line_rect.x(), empty_line_rect.y(), 1, empty_line_rect.height() };
            return CaretPaint { cursor_rect, style_source->caret_color() };
        }
    }

    if (cursor_position->node() != GC::Ptr { dom_node })
        return {};

    return CaretPaint { caret_rect_for_child_offset(block, cursor_position->offset()), styled_block.caret_color() };
}

Optional<CaretPaint> resolve_empty_editable_caret_paint(Layout::Node const& node)
{
    if (!node.paintable_ptr() || has_content(node))
        return {};
    auto const& styled_node = as<Layout::NodeWithStyle>(node);

    if (!should_paint_cursor(node))
        return {};

    auto cursor_position = node.document().cursor_position();
    VERIFY(cursor_position);

    auto const* dom_node = node.dom_node();
    if (!dom_node || cursor_position->node() != GC::Ptr { dom_node })
        return {};

    auto position = box_type_agnostic_position(node);
    return CaretPaint {
        .rect = { position.x(), position.y(), 1, styled_node.line_height() },
        .color = styled_node.caret_color(),
    };
}

Optional<CSS::BorderData> outline_data(Layout::Node const& node, CSS::ComputedValues const& computed_values)
{
    auto const* paintable = node.paintable_ptr();
    return paintable ? paintable->outline_data(computed_values) : Optional<CSS::BorderData> {};
}

CSSPixelRect transform_rect_to_viewport(Layout::Node const& node, CSSPixelRect const& rect, AccumulatedVisualContextTree::IncludeVisualViewportTransform include_visual_viewport_transform)
{
    auto const* paintable = node.paintable_ptr();
    return paintable ? paintable->transform_rect_to_viewport(rect, include_visual_viewport_transform) : CSSPixelRect {};
}

Optional<CSSPixelPoint> transform_point_to_local(Layout::Node const& node, CSSPixelPoint position)
{
    auto const* paintable = node.paintable_ptr();
    return paintable ? paintable->transform_point_to_local(position) : Optional<CSSPixelPoint> {};
}

Optional<CSSPixelPoint> transform_point_to_local_for_descendants(Layout::Node const& node, CSSPixelPoint position)
{
    auto const* paintable = node.paintable_ptr();
    return paintable ? paintable->transform_point_to_local_for_descendants(position) : Optional<CSSPixelPoint> {};
}

CSSPixelPoint inverse_transform_point(Layout::Node const& node, CSSPixelPoint position)
{
    auto const* paintable = node.paintable_ptr();
    return paintable ? paintable->inverse_transform_point(position) : CSSPixelPoint {};
}

CSSPixelPoint transform_to_local_coordinates(Layout::Node const& node, CSSPixelPoint position)
{
    auto const* paintable = node.paintable_ptr();
    return paintable ? paintable->transform_to_local_coordinates(position) : CSSPixelPoint {};
}

Optional<String> grid_layout_json(Layout::Node const& node, UniqueNodeID container_node_id)
{
    auto const* paintable = node.paintable_ptr();
    return paintable ? paintable->grid_layout_json(container_node_id) : Optional<String> {};
}

Optional<String> flex_layout_json(Layout::Node const& node, UniqueNodeID container_node_id)
{
    auto const* paintable = node.paintable_ptr();
    return paintable ? paintable->flex_layout_json(container_node_id) : Optional<String> {};
}

Paintable::SelectionStyle selection_style_for_node(Layout::Node const& node, GC::Ptr<DOM::Node const> dom_node)
{
    return Paintable::selection_style_for_node(node, dom_node);
}

void set_needs_repaint(Layout::Node const& node, InvalidateDisplayList should_invalidate_display_list)
{
    if (auto* paintable = const_cast<Paintable*>(node.paintable_ptr()))
        paintable->set_needs_repaint(should_invalidate_display_list);
}

void invalidate_paint_cache(Layout::Node const& node)
{
    if (auto const* paintable = node.paintable_ptr())
        paintable->invalidate_paint_cache();
}

void repaint_after_style_change(Layout::Node const& node, CSS::RequiredInvalidationAfterStyleChange const& invalidation)
{
    if (auto* paintable = const_cast<Paintable*>(node.paintable_ptr()))
        paintable->repaint_after_style_change(invalidation);
}

void invalidate_stacking_context(Layout::Node const& node)
{
    if (auto* paintable = const_cast<Paintable*>(node.paintable_ptr()))
        paintable->invalidate_stacking_context();
}

void clear_overflow_data(Layout::Node const& node)
{
    if (auto* paintable = const_cast<Paintable*>(node.paintable_ptr()))
        paintable->clear_overflow_data();
}

void clear_cached_overflow_data(Layout::Node const& node)
{
    if (auto* paintable = const_cast<Paintable*>(node.paintable_ptr()))
        paintable->clear_cached_overflow_data();
}

void set_sticky_insets(Layout::Node const& node, OwnPtr<StickyInsets> sticky_insets)
{
    if (auto* paintable = const_cast<Paintable*>(node.paintable_ptr()))
        paintable->set_sticky_insets(move(sticky_insets));
}

void inline_piece_border_box_rects(Layout::Node const& node, Vector<CSSPixelRect>& rects)
{
    auto const* paintable = node.paintable_ptr();
    if (!paintable)
        return;
    Layout::RustFFI::layout_arena_inline_paintable_piece_border_box_rects(
        paintable->rust_arena().handle(), paintable->rust_slot(), &rects,
        [](void* context, Layout::RustFFI::FfiCssPixelRect rect) {
            static_cast<Vector<CSSPixelRect>*>(context)->append({
                CSSPixels::from_raw(rect.x),
                CSSPixels::from_raw(rect.y),
                CSSPixels::from_raw(rect.width),
                CSSPixels::from_raw(rect.height),
            });
        });
}

CSSPixelPoint cumulative_scroll_compensation(Layout::Node const& node)
{
    auto const* paintable = node.paintable_ptr();
    if (!paintable)
        return {};
    auto index = paintable->enclosing_scroll_node_index();
    if (!index.value())
        return {};
    return paintable->document().paintable()->cumulative_scroll_offset_for_node(index);
}

}
