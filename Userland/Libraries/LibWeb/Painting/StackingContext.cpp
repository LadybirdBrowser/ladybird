/*
 * Copyright (c) 2020-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Rect.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/ReplacedBox.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/SVGPaintable.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/SVG/SVGMaskElement.h>

namespace Web::Painting {

static void paint_node(Paintable const& paintable, PaintContext& context, PaintPhase phase)
{
    paintable.before_paint(context, phase);
    paintable.paint(context, phase);
    paintable.after_paint(context, phase);
}

StackingContext::StackingContext(Paintable& paintable, StackingContext* parent, size_t index_in_tree_order)
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
        auto a_z_index = a->paintable().computed_values().z_index().value_or(0);
        auto b_z_index = b->paintable().computed_values().z_index().value_or(0);
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

void StackingContext::paint_node_as_stacking_context(Paintable const& paintable, PaintContext& context)
{
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

void StackingContext::paint_descendants(PaintContext& context, Paintable const& paintable, StackingContextPaintPhase phase)
{
    paintable.before_children_paint(context, to_paint_phase(phase));

    paintable.for_each_child([&context, phase](auto& child) {
        auto* stacking_context = child.stacking_context();
        auto const& z_index = child.computed_values().z_index();

        // NOTE: Grid specification https://www.w3.org/TR/css-grid-2/#z-order says that grid items should be treated
        //       the same way as CSS2 defines for inline-blocks:
        //       "For each one of these, treat the element as if it created a new stacking context, but any positioned
        //       descendants and descendants which actually create a new stacking context should be considered part of
        //       the parent stacking context, not this new one."
        auto should_be_treated_as_stacking_context = child.layout_node().is_grid_item() && !z_index.has_value();
        if (should_be_treated_as_stacking_context) {
            // FIXME: This may not be fully correct with respect to the paint phases.
            if (phase == StackingContextPaintPhase::Foreground)
                paint_node_as_stacking_context(child, context);
            return IterationDecision::Continue;
        }

        if (stacking_context && z_index.value_or(0) != 0)
            return IterationDecision::Continue;
        if (child.is_positioned() && z_index.value_or(0) == 0)
            return IterationDecision::Continue;

        if (stacking_context) {
            // FIXME: This may not be fully correct with respect to the paint phases.
            if (phase == StackingContextPaintPhase::Foreground) {
                paint_child(context, *stacking_context);
            }
            // Note: Don't further recurse into descendants as paint_child() will do that.
            return IterationDecision::Continue;
        }

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

    paintable.after_children_paint(context, to_paint_phase(phase));
}

void StackingContext::paint_child(PaintContext& context, StackingContext const& child)
{
    const_cast<StackingContext&>(child).set_last_paint_generation_id(context.paint_generation_id());

    auto parent_paintable = child.paintable().parent();
    if (parent_paintable)
        parent_paintable->before_children_paint(context, PaintPhase::Foreground);

    child.paint(context);

    if (parent_paintable)
        parent_paintable->after_children_paint(context, PaintPhase::Foreground);
}

void StackingContext::paint_internal(PaintContext& context) const
{
    // For a more elaborate description of the algorithm, see CSS 2.1 Appendix E
    // Draw the background and borders for the context root (steps 1, 2)
    paint_node(paintable(), context, PaintPhase::Background);
    paint_node(paintable(), context, PaintPhase::Border);

    // Stacking contexts formed by positioned descendants with negative z-indices (excluding 0) in z-index order
    // (most negative first) then tree order. (step 3)
    // NOTE: This doesn't check if a descendant is positioned as modern CSS allows for alternative methods to establish stacking contexts.
    for (auto* child : m_children) {
        if (child->paintable().computed_values().z_index().has_value() && child->paintable().computed_values().z_index().value() < 0)
            paint_child(context, *child);
    }

    // Draw the background and borders for block-level children (step 4)
    paint_descendants(context, paintable(), StackingContextPaintPhase::BackgroundAndBorders);
    // Draw the non-positioned floats (step 5)
    paint_descendants(context, paintable(), StackingContextPaintPhase::Floats);
    // Draw inline content, replaced content, etc. (steps 6, 7)
    paint_descendants(context, paintable(), StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced);
    paint_node(paintable(), context, PaintPhase::Foreground);
    paint_descendants(context, paintable(), StackingContextPaintPhase::Foreground);

    // Draw positioned descendants with z-index `0` or `auto` in tree order. (step 8)
    // FIXME: There's more to this step that we have yet to understand and implement.
    for (auto const& paintable : m_positioned_descendants_with_stack_level_0_and_stacking_contexts) {
        if (!paintable->is_positioned())
            continue;

        // At this point, `paintable_box` is a positioned descendant with z-index: auto.
        // FIXME: This is basically duplicating logic found elsewhere in this same function. Find a way to make this more elegant.
        auto* parent_paintable = paintable->parent();
        if (parent_paintable)
            parent_paintable->before_children_paint(context, PaintPhase::Foreground);
        if (auto* child = paintable->stacking_context()) {
            paint_child(context, *child);
        } else {
            paint_node_as_stacking_context(paintable, context);
        }
        if (parent_paintable)
            parent_paintable->after_children_paint(context, PaintPhase::Foreground);
    };

    // Stacking contexts formed by positioned descendants with z-indices greater than or equal to 1 in z-index order
    // (smallest first) then tree order. (Step 9)
    // NOTE: This doesn't check if a descendant is positioned as modern CSS allows for alternative methods to establish stacking contexts.
    for (auto* child : m_children) {
        if (child->paintable().computed_values().z_index().has_value() && child->paintable().computed_values().z_index().value() >= 1)
            paint_child(context, *child);
    }

    paint_node(paintable(), context, PaintPhase::Outline);

    if (context.should_paint_overlay()) {
        paint_node(paintable(), context, PaintPhase::Overlay);
        paint_descendants(context, paintable(), StackingContextPaintPhase::FocusAndOverlay);
    }
}

// FIXME: This extracts the affine 2D part of the full transformation matrix.
//  Use the whole matrix when we get better transformation support in LibGfx or use LibGL for drawing the bitmap
Gfx::AffineTransform StackingContext::affine_transform_matrix() const
{
    if (paintable().is_paintable_box())
        return Gfx::extract_2d_affine_transform(paintable_box().transform());
    return Gfx::AffineTransform {};
}

static Gfx::FloatMatrix4x4 matrix_with_scaled_translation(Gfx::FloatMatrix4x4 matrix, float scale)
{
    auto* m = matrix.elements();
    m[0][3] *= scale;
    m[1][3] *= scale;
    m[2][3] *= scale;
    return matrix;
}

void StackingContext::paint(PaintContext& context) const
{
    auto opacity = paintable().computed_values().opacity();
    if (opacity == 0.0f)
        return;

    DisplayListRecorderStateSaver saver(context.display_list_recorder());

    auto to_device_pixels_scale = float(context.device_pixels_per_css_pixel());
    Gfx::IntRect source_paintable_rect;
    if (paintable().is_paintable_box()) {
        source_paintable_rect = context.enclosing_device_rect(paintable_box().absolute_paint_rect()).to_type<int>();
    } else if (paintable().is_inline()) {
        source_paintable_rect = context.enclosing_device_rect(inline_paintable().bounding_rect()).to_type<int>();
    } else {
        VERIFY_NOT_REACHED();
    }

    auto transform_matrix = Gfx::FloatMatrix4x4::identity();
    Gfx::FloatPoint transform_origin;
    if (paintable().is_paintable_box()) {
        transform_matrix = paintable_box().transform();
        transform_origin = paintable_box().transform_origin().to_type<float>();
    }

    DisplayListRecorder::PushStackingContextParams push_stacking_context_params {
        .opacity = opacity,
        .is_fixed_position = paintable().is_fixed_position(),
        .source_paintable_rect = source_paintable_rect,
        .image_rendering = paintable().computed_values().image_rendering(),
        .transform = {
            .origin = transform_origin.scaled(to_device_pixels_scale),
            .matrix = matrix_with_scaled_translation(transform_matrix, to_device_pixels_scale),
        },
    };

    if (paintable().is_paintable_box()) {
        if (auto masking_area = paintable_box().get_masking_area(); masking_area.has_value()) {
            if (masking_area->is_empty())
                return;
            auto mask_bitmap = paintable_box().calculate_mask(context, *masking_area);
            if (mask_bitmap) {
                auto source_paintable_rect = context.enclosing_device_rect(*masking_area).to_type<int>();
                push_stacking_context_params.source_paintable_rect = source_paintable_rect;
                push_stacking_context_params.mask = StackingContextMask {
                    .mask_bitmap = mask_bitmap.release_nonnull(),
                    .mask_kind = *paintable_box().get_mask_type()
                };
            }
        }
    }

    context.display_list_recorder().save();
    if (paintable().is_paintable_box() && paintable_box().scroll_frame_id().has_value())
        context.display_list_recorder().set_scroll_frame_id(*paintable_box().scroll_frame_id());
    context.display_list_recorder().push_stacking_context(push_stacking_context_params);
    paint_internal(context);
    context.display_list_recorder().pop_stacking_context();
    context.display_list_recorder().restore();
}

TraversalDecision StackingContext::hit_test(CSSPixelPoint position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    if (!paintable().is_visible())
        return TraversalDecision::Continue;

    CSSPixelPoint transform_origin { 0, 0 };
    if (paintable().is_paintable_box())
        transform_origin = paintable_box().transform_origin();
    // NOTE: This CSSPixels -> Float -> CSSPixels conversion is because we can't AffineTransform::map() a CSSPixelPoint.
    Gfx::FloatPoint offset_position {
        (position.x() - transform_origin.x()).to_float(),
        (position.y() - transform_origin.y()).to_float()
    };
    auto transformed_position = affine_transform_matrix().inverse().value_or({}).map(offset_position).to_type<CSSPixels>() + transform_origin;

    if (paintable().is_fixed_position()) {
        auto scroll_offset = paintable().document().navigable()->viewport_scroll_offset();
        transformed_position.translate_by(-scroll_offset);
    }

    // NOTE: Hit testing basically happens in reverse painting order.
    // https://www.w3.org/TR/CSS22/visuren.html#z-index

    // 7. the child stacking contexts with positive stack levels (least positive first).
    // NOTE: Hit testing follows reverse painting order, that's why the conditions here are reversed.
    for (ssize_t i = m_children.size() - 1; i >= 0; --i) {
        auto const& child = *m_children[i];
        if (child.paintable().computed_values().z_index().value_or(0) <= 0)
            break;
        if (child.hit_test(transformed_position, type, callback) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    // 6. the child stacking contexts with stack level 0 and the positioned descendants with stack level 0.
    for (auto const& paintable : m_positioned_descendants_with_stack_level_0_and_stacking_contexts.in_reverse()) {
        if (paintable->stacking_context()) {
            if (paintable->stacking_context()->hit_test(transformed_position, type, callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        } else {
            if (paintable->hit_test(transformed_position, type, callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
    }

    // 5. the in-flow, inline-level, non-positioned descendants, including inline tables and inline blocks.
    if (paintable().layout_node().children_are_inline() && is<Layout::BlockContainer>(paintable().layout_node())) {
        for (auto const* child = paintable().last_child(); child; child = child->previous_sibling()) {
            if (child->is_inline() && !child->is_absolutely_positioned() && !child->stacking_context()) {
                if (child->hit_test(transformed_position, type, callback) == TraversalDecision::Break)
                    return TraversalDecision::Break;
            }
        }
    }

    // 4. the non-positioned floats.
    for (auto const& paintable : m_non_positioned_floating_descendants.in_reverse()) {
        if (paintable->hit_test(transformed_position, type, callback) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    // 3. the in-flow, non-inline-level, non-positioned descendants.
    if (!paintable().layout_node().children_are_inline()) {
        for (auto const* child = paintable().last_child(); child; child = child->previous_sibling()) {
            if (!child->is_paintable_box())
                continue;

            auto const& paintable_box = verify_cast<PaintableBox>(*child);
            if (!paintable_box.is_absolutely_positioned() && !paintable_box.is_floating() && !paintable_box.stacking_context()) {
                if (paintable_box.hit_test(transformed_position, type, callback) == TraversalDecision::Break)
                    return TraversalDecision::Break;
            }
        }
    }

    // 2. the child stacking contexts with negative stack levels (most negative first).
    // NOTE: Hit testing follows reverse painting order, that's why the conditions here are reversed.
    for (ssize_t i = m_children.size() - 1; i >= 0; --i) {
        auto const& child = *m_children[i];
        if (child.paintable().computed_values().z_index().value_or(0) >= 0)
            break;
        if (child.hit_test(transformed_position, type, callback) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    // 1. the background and borders of the element forming the stacking context.
    if (paintable().is_paintable_box()) {
        if (paintable_box().absolute_border_box_rect().contains(transformed_position.x(), transformed_position.y())) {
            auto hit_test_result = HitTestResult { .paintable = const_cast<PaintableBox&>(paintable_box()) };
            if (callback(hit_test_result) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
    }

    return TraversalDecision::Continue;
}

void StackingContext::dump(int indent) const
{
    StringBuilder builder;
    for (int i = 0; i < indent; ++i)
        builder.append(' ');
    CSSPixelRect rect;
    if (paintable().is_paintable_box()) {
        rect = paintable_box().absolute_rect();
    } else if (paintable().is_inline_paintable()) {
        rect = inline_paintable().bounding_rect();
    } else {
        VERIFY_NOT_REACHED();
    }
    builder.appendff("SC for {} {} [children: {}] (z-index: ", paintable().layout_node().debug_description(), rect, m_children.size());

    if (paintable().computed_values().z_index().has_value())
        builder.appendff("{}", paintable().computed_values().z_index().value());
    else
        builder.append("auto"sv);
    builder.append(')');

    auto affine_transform = affine_transform_matrix();
    if (!affine_transform.is_identity()) {
        builder.appendff(", transform: {}", affine_transform);
    }
    dbgln("{}", builder.string_view());
    for (auto& child : m_children)
        child->dump(indent + 1);
}

}
