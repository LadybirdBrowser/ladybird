/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Range.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/Blending.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ScrollFrame.h>
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

void ViewportPaintable::reset_for_relayout()
{
    PaintableWithLines::reset_for_relayout();
    m_scroll_state.clear();
    m_scroll_state_snapshot = {};
    m_needs_to_refresh_scroll_state = true;
    m_paintable_boxes_with_auto_content_visibility.clear();
    m_next_accumulated_visual_context_id = 1;
}

void ViewportPaintable::build_stacking_context_tree_if_needed()
{
    if (stacking_context())
        return;
    build_stacking_context_tree();
}

void ViewportPaintable::build_stacking_context_tree()
{
    set_stacking_context(heap().allocate<StackingContext>(*this, nullptr, 0));

    size_t index_in_tree_order = 1;
    for_each_in_subtree_of_type<PaintableBox>([&](auto& paintable_box) {
        paintable_box.invalidate_stacking_context();
        auto* parent_context = paintable_box.enclosing_stacking_context();
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
        paintable_box.set_stacking_context(heap().allocate<StackingContext>(paintable_box, parent_context, index_in_tree_order++));
        return TraversalDecision::Continue;
    });

    stacking_context()->sort();
}

void ViewportPaintable::paint_all_phases(DisplayListRecordingContext& context)
{
    build_stacking_context_tree_if_needed();
    context.display_list_recorder().save_layer();
    stacking_context()->paint(context);
    context.display_list_recorder().restore();
}

void ViewportPaintable::assign_scroll_frames()
{
    for_each_in_inclusive_subtree_of_type<PaintableBox>([&](auto& paintable_box) {
        RefPtr<ScrollFrame> sticky_scroll_frame;
        if (paintable_box.is_sticky_position()) {
            auto parent_scroll_frame = paintable_box.nearest_scroll_frame();
            sticky_scroll_frame = m_scroll_state.create_sticky_frame_for(paintable_box, parent_scroll_frame);

            paintable_box.set_enclosing_scroll_frame(sticky_scroll_frame);
            paintable_box.set_own_scroll_frame(sticky_scroll_frame);
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

    for_each_in_subtree([&](auto& paintable) {
        if (paintable.is_fixed_position() || paintable.is_sticky_position())
            return TraversalDecision::Continue;

        for (auto block = paintable.containing_block(); block; block = block->containing_block()) {
            if (auto scroll_frame = block->own_scroll_frame(); scroll_frame) {
                if (auto* paintable_box = as_if<PaintableBox>(paintable))
                    paintable_box->set_enclosing_scroll_frame(*scroll_frame);

                return TraversalDecision::Continue;
            }
            if (block->is_fixed_position()) {
                return TraversalDecision::Continue;
            }
        }
        VERIFY_NOT_REACHED();
    });
}

static CSSPixelRect effective_css_clip_rect(CSSPixelRect const& css_clip)
{
    if (css_clip.width() < 0 || css_clip.height() < 0)
        return CSSPixelRect { 0, 0, 0, 0 };
    return css_clip;
}

void ViewportPaintable::assign_accumulated_visual_contexts()
{
    m_next_accumulated_visual_context_id = 1;

    auto append_node = [&](RefPtr<AccumulatedVisualContext const> parent, VisualContextData data) {
        return AccumulatedVisualContext::create(allocate_accumulated_visual_context_id(), move(data), parent);
    };

    RefPtr<AccumulatedVisualContext const> viewport_state_for_descendants = nullptr;
    if (own_scroll_frame())
        viewport_state_for_descendants = append_node(nullptr, ScrollData { own_scroll_frame()->id(), false });
    set_accumulated_visual_context(nullptr);
    set_accumulated_visual_context_for_descendants(viewport_state_for_descendants);

    for_each_in_subtree_of_type<PaintableBox>([&](auto& paintable_box) {
        auto* visual_parent = as_if<PaintableBox>(paintable_box.parent());
        if (!visual_parent)
            return TraversalDecision::Continue;

        RefPtr<AccumulatedVisualContext const> inherited_state;

        if (paintable_box.is_fixed_position()) {
            inherited_state = nullptr;
        } else if (paintable_box.is_absolutely_positioned()) {
            // For position: absolute, use containing block's state to correctly escape scroll containers.
            // NOTE: transforms/perspectives can't be in intermediates for abspos because they establish
            //       containing blocks, so no intermediate walk is needed.
            inherited_state = paintable_box.containing_block()->accumulated_visual_context_for_descendants();
        } else {
            // For position: relative/static, use visual parent's state directly.
            // This avoids duplicate transform/perspective allocations that would occur with
            // the containing block + intermediate walk approach.
            inherited_state = visual_parent->accumulated_visual_context_for_descendants();
        }

        // Build this element's own state from inherited state.
        RefPtr<AccumulatedVisualContext const> own_state = inherited_state;

        if (paintable_box.is_sticky_position()) {
            // For sticky elements, use enclosing_scroll_frame which holds the sticky frame.
            // own_scroll_frame may be a different scroll frame if the sticky element also has scrollable overflow.
            if (auto sticky_frame = paintable_box.enclosing_scroll_frame(); sticky_frame && sticky_frame->is_sticky())
                own_state = append_node(own_state, ScrollData { sticky_frame->id(), true });
        }

        auto const& computed_values = paintable_box.computed_values();

        EffectsData effects {
            computed_values.opacity(),
            mix_blend_mode_to_compositing_and_blending_operator(computed_values.mix_blend_mode()),
            paintable_box.filter(),
            paintable_box.backdrop_filter(),
            paintable_box.absolute_border_box_rect(),
            paintable_box.normalized_border_radii_data(),
            computed_values.isolation() == CSS::Isolation::Isolate
        };

        if (effects.needs_layer())
            own_state = append_node(own_state, move(effects));

        if (paintable_box.has_css_transform())
            own_state = append_node(own_state, TransformData { paintable_box.transform(), paintable_box.transform_origin() });

        if (auto css_clip = paintable_box.get_clip_rect(); css_clip.has_value())
            own_state = append_node(own_state, ClipData { effective_css_clip_rect(*css_clip), {} });

        if (auto const& clip_path = computed_values.clip_path(); clip_path.has_value() && clip_path->is_basic_shape()) {
            auto masking_area = paintable_box.absolute_border_box_rect();
            auto reference_box = CSSPixelRect { {}, masking_area.size() };
            auto const& basic_shape = clip_path->basic_shape();
            auto path = basic_shape.to_path(reference_box, paintable_box.layout_node());
            path.offset(masking_area.top_left().template to_type<float>());
            auto fill_rule = basic_shape.basic_shape().visit(
                [](CSS::Polygon const& polygon) { return polygon.fill_rule; },
                [](CSS::Path const& path) { return path.fill_rule; },
                [](auto const&) { return Gfx::WindingRule::Nonzero; });
            own_state = append_node(own_state, ClipPathData { move(path), masking_area, fill_rule });
        }

        paintable_box.set_accumulated_visual_context(own_state);

        // Build state for descendants: own state + perspective + clip + scroll.
        RefPtr<AccumulatedVisualContext const> state_for_descendants = own_state;

        if (auto perspective = paintable_box.perspective_matrix(); perspective.has_value())
            state_for_descendants = append_node(state_for_descendants, PerspectiveData { *perspective });

        auto overflow_x = computed_values.overflow_x();
        auto overflow_y = computed_values.overflow_y();
        auto has_hidden_overflow = overflow_x != CSS::Overflow::Visible || overflow_y != CSS::Overflow::Visible;

        if (has_hidden_overflow || paintable_box.layout_node().has_paint_containment()) {
            bool clip_x = overflow_x != CSS::Overflow::Visible;
            bool clip_y = overflow_y != CSS::Overflow::Visible;

            if (paintable_box.layout_node().has_paint_containment()) {
                clip_x = true;
                clip_y = true;
            }

            if (clip_x || clip_y) {
                auto clip_rect = paintable_box.overflow_clip_edge_rect();
                if (!clip_x) {
                    clip_rect.set_left(0);
                    clip_rect.set_right(CSSPixels::max_integer_value);
                }
                if (!clip_y) {
                    clip_rect.set_top(0);
                    clip_rect.set_bottom(CSSPixels::max_integer_value);
                }
                auto radii = (clip_x && clip_y) ? paintable_box.normalized_border_radii_data(ShrinkRadiiForBorders::Yes) : BorderRadiiData {};
                state_for_descendants = append_node(state_for_descendants, ClipData { clip_rect, radii });
            }
        }

        if (paintable_box.own_scroll_frame() && !paintable_box.is_sticky_position())
            state_for_descendants = append_node(state_for_descendants, ScrollData { paintable_box.own_scroll_frame()->id(), false });

        paintable_box.set_accumulated_visual_context_for_descendants(state_for_descendants);

        return TraversalDecision::Continue;
    });
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

        // For nested sticky elements, the parent sticky's offset is applied via cumulative_offset.
        // We need to adjust all position calculations to account for this, so we work in the
        // coordinate space where the parent sticky is at its current (offset) position.
        CSSPixelPoint parent_sticky_offset;
        if (auto parent = scroll_frame->parent(); parent && parent->is_sticky())
            parent_sticky_offset = parent->cumulative_offset();

        // Min and max offsets are needed to clamp the sticky box's position to stay within bounds of containing block.
        CSSPixels min_y_offset_relative_to_nearest_scrollable_ancestor;
        CSSPixels max_y_offset_relative_to_nearest_scrollable_ancestor;
        CSSPixels min_x_offset_relative_to_nearest_scrollable_ancestor;
        CSSPixels max_x_offset_relative_to_nearest_scrollable_ancestor;
        auto const* containing_block_of_sticky_box = sticky_box.containing_block();
        if (containing_block_of_sticky_box->could_be_scrolled_by_wheel_event()) {
            min_y_offset_relative_to_nearest_scrollable_ancestor = 0;
            max_y_offset_relative_to_nearest_scrollable_ancestor = containing_block_of_sticky_box->scrollable_overflow_rect()->height() - sticky_box.absolute_border_box_rect().height();
            min_x_offset_relative_to_nearest_scrollable_ancestor = 0;
            max_x_offset_relative_to_nearest_scrollable_ancestor = containing_block_of_sticky_box->scrollable_overflow_rect()->width() - sticky_box.absolute_border_box_rect().width();
        } else {
            auto containing_block_rect = containing_block_of_sticky_box->absolute_border_box_rect().translated(-nearest_scrollable_ancestor->absolute_rect().top_left());
            containing_block_rect.translate_by(parent_sticky_offset);
            min_y_offset_relative_to_nearest_scrollable_ancestor = containing_block_rect.top();
            max_y_offset_relative_to_nearest_scrollable_ancestor = containing_block_rect.bottom() - sticky_box.absolute_border_box_rect().height();
            min_x_offset_relative_to_nearest_scrollable_ancestor = containing_block_rect.left();
            max_x_offset_relative_to_nearest_scrollable_ancestor = containing_block_rect.right() - sticky_box.absolute_border_box_rect().width();
        }

        auto sticky_rect = sticky_box.border_box_rect_relative_to_nearest_scrollable_ancestor();
        sticky_rect.translate_by(parent_sticky_offset);

        CSSPixelPoint sticky_offset;
        auto scroll_offset = nearest_scrollable_ancestor->scroll_offset();
        CSSPixelRect const scrollport_rect { scroll_offset, nearest_scrollable_ancestor->absolute_rect().size() };

        if (sticky_insets.top.has_value()) {
            auto top_inset = sticky_insets.top.value();
            if (scrollport_rect.top() > sticky_rect.top() - top_inset) {
                auto desired_y = min(scrollport_rect.top() + top_inset, max_y_offset_relative_to_nearest_scrollable_ancestor);
                sticky_offset.translate_by({ 0, desired_y - sticky_rect.top() });
            }
        }

        if (sticky_insets.left.has_value()) {
            auto left_inset = sticky_insets.left.value();
            if (scrollport_rect.left() > sticky_rect.left() - left_inset) {
                auto desired_x = min(scrollport_rect.left() + left_inset, max_x_offset_relative_to_nearest_scrollable_ancestor);
                sticky_offset.translate_by({ desired_x - sticky_rect.left(), 0 });
            }
        }

        if (sticky_insets.bottom.has_value()) {
            auto bottom_inset = sticky_insets.bottom.value();
            if (scrollport_rect.bottom() < sticky_rect.bottom() + bottom_inset) {
                auto desired_y = max(scrollport_rect.bottom() - sticky_box.absolute_border_box_rect().height() - bottom_inset, min_y_offset_relative_to_nearest_scrollable_ancestor);
                sticky_offset.translate_by({ 0, desired_y - sticky_rect.top() });
            }
        }

        if (sticky_insets.right.has_value()) {
            auto right_inset = sticky_insets.right.value();
            if (scrollport_rect.right() < sticky_rect.right() + right_inset) {
                auto desired_x = max(scrollport_rect.right() - sticky_box.absolute_border_box_rect().width() - right_inset, min_x_offset_relative_to_nearest_scrollable_ancestor);
                sticky_offset.translate_by({ desired_x - sticky_rect.left(), 0 });
            }
        }

        scroll_frame->set_own_offset(sticky_offset);
    });

    m_scroll_state.for_each_scroll_frame([&](auto& scroll_frame) {
        scroll_frame->set_own_offset(-scroll_frame->paintable_box().scroll_offset());
    });

    m_scroll_state_snapshot = m_scroll_state.snapshot();
}

static void resolve_paint_only_properties_in_subtree(Paintable& root)
{
    root.for_each_in_inclusive_subtree([&](auto& paintable) {
        paintable.resolve_paint_properties();
        paintable.set_needs_paint_only_properties_update(false);
        return TraversalDecision::Continue;
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
        if (paintable.needs_paint_only_properties_update()) {
            resolve_paint_only_properties_in_subtree(paintable);
            return TraversalDecision::SkipChildrenAndContinue;
        }
        return TraversalDecision::Continue;
    });
}

GC::Ptr<Selection::Selection> ViewportPaintable::selection() const
{
    return document().get_selection();
}

void ViewportPaintable::recompute_selection_states(DOM::Range& range)
{
    // 1. Start by resetting the selection state of all layout nodes to None.
    for_each_in_inclusive_subtree([&](auto& layout_node) {
        layout_node.set_selection_state(SelectionState::None);
        return TraversalDecision::Continue;
    });

    auto start_container = range.start_container();
    auto end_container = range.end_container();

    // 2. If the selection starts and ends in the same node:
    if (start_container == end_container) {
        // 1. If the selection starts and ends at the same offset, return.
        if (range.start_offset() == range.end_offset()) {
            // NOTE: A zero-length selection should not be visible.
            return;
        }

        // 2. If it's a text node, mark it as StartAndEnd and return.
        if (is<DOM::Text>(*start_container) && !range.start().node->is_inert()) {
            if (auto* paintable = start_container->paintable())
                paintable->set_selection_state(SelectionState::StartAndEnd);
            return;
        }
    }

    // 3. Mark the selection start node as Start (if text) or Full (if anything else).
    if (auto* paintable = start_container->paintable(); paintable && !range.start().node->is_inert()) {
        if (is<DOM::Text>(*start_container))
            paintable->set_selection_state(SelectionState::Start);
        else
            paintable->set_selection_state(SelectionState::Full);
    }

    // 4. Mark the nodes between the start and end of the selection as Full.
    auto* start_at = start_container->child_at_index(range.start_offset());
    // If the start container has no child at that index, we need to start on the node right after the start container.
    if (!start_at) {
        if (auto* last_child = start_container->last_child()) {
            start_at = last_child->next_in_pre_order();
        } else {
            start_at = start_container->next_in_pre_order();
        }
    }

    DOM::Node* stop_at = end_container->child_at_index(range.end_offset());
    // Only stop at the end container if it has no children that may need to be included.
    for (auto* node = start_at; node && (node != stop_at && !(node == end_container && !end_container->has_children())); node = node->next_in_pre_order(end_container)) {
        if (node->is_inert())
            continue;
        if (auto* paintable = node->paintable())
            paintable->set_selection_state(SelectionState::Full);
    }

    // 5. Mark the selection end node as End if it is a text node.
    if (auto* paintable = end_container->paintable(); paintable && !range.end().node->is_inert() && is<DOM::Text>(*end_container)) {
        paintable->set_selection_state(SelectionState::End);
    }
}

bool ViewportPaintable::handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned, int, int)
{
    return false;
}

void ViewportPaintable::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_paintable_boxes_with_auto_content_visibility);
}

}
