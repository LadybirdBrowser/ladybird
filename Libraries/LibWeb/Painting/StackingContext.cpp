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
#include <LibCore/Environment.h>
#include <LibGfx/Rect.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/ReplacedBox.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BackgroundPainting.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::Painting {

static bool verify_display_list_cache_enabled()
{
    static bool enabled = Core::Environment::has("LADYBIRD_VERIFY_DISPLAY_LIST_CACHE"sv);
    return enabled;
}

// Commands are compared through their dump() representations — the same canonical form the display list
// dump tests diff — because raw payload bytes contain uninitialized struct padding. Compositor metadata is
// dropped because the scratch context used for the fresh side has no async scrolling state, so the fresh
// recording never emits it. Context indices are not part of the comparison: splice rewrites them.
static Vector<String> dump_commands_for_cache_verification(ReadonlyBytes command_bytes)
{
    Vector<String> dumped_commands;
    DisplayList::for_each_command_header(command_bytes, [&](DisplayListCommandHeader const& header, ReadonlyBytes payload) {
        if (display_list_command_is_compositor_metadata(header.type))
            return;
        StringBuilder builder;
        visit_display_list_command(header.type, payload, [&]<typename Command>(Command const& command) {
            builder.appendff("{} payload_size={}", Command::command_name, header.payload_size);
            command.dump(builder);
        });
        dumped_commands.append(MUST(builder.to_string()));
    });
    return dumped_commands;
}

static void verify_spliced_commands_match_fresh_recording(Paintable const& paintable, DisplayListRecordingContext& context, PaintPhase phase, DisplayListCommandRange spliced_range)
{
    auto& recorder = context.display_list_recorder();
    auto const& visual_context_tree = recorder.visual_context_tree();

    auto scratch_display_list = DisplayList::create(visual_context_tree);
    DisplayListRecorder scratch_recorder(scratch_display_list, visual_context_tree, recorder.resource_storage());
    auto scratch_context = context.clone(scratch_recorder);
    scratch_recorder.set_accumulated_visual_context(recorder.accumulated_visual_context());
    paintable.paint(scratch_context, phase);

    auto spliced_bytes = recorder.display_list().command_bytes().slice(spliced_range.offset, spliced_range.size);
    auto fresh_bytes = scratch_display_list->command_bytes();

    // Commands that embed nested display list resources reference lists freshly minted per recording
    // (e.g. pattern tile backgrounds), so their ids differ from the spliced ones by construction.
    auto& resource_storage = recorder.resource_storage();
    if (!resource_storage.collect_referenced_resources(spliced_bytes).display_lists.is_empty()
        || !resource_storage.collect_referenced_resources(fresh_bytes).display_lists.is_empty())
        return;

    auto spliced_commands = dump_commands_for_cache_verification(spliced_bytes);
    auto fresh_commands = dump_commands_for_cache_verification(fresh_bytes);

    if (spliced_commands == fresh_commands)
        return;

    dbgln("Spliced display list cache mismatch for {} in paint phase {} ({} spliced commands, {} fresh commands)",
        paintable.layout_node().debug_description(), to_underlying(phase), spliced_commands.size(), fresh_commands.size());
    for (auto const& command : spliced_commands)
        dbgln("  spliced: {}", command);
    for (auto const& command : fresh_commands)
        dbgln("  fresh:   {}", command);
    VERIFY_NOT_REACHED();
}

static void paint_node(Paintable const& paintable, DisplayListRecordingContext& context, PaintPhase phase)
{
    TemporaryChange save_nesting_level(context.display_list_recorder().m_save_nesting_level, 0);

    // Text fragments are content of the block container (or of a self-painting inline box).
    // They need the descendants' visual context, not the element's own visual context.
    if (paintable.foreground_paints_descendant_content() && phase == PaintPhase::Foreground)
        context.display_list_recorder().set_accumulated_visual_context(paintable.accumulated_visual_context_for_descendants_index());
    else
        context.display_list_recorder().set_accumulated_visual_context(paintable.accumulated_visual_context_index());

    paintable.record_hit_test_items(context, phase);

    auto& recorder = context.display_list_recorder();
    auto const* cache_source_display_list = context.paint_command_cache_source_display_list();
    // NB: Some commands embed visual context indices in their payloads. Those
    //     indices can change when the visual context tree is rebuilt, so commands
    //     from an incompatible tree must be recorded and cached against the new tree.
    bool const cache_reads_enabled = cache_source_display_list
        && cache_source_display_list->compatible_visual_context_tree_version() == recorder.visual_context_tree().version();
    bool const skip_cache = paintable.fixed_background_visual_context().has_value();
    bool const cache_writes_enabled = context.paint_command_cache_mode() == PaintCommandCacheMode::ReadWrite;
    auto const phase_context_index = recorder.accumulated_visual_context();
    bool const phase_has_empty_effective_clip = recorder.visual_context_tree().has_empty_effective_clip(phase_context_index);

    auto cached_commands = !skip_cache && cache_reads_enabled
        ? paintable.valid_cached_commands(phase, cache_source_display_list->id(), phase_has_empty_effective_clip)
        : Optional<Paintable::CachedCommandRange> {};

    if (cached_commands.has_value()) {
        auto destination_range = recorder.append_cached_command_range(*cache_source_display_list, cached_commands->range, cached_commands->recorded_context_index);
        if (verify_display_list_cache_enabled()) [[unlikely]]
            verify_spliced_commands_match_fresh_recording(paintable, context, phase, destination_range);
        if (cache_writes_enabled)
            paintable.set_cached_commands(phase, recorder.display_list().id(), destination_range, phase_context_index, phase_has_empty_effective_clip);
    } else {
        auto const command_range_start = recorder.display_list().command_byte_size();
        if (phase == PaintPhase::Background)
            paintable.record_async_scrolling_metadata(context);
        paintable.paint(context, phase);
        if (!skip_cache && cache_writes_enabled) {
            auto const command_range_end = recorder.display_list().command_byte_size();
            DisplayListCommandRange command_range {
                static_cast<u32>(command_range_start),
                static_cast<u32>(command_range_end - command_range_start),
            };
            paintable.set_cached_commands(phase, recorder.display_list().id(), command_range, phase_context_index, phase_has_empty_effective_clip);
        }
    }

    context.display_list_recorder().set_accumulated_visual_context(VISUAL_VIEWPORT_NODE_INDEX);

    VERIFY(context.display_list_recorder().m_save_nesting_level == 0);
}

NonnullRefPtr<StackingContext> StackingContext::create(Paintable& paintable, RefPtr<StackingContext> parent, size_t index_in_tree_order)
{
    auto stacking_context = adopt_ref(*new StackingContext(paintable, parent, index_in_tree_order));
    if (parent)
        parent->m_children.append(stacking_context);
    return stacking_context;
}

StackingContext::StackingContext(Paintable& paintable, RefPtr<StackingContext> parent, size_t index_in_tree_order)
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

static bool is_pure_inline_box(Paintable const& paintable)
{
    return paintable.layout_node().is_fragmented_inline()
        && !paintable.is_floating()
        && !paintable.is_positioned();
}

static void paint_inline_level_non_positioned_descendant(DisplayListRecordingContext& context, Paintable const& paintable)
{
    paint_node(paintable, context, PaintPhase::Background);
    paint_node(paintable, context, PaintPhase::Border);
    paint_node(paintable, context, PaintPhase::TableCollapsedBorder);
    // A pure inline paintable paints its own background/border in the inline-level phase. Its block descendants, if
    // any, are painted by the earlier BackgroundAndBorders descent through pure inline boxes. In today's layout trees,
    // this subtree sweep is a no-op for InlineNodes: it can only find inline children, floats, or positioned boxes,
    // all of which are skipped by the BackgroundAndBorders phase.
    if (!is_pure_inline_box(paintable))
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
        paint_svg(context, static_cast<Paintable const&>(paintable), PaintPhase::Foreground);
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

void StackingContext::paint_svg(DisplayListRecordingContext& context, Paintable const& paintable, PaintPhase phase)
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
            paint_svg(context, static_cast<Paintable const&>(child), to_paint_phase(phase));
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
            } else if (is_pure_inline_box(child)) {
                paint_descendants(context, child, phase);
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
    Vector<MaskLayerDisplayList> masks;

    for (auto const& mask_layer : paintable_box().mask_layer_presence(MaskLayerSet::CssAndSvg)) {
        switch (mask_layer.origin) {
        case MaskLayerOrigin::CssMaskLayers: {
            auto visual_context_tree = AccumulatedVisualContextTree::create();
            auto mask_display_list = DisplayList::create(visual_context_tree);
            DisplayListRecorder display_list_recorder(*mask_display_list, visual_context_tree, context.display_list_recorder().resource_storage());
            auto mask_painting_context = context.clone(display_list_recorder);
            auto mask_rect = CSSPixelRect { {}, mask_layer.area.size() };
            auto resolved_mask = resolve_background_layers(mask_layers, paintable_box(), Color::Transparent, CSS::BackgroundBox::BorderBox, mask_rect, {});

            // FIXME: Respect `image-rendering` here.
            paint_background(mask_painting_context, paintable_box(), CSS::ImageRendering::Auto, resolved_mask, {});
            masks.append({ MaskLayerOrigin::CssMaskLayers, { *mask_display_list, move(visual_context_tree) } });
            break;
        }
        case MaskLayerOrigin::SvgMask:
            if (auto mask_display_list = paintable_box().calculate_mask(context, mask_layer.area); mask_display_list.has_value())
                masks.append({ MaskLayerOrigin::SvgMask, mask_display_list.release_value() });
            break;
        case MaskLayerOrigin::SvgClip:
            if (auto clip_display_list = paintable_box().calculate_clip(context, mask_layer.area); clip_display_list.has_value())
                masks.append({ MaskLayerOrigin::SvgClip, clip_display_list.release_value() });
            break;
        }
    }

    register_mask_display_lists(context, paintable_box(), masks);

    auto context_before_children = context.display_list_recorder().accumulated_visual_context();

    paint_internal(context);

    context.display_list_recorder().set_accumulated_visual_context(context_before_children);
}

void StackingContext::dump(StringBuilder& builder, int indent) const
{
    for (int i = 0; i < indent; ++i)
        builder.append(' ');
    CSSPixelRect rect = paintable_box().absolute_rect();
    builder.appendff("SC for {} {} (z-index: ", paintable_box().layout_node().debug_description(), rect);

    if (paintable_box().effective_z_index().has_value())
        builder.appendff("{}", paintable_box().effective_z_index().value());
    else
        builder.append("auto"sv);
    builder.append(')');

    if (paintable_box().has_css_transform())
        builder.append(", has_transform"sv);

    builder.append('\n');
    for (auto const& child : m_children)
        child->dump(builder, indent + 1);
}

}
