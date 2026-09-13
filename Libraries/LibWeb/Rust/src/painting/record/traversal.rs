/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::record::trace::Observer;

use super::{PaintPhase, PaintRecorder};
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::layout::node_data::{NodeFlag, NodeKind};
use crate::layout::{node_facts, used_values};
use crate::painting::display_list::builder::{CommandRange, DisplayListBuilder, RecordedDisplayList};
use crate::painting::display_list::commands::{ContextRef, VISUAL_VIEWPORT_NODE_INDEX};
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::display_list::recorder::DisplayListRecorder;
use crate::painting::force_dark::{ForceDarkRole, ForceDarkSettings};
use crate::painting::hit_test::*;
use crate::painting::node_painting;
use crate::painting::record::RecordingInputs;
use crate::painting::record::cache::{
    CachedSubtreeCapture, CaptureAddress, CaptureKind, CaptureSite, EnclosingCaptureAnchor, OpenCapture, RecordGen,
    SourceTapePosition, SubtreeCaptureWalkOutcome, narrow_record_gen, resolve_capture_address_in_source_tape,
};
use crate::painting::record::cache_compatibility::PaintCacheInputs;
use crate::painting::record::resources::RecordingResourceManifest;
use crate::painting::record::scratch::RecordingScratch;
use crate::painting::record::svg_resources::MaskLayerSet;
use crate::painting::record::trace::{Action, Operation};
use crate::painting::record::verify::LoggedCapture;
use crate::painting::record::{DeferredWholeTapeSplice, RecordingOutput, RecordingResult};
use crate::painting::style_queries;
use std::rc::Rc;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
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
    scratch: &mut RecordingScratch,
    viewport: NodeSlotId,
    inputs: RecordingInputs,
    hit_test_list_generation: u64,
    command_cache_source: Option<Rc<RecordingOutput>>,
    item_cache_source: Option<Rc<crate::painting::record::cache::HitTestItemCacheSource>>,
    trace: bool,
) -> RecordingResult {
    scratch.begin_recording(layout_arena.paintable_row_count());
    macro_rules! record {
        ($observer:ty) => {
            record_display_list_impl::<$observer>(
                layout_arena,
                paint_state,
                scratch,
                viewport,
                inputs,
                hit_test_list_generation,
                command_cache_source,
                item_cache_source,
            )
        };
    }
    let result = if trace {
        record!(super::trace::Trace)
    } else {
        record!(super::trace::NoTrace)
    };
    scratch.clear_temporary_caches();
    result
}

#[allow(clippy::too_many_arguments)]
fn record_display_list_impl<O: Observer>(
    layout_arena: &LayoutNodeArena,
    paint_state: &crate::painting::paint_state::PaintState,
    scratch: &mut RecordingScratch,
    viewport: NodeSlotId,
    inputs: RecordingInputs,
    hit_test_list_generation: u64,
    command_cache_source: Option<Rc<RecordingOutput>>,
    item_cache_source: Option<Rc<crate::painting::record::cache::HitTestItemCacheSource>>,
) -> RecordingResult {
    let structural_epoch = paint_state.visual_context.structural_epoch();
    let cache_inputs = PaintCacheInputs::from_recording_inputs(&inputs);
    let cache_compatibility = command_cache_source.as_ref().map_or_else(Default::default, |source| {
        cache_inputs.compatibility_with(&source.cache_inputs)
    });
    let paintable_rows = layout_arena.paintable_rows();
    let force_dark_settings = inputs.force_dark_enabled.then_some(ForceDarkSettings {
        foreground_brightness_threshold: inputs.force_dark_foreground_threshold,
        background_brightness_threshold: inputs.force_dark_background_threshold,
    });
    let mut recorder = PaintRecorder {
        layout_arena: &paintable_rows,
        paint_state,
        inputs,
        recorder: DisplayListRecorder::new(force_dark_settings),
        converter: DevicePixelConverter::new(inputs.device_pixels_per_css_pixel),
        svg_resource_walk: None,
        command_cache_source,
        item_cache_source,
        cache_compatibility,
        open_capture_stack: Vec::new(),
        cache_updates: Default::default(),
        deferred_whole_tape_splice: None,
        viewport,
        blocking_wheel_event_region_count: 0,
        uncacheable_paint_generation: 0,
        observer: O::default(),
        list: HitTestList {
            item_capacity_hint_from_previous_list: paint_state
                .hit_test_list
                .as_ref()
                .map_or(0, |list| list.items.len()),
            ..HitTestList::default()
        },
        scratch,
        completed_record_gen: narrow_record_gen(layout_arena.paint_cache_completed_record_gen()),
        all_paint_caches_dirty: layout_arena.all_paint_caches_dirty(),
        resources: RecordingResourceManifest::default(),
    };
    recorder.trace_paint(Operation::Producer(None, "canvas"), |this| {
        if inputs.canvas_fill_rect.has_value {
            this.recorder.fill_rect(
                inputs.canvas_fill_rect.value,
                inputs.canvas_color,
                ForceDarkRole::Background,
            );
        }
        // .. in the case of embedded documents typically rendered over a transparent canvas
        // (such as provided via an HTML iframe element), if the used color scheme of the element
        // and the used color scheme of the embedded document’s root element do not match,
        // then the UA must use an opaque canvas of the Canvas color appropriate to the
        // embedded document’s used color scheme instead of a transparent canvas.
        if inputs.opaque_canvas {
            this.recorder
                .fill_rect(inputs.bitmap_rect, inputs.canvas_color, ForceDarkRole::Background);
        }
        this.recorder
            .fill_rect(inputs.bitmap_rect, inputs.background_color, ForceDarkRole::Background);
    });
    recorder.paint_and_capture_as_stacking_context(viewport);
    if inputs.has_inspector_highlight
        || inputs.grid_overlay_count > 0
        || inputs.flex_overlay_count > 0
        || inputs.caret_debug_rect.has_value
    {
        recorder.trace_paint(
            Operation::Producer(None, "inspector-overlays"),
            crate::painting::record::paint::inspector_overlay::record_inspector_overlays,
        );
    }
    let mut hit_test_list = recorder.list;
    hit_test_list.generation = hit_test_list_generation;
    let recorded = recorder.recorder.into_builder().finish();
    let display_list = match recorder.deferred_whole_tape_splice {
        Some(deferred) if recorded.bytes.len() == deferred.prologue_byte_count => deferred.source_display_list,
        Some(deferred) => Rc::new(materialize_deferred_whole_tape_splice(&recorded, &deferred)),
        None => Rc::new(recorded),
    };
    let output = RecordingOutput {
        recorded_structural_epoch: structural_epoch,
        cache_inputs,
        hit_test_list,
        display_list,
        has_blocking_wheel_event_listeners: recorder.blocking_wheel_event_region_count > 0,
        wheel_event_listener_state_generation: inputs.wheel_event_listener_state_generation,
        is_identical_to_cache_source: false,
        capture_log_for_verification: recorder.observer.finish(),
    };
    RecordingResult {
        output,
        resources: recorder.resources,
        cache_updates: recorder.cache_updates,
    }
}

fn materialize_deferred_whole_tape_splice(
    recorded: &RecordedDisplayList,
    deferred: &DeferredWholeTapeSplice,
) -> RecordedDisplayList {
    let mut builder = DisplayListBuilder::new();
    builder.append_command_range(
        recorded,
        CommandRange {
            offset: 0,
            size: deferred.prologue_byte_count as u32,
        },
        None,
    );
    builder.append_command_range(&deferred.source_display_list, deferred.source_range, None);
    builder.append_command_range(
        recorded,
        CommandRange {
            offset: deferred.prologue_byte_count as u32,
            size: (recorded.bytes.len() - deferred.prologue_byte_count) as u32,
        },
        None,
    );
    builder.finish()
}

impl<O: Observer> PaintRecorder<'_, O> {
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

    fn paint_stacking_context(&mut self, paintable: NodeSlotId) {
        debug_assert!(self.layout_arena.paintable_row_is_populated(paintable));
        if !self.layout_arena.paintable_row_is_populated(paintable) {
            return;
        }
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
            self.mark_open_captures_unsplicable();
            let device_rect = self
                .converter
                .enclosing_device_rect(crate::css::css_pixels::CssPixelRect::from(svg_filter_bounds));
            self.recorder.fill_rect_transparent(device_rect);
        }

        self.declare_mask_contents(paintable, MaskLayerSet::CssAndSvg);

        let context_before_children = self.recorder.accumulated_visual_context();
        self.with_context(context_before_children, |this| this.paint_internal(paintable));
    }

    fn paint_and_capture_as_stacking_context(&mut self, paintable: NodeSlotId) {
        debug_assert!(self.layout_arena.paintable_row_is_populated(paintable));
        if !self.layout_arena.paintable_row_is_populated(paintable) {
            return;
        }
        let site = CaptureSite {
            paintable,
            kind: CaptureKind::PaintedAsStackingContext,
        };
        self.splice_or_record_capture(site, |this| {
            if this.has_stacking_context(paintable) {
                this.paint_stacking_context(paintable);
            } else {
                this.paint_node_as_stacking_context(paintable);
            }
        });
    }

    fn paint_internal(&mut self, paintable: NodeSlotId) {
        let entries = self.layout_arena.stacking_context_entries(paintable);
        if self.layout_kind(paintable) == Some(NodeKind::SVGSVGBox) {
            self.paint_node(paintable, PaintPhase::Background);
            self.paint_node(paintable, PaintPhase::Border);
            self.paint_svg_box(paintable, PaintPhase::Foreground);
            // An `<svg>` that establishes a stacking context still has descendants that establish
            // one of their own - a `<foreignObject>` always does - and those are painted by their
            // own context rather than by the SVG walk.
            if let Some(entries) = &entries {
                for entry in entries.negative_z_index_child_contexts() {
                    self.paint_and_capture_as_stacking_context(entry.slot);
                }
                for &descendant in &entries.stack_level_zero_boxes {
                    if self.layout_arena.paintable_row_is_populated(descendant)
                        && self.data(descendant).establishes_stacking_context
                    {
                        self.paint_and_capture_as_stacking_context(descendant);
                    }
                }
                for entry in entries.positive_z_index_child_contexts() {
                    self.paint_and_capture_as_stacking_context(entry.slot);
                }
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
        if let Some(entries) = &entries {
            for entry in entries.negative_z_index_child_contexts() {
                self.paint_and_capture_as_stacking_context(entry.slot);
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
        if entries
            .as_ref()
            .is_some_and(|entries| entries.non_positioned_float_count > 0)
        {
            self.paint_descendants(paintable, StackingContextPaintPhase::Floats);
        }
        // Draw inline content, replaced content, etc. (steps 6, 7)
        if entries
            .as_ref()
            .is_some_and(|entries| entries.inline_or_replaced_count > 0)
        {
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
        if let Some(entries) = &entries {
            for &descendant in &entries.stack_level_zero_boxes {
                debug_assert!(self.layout_arena.paintable_row_is_populated(descendant));
                if !self.layout_arena.paintable_row_is_populated(descendant) {
                    continue;
                }
                self.paint_and_capture_as_stacking_context(descendant);
            }
        }

        // Stacking contexts formed by positioned descendants with z-indices greater than or equal
        // to 1 in z-index order (smallest first) then tree order. (Step 9)
        // Here, we treat non-positioned stacking contexts as if they were positioned, because CSS 2.0 spec does not
        // account for new properties like `transform` and `opacity` that can create stacking contexts.
        // https://github.com/w3c/csswg-drafts/issues/2717
        if let Some(entries) = &entries {
            for entry in entries.positive_z_index_child_contexts() {
                self.paint_and_capture_as_stacking_context(entry.slot);
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
        self.paint_node(paintable, PaintPhase::Background);
        self.paint_node(paintable, PaintPhase::Border);
        self.paint_svg_box(paintable, phase);
    }

    fn paint_descendants(&mut self, paintable: NodeSlotId, phase: StackingContextPaintPhase) {
        // CSS 2.2 §17.5.1 stacks the backgrounds of a table's parts in layers: the table, then the column groups,
        // the columns, the row groups, the rows and the cells. Column boxes may come after the row groups in the
        // tree (the HTML parser puts a <colgroup> that follows a row after it), so a table box paints its column
        // groups and columns before its other children. https://www.w3.org/TR/CSS22/tables.html#table-layers
        let is_column_box = |this: &Self, child: NodeSlotId| {
            let display = this.display(child);
            display.is_table_column_group() || display.is_table_column()
        };
        let paints_columns_first = phase == StackingContextPaintPhase::BackgroundAndBorders
            && self.layout_kind(paintable) == Some(NodeKind::Box)
            && self.display(paintable).is_table_inside();
        if paints_columns_first {
            self.paint_descendants_matching(paintable, phase, |this, child| is_column_box(this, child));
            self.paint_descendants_matching(paintable, phase, |this, child| !is_column_box(this, child));
        } else {
            self.paint_descendants_matching(paintable, phase, |_, _| true);
        }
    }

    fn paint_descendants_matching(
        &mut self,
        paintable: NodeSlotId,
        phase: StackingContextPaintPhase,
        matches: impl Fn(&Self, NodeSlotId) -> bool,
    ) {
        let mut next_child = crate::painting::paint_order::first_paint_child(self.layout_arena, paintable);
        while let Some(child) = next_child {
            next_child = crate::painting::paint_order::next_paint_sibling(self.layout_arena, child);
            if !matches(self, child) {
                continue;
            }
            let site = CaptureSite {
                paintable: child,
                kind: CaptureKind::DescendantSubtreePhase(phase),
            };
            self.splice_or_record_capture(site, |this| this.paint_descendant(child, phase));
        }
    }

    fn splice_or_record_capture(&mut self, site: CaptureSite, body: impl FnOnce(&mut Self)) {
        if self.try_splice_cached_subtree_capture(site) {
            self.observer
                .observe(|log| log.leaf(Operation::Capture(site), Action::Reuse, false));
            return;
        }
        let command_byte_start = self.recorder.byte_size();
        let hit_test_item_start = self.list.items.len();
        let uncacheable_paint_generation = self.uncacheable_paint_generation;
        let blocking_wheel_event_region_count_before = self.blocking_wheel_event_region_count;
        self.open_capture_stack.push(OpenCapture {
            site,
            command_byte_start: command_byte_start as u32,
            hit_test_item_start: hit_test_item_start as u32,
        });
        self.trace_scope(Operation::Capture(site), Action::Walk, body);
        self.open_capture_stack.pop();
        if self.is_recording_svg_resource_content() {
            return;
        }
        let command_range = CommandRange {
            offset: command_byte_start as u32,
            size: (self.recorder.byte_size() - command_byte_start) as u32,
        };
        let hit_test_item_count = self.list.items.len() - hit_test_item_start;
        self.log_command_byte_capture_for_verification(site.paintable, site.kind, command_range, false);
        self.log_hit_test_item_capture_for_verification(
            site.paintable,
            site.kind,
            hit_test_item_start,
            hit_test_item_count,
            false,
        );
        if !self.inputs.paint_command_cache_read_write {
            return;
        }
        self.store_subtree_capture(
            site,
            command_range,
            hit_test_item_start,
            hit_test_item_count,
            SubtreeCaptureWalkOutcome {
                gen_of_last_fresh_walk: self.current_record_gen(),
                may_be_spliced_verbatim: self.uncacheable_paint_generation == uncacheable_paint_generation,
                contains_blocking_wheel_event_region: self.blocking_wheel_event_region_count
                    != blocking_wheel_event_region_count_before,
            },
        );
    }

    fn address_relative_to_innermost_open_capture(
        &self,
        command_byte_start: u32,
        hit_test_item_start: u32,
    ) -> CaptureAddress {
        match self.open_capture_stack.last() {
            Some(open) => CaptureAddress {
                enclosing_capture: Some(open.site),
                command_byte_offset_from_enclosing_start: command_byte_start - open.command_byte_start,
                hit_test_item_index_from_enclosing_start: hit_test_item_start - open.hit_test_item_start,
                written_in_record_gen: self.current_record_gen(),
            },
            None => CaptureAddress {
                enclosing_capture: None,
                command_byte_offset_from_enclosing_start: command_byte_start,
                hit_test_item_index_from_enclosing_start: hit_test_item_start,
                written_in_record_gen: self.current_record_gen(),
            },
        }
    }

    fn resolve_capture_address_in_source_tape(&mut self, address: CaptureAddress) -> Option<SourceTapePosition> {
        let layout_arena = self.layout_arena;
        let lookup_enclosing_capture_anchor = |site: CaptureSite| -> Option<EnclosingCaptureAnchor> {
            if !layout_arena.paintable_row_is_populated(site.paintable) {
                return None;
            }
            layout_arena
                .paintable_paint_cache_if_allocated(site.paintable)?
                .enclosing_capture_anchor(site.kind)
        };
        resolve_capture_address_in_source_tape(
            self.completed_record_gen,
            address,
            &lookup_enclosing_capture_anchor,
            self.scratch.resolved_enclosing_capture_memo(),
        )
    }

    fn current_record_gen(&self) -> RecordGen {
        self.completed_record_gen + 1
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

    fn try_splice_cached_subtree_capture(&mut self, site: CaptureSite) -> bool {
        if self.is_recording_svg_resource_content() || !self.cache_compatibility.allows_subtree(site.kind) {
            return false;
        }
        let Some(item_source) = self.item_cache_source.clone() else {
            return false;
        };
        let cache = self.layout_arena.paintable_paint_cache(site.paintable);
        if self.all_paint_caches_dirty
            || cache.is_self_dirty_since(self.completed_record_gen)
            || cache.has_dirty_descendants_since(self.completed_record_gen)
        {
            return false;
        }
        let Some(cached) = cache.subtree_capture(site.kind) else {
            return false;
        };
        let captured_position = cache.captured_absolute_position();
        drop(cache);
        if !cached.may_be_spliced_verbatim {
            return false;
        }
        if captured_position != self.current_absolute_position(site.paintable) {
            return false;
        }
        let Some(source_position) = self.resolve_capture_address_in_source_tape(cached.address) else {
            return false;
        };
        let Some(command_source) = self.command_cache_source.as_ref() else {
            return false;
        };

        let hit_test_item_start = self.list.items.len();
        let source_start = source_position.hit_test_item_index as usize;
        let source_end = source_start + cached.hit_test_item_count as usize;
        let splice_covers_entire_source_hit_test_list =
            hit_test_item_start == 0 && source_start == 0 && source_end == item_source.items.len();
        let range = CommandRange {
            offset: source_position.command_byte_offset,
            size: cached.command_byte_count,
        };
        let prologue_byte_count = self.recorder.byte_size();
        let splice_covers_entire_source_tape_after_prologue = splice_covers_entire_source_hit_test_list
            && source_position.command_byte_offset as usize == prologue_byte_count
            && (source_position.command_byte_offset + cached.command_byte_count) as usize
                == command_source.display_list.bytes.len()
            && self.recorder.bytes() == &command_source.display_list.bytes[..prologue_byte_count];
        let command_range = if splice_covers_entire_source_tape_after_prologue {
            self.deferred_whole_tape_splice = Some(DeferredWholeTapeSplice {
                source_display_list: command_source.display_list.clone(),
                prologue_byte_count,
                source_range: range,
            });
            range
        } else {
            self.recorder
                .append_cached_command_range_verbatim(&command_source.display_list, range)
        };
        if splice_covers_entire_source_hit_test_list {
            self.list.items = item_source.items.clone();
        } else {
            self.append_spliced_hit_test_items(&item_source.items[source_start..source_end], site.paintable);
        }
        if cached.contains_blocking_wheel_event_region {
            self.blocking_wheel_event_region_count += 1;
        }
        self.log_command_byte_capture_for_verification(site.paintable, site.kind, command_range, true);
        self.log_hit_test_item_capture_for_verification(
            site.paintable,
            site.kind,
            hit_test_item_start,
            cached.hit_test_item_count as usize,
            true,
        );
        if self.inputs.paint_command_cache_read_write {
            self.store_subtree_capture(
                site,
                command_range,
                hit_test_item_start,
                cached.hit_test_item_count as usize,
                SubtreeCaptureWalkOutcome {
                    gen_of_last_fresh_walk: cached.gen_of_last_fresh_walk,
                    may_be_spliced_verbatim: true,
                    contains_blocking_wheel_event_region: cached.contains_blocking_wheel_event_region,
                },
            );
        }
        true
    }

    fn store_subtree_capture(
        &mut self,
        site: CaptureSite,
        command_range: CommandRange,
        hit_test_item_start: usize,
        hit_test_item_count: usize,
        walk_outcome: SubtreeCaptureWalkOutcome,
    ) {
        let absolute_position = self.current_absolute_position(site.paintable);
        self.cache_updates.set_subtree_capture(
            site,
            absolute_position,
            CachedSubtreeCapture {
                address: self
                    .address_relative_to_innermost_open_capture(command_range.offset, hit_test_item_start as u32),
                command_byte_count: command_range.size,
                hit_test_item_count: hit_test_item_count as u32,
                gen_of_last_fresh_walk: walk_outcome.gen_of_last_fresh_walk,
                may_be_spliced_verbatim: walk_outcome.may_be_spliced_verbatim,
                contains_blocking_wheel_event_region: walk_outcome.contains_blocking_wheel_event_region,
            },
        );
    }

    fn log_command_byte_capture_for_verification(
        &mut self,
        paintable: NodeSlotId,
        kind: CaptureKind,
        range: CommandRange,
        spliced_from_cache: bool,
    ) {
        self.observer.observe(|log| {
            if spliced_from_cache && matches!(kind, CaptureKind::BoxPhase(_)) {
                log.leaf(
                    Operation::Capture(CaptureSite { paintable, kind }),
                    Action::Reuse,
                    range.size == 0,
                );
            }
            log.command_byte_captures.push(LoggedCapture {
                start: range.offset,
                length: range.size,
                paintable,
                kind,
                spliced_from_cache,
            });
        });
    }

    fn log_hit_test_item_capture_for_verification(
        &mut self,
        paintable: NodeSlotId,
        kind: CaptureKind,
        start: usize,
        count: usize,
        spliced_from_cache: bool,
    ) {
        self.observer.observe(|log| {
            if let CaptureKind::BoxPhase(phase) = kind {
                log.leaf(
                    Operation::HitTest(paintable, phase),
                    if spliced_from_cache {
                        Action::Reuse
                    } else {
                        Action::Record
                    },
                    count == 0,
                );
            }
            log.hit_test_item_captures.push(LoggedCapture {
                start: start as u32,
                length: count as u32,
                paintable,
                kind,
                spliced_from_cache,
            });
        });
    }

    fn current_absolute_position(&mut self, paintable: NodeSlotId) -> used_values::FfiCssPixelPoint {
        if let Some(position) = self.scratch.absolute_position(paintable) {
            return position;
        }
        let position: used_values::FfiCssPixelPoint =
            crate::painting::paintable_geometry::absolute_position(self.layout_arena, paintable).into();
        self.scratch.set_absolute_position(paintable, position);
        position
    }

    fn paint_svg_box(&mut self, svg_box: NodeSlotId, phase: PaintPhase) {
        self.trace_scope(Operation::Producer(Some(svg_box), "svg"), Action::Walk, |this| {
            this.paint_svg_box_impl(svg_box, phase);
        });
    }

    fn paint_svg_box_impl(&mut self, svg_box: NodeSlotId, phase: PaintPhase) {
        if self.is_recording_svg_resource_content() {
            let parent_to_enclosing_space = self
                .recorder
                .ambient_inline_transform()
                .unwrap_or_else(libgfx_rust::AffineTransform::identity);
            self.paint_svg_box_inside_resource(svg_box, parent_to_enclosing_space, true);
            return;
        }
        let context = self.own_context(svg_box);
        self.recorder.set_accumulated_visual_context(context);

        // For elements with SVG filters, emit a transparent FillRect to trigger filter application.
        // This ensures content-generating filters (feFlood, feImage) work even with empty source.
        if let Some(svg_filter_bounds) = self.layout_arena.paintable_side_data(svg_box).svg_filter_bounds.get() {
            self.mark_open_captures_unsplicable();
            let device_rect = self
                .converter
                .enclosing_device_rect(crate::css::css_pixels::CssPixelRect::from(svg_filter_bounds));
            self.recorder.fill_rect_transparent(device_rect);
        }

        if self.declare_mask_contents(svg_box, MaskLayerSet::SvgOnly) {
            return;
        }
        let before = self.list.items.len();
        self.record_hit_test_items(svg_box, phase);
        self.observer.observe(|log| {
            log.leaf(
                Operation::HitTest(svg_box, phase),
                Action::Record,
                self.list.items.len() == before,
            );
        });
        if self.layout_kind(svg_box) == Some(NodeKind::SVGForeignObjectBox) {
            self.record_foreign_object_descendant_hit_test_items(svg_box);
        }
        let kind = self.layout_kind(svg_box);
        if kind != Some(NodeKind::SVGSVGBox)
            && !kind.is_some_and(node_painting::is_svg)
            && kind.is_some_and(node_facts::kind_is_replaced_box)
        {
            self.trace_paint(
                Operation::Capture(CaptureSite {
                    paintable: svg_box,
                    kind: CaptureKind::BoxPhase(PaintPhase::Background),
                }),
                |this| crate::painting::record::paint::paint(this, svg_box, PaintPhase::Background),
            );
        }
        self.trace_paint(
            Operation::Capture(CaptureSite {
                paintable: svg_box,
                kind: CaptureKind::BoxPhase(PaintPhase::Foreground),
            }),
            |this| crate::painting::record::paint::paint(this, svg_box, PaintPhase::Foreground),
        );
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
        if let Some(walk) = &self.svg_resource_walk {
            return walk.enclosing_context;
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

    pub(crate) fn paint_node(&mut self, paintable: NodeSlotId, phase: PaintPhase) {
        let context = self.context_for_phase(paintable, phase);
        self.recorder.set_accumulated_visual_context(context);

        // Hit-test items are only ever recorded in the Background, Foreground, and Overlay phases.
        let phase_can_record_hit_test_items = matches!(
            phase,
            PaintPhase::Background | PaintPhase::Foreground | PaintPhase::Overlay
        );
        let is_nested = self.is_recording_svg_resource_content();
        let data = self.data(paintable);
        // Scrolling repaints without invalidating paint caches, so scroll-offset-dependent
        // captures can never be reused.
        let skip_cache = data.has_fixed_background_visual_context
            || (data.has_scroll_offset_dependent_background && phase == PaintPhase::Background);
        let phase_records_scrollbars_with_scroll_node_indices = phase == PaintPhase::Overlay
            && (data.own_scroll_node_index != VISUAL_VIEWPORT_NODE_INDEX
                || self.layout_kind(paintable) == Some(NodeKind::Viewport));
        if skip_cache {
            self.mark_open_captures_unsplicable();
        }
        let skip_phase_capture = skip_cache || phase_records_scrollbars_with_scroll_node_indices;
        let cache_writes_enabled = self.inputs.paint_command_cache_read_write && !is_nested;

        if phase_can_record_hit_test_items && !is_nested {
            let own_context = self.own_context(paintable);
            let for_descendants_context = self.for_descendants_context(paintable);
            let cached_items = if skip_phase_capture {
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
                for item in &source[start..start + count] {
                    self.append_spliced_hit_test_item(item);
                }
                self.log_hit_test_item_capture_for_verification(
                    paintable,
                    CaptureKind::BoxPhase(phase),
                    destination_start,
                    count,
                    true,
                );

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
                let items_after = self.list.items.len();
                self.log_hit_test_item_capture_for_verification(
                    paintable,
                    CaptureKind::BoxPhase(phase),
                    items_before,
                    items_after - items_before,
                    false,
                );
                if !skip_phase_capture && cache_writes_enabled {
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

        if phase == PaintPhase::Background && !is_nested {
            self.trace_paint(Operation::Producer(Some(paintable), "scroll-metadata"), |this| {
                this.record_async_scrolling_metadata(paintable);
            });
        }

        // A visually empty phase can still contribute hit-test and scrolling metadata.
        // Only skip command capture, after recording that metadata above.
        if self.base_paint_facts(paintable).paint_phase_mask & phase.bit() == 0 {
            self.observer.observe(|log| {
                log.leaf(
                    Operation::Capture(CaptureSite {
                        paintable,
                        kind: CaptureKind::BoxPhase(phase),
                    }),
                    Action::Skip,
                    true,
                );
            });
            self.recorder.set_accumulated_visual_context(ContextRef::default());
            return;
        }

        // SVG subtrees are recorded outside per-paintable captures, so path-bearing items are never spliced.
        let phase_context = self.recorder.accumulated_visual_context();
        let cached_commands = if skip_phase_capture || is_nested {
            None
        } else {
            self.valid_cached_commands(paintable, phase)
        };
        if let Some((source, cached_range, recorded_context)) = cached_commands {
            let destination_range =
                self.recorder
                    .append_cached_command_range(&source.display_list, cached_range, recorded_context);
            self.log_command_byte_capture_for_verification(
                paintable,
                CaptureKind::BoxPhase(phase),
                destination_range,
                true,
            );
            if cache_writes_enabled {
                self.set_cached_commands(paintable, phase, destination_range, phase_context);
            }
        } else {
            let command_range_start = self.recorder.byte_size();
            self.trace_paint(
                Operation::Capture(CaptureSite {
                    paintable,
                    kind: CaptureKind::BoxPhase(phase),
                }),
                |this| this.paint(paintable, phase),
            );
            let command_range_end = self.recorder.byte_size();
            let command_range = CommandRange {
                offset: command_range_start as u32,
                size: (command_range_end - command_range_start) as u32,
            };
            if !is_nested {
                self.log_command_byte_capture_for_verification(
                    paintable,
                    CaptureKind::BoxPhase(phase),
                    command_range,
                    false,
                );
            }
            if !skip_phase_capture && cache_writes_enabled {
                self.set_cached_commands(paintable, phase, command_range, phase_context);
            }
        }

        self.recorder.set_accumulated_visual_context(ContextRef::default());
    }

    fn valid_cached_commands(
        &mut self,
        paintable: NodeSlotId,
        phase: PaintPhase,
    ) -> Option<(Rc<RecordingOutput>, CommandRange, ContextRef)> {
        if !self.cache_compatibility.commands {
            return None;
        }
        let cache = self.layout_arena.paintable_paint_cache_if_allocated(paintable)?;
        // Checked before loading the entry so a dirty row's miss stays as cheap as the
        // absent-entry miss the eager clearing model produced.
        if self.all_paint_caches_dirty || cache.is_self_dirty_since(self.completed_record_gen) {
            return None;
        }
        let entry = cache.commands(phase)?;
        let captured_position = cache.captured_absolute_position();
        drop(cache);
        if captured_position != self.current_absolute_position(paintable) {
            return None;
        }
        let offset = self
            .resolve_capture_address_in_source_tape(entry.address)?
            .command_byte_offset;
        let source = self.command_cache_source.as_ref()?;
        Some((
            source.clone(),
            CommandRange {
                offset,
                size: entry.command_byte_count,
            },
            entry.recorded_context,
        ))
    }

    fn append_spliced_hit_test_item(&mut self, spliced: &HitTestItem) {
        let mut item = spliced.clone();
        if !item.block_container.is_invalid() {
            item.block_container_margin_rect = self.containing_block_margin_rect(item.block_container);
        }
        if item.kind == HitTestItemKind::Box {
            (item.caret_line_index, item.caret_line_rect) = self.containing_line_of_box(item.paintable);
        }
        self.list.append(item);
    }

    fn append_spliced_hit_test_items(&mut self, items: &[HitTestItem], spliced_subtree_root: NodeSlotId) {
        let mut last_container_lookup: Option<(NodeSlotId, bool)> = None;
        for item in items {
            let container = item.block_container;
            let container_is_inside_spliced_subtree = if container.is_invalid() {
                false
            } else if let Some((memoized, inside)) = last_container_lookup
                && memoized == container
            {
                inside
            } else {
                let inside = self.is_inclusive_paint_descendant(container, spliced_subtree_root);
                last_container_lookup = Some((container, inside));
                inside
            };
            if container_is_inside_spliced_subtree {
                self.list.append(item.clone());
            } else {
                self.append_spliced_hit_test_item(item);
            }
        }
    }

    fn is_inclusive_paint_descendant(&self, node: NodeSlotId, root: NodeSlotId) -> bool {
        let mut climbing_from_node = Some(node);
        let mut climbing_from_root = Some(root);
        loop {
            match (climbing_from_node, climbing_from_root) {
                (Some(current), _) if current == root => return true,
                (_, Some(current)) if current == node => return false,
                (None, None) => return false,
                _ => {}
            }
            climbing_from_node =
                climbing_from_node.and_then(|slot| crate::painting::paint_order::paint_parent(self.layout_arena, slot));
            climbing_from_root =
                climbing_from_root.and_then(|slot| crate::painting::paint_order::paint_parent(self.layout_arena, slot));
        }
    }

    fn valid_cached_hit_test_items(
        &mut self,
        paintable: NodeSlotId,
        phase: PaintPhase,
        own_context: ContextRef,
        for_descendants_context: ContextRef,
    ) -> Option<(Rc<Vec<HitTestItem>>, usize, usize)> {
        if !self.cache_compatibility.hit_test_items {
            return None;
        }
        let cache = self.layout_arena.paintable_paint_cache_if_allocated(paintable)?;
        if self.all_paint_caches_dirty || cache.is_self_dirty_since(self.completed_record_gen) {
            return None;
        }
        let entry = cache.hit_test_items(phase)?;
        if entry.recorded_context != own_context || entry.recorded_context_for_descendants != for_descendants_context {
            return None;
        }
        let captured_position = cache.captured_absolute_position();
        drop(cache);
        if captured_position != self.current_absolute_position(paintable) {
            return None;
        }
        let start = self
            .resolve_capture_address_in_source_tape(entry.address)?
            .hit_test_item_index;
        let source = self.item_cache_source.as_ref()?;
        Some((source.items.clone(), start as usize, entry.count as usize))
    }

    fn set_cached_commands(
        &mut self,
        paintable: NodeSlotId,
        phase: PaintPhase,
        range: CommandRange,
        recorded_context: ContextRef,
    ) {
        debug_assert!(
            self.captured_range_embeds_no_scroll_node_index_payload(range),
            "a per-phase paint capture must not embed scroll node indices"
        );
        debug_assert!(
            self.captured_range_references_only_the_phase_context(paintable, range, recorded_context),
            "a per-phase paint capture records under its phase context or without clips and effects"
        );
        let absolute_position = self.current_absolute_position(paintable);
        self.cache_updates.set_commands(
            paintable,
            absolute_position,
            phase,
            crate::painting::record::cache::CachedBoxPhaseCommands {
                address: self.address_relative_to_innermost_open_capture(range.offset, self.list.items.len() as u32),
                command_byte_count: range.size,
                recorded_context,
            },
        );
    }

    #[cfg(debug_assertions)]
    fn captured_range_references_only_the_phase_context(
        &self,
        _paintable: NodeSlotId,
        range: CommandRange,
        recorded_context: ContextRef,
    ) -> bool {
        let bytes = &self.recorder.bytes()[range.offset as usize..(range.offset + range.size) as usize];
        let mut only_the_phase_context = true;
        crate::painting::display_list::builder::for_each_command(bytes, |header, _, _| {
            let context = header.context;
            only_the_phase_context &= context.spatial == recorded_context.spatial
                && ((context.clip.is_none() && context.effect.is_none())
                    || (context.clip == recorded_context.clip && context.effect == recorded_context.effect));
        });
        only_the_phase_context
    }

    #[cfg(not(debug_assertions))]
    fn captured_range_references_only_the_phase_context(
        &self,
        _paintable: NodeSlotId,
        _range: CommandRange,
        _recorded_context: ContextRef,
    ) -> bool {
        true
    }

    #[cfg(debug_assertions)]
    fn captured_range_embeds_no_scroll_node_index_payload(&self, range: CommandRange) -> bool {
        use crate::painting::display_list::commands::DisplayListCommandType;
        let bytes = &self.recorder.bytes()[range.offset as usize..(range.offset + range.size) as usize];
        let mut embeds_no_scroll_node_index = true;
        crate::painting::display_list::builder::for_each_command(bytes, |header, _, _| {
            embeds_no_scroll_node_index &= !header.command_type.is_compositor_metadata()
                && header.command_type != DisplayListCommandType::PaintScrollBar;
        });
        embeds_no_scroll_node_index
    }

    #[cfg(not(debug_assertions))]
    fn captured_range_embeds_no_scroll_node_index_payload(&self, _range: CommandRange) -> bool {
        true
    }

    fn set_cached_hit_test_items(
        &mut self,
        paintable: NodeSlotId,
        phase: PaintPhase,
        start: usize,
        count: usize,
        recorded_context: ContextRef,
        recorded_context_for_descendants: ContextRef,
    ) {
        let absolute_position = self.current_absolute_position(paintable);
        self.cache_updates.set_hit_test_items(
            paintable,
            absolute_position,
            phase,
            crate::painting::record::cache::CachedBoxPhaseHitTestItems {
                address: self
                    .address_relative_to_innermost_open_capture(self.recorder.byte_size() as u32, start as u32),
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
