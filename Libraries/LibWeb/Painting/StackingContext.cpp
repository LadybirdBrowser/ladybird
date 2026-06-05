/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <AK/TemporaryChange.h>
#include <LibGfx/Rect.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/ReplacedBox.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BackgroundPainting.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/SVG/SVGMaskElement.h>

namespace Web::Painting {

static void paint_node(Paintable const& paintable, DisplayListRecordingContext& context, PaintPhase phase)
{
    TemporaryChange save_nesting_level(context.display_list_recorder().m_save_nesting_level, 0);

    RefPtr<PaintableBox const> paintable_box = as_if<PaintableBox>(paintable);

    if (paintable_box) {
        // Text fragments in a PaintableWithLines are content of the block container.
        // They need the descendants' visual context, not the element's own visual context.
        if (is<PaintableWithLines>(paintable) && phase == PaintPhase::Foreground)
            context.display_list_recorder().set_accumulated_visual_context(paintable_box->accumulated_visual_context_for_descendants_index());
        else
            context.display_list_recorder().set_accumulated_visual_context(paintable_box->accumulated_visual_context_index());
    }

    if (paintable_box)
        paintable_box->record_hit_test_items(context, phase);

    bool const skip_cache = !paintable_box || context.should_show_line_box_borders() || paintable_box->fixed_background_visual_context().has_value();
    if (!skip_cache && paintable_box->has_cached_commands(phase)) {
        context.display_list_recorder().replay_cached_commands(paintable_box->cached_commands(phase));
    } else if (!skip_cache) {
        auto capture = context.display_list_recorder().begin_command_capture();
        if (phase == PaintPhase::Background)
            paintable_box->record_async_scrolling_metadata(context);
        paintable.paint(context, phase);
        paintable_box->set_cached_commands(phase, capture.take());
    } else {
        if (paintable_box && phase == PaintPhase::Background)
            paintable_box->record_async_scrolling_metadata(context);
        paintable.paint(context, phase);
    }

    context.display_list_recorder().set_accumulated_visual_context(VISUAL_VIEWPORT_NODE_INDEX);

    VERIFY(context.display_list_recorder().m_save_nesting_level == 0);
}

NonnullRefPtr<StackingContext> StackingContext::create(PaintableBox& paintable, RefPtr<StackingContext> parent, size_t index_in_tree_order)
{
    auto stacking_context = adopt_ref(*new StackingContext(paintable, parent, index_in_tree_order));
    if (parent)
        parent->m_children.append(stacking_context);
    return stacking_context;
}

StackingContext::StackingContext(PaintableBox& paintable, RefPtr<StackingContext> parent, size_t index_in_tree_order)
    : m_paintable(paintable)
    , m_parent(parent)
    , m_index_in_tree_order(index_in_tree_order)
{
    VERIFY(!parent || parent.ptr() != this);
}

void StackingContext::sort()
{
    quick_sort(m_children, [](auto& a, auto& b) {
        auto a_z_index = a->paintable_box().effective_z_index().value_or(0);
        auto b_z_index = b->paintable_box().effective_z_index().value_or(0);
        if (a_z_index == b_z_index)
            return a->m_index_in_tree_order < b->m_index_in_tree_order;
        return a_z_index < b_z_index;
    });

    for (auto& child : m_children)
        child->sort();
}

void StackingContext::set_last_paint_generation_id(u64 generation_id)
{
    if (m_last_paint_generation_id.has_value() && m_last_paint_generation_id.value() >= generation_id) {
        dbgln("FIXME: Painting commands are recorded twice for stacking context: {}", paintable_box().layout_node().debug_description());
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
    default:
        VERIFY_NOT_REACHED();
    }
}

static bool establishes_inline_level_painting_context(Paintable const& paintable)
{
    // CSS 2.2 painting order puts inline-block and inline-table boxes in the inline-level painting step and
    // says to paint each "as if it created a new stacking context", while keeping positioned descendants and
    // actual child stacking contexts in the parent stacking context:
    // https://drafts.csswg.org/css2/#painting-order
    // https://drafts.csswg.org/css2/#elaborate-stacking-contexts
    auto const& layout_node = paintable.layout_node();
    return layout_node.is_inline_block() || layout_node.is_inline_table();
}

static void paint_inline_level_non_positioned_descendant(DisplayListRecordingContext& context, Paintable const& paintable)
{
    paint_node(paintable, context, PaintPhase::Background);
    paint_node(paintable, context, PaintPhase::Border);
    paint_node(paintable, context, PaintPhase::TableCollapsedBorder);
    StackingContext::paint_descendants(context, paintable, StackingContext::StackingContextPaintPhase::BackgroundAndBorders);

    // https://drafts.csswg.org/css2/#elaborate-stacking-contexts
    // "For inline-block and inline-table elements: [...] treat the element as if it created a new stacking context,
    // but any positioned descendants and descendants which actually create a new stacking context should be
    // considered part of the parent stacking context, not this new one."
    if (establishes_inline_level_painting_context(paintable))
        StackingContext::paint_descendants(context, paintable, StackingContext::StackingContextPaintPhase::Floats);
}

void StackingContext::paint_node_as_stacking_context(Paintable const& paintable, DisplayListRecordingContext& context)
{
    if (paintable.is_svg_svg_paintable()) {
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
}

void StackingContext::paint_svg(DisplayListRecordingContext& context, PaintableBox const& paintable, PaintPhase phase)
{
    if (phase != PaintPhase::Foreground)
        return;

    paint_node(paintable, context, PaintPhase::Background);
    paint_node(paintable, context, PaintPhase::Border);
    SVGSVGPaintable::paint_svg_box(context, paintable, phase);
}

void StackingContext::paint_descendants(DisplayListRecordingContext& context, Paintable const& paintable, StackingContextPaintPhase phase)
{
    paintable.for_each_child([&context, phase](auto& child) {
        if (child.has_stacking_context())
            return IterationDecision::Continue;

        auto const& z_index = [&] { return child.computed_values().z_index(); };

        // Positioned descendants at stack level 0 are painted in a separate pass.
        // See `m_positioned_descendants_and_stacking_contexts_with_stack_level_0`.
        if (child.is_positioned() && z_index().value_or(0) == 0)
            return IterationDecision::Continue;

        if (child.is_svg_svg_paintable()) {
            paint_svg(context, static_cast<PaintableBox const&>(child), to_paint_phase(phase));
            return IterationDecision::Continue;
        }

        // NOTE: Flex and grid items should be treated the same way as CSS2 defines for inline-blocks:
        //       - https://drafts.csswg.org/css-flexbox-1/#painting
        //       - https://www.w3.org/TR/css-grid-2/#z-order
        //       "For each one of these, treat the element as if it created a new stacking context, but any positioned
        //       descendants and descendants which actually create a new stacking context should be considered part of
        //       the parent stacking context, not this new one."
        if ((child.layout_node().is_flex_item() || child.layout_node().is_grid_item()) && !z_index().has_value()) {
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

        bool child_is_inline_or_replaced = child.is_inline() || is<Layout::ReplacedBox>(child.layout_node());
        bool child_has_inline_level_painting_context = establishes_inline_level_painting_context(child);
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
            // Atomic inline-level descendants such as inline-blocks and inline tables participate in the parent's
            // inline-level painting step, so their internal floats must not be painted early during the ancestor's
            // float sweep.
            if (!child_has_inline_level_painting_context)
                paint_descendants(context, child, phase);
            break;
        case StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced:
            if (child_is_inline_or_replaced) {
                paint_inline_level_non_positioned_descendant(context, child);
            }
            paint_descendants(context, child, phase);
            break;
        case StackingContextPaintPhase::Foreground:
            paint_node(child, context, PaintPhase::Foreground);
            paint_descendants(context, child, phase);
            paint_node(child, context, PaintPhase::Outline);
            paint_node(child, context, PaintPhase::Overlay);
            break;
        }

        return IterationDecision::Continue;
    });
}

void StackingContext::paint_child(DisplayListRecordingContext& context, StackingContext const& child)
{
    VERIFY(!child.paintable_box().is_svg_paintable());
    const_cast<StackingContext&>(child).set_last_paint_generation_id(context.paint_generation_id());
    child.paint(context);
}

void StackingContext::paint_internal(DisplayListRecordingContext& context) const
{
    VERIFY(!paintable_box().is_svg_paintable());
    if (paintable_box().is_svg_svg_paintable()) {
        auto const& svg_svg_paintable = static_cast<SVGSVGPaintable const&>(paintable_box());
        paint_node(svg_svg_paintable, context, PaintPhase::Background);
        paint_node(svg_svg_paintable, context, PaintPhase::Border);

        SVGSVGPaintable::paint_svg_box(context, svg_svg_paintable, PaintPhase::Foreground);

        paint_node(svg_svg_paintable, context, PaintPhase::Outline);
        if (context.should_paint_overlay()) {
            paint_node(svg_svg_paintable, context, PaintPhase::Overlay);
        }
        return;
    }

    // For a more elaborate description of the algorithm, see CSS 2.1 Appendix E
    // Draw the background and borders for the context root (steps 1, 2)
    paint_node(paintable_box(), context, PaintPhase::Background);
    paint_node(paintable_box(), context, PaintPhase::Border);

    // Stacking contexts formed by positioned descendants with negative z-indices (excluding 0) in z-index order
    // (most negative first) then tree order. (step 3)
    // Here, we treat non-positioned stacking contexts as if they were positioned, because CSS 2.0 spec does not
    // account for new properties like `transform` and `opacity` that can create stacking contexts.
    // https://github.com/w3c/csswg-drafts/issues/2717
    for (auto& child : m_children) {
        if (child->paintable_box().effective_z_index().has_value() && child->paintable_box().effective_z_index().value() < 0)
            paint_child(context, *child);
    }

    // Draw the background and borders for block-level children (step 4)
    paint_descendants(context, paintable_box(), StackingContextPaintPhase::BackgroundAndBorders);
    // Draw the non-positioned floats (step 5)
    if (!m_non_positioned_floating_descendants.is_empty())
        paint_descendants(context, paintable_box(), StackingContextPaintPhase::Floats);
    // Draw inline content, replaced content, etc. (steps 6, 7)
    if (m_contains_inline_or_replaced_descendants)
        paint_descendants(context, paintable_box(), StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced);
    paint_node(paintable_box(), context, PaintPhase::Foreground);
    paint_descendants(context, paintable_box(), StackingContextPaintPhase::Foreground);

    // Draw positioned descendants with z-index `0` or `auto` in tree order. (step 8)
    // Here, we treat non-positioned stacking contexts as if they were positioned, because CSS 2.0 spec does not
    // account for new properties like `transform` and `opacity` that can create stacking contexts.
    // https://github.com/w3c/csswg-drafts/issues/2717
    for (auto const& weak_paintable : m_positioned_descendants_and_stacking_contexts_with_stack_level_0) {
        auto paintable = weak_paintable.strong_ref();
        if (!paintable)
            continue;
        // At this point, `paintable_box` is a positioned descendant with z-index: auto.
        // FIXME: This is basically duplicating logic found elsewhere in this same function. Find a way to make this more elegant.
        if (auto child = paintable->stacking_context()) {
            paint_child(context, *child);
        } else {
            paint_node_as_stacking_context(*paintable, context);
        }
    };

    // Stacking contexts formed by positioned descendants with z-indices greater than or equal to 1 in z-index order
    // (smallest first) then tree order. (Step 9)
    // Here, we treat non-positioned stacking contexts as if they were positioned, because CSS 2.0 spec does not
    // account for new properties like `transform` and `opacity` that can create stacking contexts.
    // https://github.com/w3c/csswg-drafts/issues/2717
    for (auto& child : m_children) {
        if (child->paintable_box().effective_z_index().has_value() && child->paintable_box().effective_z_index().value() >= 1)
            paint_child(context, *child);
    }

    paint_node(paintable_box(), context, PaintPhase::Outline);

    if (context.should_paint_overlay()) {
        paint_node(paintable_box(), context, PaintPhase::Overlay);
    }
}

void StackingContext::paint(DisplayListRecordingContext& context) const
{
    // https://drafts.csswg.org/css-transforms-1/#transform-function-lists
    // If a transform function causes the current transformation matrix of an object to be non-invertible, the object
    // and its content do not get displayed.
    if (paintable_box().has_non_invertible_css_transform())
        return;

    TemporaryChange save_nesting_level(context.display_list_recorder().m_save_nesting_level, 0);
    ScopeGuard verify_save_and_restore_are_balanced([&] {
        VERIFY(context.display_list_recorder().m_save_nesting_level == 0);
    });

    auto const& computed_values = paintable_box().computed_values();
    auto const& mask_layers = computed_values.mask_layers();

    auto effective_context_index = paintable_box().accumulated_visual_context_index();
    context.display_list_recorder().set_accumulated_visual_context(effective_context_index);

    // For elements with SVG filters, emit a transparent FillRect to trigger filter application.
    // This ensures content-generating filters (feFlood, feImage) work even with empty source.
    if (auto const& bounds = paintable_box().filter().svg_filter_bounds; bounds.has_value()) {
        auto device_rect = context.enclosing_device_rect(*bounds).to_type<int>();
        context.display_list_recorder().fill_rect_transparent(device_rect);
    }

    // Collect all masks (CSS mask-image, SVG <mask>, SVG <clipPath>).
    Vector<DisplayListRecorder::MaskInfo> masks;

    if (!mask_layers.is_empty()) {
        auto visual_context_tree = AccumulatedVisualContextTree::create();
        auto mask_display_list = DisplayList::create(visual_context_tree);
        DisplayListRecorder display_list_recorder(*mask_display_list, visual_context_tree, context.display_list_recorder().resource_storage());
        auto mask_painting_context = context.clone(display_list_recorder);
        auto absolute_mask_rect = paintable_box().absolute_border_box_rect();
        auto mask_rect_in_device_pixels = context.enclosing_device_rect(absolute_mask_rect);
        auto mask_rect = CSSPixelRect { {}, absolute_mask_rect.size() };
        auto resolved_mask = resolve_background_layers(mask_layers, paintable_box(), Color::Transparent, CSS::BackgroundBox::BorderBox, mask_rect, {});
        paint_background(mask_painting_context, paintable_box(), CSS::ImageRendering::Auto, resolved_mask, {});
        masks.append({ { *mask_display_list, move(visual_context_tree) }, mask_rect_in_device_pixels.to_type<int>(), Gfx::MaskKind::Alpha });
    }

    if (auto mask_area = paintable_box().get_mask_area(); mask_area.has_value()) {
        if (auto mask_display_list = paintable_box().calculate_mask(context, *mask_area); mask_display_list.has_value()) {
            auto rect = context.enclosing_device_rect(*mask_area).to_type<int>();
            auto kind = paintable_box().get_mask_type().value_or(Gfx::MaskKind::Alpha);
            masks.append({ mask_display_list.release_value(), rect, kind });
        }
    }

    if (auto clip_area = paintable_box().get_clip_area(); clip_area.has_value()) {
        if (auto clip_display_list = paintable_box().calculate_clip(context, *clip_area); clip_display_list.has_value()) {
            auto rect = context.enclosing_device_rect(*clip_area).to_type<int>();
            masks.append({ clip_display_list.release_value(), rect, Gfx::MaskKind::Alpha });
        }
    }

    context.display_list_recorder().begin_masks(masks);

    auto context_before_children = context.display_list_recorder().accumulated_visual_context();

    paint_internal(context);

    context.display_list_recorder().set_accumulated_visual_context(context_before_children);

    context.display_list_recorder().end_masks(masks);
}

void StackingContext::dump(StringBuilder& builder, int indent) const
{
    for (int i = 0; i < indent; ++i)
        builder.append(' ');
    CSSPixelRect rect = paintable_box().absolute_rect();
    builder.appendff("SC for {} {} [children: {}] (z-index: ", paintable_box().layout_node().debug_description(), rect, m_children.size());

    if (paintable_box().effective_z_index().has_value())
        builder.appendff("{}", paintable_box().effective_z_index().value());
    else
        builder.append("auto"sv);
    builder.append(')');

    if (paintable_box().has_css_transform())
        builder.append(", has_transform"sv);

    builder.append('\n');
    for (auto& child : m_children)
        child->dump(builder, indent + 1);
}

}
