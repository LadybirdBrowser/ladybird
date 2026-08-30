/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{PaintPhase, PaintRecorder};
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::layout::node_data::{NodeFlag, NodeKind};
use crate::layout::{node_facts, used_values};
use crate::painting::display_list::builder::CommandRange;
use crate::painting::display_list::commands::ContextRef;
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::display_list::recorder::DisplayListRecorder;
use crate::painting::hit_test::*;
use crate::painting::host::{
    FfiHitTestHostCallbacks, FfiPaintHostCallbacks, FfiRecordingInputs, FfiVisualContextHostCallbacks,
};
use crate::painting::node_painting;
use crate::painting::record::RecordingOutput;
use crate::painting::record::masks::MaskLayerSet;
use crate::painting::stacking_context::NO_STACKING_CONTEXT;
use crate::painting::style_queries;
use std::collections::HashMap;
use std::rc::Rc;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum StackingContextPaintPhase {
    BackgroundAndBorders = 0,
    Floats = 1,
    BackgroundAndBordersForInlineLevelAndReplaced = 2,
    Foreground = 3,
}

impl StackingContextPaintPhase {
    pub(crate) const COUNT: usize = Self::Foreground as usize + 1;
}

const _: () = assert!(
    StackingContextPaintPhase::Floats as usize == StackingContextPaintPhase::BackgroundAndBorders as usize + 1
        && StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced as usize
            == StackingContextPaintPhase::Floats as usize + 1
        && StackingContextPaintPhase::Foreground as usize
            == StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced as usize + 1
);

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
    paint_state: &crate::painting::paint_state::PaintState,
    host: &FfiHitTestHostCallbacks,
    paint_host: &FfiPaintHostCallbacks,
    visual_context_host: &FfiVisualContextHostCallbacks,
    inputs: FfiRecordingInputs,
    hit_test_list_generation: u64,
    command_cache_source: Option<Rc<RecordingOutput>>,
    item_cache_source: Option<Rc<crate::painting::record::cache::HitTestItemCacheSource>>,
) -> RecordingOutput {
    let stacking_contexts = paint_state
        .stacking_context_tree
        .as_ref()
        .expect("recording needs a built stacking context tree");
    let visual_context_tree_version = paint_state.visual_context.tree_version();
    // NB: Some commands embed visual context indices in their payloads. Those indices can change
    //     when the visual context tree is rebuilt, so commands from an incompatible tree must be
    //     recorded and cached against the new tree.
    let command_cache_source = command_cache_source.filter(|source| {
        source.compatible_visual_context_tree_version == visual_context_tree_version
            && source.recorded_device_pixels_per_css_pixel == inputs.device_pixels_per_css_pixel
    });
    let item_cache_source =
        item_cache_source.filter(|source| source.visual_context_tree_version == visual_context_tree_version);
    let paintable_rows = layout_arena.paintable_rows();
    let mut recorder = PaintRecorder {
        layout_arena: &paintable_rows,
        paint_state,
        stacking_contexts,
        host,
        paint_host,
        inputs,
        recorder: DisplayListRecorder::new(),
        converter: DevicePixelConverter::new(inputs.device_pixels_per_css_pixel),
        draw_svg_geometry_for_clip_path: false,
        visual_context_host,
        nested: None,
        nested_tree: None,
        last_looked_up_box_local_frames_by_role: std::cell::RefCell::new(None),
        prerecorded: crate::painting::record::masks::PrerecordedNestedDisplayLists::default(),
        command_cache_source,
        item_cache_source,
        display_list_id: inputs.display_list_id,
        hit_test_list_generation,
        viewport: stacking_contexts
            .nodes
            .first()
            .map_or(NodeSlotId::INVALID, |root| root.paintable),
        has_blocking_wheel_event_listeners: false,
        spliced_capture_count: 0,
        uncacheable_paint_generation: 0,
        list: HitTestList::default(),
        base_paint_facts_cache: vec![None; layout_arena.paintable_row_count()],
        paintable_facts_cache: vec![None; layout_arena.paintable_row_count()],
        absolute_position_cache: (0..layout_arena.paintable_row_count())
            .map(|_| std::cell::Cell::new(None))
            .collect(),
        previously_captured_position_cache: (0..layout_arena.paintable_row_count())
            .map(|_| std::cell::Cell::new(None))
            .collect(),
        completed_record_gen: layout_arena.paint_cache_completed_record_gen(),
        all_paint_caches_dirty: layout_arena.all_paint_caches_dirty(),
        all_descendant_subtree_caches_dirty: layout_arena.all_descendant_subtree_caches_dirty(),
        text_node_facts_cache: HashMap::new(),
        font_resource_id_cache: HashMap::new(),
        text_control_selection_cache: HashMap::new(),
        selection_style_cache: HashMap::new(),
        wheel_hit_test_target_cache: HashMap::new(),
    };
    if inputs.canvas_fill_rect.has_value {
        recorder
            .recorder
            .fill_rect(inputs.canvas_fill_rect.value, inputs.canvas_color);
    }
    // .. in the case of embedded documents typically rendered over a transparent canvas
    // (such as provided via an HTML iframe element), if the used color scheme of the element
    // and the used color scheme of the embedded document’s root element do not match,
    // then the UA must use an opaque canvas of the Canvas color appropriate to the
    // embedded document’s used color scheme instead of a transparent canvas.
    if inputs.opaque_canvas {
        recorder.recorder.fill_rect(inputs.bitmap_rect, inputs.canvas_color);
    }
    recorder.recorder.fill_rect(inputs.bitmap_rect, inputs.background_color);
    recorder.prerecord_nested_display_lists();
    if !stacking_contexts.nodes.is_empty() {
        recorder.paint_stacking_context(0);
    }
    crate::painting::record::paint::inspector_overlay::record_inspector_overlays(&mut recorder);
    let mask_display_lists = recorder.recorder.take_mask_display_lists();
    let mut hit_test_list = recorder.list;
    hit_test_list.generation = hit_test_list_generation;
    RecordingOutput {
        id: inputs.display_list_id,
        compatible_visual_context_tree_version: visual_context_tree_version,
        recorded_device_pixels_per_css_pixel: inputs.device_pixels_per_css_pixel,
        hit_test_list,
        display_list: recorder.recorder.into_builder().finish(),
        has_blocking_wheel_event_listeners: recorder.has_blocking_wheel_event_listeners,
        mask_display_lists,
        spliced_capture_count: recorder.spliced_capture_count,
    }
}

impl PaintRecorder<'_> {
    fn z_index(&mut self, paintable: NodeSlotId) -> Option<i32> {
        crate::painting::style_queries::z_index(self.layout_arena, paintable)
    }

    fn is_fragmented_inline(&self, paintable: NodeSlotId) -> bool {
        node_painting::is_fragmented_inline(self.layout_arena, paintable)
    }

    fn establishes_inline_level_painting_context(&self, paintable: NodeSlotId) -> bool {
        // CSS 2.2 painting order puts inline-block and inline-table boxes in the inline-level painting step and
        // says to paint each "as if it created a new stacking context", while keeping positioned descendants and
        // actual child stacking contexts in the parent stacking context:
        // https://drafts.csswg.org/css2/#painting-order
        // https://drafts.csswg.org/css2/#elaborate-stacking-contexts
        let display = self.display(paintable);
        display.is_inline_outside() && (display.is_flow_root_inside() || display.is_table_inside())
    }

    fn is_pure_inline_box(&self, paintable: NodeSlotId) -> bool {
        self.is_fragmented_inline(paintable)
            && !style_queries::is_floating(self.layout_arena, paintable)
            && !style_queries::is_positioned(self.layout_arena, paintable)
    }

    fn paint_stacking_context(&mut self, index: u32) {
        let paintable = self.stacking_contexts.nodes[index as usize].paintable;
        // https://drafts.csswg.org/css-transforms-1/#transform-function-lists
        // If a transform function causes the current transformation matrix of an object to be
        // non-invertible, the object and its content do not get displayed. Retain content whose
        // transform is animated so the compositor can reveal it without a main-thread repaint.
        if self
            .data(paintable)
            .has_flag(crate::painting::paintable_data::PaintableFlag::HasNonInvertibleCssTransform)
            && self.layout_arena.node_flags_if_live(paintable) & NodeFlag::HasAnimatedOpacityOrTransform as u32 == 0
        {
            return;
        }
        let effective_context = self.own_context(paintable);
        self.recorder.set_accumulated_visual_context(effective_context);

        // For elements with SVG filters, emit a transparent FillRect to trigger filter application.
        // This ensures content-generating filters (feFlood, feImage) work even with empty source.
        if let Some(svg_filter_bounds) = self.layout_arena.paintable_side_data(paintable).svg_filter_bounds.get() {
            let device_rect = self
                .converter
                .enclosing_device_rect(crate::css::css_pixels::CssPixelRect::from(svg_filter_bounds));
            self.recorder.fill_rect_transparent(device_rect);
        }

        self.register_mask_display_lists(paintable, MaskLayerSet::CssAndSvg);

        let context_before_children = self.recorder.accumulated_visual_context();
        self.with_context(context_before_children, |this| this.paint_internal(index));
    }

    fn paint_child(&mut self, child: u32) {
        self.paint_stacking_context(child);
    }

    fn paint_internal(&mut self, index: u32) {
        let stacking_contexts = self.stacking_contexts;
        let node = &stacking_contexts.nodes[index as usize];
        let paintable = node.paintable;
        let children = &node.children;
        if self.layout_kind(paintable) == Some(NodeKind::SVGSVGBox) {
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
        if crate::painting::paintable_geometry::committed_collapsed_table_borders(self.layout_arena, paintable)
            .is_some()
        {
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
            if !self.layout_arena.paintable_row_is_populated(descendant) {
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

    fn paint_subtree_backgrounds_and_borders(&mut self, paintable: NodeSlotId) {
        self.paint_node(paintable, PaintPhase::Background);
        self.paint_node(paintable, PaintPhase::Border);
        // A pure inline paintable paints its own background/border in the inline-level phase. Its block descendants, if
        // any, are painted by the earlier BackgroundAndBorders descent through pure inline boxes. In today's layout
        // trees, this subtree sweep is a no-op for InlineNodes: it can only find inline children, floats, or positioned
        // boxes, all of which are skipped by the BackgroundAndBorders phase.
        if !self.is_pure_inline_box(paintable) {
            self.paint_descendants(paintable, StackingContextPaintPhase::BackgroundAndBorders);
        }
        if crate::painting::paintable_geometry::committed_collapsed_table_borders(self.layout_arena, paintable)
            .is_some()
        {
            self.paint_node(paintable, PaintPhase::TableCollapsedBorder);
        }
    }

    fn paint_inline_level_non_positioned_descendant(&mut self, paintable: NodeSlotId) {
        self.paint_subtree_backgrounds_and_borders(paintable);
        // https://drafts.csswg.org/css2/#elaborate-stacking-contexts
        // "For inline-block and inline-table elements: [...] treat the element as if it created a new stacking context,
        // but any positioned descendants and descendants which actually create a new stacking context should be
        // considered part of the parent stacking context, not this new one."
        if self.establishes_inline_level_painting_context(paintable) {
            self.paint_descendants(paintable, StackingContextPaintPhase::Floats);
        }
    }

    fn paint_node_as_stacking_context(&mut self, paintable: NodeSlotId) {
        if self.layout_kind(paintable) == Some(NodeKind::SVGSVGBox) {
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

    pub(crate) fn paint_svg(&mut self, paintable: NodeSlotId, phase: PaintPhase) {
        if phase != PaintPhase::Foreground {
            return;
        }
        // SVG paint servers and filters are resolved while recording and can change without invalidating the
        // paintable that references them. Do not cache an enclosing descendant subtree.
        self.prevent_descendant_subtree_caching();
        self.paint_node(paintable, PaintPhase::Background);
        self.paint_node(paintable, PaintPhase::Border);
        self.paint_svg_box(paintable, phase);
    }

    fn paint_descendants(&mut self, paintable: NodeSlotId, phase: StackingContextPaintPhase) {
        let mut next_child = crate::painting::paint_order::first_paint_child(self.layout_arena, paintable);
        while let Some(child) = next_child {
            next_child = crate::painting::paint_order::next_paint_sibling(self.layout_arena, child);
            if self.append_cached_descendant_subtree(child, phase) {
                continue;
            }
            let command_start = self.recorder.byte_size();
            let hit_test_start = self.list.items.len();
            let uncacheable_paint_generation = self.uncacheable_paint_generation;
            self.paint_descendant(child, phase);
            if self.uncacheable_paint_generation == uncacheable_paint_generation {
                self.cache_descendant_subtree(child, phase, command_start, hit_test_start);
            }
        }
    }

    fn paint_descendant(&mut self, child: NodeSlotId, phase: StackingContextPaintPhase) {
        if self.has_stacking_context(child) {
            return;
        }
        let positioned = style_queries::is_positioned(self.layout_arena, child);
        let floating = style_queries::is_floating(self.layout_arena, child);
        let inline = style_queries::is_inline(self.layout_arena, child);
        let is_item = style_queries::is_flex_or_grid_item(self.layout_arena, child);

        // Positioned descendants at stack level 0 are painted in a separate pass.
        if positioned && self.z_index(child).unwrap_or(0) == 0 {
            return;
        }

        if self.layout_kind(child) == Some(NodeKind::SVGSVGBox) {
            self.paint_svg(child, to_paint_phase(phase));
            return;
        }

        // NOTE: Flex and grid items should be treated the same way as CSS2 defines for inline-blocks:
        //       - https://drafts.csswg.org/css-flexbox-1/#painting
        //       - https://www.w3.org/TR/css-grid-2/#z-order
        //       "For each one of these, treat the element as if it created a new stacking context, but any positioned
        //       descendants and descendants which actually create a new stacking context should be considered part of
        //       the parent stacking context, not this new one."
        if is_item && self.z_index(child).is_none() {
            // FIXME: This may not be fully correct with respect to the paint phases.
            if phase == StackingContextPaintPhase::Foreground {
                self.paint_node_as_stacking_context(child);
            }
            return;
        }

        // All non-positioned floating descendants, in tree order.
        if floating && !positioned && self.z_index(child).is_none() {
            if phase == StackingContextPaintPhase::Floats {
                self.paint_node_as_stacking_context(child);
            }
            return;
        }

        let child_is_inline_or_replaced = inline || self.is_replaced_box(child);
        let child_has_inline_level_painting_context = self.establishes_inline_level_painting_context(child);
        match phase {
            StackingContextPaintPhase::BackgroundAndBorders => {
                if !child_is_inline_or_replaced && !floating {
                    self.paint_subtree_backgrounds_and_borders(child);
                } else if self.is_pure_inline_box(child) {
                    self.paint_descendants(child, phase);
                }
            }
            StackingContextPaintPhase::Floats => {
                if floating {
                    self.paint_subtree_backgrounds_and_borders(child);
                }
                // Atomic inline-level descendants participate in the parent's inline-level
                // painting step, so their internal floats are not painted early.
                if !child_has_inline_level_painting_context {
                    self.paint_descendants(child, phase);
                }
            }
            StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced => {
                if child_is_inline_or_replaced {
                    self.paint_inline_level_non_positioned_descendant(child);
                }
                self.paint_descendants(child, phase);
            }
            StackingContextPaintPhase::Foreground => {
                self.paint_node(child, PaintPhase::Foreground);
                self.paint_descendants(child, phase);
                self.paint_node(child, PaintPhase::Outline);
                self.paint_node(child, PaintPhase::Overlay);
            }
        }
    }

    fn append_cached_descendant_subtree(&mut self, paintable: NodeSlotId, phase: StackingContextPaintPhase) -> bool {
        if self.nested.is_some() {
            return false;
        }
        let Some(command_source) = self.command_cache_source.as_ref() else {
            return false;
        };
        let Some(item_source) = self.item_cache_source.as_ref() else {
            return false;
        };
        let cache = self.layout_arena.paintable_paint_cache(paintable);
        if self.all_paint_caches_dirty
            || self.all_descendant_subtree_caches_dirty
            || cache.is_self_dirty_since(self.completed_record_gen)
            || cache.has_dirty_descendants_since(self.completed_record_gen)
        {
            return false;
        }
        let Some(cached) = cache.descendant_subtree(phase) else {
            return false;
        };
        if cached.source_display_list_id != command_source.id
            || cached.source_hit_test_display_list_id != item_source.id
        {
            return false;
        }
        drop(cache);
        if self.previously_captured_position(paintable) != self.current_absolute_position(paintable) {
            return false;
        }

        let command_range = self
            .recorder
            .append_cached_command_range_verbatim(&command_source.display_list.bytes, cached.command_range);
        let hit_test_start = self.list.items.len();
        let source_start = cached.hit_test_start as usize;
        let source_end = source_start + cached.hit_test_count as usize;
        for item in &item_source.items[source_start..source_end] {
            self.list.append(item.clone());
        }
        if self.inputs.paint_command_cache_read_write {
            self.set_cached_descendant_subtree(
                paintable,
                phase,
                command_range,
                hit_test_start,
                cached.hit_test_count as usize,
            );
        }
        self.spliced_capture_count += 1;
        true
    }

    fn cache_descendant_subtree(
        &self,
        paintable: NodeSlotId,
        phase: StackingContextPaintPhase,
        command_start: usize,
        hit_test_start: usize,
    ) {
        if !self.inputs.paint_command_cache_read_write || self.nested.is_some() {
            return;
        }
        self.set_cached_descendant_subtree(
            paintable,
            phase,
            CommandRange {
                offset: command_start as u32,
                size: (self.recorder.byte_size() - command_start) as u32,
            },
            hit_test_start,
            self.list.items.len() - hit_test_start,
        );
    }

    fn previously_captured_position(&self, paintable: NodeSlotId) -> used_values::FfiCssPixelPoint {
        let index = paintable.slot_index() as usize;
        if let Some(cell) = self.previously_captured_position_cache.get(index)
            && let Some((memoized_id, position)) = cell.get()
            && memoized_id == paintable
        {
            return position;
        }
        let position = self
            .layout_arena
            .paintable_paint_cache(paintable)
            .captured_absolute_position();
        if let Some(cell) = self.previously_captured_position_cache.get(index) {
            cell.set(Some((paintable, position)));
        }
        position
    }

    fn current_absolute_position(&self, paintable: NodeSlotId) -> used_values::FfiCssPixelPoint {
        let index = paintable.slot_index() as usize;
        if let Some(cell) = self.absolute_position_cache.get(index)
            && let Some((memoized_id, position)) = cell.get()
            && memoized_id == paintable
        {
            return position;
        }
        let position: used_values::FfiCssPixelPoint =
            crate::painting::paintable_geometry::absolute_position(self.layout_arena, paintable).into();
        if let Some(cell) = self.absolute_position_cache.get(index) {
            cell.set(Some((paintable, position)));
        }
        position
    }

    fn set_cached_descendant_subtree(
        &self,
        paintable: NodeSlotId,
        phase: StackingContextPaintPhase,
        command_range: CommandRange,
        hit_test_start: usize,
        hit_test_count: usize,
    ) {
        let cache = self.layout_arena.paintable_paint_cache(paintable);
        cache.register_capture_position(self.current_absolute_position(paintable));
        cache.set_descendant_subtree(
            phase,
            crate::painting::record::cache::CachedDescendantSubtree {
                source_display_list_id: self.display_list_id,
                command_range,
                source_hit_test_display_list_id: self.hit_test_list_generation,
                hit_test_start: hit_test_start as u32,
                hit_test_count: hit_test_count as u32,
            },
        );
    }

    fn paint_svg_box(&mut self, svg_box: NodeSlotId, phase: PaintPhase) {
        let context = self.own_context(svg_box);
        self.recorder.set_accumulated_visual_context(context);

        // For elements with SVG filters, emit a transparent FillRect to trigger filter application.
        // This ensures content-generating filters (feFlood, feImage) work even with empty source.
        if let Some(svg_filter_bounds) = self.layout_arena.paintable_side_data(svg_box).svg_filter_bounds.get() {
            let device_rect = self
                .converter
                .enclosing_device_rect(crate::css::css_pixels::CssPixelRect::from(svg_filter_bounds));
            self.recorder.fill_rect_transparent(device_rect);
        }

        if self.register_mask_display_lists(svg_box, MaskLayerSet::SvgOnly) {
            return;
        }
        self.record_hit_test_items(svg_box, phase);
        if self.layout_kind(svg_box) == Some(NodeKind::SVGForeignObjectBox) {
            self.record_foreign_object_descendant_hit_test_items(svg_box);
        }
        let kind = self.layout_kind(svg_box);
        if kind != Some(NodeKind::SVGSVGBox)
            && !kind.is_some_and(node_painting::is_svg)
            && kind.is_some_and(node_facts::kind_is_replaced_box)
        {
            crate::painting::record::paint::paint(self, svg_box, PaintPhase::Background);
        }
        crate::painting::record::paint::paint(self, svg_box, PaintPhase::Foreground);
        self.svg_paint_descendants(svg_box, phase);
    }

    fn svg_paint_descendants(&mut self, paintable: NodeSlotId, phase: PaintPhase) {
        if phase != PaintPhase::Foreground {
            return;
        }
        let mut next_child = crate::painting::paint_order::first_paint_child(self.layout_arena, paintable);
        while let Some(child) = next_child {
            next_child = crate::painting::paint_order::next_paint_sibling(self.layout_arena, child);
            // A child that establishes a stacking context is painted by that context.
            if self.has_stacking_context(child) {
                continue;
            }
            self.paint_svg_box(child, phase);
        }
    }

    fn for_descendants_context(&self, paintable: NodeSlotId) -> ContextRef {
        if let Some(nested) = &self.nested
            && let Some((_, for_descendants)) = nested.assignments.paintable_contexts.get(&paintable.index)
        {
            return *for_descendants;
        }
        self.data(paintable).accumulated_visual_context_for_descendants
    }

    fn context_for_phase(&self, paintable: NodeSlotId, phase: PaintPhase) -> ContextRef {
        // Text fragments are content of the block container (or of a self-painting inline box).
        // They need the descendants' visual context, not the element's own visual context.
        let foreground_paints_descendant_content = node_painting::has_lines(self.layout_arena, paintable)
            || node_painting::is_inline(self.layout_arena, paintable);
        if foreground_paints_descendant_content && phase == PaintPhase::Foreground {
            self.for_descendants_context(paintable)
        } else {
            self.own_context(paintable)
        }
    }

    fn paint_node(&mut self, paintable: NodeSlotId, phase: PaintPhase) {
        let context = self.context_for_phase(paintable, phase);
        self.recorder.set_accumulated_visual_context(context);

        // Hit-test items are only ever recorded in the Background, Foreground, and Overlay phases.
        let phase_can_record_hit_test_items = matches!(
            phase,
            PaintPhase::Background | PaintPhase::Foreground | PaintPhase::Overlay
        );
        let is_nested = self.nested.is_some();
        let data = self.data(paintable);
        // Scrolling repaints without invalidating paint caches, so scroll-offset-dependent
        // captures can never be reused.
        let skip_cache = data.has_fixed_background_visual_context
            || (data.has_scroll_offset_dependent_background && phase == PaintPhase::Background)
            || (self
                .layout_kind(paintable)
                .is_some_and(node_painting::foreground_is_never_cached)
                && phase == PaintPhase::Foreground);
        if skip_cache {
            self.prevent_descendant_subtree_caching();
        }
        let cache_writes_enabled = self.inputs.paint_command_cache_read_write && !is_nested;

        if phase_can_record_hit_test_items && !is_nested {
            let own_context = self.own_context(paintable);
            let for_descendants_context = self.for_descendants_context(paintable);
            let cached_items = if skip_cache {
                None
            } else {
                self.valid_cached_hit_test_items(paintable, phase, own_context, for_descendants_context)
            };
            if let Some((source, start, count)) = cached_items {
                // Copies a validated range of items recorded by one (paintable, phase) from the retained previous
                // list into this one. Items are copied, never inspected: source ranges belonging to relaid-out
                // Paintable rows may hold dangling fragment pointers, but only ranges whose owners kept a valid cache
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
                        own_context,
                        for_descendants_context,
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
                        own_context,
                        for_descendants_context,
                    );
                }
            }
        }

        // SVG subtrees are recorded outside per-paintable captures, so path-bearing items are never spliced.
        let phase_context = self.recorder.accumulated_visual_context();
        let cached_commands = if skip_cache || is_nested {
            None
        } else {
            self.valid_cached_commands(paintable, phase)
        };
        if let Some((source, cached)) = cached_commands {
            let destination_range = self.recorder.append_cached_command_range(
                &source.display_list.bytes,
                cached.range,
                cached.recorded_context,
                cached.recorded_local_frame_range,
                self.local_frame_range(paintable),
            );
            if cache_writes_enabled {
                self.set_cached_commands(paintable, phase, destination_range, phase_context);
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
                self.set_cached_commands(paintable, phase, command_range, phase_context);
            }
        }

        self.recorder.set_accumulated_visual_context(ContextRef::default());
    }

    fn valid_cached_commands(
        &self,
        paintable: NodeSlotId,
        phase: PaintPhase,
    ) -> Option<(Rc<RecordingOutput>, crate::painting::record::cache::CachedCommands)> {
        let source = self.command_cache_source.as_ref()?;
        let cache = self.layout_arena.paintable_paint_cache_if_allocated(paintable)?;
        // Checked before loading the entry so a dirty row's miss stays as cheap as the
        // absent-entry miss the eager clearing model produced.
        if self.all_paint_caches_dirty || cache.is_self_dirty_since(self.completed_record_gen) {
            return None;
        }
        let entry = cache.commands(phase)?;
        if entry.source_display_list_id != source.id {
            return None;
        }
        drop(cache);
        if self.previously_captured_position(paintable) != self.current_absolute_position(paintable) {
            return None;
        }
        Some((source.clone(), entry))
    }

    fn valid_cached_hit_test_items(
        &self,
        paintable: NodeSlotId,
        phase: PaintPhase,
        own_context: ContextRef,
        for_descendants_context: ContextRef,
    ) -> Option<(Rc<Vec<HitTestItem>>, usize, usize)> {
        let source = self.item_cache_source.as_ref()?;
        let cache = self.layout_arena.paintable_paint_cache_if_allocated(paintable)?;
        if self.all_paint_caches_dirty || cache.is_self_dirty_since(self.completed_record_gen) {
            return None;
        }
        let entry = cache.hit_test_items(phase)?;
        if entry.source_hit_test_display_list_id != source.id
            || entry.recorded_context != own_context
            || entry.recorded_context_for_descendants != for_descendants_context
        {
            return None;
        }
        drop(cache);
        if self.previously_captured_position(paintable) != self.current_absolute_position(paintable) {
            return None;
        }
        Some((source.items.clone(), entry.start as usize, entry.count as usize))
    }

    fn set_cached_commands(
        &self,
        paintable: NodeSlotId,
        phase: PaintPhase,
        range: CommandRange,
        recorded_context: ContextRef,
    ) {
        let recorded_local_frame_range = self.local_frame_range(paintable);
        let cache = self.layout_arena.paintable_paint_cache(paintable);
        cache.register_capture_position(self.current_absolute_position(paintable));
        cache.set_commands(
            phase,
            crate::painting::record::cache::CachedCommands {
                source_display_list_id: self.display_list_id,
                range,
                recorded_context,
                recorded_local_frame_range,
            },
        );
    }

    fn local_frame_range(&self, paintable: NodeSlotId) -> (u32, u32) {
        self.data(paintable).local_frame_range()
    }

    fn set_cached_hit_test_items(
        &self,
        paintable: NodeSlotId,
        phase: PaintPhase,
        start: usize,
        count: usize,
        recorded_context: ContextRef,
        recorded_context_for_descendants: ContextRef,
    ) {
        let cache = self.layout_arena.paintable_paint_cache(paintable);
        cache.register_capture_position(self.current_absolute_position(paintable));
        cache.set_hit_test_items(
            phase,
            crate::painting::record::cache::CachedHitTestItems {
                source_hit_test_display_list_id: self.hit_test_list_generation,
                start: start as u32,
                count: count as u32,
                recorded_context,
                recorded_context_for_descendants,
            },
        );
    }

    fn paint(&mut self, paintable: NodeSlotId, phase: PaintPhase) {
        crate::painting::record::paint::paint(self, paintable, phase);
    }
}
