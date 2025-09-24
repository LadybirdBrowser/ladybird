/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <AK/TemporaryChange.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Layout/ReplacedBox.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/Blending.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/SVG/SVGMaskElement.h>

namespace Web::Painting {

static void paint_node(Paintable const& paintable, DisplayListRecordingContext& context, PaintPhase phase)
{
    TemporaryChange save_nesting_level(context.display_list_recorder().m_save_nesting_level, 0);

    paintable.before_paint(context, phase);
    paintable.paint(context, phase);
    paintable.after_paint(context, phase);

    VERIFY(context.display_list_recorder().m_save_nesting_level == 0);
}

StackingContext::StackingContext(PaintableBox& paintable, StackingContext* parent, size_t index_in_tree_order)
    : m_paintable(paintable)
    , m_parent(parent)
    , m_index_in_tree_order(index_in_tree_order)
{
    VERIFY(m_parent != this);
    if (m_parent)
        m_parent->m_children.append(this);
}

void StackingContext::sort()
{
    quick_sort(m_children, [](auto& a, auto& b) {
        auto a_z_index = a->paintable_box().computed_values().z_index().value_or(0);
        auto b_z_index = b->paintable_box().computed_values().z_index().value_or(0);
        if (a_z_index == b_z_index)
            return a->m_index_in_tree_order < b->m_index_in_tree_order;
        return a_z_index < b_z_index;
    });

    for (auto* child : m_children)
        child->sort();
}

void StackingContext::set_last_paint_generation_id(u64 generation_id)
{
    if (m_last_paint_generation_id.has_value() && m_last_paint_generation_id.value() >= generation_id) {
        dbgln("FIXME: Painting commands are recorded twice for stacking context: {}", m_paintable->layout_node().debug_description());
    }
    m_last_paint_generation_id = generation_id;
}

static PaintPhase to_paint_phase(StackingContext::StackingContextPaintPhase phase)
{
    // There are not a fully correct mapping since some stacking context phases are combined.
    switch (phase) {
    case StackingContext::StackingContextPaintPhase::Floats:
    case StackingContext::StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced:
    case StackingContext::StackingContextPaintPhase::BackgroundAndBorders:
        return PaintPhase::Background;
    case StackingContext::StackingContextPaintPhase::Foreground:
        return PaintPhase::Foreground;
    case StackingContext::StackingContextPaintPhase::FocusAndOverlay:
        return PaintPhase::Overlay;
    default:
        VERIFY_NOT_REACHED();
    }
}

void StackingContext::paint_node_as_stacking_context(Paintable const& paintable, DisplayListRecordingContext& context)
{
    if (paintable.layout_node().is_svg_svg_box()) {
        paint_svg(context, static_cast<PaintableBox const&>(paintable), PaintPhase::Foreground);
        return;
    }

    paint_node(paintable, context, PaintPhase::Background);
    paint_node(paintable, context, PaintPhase::Border);
    paint_descendants(context, paintable, StackingContextPaintPhase::BackgroundAndBorders);
    paint_descendants(context, paintable, StackingContextPaintPhase::Floats);
    paint_descendants(context, paintable, StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced);
    paint_node(paintable, context, PaintPhase::Foreground);
    paint_descendants(context, paintable, StackingContextPaintPhase::Foreground);
    paint_node(paintable, context, PaintPhase::Outline);
    paint_node(paintable, context, PaintPhase::Overlay);
    paint_descendants(context, paintable, StackingContextPaintPhase::FocusAndOverlay);
}

void StackingContext::paint_svg(DisplayListRecordingContext& context, PaintableBox const& paintable, PaintPhase phase)
{
    if (phase != PaintPhase::Foreground)
        return;

    paint_node(paintable, context, PaintPhase::Background);
    paint_node(paintable, context, PaintPhase::Border);
    paintable.before_paint(context, PaintPhase::Foreground);
    SVGSVGPaintable::paint_svg_box(context, paintable, phase);
    paintable.after_paint(context, PaintPhase::Foreground);
}

void StackingContext::paint_descendants(DisplayListRecordingContext& context, Paintable const& paintable, StackingContextPaintPhase phase)
{
    paintable.for_each_child([&context, phase](auto& child) {
        if (child.has_stacking_context())
            return IterationDecision::Continue;

        if (child.layout_node().is_svg_svg_box()) {
            paint_svg(context, static_cast<PaintableBox const&>(child), to_paint_phase(phase));
            return IterationDecision::Continue;
        }

        // NOTE: Grid specification https://www.w3.org/TR/css-grid-2/#z-order says that grid items should be treated
        //       the same way as CSS2 defines for inline-blocks:
        //       "For each one of these, treat the element as if it created a new stacking context, but any positioned
        //       descendants and descendants which actually create a new stacking context should be considered part of
        //       the parent stacking context, not this new one."
        auto const& z_index = [&] { return child.computed_values().z_index(); };
        if (child.layout_node().is_grid_item() && !z_index().has_value()) {
            // FIXME: This may not be fully correct with respect to the paint phases.
            if (phase == StackingContextPaintPhase::Foreground)
                paint_node_as_stacking_context(child, context);
            return IterationDecision::Continue;
        }

        // https://drafts.csswg.org/css2/#painting-order
        // All non-positioned floating descendants, in tree order. For each one of these, treat the
        // element as if it created a new stacking context, but any positioned descendants and
        // descendants which actually create a new stacking context should be considered part of the
        // parent stacking context, not this new one.
        if (child.is_floating() && !child.is_positioned() && !z_index().has_value()) {
            if (phase == StackingContextPaintPhase::Floats) {
                paint_node_as_stacking_context(child, context);
            }
            return IterationDecision::Continue;
        }

        if (child.is_positioned() && z_index().value_or(0) == 0)
            return IterationDecision::Continue;

        bool child_is_inline_or_replaced = child.is_inline() || is<Layout::ReplacedBox>(child.layout_node());
        switch (phase) {
        case StackingContextPaintPhase::BackgroundAndBorders:
            if (!child_is_inline_or_replaced && !child.is_floating()) {
                paint_node(child, context, PaintPhase::Background);
                paint_node(child, context, PaintPhase::Border);
                paint_descendants(context, child, phase);
                paint_node(child, context, PaintPhase::TableCollapsedBorder);
            }
            break;
        case StackingContextPaintPhase::Floats:
            if (child.is_floating()) {
                paint_node(child, context, PaintPhase::Background);
                paint_node(child, context, PaintPhase::Border);
                paint_descendants(context, child, StackingContextPaintPhase::BackgroundAndBorders);
            }
            paint_descendants(context, child, phase);
            break;
        case StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced:
            if (child_is_inline_or_replaced) {
                paint_node(child, context, PaintPhase::Background);
                paint_node(child, context, PaintPhase::Border);
                paint_node(child, context, PaintPhase::TableCollapsedBorder);
                paint_descendants(context, child, StackingContextPaintPhase::BackgroundAndBorders);
            }
            paint_descendants(context, child, phase);
            break;
        case StackingContextPaintPhase::Foreground:
            paint_node(child, context, PaintPhase::Foreground);
            paint_descendants(context, child, phase);
            break;
        case StackingContextPaintPhase::FocusAndOverlay:
            paint_node(child, context, PaintPhase::Outline);
            paint_node(child, context, PaintPhase::Overlay);
            paint_descendants(context, child, phase);
            break;
        }

        return IterationDecision::Continue;
    });
}

void StackingContext::paint_child(DisplayListRecordingContext& context, StackingContext const& child)
{
    VERIFY(!child.paintable_box().layout_node().is_svg_box());
    const_cast<StackingContext&>(child).set_last_paint_generation_id(context.paint_generation_id());
    child.paint(context);
}

void StackingContext::paint_internal(DisplayListRecordingContext& context) const
{
    VERIFY(!paintable_box().layout_node().is_svg_box());
    if (paintable_box().layout_node().is_svg_svg_box()) {
        auto const& svg_svg_paintable = static_cast<SVGSVGPaintable const&>(paintable_box());
        paint_node(svg_svg_paintable, context, PaintPhase::Background);
        paint_node(svg_svg_paintable, context, PaintPhase::Border);

        svg_svg_paintable.before_paint(context, PaintPhase::Foreground);
        SVGSVGPaintable::paint_descendants(context, svg_svg_paintable, PaintPhase::Foreground);
        svg_svg_paintable.after_paint(context, PaintPhase::Foreground);

        paint_node(svg_svg_paintable, context, PaintPhase::Outline);
        if (context.should_paint_overlay()) {
            paint_node(svg_svg_paintable, context, PaintPhase::Overlay);
            paint_descendants(context, svg_svg_paintable, StackingContextPaintPhase::FocusAndOverlay);
        }
        return;
    }

    context.display_list_recorder().push_scroll_frame_id({});
    ScopeGuard restore_scroll_frame_id([&] {
        context.display_list_recorder().pop_scroll_frame_id();
    });

    // For a more elaborate description of the algorithm, see CSS 2.1 Appendix E
    // Draw the background and borders for the context root (steps 1, 2)
    paint_node(paintable_box(), context, PaintPhase::Background);
    paint_node(paintable_box(), context, PaintPhase::Border);

    // Stacking contexts formed by positioned descendants with negative z-indices (excluding 0) in z-index order
    // (most negative first) then tree order. (step 3)
    // Here, we treat non-positioned stacking contexts as if they were positioned, because CSS 2.0 spec does not
    // account for new properties like `transform` and `opacity` that can create stacking contexts.
    // https://github.com/w3c/csswg-drafts/issues/2717
    for (auto* child : m_children) {
        if (child->paintable_box().computed_values().z_index().has_value() && child->paintable_box().computed_values().z_index().value() < 0)
            paint_child(context, *child);
    }

    // Draw the background and borders for block-level children (step 4)
    paint_descendants(context, paintable_box(), StackingContextPaintPhase::BackgroundAndBorders);
    // Draw the non-positioned floats (step 5)
    paint_descendants(context, paintable_box(), StackingContextPaintPhase::Floats);
    // Draw inline content, replaced content, etc. (steps 6, 7)
    paint_descendants(context, paintable_box(), StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced);
    paint_node(paintable_box(), context, PaintPhase::Foreground);
    paint_descendants(context, paintable_box(), StackingContextPaintPhase::Foreground);

    // Draw positioned descendants with z-index `0` or `auto` in tree order. (step 8)
    // Here, we treat non-positioned stacking contexts as if they were positioned, because CSS 2.0 spec does not
    // account for new properties like `transform` and `opacity` that can create stacking contexts.
    // https://github.com/w3c/csswg-drafts/issues/2717
    for (auto const& paintable : m_positioned_descendants_and_stacking_contexts_with_stack_level_0) {
        // At this point, `paintable_box` is a positioned descendant with z-index: auto.
        // FIXME: This is basically duplicating logic found elsewhere in this same function. Find a way to make this more elegant.
        if (auto* child = paintable->stacking_context()) {
            paint_child(context, *child);
        } else {
            paint_node_as_stacking_context(paintable, context);
        }
    };

    // Stacking contexts formed by positioned descendants with z-indices greater than or equal to 1 in z-index order
    // (smallest first) then tree order. (Step 9)
    // Here, we treat non-positioned stacking contexts as if they were positioned, because CSS 2.0 spec does not
    // account for new properties like `transform` and `opacity` that can create stacking contexts.
    // https://github.com/w3c/csswg-drafts/issues/2717
    for (auto* child : m_children) {
        if (child->paintable_box().computed_values().z_index().has_value() && child->paintable_box().computed_values().z_index().value() >= 1)
            paint_child(context, *child);
    }

    paint_node(paintable_box(), context, PaintPhase::Outline);

    if (context.should_paint_overlay()) {
        paint_node(paintable_box(), context, PaintPhase::Overlay);
        paint_descendants(context, paintable_box(), StackingContextPaintPhase::FocusAndOverlay);
    }
}

// FIXME: This extracts the affine 2D part of the full transformation matrix.
//  Use the whole matrix when we get better transformation support in LibGfx or use LibGL for drawing the bitmap
Gfx::AffineTransform StackingContext::affine_transform_matrix() const
{
    return Gfx::extract_2d_affine_transform(paintable_box().transform());
}

void StackingContext::paint(DisplayListRecordingContext& context) const
{
    auto opacity = paintable_box().computed_values().opacity();
    if (opacity == 0.0f)
        return;

    TemporaryChange save_nesting_level(context.display_list_recorder().m_save_nesting_level, 0);
    ScopeGuard verify_save_and_restore_are_balanced([&] {
        VERIFY(context.display_list_recorder().m_save_nesting_level == 0);
    });

    auto to_device_pixels_scale = float(context.device_pixels_per_css_pixel());
    auto source_paintable_rect = context.enclosing_device_rect(paintable_box().absolute_paint_rect()).to_type<int>();

    auto transform_matrix = paintable_box().transform();
    auto transform_origin = paintable_box().transform_origin().to_type<float>();

    Gfx::CompositingAndBlendingOperator compositing_and_blending_operator = mix_blend_mode_to_compositing_and_blending_operator(paintable_box().computed_values().mix_blend_mode());

    DisplayListRecorder::PushStackingContextParams push_stacking_context_params {
        .opacity = opacity,
        .compositing_and_blending_operator = compositing_and_blending_operator,
        .isolate = paintable_box().computed_values().isolation() == CSS::Isolation::Isolate,
        .transform = StackingContextTransform(transform_origin, transform_matrix, to_device_pixels_scale),
    };

    auto const& computed_values = paintable_box().computed_values();
    if (auto clip_path = computed_values.clip_path(); clip_path.has_value() && clip_path->is_basic_shape()) {
        auto const& masking_area = paintable_box().get_masking_area();
        auto const& basic_shape = computed_values.clip_path()->basic_shape();
        auto path = basic_shape.to_path(*masking_area, paintable_box().layout_node());
        auto device_pixel_scale = context.device_pixels_per_css_pixel();
        push_stacking_context_params.clip_path = path.copy_transformed(Gfx::AffineTransform {}.set_scale(device_pixel_scale, device_pixel_scale).set_translation(source_paintable_rect.location().to_type<float>()));
    }

    if (paintable_box().layout_node().has_paint_containment()) {
        push_stacking_context_params.bounding_rect = context.enclosing_device_rect(paintable_box().overflow_clip_edge_rect());
    }

    if (!transform_matrix.is_identity())
        paintable_box().apply_clip_overflow_rect(context, PaintPhase::Foreground);
    paintable_box().apply_scroll_offset(context);

    auto mask_image = computed_values.mask_image();
    Optional<Gfx::Filter> resolved_filter;
    if (computed_values.filter().has_filters())
        resolved_filter = paintable_box().resolve_filter(computed_values.filter());

    bool needs_to_save_state = mask_image || paintable_box().get_masking_area().has_value();

    if (push_stacking_context_params.has_effect()) {
        context.display_list_recorder().push_stacking_context(push_stacking_context_params);
    } else if (needs_to_save_state) {
        context.display_list_recorder().save();
    }

    if (resolved_filter.has_value())
        context.display_list_recorder().apply_filter(*resolved_filter);

    if (mask_image) {
        auto mask_display_list = DisplayList::create(context.device_pixels_per_css_pixel());
        DisplayListRecorder display_list_recorder(*mask_display_list);
        auto mask_painting_context = context.clone(display_list_recorder);
        auto mask_rect_in_device_pixels = context.enclosing_device_rect(paintable_box().absolute_padding_box_rect());
        mask_image->paint(mask_painting_context, { {}, mask_rect_in_device_pixels.size() }, CSS::ImageRendering::Auto);
        context.display_list_recorder().add_mask(mask_display_list, mask_rect_in_device_pixels.to_type<int>());
    }

    if (auto masking_area = paintable_box().get_masking_area(); masking_area.has_value()) {
        auto mask_bitmap = paintable_box().calculate_mask(context, *masking_area);
        if (mask_bitmap) {
            auto masking_area_rect = context.enclosing_device_rect(*masking_area).to_type<int>();
            context.display_list_recorder().apply_mask_bitmap(masking_area_rect.location(), mask_bitmap.release_nonnull(), *paintable_box().get_mask_type());
        }
    }

    paint_internal(context);

    if (resolved_filter.has_value())
        context.display_list_recorder().restore();

    if (push_stacking_context_params.has_effect()) {
        context.display_list_recorder().pop_stacking_context();
    } else if (needs_to_save_state) {
        context.display_list_recorder().restore();
    }
    paintable_box().reset_scroll_offset(context);
    if (!transform_matrix.is_identity())
        paintable_box().clear_clip_overflow_rect(context, PaintPhase::Foreground);
}

TraversalDecision StackingContext::hit_test(CSSPixelPoint position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    if (paintable_box().computed_values().visibility() != CSS::Visibility::Visible)
        return TraversalDecision::Continue;

    auto const inverse_transform = affine_transform_matrix().inverse().value_or({});
    auto const transform_origin = paintable_box().transform_origin();
    // NOTE: This CSSPixels -> Float -> CSSPixels conversion is because we can't AffineTransform::map() a CSSPixelPoint.
    auto const offset_position = position.translated(-transform_origin).to_type<float>();
    auto const transformed_position = inverse_transform.map(offset_position).to_type<CSSPixels>() + transform_origin;

    // NOTE: Hit testing basically happens in reverse painting order.
    // https://www.w3.org/TR/CSS22/visuren.html#z-index

    // 7. the child stacking contexts with positive stack levels (least positive first).
    // NOTE: Hit testing follows reverse painting order, that's why the conditions here are reversed.
    for (auto const* child : m_children.in_reverse()) {
        if (child->paintable_box().computed_values().z_index().value_or(0) <= 0)
            break;
        if (child->hit_test(transformed_position, type, callback) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    // 6. the child stacking contexts with stack level 0 and the positioned descendants with stack level 0.
    for (auto const& paintable_box : m_positioned_descendants_and_stacking_contexts_with_stack_level_0.in_reverse()) {
        if (paintable_box->stacking_context()) {
            if (paintable_box->stacking_context()->hit_test(transformed_position, type, callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        } else {
            if (paintable_box->hit_test(transformed_position, type, callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
    }

    // 5. the in-flow, inline-level, non-positioned descendants, including inline tables and inline blocks.
    if (paintable_box().layout_node().children_are_inline() && is<Layout::BlockContainer>(paintable_box().layout_node())) {
        for (auto const* paintable = paintable_box().last_child(); paintable; paintable = paintable->previous_sibling()) {
            if (paintable->is_inline() && !paintable->is_absolutely_positioned() && !paintable->has_stacking_context()) {
                if (paintable->hit_test(transformed_position, type, callback) == TraversalDecision::Break)
                    return TraversalDecision::Break;
            }
        }
    }

    // 4. the non-positioned floats.
    for (auto const& paintable_box : m_non_positioned_floating_descendants.in_reverse()) {
        if (paintable_box->hit_test(transformed_position, type, callback) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    // 3. the in-flow, non-inline-level, non-positioned descendants.
    if (!paintable_box().layout_node().children_are_inline()) {
        for (auto const* child = paintable_box().last_child(); child; child = child->previous_sibling()) {
            if (!child->is_paintable_box())
                continue;

            auto const& paintable_box = as<PaintableBox>(*child);
            if (!paintable_box.is_absolutely_positioned() && !paintable_box.is_floating() && !paintable_box.stacking_context()) {
                if (paintable_box.hit_test(transformed_position, type, callback) == TraversalDecision::Break)
                    return TraversalDecision::Break;
            }
        }
    }

    // 2. the child stacking contexts with negative stack levels (most negative first).
    // NOTE: Hit testing follows reverse painting order, that's why the conditions here are reversed.
    for (auto const* child : m_children.in_reverse()) {
        if (child->paintable_box().computed_values().z_index().value_or(0) >= 0)
            break;
        if (child->hit_test(transformed_position, type, callback) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    if (!paintable_box().visible_for_hit_testing())
        return TraversalDecision::Continue;

    auto const enclosing_scroll_offset = paintable_box().cumulative_offset_of_enclosing_scroll_frame();
    auto const raw_position_adjusted_by_scroll_offset = position.translated(-enclosing_scroll_offset);
    // NOTE: This CSSPixels -> Float -> CSSPixels conversion is because we can't AffineTransform::map() a CSSPixelPoint.
    auto const offset_position_adjusted_by_scroll_offset = raw_position_adjusted_by_scroll_offset.translated(-transform_origin).to_type<float>();
    auto const transformed_position_adjusted_by_scroll_offset = inverse_transform.map(offset_position_adjusted_by_scroll_offset).to_type<CSSPixels>() + transform_origin;

    // 1. the background and borders of the element forming the stacking context.
    if (paintable_box().visible_for_hit_testing()
        && paintable_box().absolute_border_box_rect().contains(transformed_position_adjusted_by_scroll_offset)) {
        if (callback({ const_cast<PaintableBox&>(paintable_box()) }) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    return TraversalDecision::Continue;
}

void StackingContext::dump(StringBuilder& builder, int indent) const
{
    for (int i = 0; i < indent; ++i)
        builder.append(' ');
    CSSPixelRect rect = paintable_box().absolute_rect();
    builder.appendff("SC for {} {} [children: {}] (z-index: ", paintable_box().layout_node().debug_description(), rect, m_children.size());

    if (paintable_box().computed_values().z_index().has_value())
        builder.appendff("{}", paintable_box().computed_values().z_index().value());
    else
        builder.append("auto"sv);
    builder.append(')');

    auto affine_transform = affine_transform_matrix();
    if (!affine_transform.is_identity()) {
        builder.appendff(", transform: {}", affine_transform);
    }
    builder.append('\n');
    for (auto& child : m_children)
        child->dump(builder, indent + 1);
}

}
