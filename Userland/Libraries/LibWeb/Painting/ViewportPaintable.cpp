/*
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Range.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/SVGPaintable.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>
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

static Painting::BorderRadiiData normalize_border_radii_data(Layout::Node const& node, CSSPixelRect const& rect, CSS::BorderRadiusData const& top_left_radius, CSS::BorderRadiusData const& top_right_radius, CSS::BorderRadiusData const& bottom_right_radius, CSS::BorderRadiusData const& bottom_left_radius)
{
    Painting::BorderRadiusData bottom_left_radius_px {};
    Painting::BorderRadiusData bottom_right_radius_px {};
    Painting::BorderRadiusData top_left_radius_px {};
    Painting::BorderRadiusData top_right_radius_px {};

    bottom_left_radius_px.horizontal_radius = bottom_left_radius.horizontal_radius.to_px(node, rect.width());
    bottom_right_radius_px.horizontal_radius = bottom_right_radius.horizontal_radius.to_px(node, rect.width());
    top_left_radius_px.horizontal_radius = top_left_radius.horizontal_radius.to_px(node, rect.width());
    top_right_radius_px.horizontal_radius = top_right_radius.horizontal_radius.to_px(node, rect.width());

    bottom_left_radius_px.vertical_radius = bottom_left_radius.vertical_radius.to_px(node, rect.height());
    bottom_right_radius_px.vertical_radius = bottom_right_radius.vertical_radius.to_px(node, rect.height());
    top_left_radius_px.vertical_radius = top_left_radius.vertical_radius.to_px(node, rect.height());
    top_right_radius_px.vertical_radius = top_right_radius.vertical_radius.to_px(node, rect.height());

    // Scale overlapping curves according to https://www.w3.org/TR/css-backgrounds-3/#corner-overlap
    // Let f = min(Li/Si), where i ∈ {top, right, bottom, left},
    // Si is the sum of the two corresponding radii of the corners on side i,
    // and Ltop = Lbottom = the width of the box, and Lleft = Lright = the height of the box.
    auto l_top = rect.width();
    auto l_bottom = l_top;
    auto l_left = rect.height();
    auto l_right = l_left;
    auto s_top = (top_left_radius_px.horizontal_radius + top_right_radius_px.horizontal_radius);
    auto s_right = (top_right_radius_px.vertical_radius + bottom_right_radius_px.vertical_radius);
    auto s_bottom = (bottom_left_radius_px.horizontal_radius + bottom_right_radius_px.horizontal_radius);
    auto s_left = (top_left_radius_px.vertical_radius + bottom_left_radius_px.vertical_radius);
    CSSPixelFraction f = 1;
    f = (s_top != 0) ? min(f, l_top / s_top) : f;
    f = (s_right != 0) ? min(f, l_right / s_right) : f;
    f = (s_bottom != 0) ? min(f, l_bottom / s_bottom) : f;
    f = (s_left != 0) ? min(f, l_left / s_left) : f;

    // If f < 1, then all corner radii are reduced by multiplying them by f.
    if (f < 1) {
        top_left_radius_px.horizontal_radius *= f;
        top_left_radius_px.vertical_radius *= f;
        top_right_radius_px.horizontal_radius *= f;
        top_right_radius_px.vertical_radius *= f;
        bottom_right_radius_px.horizontal_radius *= f;
        bottom_right_radius_px.vertical_radius *= f;
        bottom_left_radius_px.horizontal_radius *= f;
        bottom_left_radius_px.vertical_radius *= f;
    }

    return Painting::BorderRadiiData { top_left_radius_px, top_right_radius_px, bottom_right_radius_px, bottom_left_radius_px };
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
        auto& layout_node = paintable.layout_node();

        auto const is_inline_paintable = paintable.is_inline_paintable();
        auto const is_paintable_box = paintable.is_paintable_box();
        auto const is_paintable_with_lines = paintable.is_paintable_with_lines();
        auto const& computed_values = layout_node.computed_values();

        // Border radii
        if (is_inline_paintable) {
            auto& inline_paintable = static_cast<Painting::InlinePaintable&>(paintable);
            auto& fragments = inline_paintable.fragments();

            auto const& top_left_border_radius = computed_values.border_top_left_radius();
            auto const& top_right_border_radius = computed_values.border_top_right_radius();
            auto const& bottom_right_border_radius = computed_values.border_bottom_right_radius();
            auto const& bottom_left_border_radius = computed_values.border_bottom_left_radius();

            auto containing_block_position_in_absolute_coordinates = inline_paintable.containing_block()->absolute_position();
            for (size_t i = 0; i < fragments.size(); ++i) {
                auto is_first_fragment = i == 0;
                auto is_last_fragment = i == fragments.size() - 1;
                auto& fragment = fragments[i];
                CSSPixelRect absolute_fragment_rect {
                    containing_block_position_in_absolute_coordinates.translated(fragment.offset()),
                    fragment.size()
                };
                if (is_first_fragment) {
                    auto extra_start_width = inline_paintable.box_model().padding.left;
                    absolute_fragment_rect.translate_by(-extra_start_width, 0);
                    absolute_fragment_rect.set_width(absolute_fragment_rect.width() + extra_start_width);
                }
                if (is_last_fragment) {
                    auto extra_end_width = inline_paintable.box_model().padding.right;
                    absolute_fragment_rect.set_width(absolute_fragment_rect.width() + extra_end_width);
                }
                auto border_radii_data = normalize_border_radii_data(layout_node,
                    absolute_fragment_rect, top_left_border_radius,
                    top_right_border_radius,
                    bottom_right_border_radius,
                    bottom_left_border_radius);
                fragment.set_border_radii_data(border_radii_data);
            }
        }

        // Border radii
        if (is_paintable_box) {
            auto& paintable_box = static_cast<Painting::PaintableBox&>(paintable);

            CSSPixelRect const border_rect { 0, 0, paintable_box.border_box_width(), paintable_box.border_box_height() };
            auto const& border_top_left_radius = computed_values.border_top_left_radius();
            auto const& border_top_right_radius = computed_values.border_top_right_radius();
            auto const& border_bottom_right_radius = computed_values.border_bottom_right_radius();
            auto const& border_bottom_left_radius = computed_values.border_bottom_left_radius();

            auto radii_data = normalize_border_radii_data(layout_node, border_rect, border_top_left_radius,
                border_top_right_radius, border_bottom_right_radius,
                border_bottom_left_radius);
            paintable_box.set_border_radii_data(radii_data);
        }

        // Box shadows
        auto const& box_shadow_data = computed_values.box_shadow();
        if (!box_shadow_data.is_empty()) {
            Vector<Painting::ShadowData> resolved_box_shadow_data;
            resolved_box_shadow_data.ensure_capacity(box_shadow_data.size());
            for (auto const& layer : box_shadow_data) {
                resolved_box_shadow_data.empend(
                    layer.color,
                    layer.offset_x.to_px(layout_node),
                    layer.offset_y.to_px(layout_node),
                    layer.blur_radius.to_px(layout_node),
                    layer.spread_distance.to_px(layout_node),
                    layer.placement == CSS::ShadowPlacement::Outer ? Painting::ShadowPlacement::Outer
                                                                   : Painting::ShadowPlacement::Inner);
            }

            if (is<Painting::PaintableBox>(paintable)) {
                auto& paintable_box = static_cast<Painting::PaintableBox&>(paintable);
                paintable_box.set_box_shadow_data(move(resolved_box_shadow_data));
            } else if (is<Painting::InlinePaintable>(paintable)) {
                auto& inline_paintable = static_cast<Painting::InlinePaintable&>(paintable);
                inline_paintable.set_box_shadow_data(move(resolved_box_shadow_data));
            }
        }

        // Text shadows
        if (is_paintable_with_lines) {
            auto const& paintable_with_lines = static_cast<Painting::PaintableWithLines const&>(paintable);
            for (auto const& fragment : paintable_with_lines.fragments()) {
                auto const& text_shadow = fragment.m_layout_node->computed_values().text_shadow();
                if (!text_shadow.is_empty()) {
                    Vector<Painting::ShadowData> resolved_shadow_data;
                    resolved_shadow_data.ensure_capacity(text_shadow.size());
                    for (auto const& layer : text_shadow) {
                        resolved_shadow_data.empend(
                            layer.color,
                            layer.offset_x.to_px(layout_node),
                            layer.offset_y.to_px(layout_node),
                            layer.blur_radius.to_px(layout_node),
                            layer.spread_distance.to_px(layout_node),
                            Painting::ShadowPlacement::Outer);
                    }
                    const_cast<Painting::PaintableFragment&>(fragment).set_shadows(move(resolved_shadow_data));
                }
            }
        }

        // Transform and transform origin
        if (is_paintable_box) {
            auto& paintable_box = static_cast<Painting::PaintableBox&>(paintable);
            auto const& transformations = paintable_box.computed_values().transformations();
            if (!transformations.is_empty()) {
                auto matrix = Gfx::FloatMatrix4x4::identity();
                for (auto const& transform : transformations)
                    matrix = matrix * transform.to_matrix(paintable_box).release_value();
                paintable_box.set_transform(matrix);
            }

            auto const& transform_origin = paintable_box.computed_values().transform_origin();
            // https://www.w3.org/TR/css-transforms-1/#transform-box
            auto transform_box = paintable_box.computed_values().transform_box();
            // For SVG elements without associated CSS layout box, the used value for content-box is fill-box and for
            // border-box is stroke-box.
            // FIXME: This currently detects any SVG element except the <svg> one. Is that correct?
            //        And is it correct to use `else` below?
            if (is<Painting::SVGPaintable>(paintable_box)) {
                switch (transform_box) {
                case CSS::TransformBox::ContentBox:
                    transform_box = CSS::TransformBox::FillBox;
                    break;
                case CSS::TransformBox::BorderBox:
                    transform_box = CSS::TransformBox::StrokeBox;
                    break;
                default:
                    break;
                }
            }
            // For elements with associated CSS layout box, the used value for fill-box is content-box and for
            // stroke-box and view-box is border-box.
            else {
                switch (transform_box) {
                case CSS::TransformBox::FillBox:
                    transform_box = CSS::TransformBox::ContentBox;
                    break;
                case CSS::TransformBox::StrokeBox:
                case CSS::TransformBox::ViewBox:
                    transform_box = CSS::TransformBox::BorderBox;
                    break;
                default:
                    break;
                }
            }

            CSSPixelRect reference_box = [&]() {
                switch (transform_box) {
                case CSS::TransformBox::ContentBox:
                    // Uses the content box as reference box.
                    // FIXME: The reference box of a table is the border box of its table wrapper box, not its table box.
                    return paintable_box.absolute_rect();
                case CSS::TransformBox::BorderBox:
                    // Uses the border box as reference box.
                    // FIXME: The reference box of a table is the border box of its table wrapper box, not its table box.
                    return paintable_box.absolute_border_box_rect();
                case CSS::TransformBox::FillBox:
                    // Uses the object bounding box as reference box.
                    // FIXME: For now we're using the content rect as an approximation.
                    return paintable_box.absolute_rect();
                case CSS::TransformBox::StrokeBox:
                    // Uses the stroke bounding box as reference box.
                    // FIXME: For now we're using the border rect as an approximation.
                    return paintable_box.absolute_border_box_rect();
                case CSS::TransformBox::ViewBox:
                    // Uses the nearest SVG viewport as reference box.
                    // FIXME: If a viewBox attribute is specified for the SVG viewport creating element:
                    //  - The reference box is positioned at the origin of the coordinate system established by the viewBox attribute.
                    //  - The dimension of the reference box is set to the width and height values of the viewBox attribute.
                    auto* svg_paintable = paintable_box.first_ancestor_of_type<Painting::SVGSVGPaintable>();
                    if (!svg_paintable)
                        return paintable_box.absolute_border_box_rect();
                    return svg_paintable->absolute_rect();
                }
                VERIFY_NOT_REACHED();
            }();
            auto x = reference_box.left() + transform_origin.x.to_px(layout_node, reference_box.width());
            auto y = reference_box.top() + transform_origin.y.to_px(layout_node, reference_box.height());
            paintable_box.set_transform_origin({ x, y });
            paintable_box.set_transform_origin({ x, y });
        }

        // Outlines
        auto outline_width = computed_values.outline_width().to_px(layout_node);
        auto outline_data = borders_data_for_outline(layout_node, computed_values.outline_color(), computed_values.outline_style(), outline_width);
        auto outline_offset = computed_values.outline_offset().to_px(layout_node);
        if (is_paintable_box) {
            auto& paintable_box = static_cast<Painting::PaintableBox&>(paintable);
            paintable_box.set_outline_data(outline_data);
            paintable_box.set_outline_offset(outline_offset);
        } else if (is_inline_paintable) {
            auto& inline_paintable = static_cast<Painting::InlinePaintable&>(paintable);
            inline_paintable.set_outline_data(outline_data);
            inline_paintable.set_outline_offset(outline_offset);
        }

        if (is_paintable_box) {
            auto& paintable_box = static_cast<Painting::PaintableBox&>(paintable);
            auto combined_transform = paintable.compute_combined_css_transform();
            paintable_box.set_combined_css_transform(combined_transform);
        } else if (is_inline_paintable) {
            auto& inline_paintable = static_cast<Painting::InlinePaintable&>(paintable);
            auto combined_transform = paintable.compute_combined_css_transform();
            inline_paintable.set_combined_css_transform(combined_transform);
        }

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
