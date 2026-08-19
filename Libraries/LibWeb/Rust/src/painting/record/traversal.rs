/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{PaintPhase, PaintRecorder};
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{NodeFlag, NodeKind};
use crate::painting::display_list::builder::CommandRange;
use crate::painting::display_list::commands::VisualContextIndex;
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::display_list::recorder::DisplayListRecorder;
use crate::painting::fragment_ownership;
use crate::painting::hit_test::*;
use crate::painting::host::{
    FfiHitTestHostCallbacks, FfiPaintHostCallbacks, FfiRecordingInputs, FfiVisualContextHostCallbacks,
};
use crate::painting::paintable_arena::PaintableArena;
use crate::painting::paintable_data::*;
use crate::painting::record::RecordingOutput;
use crate::painting::record::masks::MaskLayerSet;
use crate::painting::stacking_context::NO_STACKING_CONTEXT;
use std::collections::HashMap;
use std::rc::Rc;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum StackingContextPaintPhase {
    BackgroundAndBorders,
    Floats,
    BackgroundAndBordersForInlineLevelAndReplaced,
    Foreground,
}

fn to_paint_phase(phase: StackingContextPaintPhase) -> PaintPhase {
    // There are not a fully correct mapping since some stacking context phases are combined.
    match phase {
        StackingContextPaintPhase::Floats
        | StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced
        | StackingContextPaintPhase::BackgroundAndBorders => PaintPhase::Background,
        StackingContextPaintPhase::Foreground => PaintPhase::Foreground,
    }
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn record_display_list(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    host: &FfiHitTestHostCallbacks,
    paint_host: &FfiPaintHostCallbacks,
    visual_context_host: &FfiVisualContextHostCallbacks,
    inputs: FfiRecordingInputs,
    hit_test_list_generation: u64,
    command_cache_source: Option<Rc<RecordingOutput>>,
    item_cache_source: Option<Rc<crate::painting::record::cache::HitTestItemCacheSource>>,
) -> RecordingOutput {
    let stacking_contexts = paintables
        .stacking_context_tree
        .as_ref()
        .expect("recording needs a built stacking context tree");
    let empty_effective_clips = paintables
        .visual_context
        .tree
        .as_ref()
        .map(|tree| tree.nodes.iter().map(|node| node.has_empty_effective_clip).collect())
        .unwrap_or_default();
    let visual_context_tree_version = paintables.visual_context.tree_version();
    // NB: Some commands embed visual context indices in their payloads. Those indices can change
    //     when the visual context tree is rebuilt, so commands from an incompatible tree must be
    //     recorded and cached against the new tree.
    let command_cache_source = command_cache_source
        .filter(|source| source.compatible_visual_context_tree_version == visual_context_tree_version);
    let item_cache_source =
        item_cache_source.filter(|source| source.visual_context_tree_version == visual_context_tree_version);
    let mut recorder = PaintRecorder {
        layout_arena,
        paintables,
        stacking_contexts,
        host,
        paint_host,
        inputs,
        recorder: DisplayListRecorder::new(empty_effective_clips),
        converter: DevicePixelConverter::new(inputs.device_pixels_per_css_pixel),
        draw_svg_geometry_for_clip_path: false,
        visual_context_host,
        nested: None,
        nested_tree: None,
        prerecorded: crate::painting::record::masks::PrerecordedNestedDisplayLists::default(),
        command_cache_source,
        item_cache_source,
        display_list_id: inputs.display_list_id,
        hit_test_list_generation,
        viewport: stacking_contexts
            .nodes
            .first()
            .map_or(PaintableSlotId::INVALID, |root| root.paintable),
        has_blocking_wheel_event_listeners: false,
        spliced_capture_count: 0,
        list: HitTestList::default(),
        paintable_facts_cache: HashMap::new(),
        text_node_facts_cache: HashMap::new(),
        wheel_hit_test_target_cache: HashMap::new(),
    };
    let int_rect = |values: [i32; 4]| libgfx_rust::IntRect::new(values[0], values[1], values[2], values[3]);
    if inputs.has_canvas_fill_rect {
        recorder.recorder.fill_rect(
            int_rect(inputs.canvas_fill_rect),
            libgfx_rust::Color(inputs.canvas_color),
        );
    }
    // .. in the case of embedded documents typically rendered over a transparent canvas
    // (such as provided via an HTML iframe element), if the used color scheme of the element
    // and the used color scheme of the embedded document’s root element do not match,
    // then the UA must use an opaque canvas of the Canvas color appropriate to the
    // embedded document’s used color scheme instead of a transparent canvas.
    if inputs.opaque_canvas {
        recorder
            .recorder
            .fill_rect(int_rect(inputs.bitmap_rect), libgfx_rust::Color(inputs.canvas_color));
    }
    recorder.recorder.fill_rect(
        int_rect(inputs.bitmap_rect),
        libgfx_rust::Color(inputs.background_color),
    );
    recorder.prerecord_nested_display_lists();
    if !stacking_contexts.nodes.is_empty() {
        recorder.recorder.save_layer();
        recorder.paint_stacking_context(0);
        recorder.recorder.restore();
    }
    crate::painting::record::paint::inspector_overlay::record_inspector_overlays(&mut recorder);
    let mask_display_lists = recorder.recorder.take_mask_display_lists();
    let mut hit_test_list = recorder.list;
    hit_test_list.generation = hit_test_list_generation;
    RecordingOutput {
        id: inputs.display_list_id,
        compatible_visual_context_tree_version: visual_context_tree_version,
        hit_test_list,
        display_list_bytes: recorder.recorder.into_builder().into_bytes(),
        has_blocking_wheel_event_listeners: recorder.has_blocking_wheel_event_listeners,
        mask_display_lists,
        spliced_capture_count: recorder.spliced_capture_count,
    }
}

impl PaintRecorder<'_> {
    fn z_index(&mut self, paintable: PaintableSlotId) -> Option<i32> {
        crate::painting::style_queries::z_index(self.layout_arena, self.data(paintable).layout_node)
    }

    fn is_fragmented_inline(&self, paintable: PaintableSlotId) -> bool {
        fragment_ownership::node_is_fragmented_inline(self.layout_arena, self.data(paintable).layout_node)
    }

    fn establishes_inline_level_painting_context(&self, paintable: PaintableSlotId) -> bool {
        // CSS 2.2 painting order puts inline-block and inline-table boxes in the inline-level painting step and
        // says to paint each "as if it created a new stacking context", while keeping positioned descendants and
        // actual child stacking contexts in the parent stacking context:
        // https://drafts.csswg.org/css2/#painting-order
        // https://drafts.csswg.org/css2/#elaborate-stacking-contexts
        self.display(paintable).is_some_and(|display| {
            display.is_inline_outside() && (display.is_flow_root_inside() || display.is_table_inside())
        })
    }

    fn is_pure_inline_box(&self, paintable: PaintableSlotId) -> bool {
        let data = self.data(paintable);
        self.is_fragmented_inline(paintable)
            && !data.has_flag(PaintableFlag::Floating)
            && !data.has_flag(PaintableFlag::Positioned)
    }

    fn paint_stacking_context(&mut self, index: u32) {
        let paintable = self.stacking_contexts.nodes[index as usize].paintable;
        // https://drafts.csswg.org/css-transforms-1/#transform-function-lists
        // If a transform function causes the current transformation matrix of an object to be
        // non-invertible, the object and its content do not get displayed.
        if self
            .data(paintable)
            .has_flag(PaintableFlag::HasNonInvertibleCssTransform)
        {
            return;
        }
        let effective_context_index = VisualContextIndex(self.own_context_index(paintable));
        self.recorder.set_accumulated_visual_context(effective_context_index);

        // For elements with SVG filters, emit a transparent FillRect to trigger filter application.
        // This ensures content-generating filters (feFlood, feImage) work even with empty source.
        let facts = self.paintable_facts(paintable);
        if facts.has_svg_filter_bounds {
            let device_rect = self
                .converter
                .enclosing_device_rect(crate::css::css_pixels::CssPixelRect::from(facts.svg_filter_bounds));
            self.recorder.fill_rect_transparent(device_rect);
        }

        self.register_mask_display_lists(paintable, MaskLayerSet::CssAndSvg);

        let context_before_children = self.recorder.accumulated_visual_context();
        self.paint_internal(index);
        self.recorder.set_accumulated_visual_context(context_before_children);
    }

    fn paint_child(&mut self, child: u32) {
        self.paint_stacking_context(child);
    }

    fn paint_internal(&mut self, index: u32) {
        let stacking_contexts = self.stacking_contexts;
        let node = &stacking_contexts.nodes[index as usize];
        let paintable = node.paintable;
        let children = &node.children;
        if self.data(paintable).kind == PaintableKind::SVGSVGPaintable {
            self.paint_node(paintable, PaintPhase::Background);
            self.paint_node(paintable, PaintPhase::Border);
            self.paint_svg_box(paintable, PaintPhase::Foreground);
            // An `<svg>` that establishes a stacking context still has descendants that establish
            // one of their own - a `<foreignObject>` always does - and those are painted by their
            // own context rather than by the SVG walk.
            for child in children {
                self.paint_child(*child);
            }
            self.paint_node(paintable, PaintPhase::Outline);
            if self.inputs.should_paint_overlay {
                self.paint_node(paintable, PaintPhase::Overlay);
            }
            return;
        }

        // For a more elaborate description of the algorithm, see CSS 2.1 Appendix E
        // Draw the background and borders for the context root (steps 1, 2)
        self.paint_node(paintable, PaintPhase::Background);
        self.paint_node(paintable, PaintPhase::Border);

        // Stacking contexts formed by positioned descendants with negative z-indices (excluding 0) in z-index order
        // (most negative first) then tree order. (step 3)
        // Here, we treat non-positioned stacking contexts as if they were positioned, because CSS 2.0 spec does not
        // account for new properties like `transform` and `opacity` that can create stacking contexts.
        // https://github.com/w3c/csswg-drafts/issues/2717
        for child in children {
            let z_index = stacking_contexts.nodes[*child as usize].effective_z_index;
            if z_index.is_some_and(|z| z < 0) {
                self.paint_child(*child);
            }
        }

        // Draw the background and borders for block-level children (step 4)
        self.paint_descendants(paintable, StackingContextPaintPhase::BackgroundAndBorders);
        if self.paintables.side(paintable).collapsed_table_borders.is_some() {
            self.paint_node(paintable, PaintPhase::TableCollapsedBorder);
        }
        // Draw the non-positioned floats (step 5)
        if !self.stacking_contexts.nodes[index as usize]
            .non_positioned_floating_descendants
            .is_empty()
        {
            self.paint_descendants(paintable, StackingContextPaintPhase::Floats);
        }
        // Draw inline content, replaced content, etc. (steps 6, 7)
        if self.stacking_contexts.nodes[index as usize].contains_inline_or_replaced_descendants {
            self.paint_descendants(
                paintable,
                StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced,
            );
        }
        self.paint_node(paintable, PaintPhase::Foreground);
        self.paint_descendants(paintable, StackingContextPaintPhase::Foreground);

        // Draw positioned descendants with z-index `0` or `auto` in tree order. (step 8)
        // Here, we treat non-positioned stacking contexts as if they were positioned, because CSS 2.0 spec does not
        // account for new properties like `transform` and `opacity` that can create stacking contexts.
        // https://github.com/w3c/csswg-drafts/issues/2717
        for &descendant in
            &stacking_contexts.nodes[index as usize].positioned_descendants_and_stacking_contexts_with_stack_level_0
        {
            if !self.paintables.is_live(descendant) {
                continue;
            }
            let child_context = self.data(descendant).stacking_context;
            // At this point, `paintable` is a positioned descendant with z-index: auto.
            // FIXME: This is basically duplicating logic found elsewhere in this same function. Find a way to make this more elegant.
            if child_context != NO_STACKING_CONTEXT {
                self.paint_child(child_context);
            } else {
                self.paint_node_as_stacking_context(descendant);
            }
        }

        // Stacking contexts formed by positioned descendants with z-indices greater than or equal
        // to 1 in z-index order (smallest first) then tree order. (Step 9)
        // Here, we treat non-positioned stacking contexts as if they were positioned, because CSS 2.0 spec does not
        // account for new properties like `transform` and `opacity` that can create stacking contexts.
        // https://github.com/w3c/csswg-drafts/issues/2717
        for child in children {
            let z_index = stacking_contexts.nodes[*child as usize].effective_z_index;
            if z_index.is_some_and(|z| z >= 1) {
                self.paint_child(*child);
            }
        }

        self.paint_node(paintable, PaintPhase::Outline);
        if self.inputs.should_paint_overlay {
            self.paint_node(paintable, PaintPhase::Overlay);
        }
    }

    fn paint_subtree_backgrounds_and_borders(&mut self, paintable: PaintableSlotId) {
        self.paint_node(paintable, PaintPhase::Background);
        self.paint_node(paintable, PaintPhase::Border);
        // A pure inline paintable paints its own background/border in the inline-level phase. Its block descendants, if
        // any, are painted by the earlier BackgroundAndBorders descent through pure inline boxes. In today's layout
        // trees, this subtree sweep is a no-op for InlineNodes: it can only find inline children, floats, or positioned
        // boxes, all of which are skipped by the BackgroundAndBorders phase.
        if !self.is_pure_inline_box(paintable) {
            self.paint_descendants(paintable, StackingContextPaintPhase::BackgroundAndBorders);
        }
        if self.paintables.side(paintable).collapsed_table_borders.is_some() {
            self.paint_node(paintable, PaintPhase::TableCollapsedBorder);
        }
    }

    fn paint_inline_level_non_positioned_descendant(&mut self, paintable: PaintableSlotId) {
        self.paint_subtree_backgrounds_and_borders(paintable);
        // https://drafts.csswg.org/css2/#elaborate-stacking-contexts
        // "For inline-block and inline-table elements: [...] treat the element as if it created a new stacking context,
        // but any positioned descendants and descendants which actually create a new stacking context should be
        // considered part of the parent stacking context, not this new one."
        if self.establishes_inline_level_painting_context(paintable) {
            self.paint_descendants(paintable, StackingContextPaintPhase::Floats);
        }
    }

    fn paint_node_as_stacking_context(&mut self, paintable: PaintableSlotId) {
        if self.data(paintable).kind == PaintableKind::SVGSVGPaintable {
            self.paint_svg(paintable, PaintPhase::Foreground);
            return;
        }
        self.paint_subtree_backgrounds_and_borders(paintable);
        self.paint_descendants(paintable, StackingContextPaintPhase::Floats);
        self.paint_descendants(
            paintable,
            StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced,
        );
        self.paint_node(paintable, PaintPhase::Foreground);
        self.paint_descendants(paintable, StackingContextPaintPhase::Foreground);
        self.paint_node(paintable, PaintPhase::Outline);
        self.paint_node(paintable, PaintPhase::Overlay);
    }

    pub(crate) fn paint_svg(&mut self, paintable: PaintableSlotId, phase: PaintPhase) {
        if phase != PaintPhase::Foreground {
            return;
        }
        self.paint_node(paintable, PaintPhase::Background);
        self.paint_node(paintable, PaintPhase::Border);
        self.paint_svg_box(paintable, phase);
    }

    fn paint_descendants(&mut self, paintable: PaintableSlotId, phase: StackingContextPaintPhase) {
        let paintables = self.paintables;
        let mut next_child = paintables.first_child(paintable);
        while let Some(child) = next_child {
            next_child = paintables.next_sibling(child);
            let this = &mut *self;
            if this.has_stacking_context(child) {
                continue;
            }
            let child_data = this.data(child);
            let positioned = child_data.has_flag(PaintableFlag::Positioned);
            let floating = child_data.has_flag(PaintableFlag::Floating);
            let inline = child_data.has_flag(PaintableFlag::Inline);
            let child_kind = child_data.kind;

            // Positioned descendants at stack level 0 are painted in a separate pass.
            if positioned && this.z_index(child).unwrap_or(0) == 0 {
                continue;
            }

            if child_kind == PaintableKind::SVGSVGPaintable {
                this.paint_svg(child, to_paint_phase(phase));
                continue;
            }

            let is_item = this.layout_flags(child) & (NodeFlag::IsFlexItem as u32 | NodeFlag::IsGridItem as u32) != 0;
            // NOTE: Flex and grid items should be treated the same way as CSS2 defines for inline-blocks:
            //       - https://drafts.csswg.org/css-flexbox-1/#painting
            //       - https://www.w3.org/TR/css-grid-2/#z-order
            //       "For each one of these, treat the element as if it created a new stacking context, but any positioned
            //       descendants and descendants which actually create a new stacking context should be considered part of
            //       the parent stacking context, not this new one."
            if is_item && this.z_index(child).is_none() {
                // FIXME: This may not be fully correct with respect to the paint phases.
                if phase == StackingContextPaintPhase::Foreground {
                    this.paint_node_as_stacking_context(child);
                }
                continue;
            }

            // All non-positioned floating descendants, in tree order.
            if floating && !positioned && this.z_index(child).is_none() {
                if phase == StackingContextPaintPhase::Floats {
                    this.paint_node_as_stacking_context(child);
                }
                continue;
            }

            let child_is_inline_or_replaced = inline || this.is_replaced_box(child);
            let child_has_inline_level_painting_context = this.establishes_inline_level_painting_context(child);
            match phase {
                StackingContextPaintPhase::BackgroundAndBorders => {
                    if !child_is_inline_or_replaced && !floating {
                        this.paint_subtree_backgrounds_and_borders(child);
                    } else if this.is_pure_inline_box(child) {
                        this.paint_descendants(child, phase);
                    }
                }
                StackingContextPaintPhase::Floats => {
                    if floating {
                        this.paint_subtree_backgrounds_and_borders(child);
                    }
                    // Atomic inline-level descendants participate in the parent's inline-level
                    // painting step, so their internal floats are not painted early.
                    if !child_has_inline_level_painting_context {
                        this.paint_descendants(child, phase);
                    }
                }
                StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced => {
                    if child_is_inline_or_replaced {
                        this.paint_inline_level_non_positioned_descendant(child);
                    }
                    this.paint_descendants(child, phase);
                }
                StackingContextPaintPhase::Foreground => {
                    this.paint_node(child, PaintPhase::Foreground);
                    this.paint_descendants(child, phase);
                    this.paint_node(child, PaintPhase::Outline);
                    this.paint_node(child, PaintPhase::Overlay);
                }
            }
        }
    }

    fn paint_svg_box(&mut self, svg_box: PaintableSlotId, phase: PaintPhase) {
        let context_index = VisualContextIndex(self.own_context_index(svg_box));
        self.recorder.set_accumulated_visual_context(context_index);

        // For elements with SVG filters, emit a transparent FillRect to trigger filter application.
        // This ensures content-generating filters (feFlood, feImage) work even with empty source.
        let facts = self.paintable_facts(svg_box);
        if facts.has_svg_filter_bounds {
            let device_rect = self
                .converter
                .enclosing_device_rect(crate::css::css_pixels::CssPixelRect::from(facts.svg_filter_bounds));
            self.recorder.fill_rect_transparent(device_rect);
        }

        if self.register_mask_display_lists(svg_box, MaskLayerSet::SvgOnly) {
            return;
        }
        self.record_hit_test_items(svg_box, phase);
        if self.layout_kind(svg_box) == Some(NodeKind::SVGForeignObjectBox) {
            self.record_foreign_object_descendant_hit_test_items(svg_box);
        }
        let kind = self.data(svg_box).kind;
        if kind != PaintableKind::SVGSVGPaintable
            && !crate::painting::paintable_geometry::is_svg_paintable(kind)
            && self
                .layout_kind(svg_box)
                .is_some_and(crate::layout::kind_is_replaced_box)
        {
            crate::painting::record::paint::paint(self, svg_box, PaintPhase::Background);
        }
        crate::painting::record::paint::paint(self, svg_box, PaintPhase::Foreground);
        self.svg_paint_descendants(svg_box, phase);
    }

    fn svg_paint_descendants(&mut self, paintable: PaintableSlotId, phase: PaintPhase) {
        if phase != PaintPhase::Foreground {
            return;
        }
        let paintables = self.paintables;
        let mut next_child = paintables.first_child(paintable);
        while let Some(child) = next_child {
            next_child = paintables.next_sibling(child);
            // A child that establishes a stacking context is painted by that context.
            if self.has_stacking_context(child) {
                continue;
            }
            self.paint_svg_box(child, phase);
        }
    }

    fn for_descendants_context_index(&self, paintable: PaintableSlotId) -> usize {
        if let Some(nested) = &self.nested
            && let Some((_, for_descendants)) = nested.assignments.paintable_indices.get(&paintable.index)
        {
            return *for_descendants;
        }
        self.data(paintable).accumulated_visual_context_for_descendants_index
    }

    fn context_index_for_phase(&self, paintable: PaintableSlotId, phase: PaintPhase) -> VisualContextIndex {
        let data = self.data(paintable);
        // Text fragments are content of the block container (or of a self-painting inline box).
        // They need the descendants' visual context, not the element's own visual context.
        let foreground_paints_descendant_content = data.kind.has_lines() || data.kind == PaintableKind::InlinePaintable;
        if foreground_paints_descendant_content && phase == PaintPhase::Foreground {
            VisualContextIndex(self.for_descendants_context_index(paintable))
        } else {
            VisualContextIndex(self.own_context_index(paintable))
        }
    }

    fn paint_node(&mut self, paintable: PaintableSlotId, phase: PaintPhase) {
        let saved_nesting_level = std::mem::replace(&mut self.recorder.save_nesting_level, 0);
        let context_index = self.context_index_for_phase(paintable, phase);
        self.recorder.set_accumulated_visual_context(context_index);

        // Hit-test items are only ever recorded in the Background, Foreground, and Overlay phases.
        let phase_can_record_hit_test_items = matches!(
            phase,
            PaintPhase::Background | PaintPhase::Foreground | PaintPhase::Overlay
        );
        let is_nested = self.nested.is_some();
        let data = self.data(paintable);
        let skip_cache = data.has_fixed_background_visual_context
            || (data.kind.foreground_is_never_cached() && phase == PaintPhase::Foreground);
        let cache_writes_enabled = self.inputs.paint_command_cache_read_write && !is_nested;

        if phase_can_record_hit_test_items && !is_nested {
            let own_index = self.own_context_index(paintable);
            let for_descendants_index = self.for_descendants_context_index(paintable);
            let cached_items = if skip_cache {
                None
            } else {
                self.valid_cached_hit_test_items(paintable, phase, own_index, for_descendants_index)
            };
            if let Some((source, start, count)) = cached_items {
                // Copies a validated range of items recorded by one (paintable, phase) from the retained previous
                // list into this one. Items are copied, never inspected: source ranges belonging to relaid-out
                // paintables may hold dangling fragment pointers, but only ranges whose owners kept a valid cache
                // entry (and therefore were not relaid out) are ever passed here.
                let destination_start = self.list.items.len();
                for index in start..start + count {
                    self.list.append(source[index].clone());
                }
                if cache_writes_enabled {
                    self.set_cached_hit_test_items(
                        paintable,
                        phase,
                        destination_start,
                        count,
                        own_index,
                        for_descendants_index,
                    );
                }
            } else {
                let items_before = self.list.items.len();
                self.record_hit_test_items(paintable, phase);
                if !skip_cache && cache_writes_enabled {
                    let items_after = self.list.items.len();
                    self.set_cached_hit_test_items(
                        paintable,
                        phase,
                        items_before,
                        items_after - items_before,
                        own_index,
                        for_descendants_index,
                    );
                }
            }
        }

        // SVG subtrees are recorded outside per-paintable captures, so path-bearing items are never spliced.
        let phase_context_index = self.recorder.accumulated_visual_context();
        let phase_has_empty_effective_clip = self.recorder.has_empty_effective_clip(phase_context_index);
        let cached_commands = if skip_cache || is_nested {
            None
        } else {
            self.valid_cached_commands(paintable, phase, phase_has_empty_effective_clip)
        };
        if let Some((source, cached)) = cached_commands {
            let destination_range = self.recorder.append_cached_command_range(
                &source.display_list_bytes,
                cached.range,
                VisualContextIndex(cached.recorded_context_index),
            );
            if cache_writes_enabled {
                self.set_cached_commands(
                    paintable,
                    phase,
                    destination_range,
                    phase_context_index.0,
                    phase_has_empty_effective_clip,
                );
            }
            self.spliced_capture_count += 1;
        } else {
            let command_range_start = self.recorder.byte_size();
            if phase == PaintPhase::Background && !is_nested {
                self.record_async_scrolling_metadata(paintable);
            }
            self.paint(paintable, phase);
            let command_range_end = self.recorder.byte_size();
            let command_range = CommandRange {
                offset: command_range_start as u32,
                size: (command_range_end - command_range_start) as u32,
            };
            if !skip_cache && cache_writes_enabled {
                self.set_cached_commands(
                    paintable,
                    phase,
                    command_range,
                    phase_context_index.0,
                    phase_has_empty_effective_clip,
                );
            }
        }

        self.recorder.set_accumulated_visual_context(VisualContextIndex(0));
        assert_eq!(
            self.recorder.save_nesting_level, 0,
            "unbalanced save/restore in a paint capture"
        );
        self.recorder.save_nesting_level = saved_nesting_level;
    }

    fn valid_cached_commands(
        &self,
        paintable: PaintableSlotId,
        phase: PaintPhase,
        phase_has_empty_effective_clip: bool,
    ) -> Option<(Rc<RecordingOutput>, crate::painting::record::cache::CachedCommands)> {
        let source = self.command_cache_source.as_ref()?;
        let caches = self.paintables.paint_caches.borrow();
        let cache = caches.get(paintable.slot_index() as usize)?.as_ref()?;
        let entry = cache.entry(phase).commands?;
        if entry.source_display_list_id != source.id
            || entry.captured_under_empty_effective_clip != phase_has_empty_effective_clip
        {
            return None;
        }
        Some((source.clone(), entry))
    }

    fn valid_cached_hit_test_items(
        &self,
        paintable: PaintableSlotId,
        phase: PaintPhase,
        own_index: usize,
        for_descendants_index: usize,
    ) -> Option<(Rc<Vec<HitTestItem>>, usize, usize)> {
        let source = self.item_cache_source.as_ref()?;
        let caches = self.paintables.paint_caches.borrow();
        let cache = caches.get(paintable.slot_index() as usize)?.as_ref()?;
        let entry = cache.entry(phase).hit_test_items?;
        if entry.source_hit_test_display_list_id != source.id
            || entry.recorded_context_index != own_index
            || entry.recorded_context_for_descendants_index != for_descendants_index
        {
            return None;
        }
        Some((source.items.clone(), entry.start as usize, entry.count as usize))
    }

    fn set_cached_commands(
        &self,
        paintable: PaintableSlotId,
        phase: PaintPhase,
        range: CommandRange,
        recorded_context_index: usize,
        captured_under_empty_effective_clip: bool,
    ) {
        let mut caches = self.paintables.paint_caches.borrow_mut();
        let cache = caches[paintable.slot_index() as usize].get_or_insert_with(Default::default);
        cache.entry_mut(phase).commands = Some(crate::painting::record::cache::CachedCommands {
            source_display_list_id: self.display_list_id,
            range,
            recorded_context_index,
            captured_under_empty_effective_clip,
        });
    }

    fn set_cached_hit_test_items(
        &self,
        paintable: PaintableSlotId,
        phase: PaintPhase,
        start: usize,
        count: usize,
        recorded_context_index: usize,
        recorded_context_for_descendants_index: usize,
    ) {
        let mut caches = self.paintables.paint_caches.borrow_mut();
        let cache = caches[paintable.slot_index() as usize].get_or_insert_with(Default::default);
        cache.entry_mut(phase).hit_test_items = Some(crate::painting::record::cache::CachedHitTestItems {
            source_hit_test_display_list_id: self.hit_test_list_generation,
            start: start as u32,
            count: count as u32,
            recorded_context_index,
            recorded_context_for_descendants_index,
        });
    }

    fn paint(&mut self, paintable: PaintableSlotId, phase: PaintPhase) {
        crate::painting::record::paint::paint(self, paintable, phase);
    }
}
