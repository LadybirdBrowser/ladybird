/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Range.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(ViewportPaintable);

GC::Ref<ViewportPaintable> ViewportPaintable::create(Layout::Viewport const& layout_viewport)
{
    return layout_viewport.heap().allocate<ViewportPaintable>(layout_viewport);
}

ViewportPaintable::ViewportPaintable(Layout::Viewport const& layout_viewport)
    : PaintableWithLines(layout_viewport)
{
}

ViewportPaintable::~ViewportPaintable() = default;

void ViewportPaintable::build_stacking_context_tree_if_needed()
{
    if (stacking_context())
        return;
    build_stacking_context_tree();
}

void ViewportPaintable::build_stacking_context_tree()
{
    set_stacking_context(make<StackingContext>(*this, nullptr, 0));

    size_t index_in_tree_order = 1;
    for_each_in_subtree_of_type<PaintableBox>([&](auto const& paintable_box) {
        const_cast<PaintableBox&>(paintable_box).invalidate_stacking_context();
        auto* parent_context = const_cast<PaintableBox&>(paintable_box).enclosing_stacking_context();
        auto establishes_stacking_context = paintable_box.layout_node().establishes_stacking_context();
        if ((paintable_box.is_positioned() || establishes_stacking_context) && paintable_box.computed_values().z_index().value_or(0) == 0)
            parent_context->m_positioned_descendants_and_stacking_contexts_with_stack_level_0.append(paintable_box);
        if (!paintable_box.is_positioned() && paintable_box.is_floating())
            parent_context->m_non_positioned_floating_descendants.append(paintable_box);
        if (!establishes_stacking_context) {
            VERIFY(!paintable_box.stacking_context());
            return TraversalDecision::Continue;
        }
        VERIFY(parent_context);
        const_cast<PaintableBox&>(paintable_box).set_stacking_context(make<Painting::StackingContext>(const_cast<PaintableBox&>(paintable_box), parent_context, index_in_tree_order++));
        return TraversalDecision::Continue;
    });

    stacking_context()->sort();
}

void ViewportPaintable::paint_all_phases(PaintContext& context)
{
    build_stacking_context_tree_if_needed();
    stacking_context()->paint(context);
}

void ViewportPaintable::assign_scroll_frames()
{
    for_each_in_inclusive_subtree_of_type<PaintableBox>([&](auto& paintable_box) {
        RefPtr<ScrollFrame> sticky_scroll_frame;
        if (paintable_box.is_sticky_position()) {
            auto const* nearest_scrollable_ancestor = paintable_box.nearest_scrollable_ancestor();
            RefPtr<ScrollFrame const> parent_scroll_frame;
            if (nearest_scrollable_ancestor) {
                parent_scroll_frame = nearest_scrollable_ancestor->nearest_scroll_frame();
            }
            sticky_scroll_frame = m_scroll_state.create_sticky_frame_for(paintable_box, parent_scroll_frame);

            const_cast<PaintableBox&>(paintable_box).set_enclosing_scroll_frame(sticky_scroll_frame);
            const_cast<PaintableBox&>(paintable_box).set_own_scroll_frame(sticky_scroll_frame);
        }

        if (paintable_box.has_scrollable_overflow() || is<ViewportPaintable>(paintable_box)) {
            RefPtr<ScrollFrame const> parent_scroll_frame;
            if (sticky_scroll_frame) {
                parent_scroll_frame = sticky_scroll_frame;
            } else {
                parent_scroll_frame = paintable_box.nearest_scroll_frame();
            }
            auto scroll_frame = m_scroll_state.create_scroll_frame_for(paintable_box, parent_scroll_frame);
            paintable_box.set_own_scroll_frame(scroll_frame);
        }

        return TraversalDecision::Continue;
    });

    for_each_in_subtree([&](auto const& paintable) {
        if (paintable.is_fixed_position()) {
            return TraversalDecision::Continue;
        }
        if (paintable.is_sticky_position()) {
            return TraversalDecision::Continue;
        }
        for (auto block = paintable.containing_block(); block; block = block->containing_block()) {
            if (auto scroll_frame = block->own_scroll_frame(); scroll_frame) {
                if (paintable.is_paintable_box()) {
                    auto const& paintable_box = static_cast<PaintableBox const&>(paintable);
                    const_cast<PaintableBox&>(paintable_box).set_enclosing_scroll_frame(*scroll_frame);
                }
                return TraversalDecision::Continue;
            }
            if (block->is_fixed_position()) {
                return TraversalDecision::Continue;
            }
        }
        VERIFY_NOT_REACHED();
    });
}

void ViewportPaintable::assign_clip_frames()
{
    for_each_in_subtree_of_type<PaintableBox>([&](auto const& paintable_box) {
        auto overflow_x = paintable_box.computed_values().overflow_x();
        auto overflow_y = paintable_box.computed_values().overflow_y();
        auto has_hidden_overflow = overflow_x != CSS::Overflow::Visible && overflow_y != CSS::Overflow::Visible;
        if (has_hidden_overflow || paintable_box.get_clip_rect().has_value()) {
            auto clip_frame = adopt_ref(*new ClipFrame());
            clip_state.set(paintable_box, move(clip_frame));
        }
        return TraversalDecision::Continue;
    });

    for_each_in_subtree([&](auto const& paintable) {
        for (auto block = paintable.containing_block(); !block->is_viewport(); block = block->containing_block()) {
            if (auto clip_frame = clip_state.get(block); clip_frame.has_value()) {
                if (paintable.is_paintable_box()) {
                    auto const& paintable_box = static_cast<PaintableBox const&>(paintable);
                    const_cast<PaintableBox&>(paintable_box).set_enclosing_clip_frame(clip_frame.value());
                }
                break;
            }
            if (block->has_css_transform()) {
                break;
            }
        }
        return TraversalDecision::Continue;
    });

    for (auto& it : clip_state) {
        auto const& paintable_box = *it.key;
        auto& clip_frame = *it.value;
        for (auto const* block = &paintable_box.layout_node_with_style_and_box_metrics(); !block->is_viewport(); block = block->containing_block()) {
            auto const& paintable = block->first_paintable();
            if (!paintable->is_paintable_box()) {
                continue;
            }
            auto const& block_paintable_box = static_cast<PaintableBox const&>(*paintable);
            auto block_overflow_x = block_paintable_box.computed_values().overflow_x();
            auto block_overflow_y = block_paintable_box.computed_values().overflow_y();
            if (block_overflow_x != CSS::Overflow::Visible && block_overflow_y != CSS::Overflow::Visible) {
                auto rect = block_paintable_box.absolute_padding_box_rect();
                clip_frame.add_clip_rect(rect, block_paintable_box.normalized_border_radii_data(ShrinkRadiiForBorders::Yes), block_paintable_box.enclosing_scroll_frame());
            }
            if (auto css_clip_property_rect = block_paintable_box.get_clip_rect(); css_clip_property_rect.has_value()) {
                clip_frame.add_clip_rect(css_clip_property_rect.value(), {}, block_paintable_box.enclosing_scroll_frame());
            }
            if (block->has_css_transform()) {
                break;
            }
        }
    }
}

void ViewportPaintable::refresh_scroll_state()
{
    if (!m_needs_to_refresh_scroll_state)
        return;
    m_needs_to_refresh_scroll_state = false;

    m_scroll_state.for_each_sticky_frame([&](auto& scroll_frame) {
        auto const& sticky_box = scroll_frame->paintable_box();
        auto const& sticky_insets = sticky_box.sticky_insets();

        auto const* nearest_scrollable_ancestor = sticky_box.nearest_scrollable_ancestor();
        if (!nearest_scrollable_ancestor) {
            return;
        }

        // Min and max offsets are needed to clamp the sticky box's position to stay within bounds of containing block.
        CSSPixels min_y_offset_relative_to_nearest_scrollable_ancestor;
        CSSPixels max_y_offset_relative_to_nearest_scrollable_ancestor;
        CSSPixels min_x_offset_relative_to_nearest_scrollable_ancestor;
        CSSPixels max_x_offset_relative_to_nearest_scrollable_ancestor;
        auto const* containing_block_of_sticky_box = sticky_box.containing_block();
        if (containing_block_of_sticky_box->is_scrollable()) {
            min_y_offset_relative_to_nearest_scrollable_ancestor = 0;
            max_y_offset_relative_to_nearest_scrollable_ancestor = containing_block_of_sticky_box->scrollable_overflow_rect()->height() - sticky_box.absolute_border_box_rect().height();
            min_x_offset_relative_to_nearest_scrollable_ancestor = 0;
            max_x_offset_relative_to_nearest_scrollable_ancestor = containing_block_of_sticky_box->scrollable_overflow_rect()->width() - sticky_box.absolute_border_box_rect().width();
        } else {
            auto containing_block_rect_relative_to_nearest_scrollable_ancestor = containing_block_of_sticky_box->absolute_border_box_rect().translated(-nearest_scrollable_ancestor->absolute_rect().top_left());
            min_y_offset_relative_to_nearest_scrollable_ancestor = containing_block_rect_relative_to_nearest_scrollable_ancestor.top();
            max_y_offset_relative_to_nearest_scrollable_ancestor = containing_block_rect_relative_to_nearest_scrollable_ancestor.bottom() - sticky_box.absolute_border_box_rect().height();
            min_x_offset_relative_to_nearest_scrollable_ancestor = containing_block_rect_relative_to_nearest_scrollable_ancestor.left();
            max_x_offset_relative_to_nearest_scrollable_ancestor = containing_block_rect_relative_to_nearest_scrollable_ancestor.right() - sticky_box.absolute_border_box_rect().width();
        }

        auto border_rect_of_sticky_box_relative_to_nearest_scrollable_ancestor = sticky_box.border_box_rect_relative_to_nearest_scrollable_ancestor();

        // By default, the sticky box is shifted by the scroll offset of the nearest scrollable ancestor.
        CSSPixelPoint sticky_offset = -nearest_scrollable_ancestor->scroll_offset();
        CSSPixelRect const scrollport_rect { nearest_scrollable_ancestor->scroll_offset(), nearest_scrollable_ancestor->absolute_rect().size() };

        if (sticky_insets.top.has_value()) {
            auto top_inset = sticky_insets.top.value();
            auto stick_to_top_scroll_offset_threshold = border_rect_of_sticky_box_relative_to_nearest_scrollable_ancestor.top() - top_inset;
            if (scrollport_rect.top() > stick_to_top_scroll_offset_threshold) {
                sticky_offset.translate_by({ 0, -border_rect_of_sticky_box_relative_to_nearest_scrollable_ancestor.top() });
                sticky_offset.translate_by({ 0, min(scrollport_rect.top() + top_inset, max_y_offset_relative_to_nearest_scrollable_ancestor) });
            }
        }

        if (sticky_insets.left.has_value()) {
            auto left_inset = sticky_insets.left.value();
            auto stick_to_left_scroll_offset_threshold = border_rect_of_sticky_box_relative_to_nearest_scrollable_ancestor.left() - left_inset;
            if (scrollport_rect.left() > stick_to_left_scroll_offset_threshold) {
                sticky_offset.translate_by({ -border_rect_of_sticky_box_relative_to_nearest_scrollable_ancestor.left(), 0 });
                sticky_offset.translate_by({ min(scrollport_rect.left() + left_inset, max_x_offset_relative_to_nearest_scrollable_ancestor), 0 });
            }
        }

        if (sticky_insets.bottom.has_value()) {
            auto bottom_inset = sticky_insets.bottom.value();
            auto stick_to_bottom_scroll_offset_threshold = border_rect_of_sticky_box_relative_to_nearest_scrollable_ancestor.bottom() + bottom_inset;
            if (scrollport_rect.bottom() < stick_to_bottom_scroll_offset_threshold) {
                sticky_offset.translate_by({ 0, -border_rect_of_sticky_box_relative_to_nearest_scrollable_ancestor.top() });
                sticky_offset.translate_by({ 0, max(scrollport_rect.bottom() - sticky_box.absolute_border_box_rect().height() - bottom_inset, min_y_offset_relative_to_nearest_scrollable_ancestor) });
            }
        }

        if (sticky_insets.right.has_value()) {
            auto right_inset = sticky_insets.right.value();
            auto stick_to_right_scroll_offset_threshold = border_rect_of_sticky_box_relative_to_nearest_scrollable_ancestor.right() + right_inset;
            if (scrollport_rect.right() < stick_to_right_scroll_offset_threshold) {
                sticky_offset.translate_by({ -border_rect_of_sticky_box_relative_to_nearest_scrollable_ancestor.left(), 0 });
                sticky_offset.translate_by({ max(scrollport_rect.right() - sticky_box.absolute_border_box_rect().width() - right_inset, min_x_offset_relative_to_nearest_scrollable_ancestor), 0 });
            }
        }

        scroll_frame->set_own_offset(sticky_offset);
    });

    m_scroll_state.for_each_scroll_frame([&](auto& scroll_frame) {
        scroll_frame->set_own_offset(-scroll_frame->paintable_box().scroll_offset());
    });
}

void ViewportPaintable::resolve_paint_only_properties()
{
    // Resolves layout-dependent properties not handled during layout and stores them in the paint tree.
    // Properties resolved include:
    // - Border radii
    // - Box shadows
    // - Text shadows
    // - Transforms
    // - Transform origins
    // - Outlines
    for_each_in_inclusive_subtree([&](Paintable& paintable) {
        paintable.resolve_paint_properties();
        return TraversalDecision::Continue;
    });
}

GC::Ptr<Selection::Selection> ViewportPaintable::selection() const
{
    return const_cast<DOM::Document&>(document()).get_selection();
}

void ViewportPaintable::recompute_selection_states(DOM::Range& range)
{
    // 1. Start by resetting the selection state of all layout nodes to None.
    for_each_in_inclusive_subtree([&](auto& layout_node) {
        layout_node.set_selection_state(SelectionState::None);
        return TraversalDecision::Continue;
    });

    auto* start_container = range.start_container();
    auto* end_container = range.end_container();

    // 2. If the selection starts and ends in the same node:
    if (start_container == end_container) {
        // 1. If the selection starts and ends at the same offset, return.
        if (range.start_offset() == range.end_offset()) {
            // NOTE: A zero-length selection should not be visible.
            return;
        }

        // 2. If it's a text node, mark it as StartAndEnd and return.
        if (is<DOM::Text>(*start_container)) {
            if (auto* paintable = start_container->paintable())
                paintable->set_selection_state(SelectionState::StartAndEnd);
            return;
        }
    }

    // 3. Mark the selection start node as Start (if text) or Full (if anything else).
    if (auto* paintable = start_container->paintable()) {
        if (is<DOM::Text>(*start_container))
            paintable->set_selection_state(SelectionState::Start);
        else
            paintable->set_selection_state(SelectionState::Full);
    }

    // 4. Mark the selection end node as End (if text) or Full (if anything else).
    if (auto* paintable = end_container->paintable()) {
        if (is<DOM::Text>(*end_container))
            paintable->set_selection_state(SelectionState::End);
        else
            paintable->set_selection_state(SelectionState::Full);
    }

    // 5. Mark the nodes between start node and end node (in tree order) as Full.
    for (auto* node = start_container->next_in_pre_order(); node && node != end_container; node = node->next_in_pre_order()) {
        if (auto* paintable = node->paintable())
            paintable->set_selection_state(SelectionState::Full);
    }
}

bool ViewportPaintable::handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned, int, int)
{
    return false;
}

void ViewportPaintable::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(clip_state);
}

}
