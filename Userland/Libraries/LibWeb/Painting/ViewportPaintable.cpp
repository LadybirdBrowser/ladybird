/*
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Range.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::Painting {

JS_DEFINE_ALLOCATOR(ViewportPaintable);

JS::NonnullGCPtr<ViewportPaintable> ViewportPaintable::create(Layout::Viewport const& layout_viewport)
{
    return layout_viewport.heap().allocate_without_realm<ViewportPaintable>(layout_viewport);
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
    for_each_in_subtree([&](Paintable const& paintable) {
        const_cast<Paintable&>(paintable).invalidate_stacking_context();
        auto* parent_context = const_cast<Paintable&>(paintable).enclosing_stacking_context();
        auto establishes_stacking_context = paintable.layout_node().establishes_stacking_context();
        if ((paintable.is_positioned() || establishes_stacking_context) && paintable.computed_values().z_index().value_or(0) == 0)
            parent_context->m_positioned_descendants_with_stack_level_0_and_stacking_contexts.append(paintable);
        if (!paintable.is_positioned() && paintable.is_floating())
            parent_context->m_non_positioned_floating_descendants.append(paintable);
        if (!establishes_stacking_context) {
            VERIFY(!paintable.stacking_context());
            return TraversalDecision::Continue;
        }
        VERIFY(parent_context);
        const_cast<Paintable&>(paintable).set_stacking_context(make<Painting::StackingContext>(const_cast<Paintable&>(paintable), parent_context, index_in_tree_order++));
        return TraversalDecision::Continue;
    });

    stacking_context()->sort();
}

void ViewportPaintable::paint_all_phases(PaintContext& context)
{
    build_stacking_context_tree_if_needed();
    context.display_list_recorder().translate(-context.device_viewport_rect().location().to_type<int>());
    stacking_context()->paint(context);
}

void ViewportPaintable::assign_scroll_frames()
{
    int next_id = 0;
    for_each_in_subtree_of_type<PaintableBox>([&](auto const& paintable_box) {
        if (paintable_box.has_scrollable_overflow()) {
            auto scroll_frame = adopt_ref(*new ScrollFrame());
            scroll_frame->id = next_id++;
            scroll_state.set(paintable_box, move(scroll_frame));
        }
        return TraversalDecision::Continue;
    });

    for_each_in_subtree([&](auto const& paintable) {
        for (auto block = paintable.containing_block(); !block->is_viewport(); block = block->containing_block()) {
            if (auto scroll_frame = scroll_state.get(block); scroll_frame.has_value()) {
                if (paintable.is_paintable_box()) {
                    auto const& paintable_box = static_cast<PaintableBox const&>(paintable);
                    const_cast<PaintableBox&>(paintable_box).set_enclosing_scroll_frame(scroll_frame.value());
                } else if (paintable.is_inline_paintable()) {
                    auto const& inline_paintable = static_cast<InlinePaintable const&>(paintable);
                    const_cast<InlinePaintable&>(inline_paintable).set_enclosing_scroll_frame(scroll_frame.value());
                }
                break;
            }
        }
        return TraversalDecision::Continue;
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
                } else if (paintable.is_inline_paintable()) {
                    auto const& inline_paintable = static_cast<InlinePaintable const&>(paintable);
                    const_cast<InlinePaintable&>(inline_paintable).set_enclosing_clip_frame(clip_frame.value());
                }
                break;
            }
        }
        return TraversalDecision::Continue;
    });
}

void ViewportPaintable::refresh_scroll_state()
{
    if (!m_needs_to_refresh_scroll_state)
        return;
    m_needs_to_refresh_scroll_state = false;

    for (auto& it : scroll_state) {
        auto const& paintable_box = *it.key;
        auto& scroll_frame = *it.value;
        CSSPixelPoint offset;
        for (auto const* block = &paintable_box.layout_box(); !block->is_viewport(); block = block->containing_block()) {
            auto const& block_paintable_box = *block->paintable_box();
            offset.translate_by(block_paintable_box.scroll_offset());
        }
        scroll_frame.offset = -offset;
    }
}

void ViewportPaintable::refresh_clip_state()
{
    if (!m_needs_to_refresh_clip_state)
        return;
    m_needs_to_refresh_clip_state = false;

    for (auto& it : clip_state) {
        auto const& paintable_box = *it.key;
        auto& clip_frame = *it.value;
        auto overflow_x = paintable_box.computed_values().overflow_x();
        auto overflow_y = paintable_box.computed_values().overflow_y();
        // Start from CSS clip property if it exists.
        Optional<CSSPixelRect> clip_rect = paintable_box.get_clip_rect();

        clip_frame.clear_border_radii_clips();
        if (overflow_x != CSS::Overflow::Visible && overflow_y != CSS::Overflow::Visible) {
            auto overflow_clip_rect = paintable_box.compute_absolute_padding_rect_with_css_transform_applied();
            for (auto const* block = &paintable_box.layout_box(); !block->is_viewport(); block = block->containing_block()) {
                auto const& block_paintable_box = *block->paintable_box();
                auto block_overflow_x = block_paintable_box.computed_values().overflow_x();
                auto block_overflow_y = block_paintable_box.computed_values().overflow_y();
                if (block_overflow_x != CSS::Overflow::Visible && block_overflow_y != CSS::Overflow::Visible) {
                    auto rect = block_paintable_box.compute_absolute_padding_rect_with_css_transform_applied();
                    overflow_clip_rect.intersect(rect);
                    auto border_radii_data = block_paintable_box.normalized_border_radii_data(ShrinkRadiiForBorders::Yes);
                    if (border_radii_data.has_any_radius()) {
                        BorderRadiiClip border_radii_clip { .rect = rect, .radii = border_radii_data };
                        clip_frame.add_border_radii_clip(border_radii_clip);
                    }
                }
                if (auto css_clip_property_rect = block->paintable_box()->get_clip_rect(); css_clip_property_rect.has_value())
                    overflow_clip_rect.intersect(css_clip_property_rect.value());
            }
            clip_rect = overflow_clip_rect;
        }

        clip_frame.set_rect(*clip_rect);
    }
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

JS::GCPtr<Selection::Selection> ViewportPaintable::selection() const
{
    return const_cast<DOM::Document&>(document()).get_selection();
}

void ViewportPaintable::recompute_selection_states()
{
    // 1. Start by resetting the selection state of all layout nodes to None.
    for_each_in_inclusive_subtree([&](auto& layout_node) {
        layout_node.set_selection_state(SelectionState::None);
        return TraversalDecision::Continue;
    });

    // 2. If there is no active Selection or selected Range, return.
    auto selection = document().get_selection();
    if (!selection)
        return;
    auto range = selection->range();
    if (!range)
        return;

    auto* start_container = range->start_container();
    auto* end_container = range->end_container();

    // 3. If the selection starts and ends in the same node:
    if (start_container == end_container) {
        // 1. If the selection starts and ends at the same offset, return.
        if (range->start_offset() == range->end_offset()) {
            // NOTE: A zero-length selection should not be visible.
            return;
        }

        // 2. If it's a text node, mark it as StartAndEnd and return.
        if (is<DOM::Text>(*start_container)) {
            if (auto* paintable = start_container->paintable()) {
                paintable->set_selection_state(SelectionState::StartAndEnd);
            }
            return;
        }
    }

    if (start_container == end_container && is<DOM::Text>(*start_container)) {
        if (auto* paintable = start_container->paintable()) {
            paintable->set_selection_state(SelectionState::StartAndEnd);
        }
        return;
    }

    // 4. Mark the selection start node as Start (if text) or Full (if anything else).
    if (auto* paintable = start_container->paintable()) {
        if (is<DOM::Text>(*start_container))
            paintable->set_selection_state(SelectionState::Start);
        else
            paintable->set_selection_state(SelectionState::Full);
    }

    // 5. Mark the selection end node as End (if text) or Full (if anything else).
    if (auto* paintable = end_container->paintable()) {
        if (is<DOM::Text>(*end_container))
            paintable->set_selection_state(SelectionState::End);
        else
            paintable->set_selection_state(SelectionState::Full);
    }

    // 6. Mark the nodes between start node and end node (in tree order) as Full.
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
    visitor.visit(scroll_state);
    visitor.visit(clip_state);
}

}
