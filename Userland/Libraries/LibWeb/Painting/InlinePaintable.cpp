/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/AntiAliasingPainter.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Painting/BackgroundPainting.h>
#include <LibWeb/Painting/InlinePaintable.h>
#include <LibWeb/Painting/TextPaintable.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::Painting {

JS_DEFINE_ALLOCATOR(InlinePaintable);

JS::NonnullGCPtr<InlinePaintable> InlinePaintable::create(Layout::InlineNode const& layout_node)
{
    return layout_node.heap().allocate_without_realm<InlinePaintable>(layout_node);
}

InlinePaintable::InlinePaintable(Layout::InlineNode const& layout_node)
    : Paintable(layout_node)
{
}

Layout::InlineNode const& InlinePaintable::layout_node() const
{
    return static_cast<Layout::InlineNode const&>(Paintable::layout_node());
}

void InlinePaintable::before_paint(PaintContext& context, PaintPhase) const
{
    apply_clip(context);

    if (scroll_frame_id().has_value()) {
        context.display_list_recorder().push_scroll_frame_id(scroll_frame_id().value());
    }
}

void InlinePaintable::after_paint(PaintContext& context, PaintPhase) const
{
    if (scroll_frame_id().has_value()) {
        context.display_list_recorder().pop_scroll_frame_id();
    }

    restore_clip(context);
}

void InlinePaintable::paint(PaintContext& context, PaintPhase phase) const
{
    auto& display_list_recorder = context.display_list_recorder();

    if (phase == PaintPhase::Background) {
        auto containing_block_position_in_absolute_coordinates = containing_block()->absolute_position();

        for_each_fragment([&](auto const& fragment, bool is_first_fragment, bool is_last_fragment) {
            CSSPixelRect absolute_fragment_rect { containing_block_position_in_absolute_coordinates.translated(fragment.offset()), fragment.size() };

            if (is_first_fragment) {
                auto extra_start_width = box_model().padding.left;
                absolute_fragment_rect.translate_by(-extra_start_width, 0);
                absolute_fragment_rect.set_width(absolute_fragment_rect.width() + extra_start_width);
            }

            if (is_last_fragment) {
                auto extra_end_width = box_model().padding.right;
                absolute_fragment_rect.set_width(absolute_fragment_rect.width() + extra_end_width);
            }

            absolute_fragment_rect.translate_by(0, -box_model().padding.top);
            absolute_fragment_rect.set_height(absolute_fragment_rect.height() + box_model().padding.top + box_model().padding.bottom);

            auto const& border_radii_data = fragment.border_radii_data();
            paint_background(context, layout_node(), computed_values().image_rendering(), fragment.resolved_background(), border_radii_data);

            if (!box_shadow_data().is_empty()) {
                auto borders_data = BordersData {
                    .top = computed_values().border_top(),
                    .right = computed_values().border_right(),
                    .bottom = computed_values().border_bottom(),
                    .left = computed_values().border_left(),
                };
                auto absolute_fragment_rect_bordered = absolute_fragment_rect.inflated(
                    borders_data.top.width, borders_data.right.width,
                    borders_data.bottom.width, borders_data.left.width);
                paint_box_shadow(context, absolute_fragment_rect_bordered, absolute_fragment_rect,
                    borders_data, border_radii_data, box_shadow_data());
            }

            return IterationDecision::Continue;
        });
    }

    auto paint_border_or_outline = [&](Optional<BordersData> outline_data = {}, CSSPixels outline_offset = 0) {
        auto borders_data = BordersData {
            .top = computed_values().border_top(),
            .right = computed_values().border_right(),
            .bottom = computed_values().border_bottom(),
            .left = computed_values().border_left(),
        };

        auto containing_block_position_in_absolute_coordinates = containing_block()->absolute_position();

        for_each_fragment([&](auto const& fragment, bool is_first_fragment, bool is_last_fragment) {
            CSSPixelRect absolute_fragment_rect { containing_block_position_in_absolute_coordinates.translated(fragment.offset()), fragment.size() };

            if (is_first_fragment) {
                auto extra_start_width = box_model().padding.left;
                absolute_fragment_rect.translate_by(-extra_start_width, 0);
                absolute_fragment_rect.set_width(absolute_fragment_rect.width() + extra_start_width);
            }

            if (is_last_fragment) {
                auto extra_end_width = box_model().padding.right;
                absolute_fragment_rect.set_width(absolute_fragment_rect.width() + extra_end_width);
            }

            absolute_fragment_rect.translate_by(0, -box_model().padding.top);
            absolute_fragment_rect.set_height(absolute_fragment_rect.height() + box_model().padding.top + box_model().padding.bottom);

            auto borders_rect = absolute_fragment_rect.inflated(borders_data.top.width, borders_data.right.width, borders_data.bottom.width, borders_data.left.width);
            auto border_radii_data = fragment.border_radii_data();

            if (outline_data.has_value()) {
                auto outline_offset_x = outline_offset;
                auto outline_offset_y = outline_offset;
                // "Both the height and the width of the outside of the shape drawn by the outline should not
                // become smaller than twice the computed value of the outline-width property to make sure
                // that an outline can be rendered even with large negative values."
                // https://www.w3.org/TR/css-ui-4/#outline-offset
                // So, if the horizontal outline offset is > half the borders_rect's width then we set it to that.
                // (And the same for y)
                if ((borders_rect.width() / 2) + outline_offset_x < 0)
                    outline_offset_x = -borders_rect.width() / 2;
                if ((borders_rect.height() / 2) + outline_offset_y < 0)
                    outline_offset_y = -borders_rect.height() / 2;

                border_radii_data.inflate(outline_data->top.width + outline_offset_y, outline_data->right.width + outline_offset_x, outline_data->bottom.width + outline_offset_y, outline_data->left.width + outline_offset_x);
                borders_rect.inflate(outline_data->top.width + outline_offset_y, outline_data->right.width + outline_offset_x, outline_data->bottom.width + outline_offset_y, outline_data->left.width + outline_offset_x);
                paint_all_borders(context.display_list_recorder(), context.rounded_device_rect(borders_rect), border_radii_data.as_corners(context), outline_data->to_device_pixels(context));
            } else {
                paint_all_borders(context.display_list_recorder(), context.rounded_device_rect(borders_rect), border_radii_data.as_corners(context), borders_data.to_device_pixels(context));
            }

            return IterationDecision::Continue;
        });
    };

    if (phase == PaintPhase::Border) {
        paint_border_or_outline();
    }

    if (phase == PaintPhase::Outline) {
        auto maybe_outline_data = this->outline_data();
        if (maybe_outline_data.has_value())
            paint_border_or_outline(maybe_outline_data.value(), computed_values().outline_offset().to_px(layout_node()));
    }

    if (phase == PaintPhase::Foreground) {
        for_each_fragment([&](auto const& fragment, bool, bool) {
            if (is<TextPaintable>(fragment.paintable()))
                paint_text_fragment(context, static_cast<TextPaintable const&>(fragment.paintable()), fragment, phase);
        });
    }

    if (phase == PaintPhase::Overlay && layout_node().document().inspected_layout_node() == &layout_node()) {
        // FIXME: This paints a double-thick border between adjacent fragments, where ideally there
        //        would be none. Once we implement non-rectangular outlines for the `outline` CSS
        //        property, we can use that here instead.
        for_each_fragment([&](auto const& fragment, bool, bool) {
            display_list_recorder.draw_rect(context.enclosing_device_rect(fragment.absolute_rect()).template to_type<int>(), Color::Magenta);
            return IterationDecision::Continue;
        });
    }
}

template<typename Callback>
void InlinePaintable::for_each_fragment(Callback callback) const
{
    for (size_t i = 0; i < m_fragments.size(); ++i) {
        auto const& fragment = m_fragments[i];
        callback(fragment, i == 0, i == m_fragments.size() - 1);
    }
}

TraversalDecision InlinePaintable::hit_test(CSSPixelPoint position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    if (clip_rect_for_hit_testing().has_value() && !clip_rect_for_hit_testing().value().contains(position))
        return TraversalDecision::Continue;

    auto position_adjusted_by_scroll_offset = position;
    position_adjusted_by_scroll_offset.translate_by(-cumulative_offset_of_enclosing_scroll_frame());

    for (auto const& fragment : m_fragments) {
        if (fragment.paintable().stacking_context())
            continue;
        auto fragment_absolute_rect = fragment.absolute_rect();
        if (fragment_absolute_rect.contains(position_adjusted_by_scroll_offset)) {
            if (fragment.paintable().hit_test(position, type, callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
            auto hit_test_result = HitTestResult { const_cast<Paintable&>(fragment.paintable()), fragment.text_index_at(position_adjusted_by_scroll_offset.x()) };
            if (callback(hit_test_result) == TraversalDecision::Break)
                return TraversalDecision::Break;
        } else if (type == HitTestType::TextCursor) {
            auto const* common_ancestor_parent = [&]() -> DOM::Node const* {
                auto selection = document().get_selection();
                if (!selection)
                    return nullptr;
                auto range = selection->range();
                if (!range)
                    return nullptr;
                auto common_ancestor = range->common_ancestor_container();
                if (common_ancestor->parent())
                    return common_ancestor->parent();
                return common_ancestor;
            }();

            auto const* fragment_dom_node = fragment.layout_node().dom_node();
            if (common_ancestor_parent && fragment_dom_node && common_ancestor_parent->is_ancestor_of(*fragment_dom_node)) {
                // If we reached this point, the position is not within the fragment. However, the fragment start or end might be
                // the place to place the cursor. To determine the best place, we first find the closest fragment horizontally to
                // the cursor. If we could not find one, then find for the closest vertically above the cursor.
                // If we knew the direction of selection, we would look above if selecting upward.
                if (fragment_absolute_rect.bottom() - 1 <= position_adjusted_by_scroll_offset.y()) { // fully below the fragment
                    HitTestResult hit_test_result {
                        .paintable = const_cast<Paintable&>(fragment.paintable()),
                        .index_in_node = fragment.start() + fragment.length(),
                        .vertical_distance = position_adjusted_by_scroll_offset.y() - fragment_absolute_rect.bottom(),
                    };
                    if (callback(hit_test_result) == TraversalDecision::Break)
                        return TraversalDecision::Break;
                } else if (fragment_absolute_rect.top() <= position_adjusted_by_scroll_offset.y()) { // vertically within the fragment
                    if (position_adjusted_by_scroll_offset.x() < fragment_absolute_rect.left()) {
                        HitTestResult hit_test_result {
                            .paintable = const_cast<Paintable&>(fragment.paintable()),
                            .index_in_node = fragment.start(),
                            .vertical_distance = 0,
                            .horizontal_distance = fragment_absolute_rect.left() - position_adjusted_by_scroll_offset.x(),
                        };
                        if (callback(hit_test_result) == TraversalDecision::Break)
                            return TraversalDecision::Break;
                    } else if (position_adjusted_by_scroll_offset.x() > fragment_absolute_rect.right()) {
                        HitTestResult hit_test_result {
                            .paintable = const_cast<Paintable&>(fragment.paintable()),
                            .index_in_node = fragment.start() + fragment.length(),
                            .vertical_distance = 0,
                            .horizontal_distance = position_adjusted_by_scroll_offset.x() - fragment_absolute_rect.right(),
                        };
                        if (callback(hit_test_result) == TraversalDecision::Break)
                            return TraversalDecision::Break;
                    }
                }
            }
        }
    }

    bool should_exit = false;
    for_each_child([&](Paintable const& child) {
        if (child.stacking_context())
            return IterationDecision::Continue;
        if (child.hit_test(position, type, callback) == TraversalDecision::Break) {
            should_exit = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });

    if (should_exit)
        return TraversalDecision::Break;

    return TraversalDecision::Continue;
}

CSSPixelRect InlinePaintable::bounding_rect() const
{
    CSSPixelRect bounding_rect;
    for_each_fragment([&](auto const& fragment, bool, bool) {
        auto fragment_absolute_rect = fragment.absolute_rect();
        bounding_rect = bounding_rect.united(fragment_absolute_rect);
    });

    if (bounding_rect.is_empty()) {
        // FIXME: This is adhoc, and we should return rect of empty fragment instead.
        auto containing_block_position_in_absolute_coordinates = containing_block()->absolute_position();
        return { containing_block_position_in_absolute_coordinates, { 0, 0 } };
    }
    return bounding_rect;
}

void InlinePaintable::resolve_paint_properties()
{
    auto const& computed_values = this->computed_values();
    auto const& layout_node = this->layout_node();
    auto& fragments = this->fragments();

    // Border radii and background layers
    auto const& top_left_border_radius = computed_values.border_top_left_radius();
    auto const& top_right_border_radius = computed_values.border_top_right_radius();
    auto const& bottom_right_border_radius = computed_values.border_bottom_right_radius();
    auto const& bottom_left_border_radius = computed_values.border_bottom_left_radius();
    auto containing_block_position_in_absolute_coordinates = containing_block()->absolute_position();
    for (size_t i = 0; i < fragments.size(); ++i) {
        auto is_first_fragment = i == 0;
        auto is_last_fragment = i == fragments.size() - 1;
        auto& fragment = fragments[i];
        CSSPixelRect absolute_fragment_rect {
            containing_block_position_in_absolute_coordinates.translated(fragment.offset()),
            fragment.size()
        };
        if (is_first_fragment) {
            auto extra_start_width = box_model().padding.left;
            absolute_fragment_rect.translate_by(-extra_start_width, 0);
            absolute_fragment_rect.set_width(absolute_fragment_rect.width() + extra_start_width);
        }
        if (is_last_fragment) {
            auto extra_end_width = box_model().padding.right;
            absolute_fragment_rect.set_width(absolute_fragment_rect.width() + extra_end_width);
        }
        auto border_radii_data = normalize_border_radii_data(layout_node,
            absolute_fragment_rect, top_left_border_radius,
            top_right_border_radius,
            bottom_right_border_radius,
            bottom_left_border_radius);
        fragment.set_border_radii_data(border_radii_data);

        absolute_fragment_rect.translate_by(0, -box_model().padding.top);
        absolute_fragment_rect.set_height(absolute_fragment_rect.height() + box_model().padding.top + box_model().padding.bottom);

        auto resolved_background = resolve_background_layers(computed_values.background_layers(), layout_node, computed_values.background_color(), absolute_fragment_rect, border_radii_data);
        fragment.set_resolved_background(move(resolved_background));
    }

    auto const& box_shadow_data = computed_values.box_shadow();
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
    set_box_shadow_data(move(resolved_box_shadow_data));

    for (auto const& fragment : fragments) {
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

    // Outlines
    auto outline_width = computed_values.outline_width().to_px(layout_node);
    auto outline_data = borders_data_for_outline(layout_node, computed_values.outline_color(), computed_values.outline_style(), outline_width);
    auto outline_offset = computed_values.outline_offset().to_px(layout_node);
    set_outline_data(outline_data);
    set_outline_offset(outline_offset);

    auto combined_transform = compute_combined_css_transform();
    set_combined_css_transform(combined_transform);
}

}
