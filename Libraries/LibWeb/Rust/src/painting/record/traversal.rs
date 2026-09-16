/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::record::trace::Observer;

use super::{PaintPhase, PaintRecorder};
use crate::css::style::fast_hash::FastSet;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::layout::node_facts;
use crate::painting::display_list::commands::ContextRef;
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::display_list::recorder::DisplayListRecorder;
use crate::painting::hit_test::HitTestList;
use crate::painting::node_painting;
use crate::painting::paint_order_plan::PaintScope;
use crate::painting::record::RecordingInputs;
use crate::painting::record::assemble::{Assembler, frame_is_unchanged};
use crate::painting::record::cache::HitTestItemCacheSource;
use crate::painting::record::cache_compatibility::PaintCacheInputs;
use crate::painting::record::order_tree::{PaintOrderTree, ProducerKind};
use crate::painting::record::resources::RecordingResourceManifest;
use crate::painting::record::scratch::RecordingScratch;
use crate::painting::record::svg_resources::MaskLayerSet;
use crate::painting::record::trace::{Action, Operation};
use crate::painting::record::{RecordingOutput, RecordingResult};
use std::rc::Rc;
use std::sync::Arc;

pub(crate) use crate::painting::paint_order_plan::StackingContextPaintPhase;

#[allow(clippy::too_many_arguments)]
pub(crate) fn record_display_list(
    layout_arena: &LayoutNodeArena,
    paint_state: &crate::painting::paint_state::PaintState,
    scratch: &mut RecordingScratch,
    tree: &mut PaintOrderTree,
    viewport: NodeSlotId,
    inputs: &RecordingInputs<'_>,
    hit_test_list_generation: u64,
    source_frame: Option<Rc<RecordingOutput>>,
    source_items: Option<Rc<HitTestItemCacheSource>>,
    plan_from_prepared_inputs: bool,
    trace: bool,
) -> RecordingResult {
    scratch.begin_recording(layout_arena.paintable_row_count());
    macro_rules! record {
        ($observer:ty) => {
            record_display_list_impl::<$observer>(
                layout_arena,
                paint_state,
                scratch,
                tree,
                viewport,
                inputs,
                hit_test_list_generation,
                source_frame,
                source_items,
                plan_from_prepared_inputs,
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
    tree: &mut PaintOrderTree,
    viewport: NodeSlotId,
    inputs: &RecordingInputs<'_>,
    hit_test_list_generation: u64,
    source_frame: Option<Rc<RecordingOutput>>,
    source_items: Option<Rc<HitTestItemCacheSource>>,
    plan_from_prepared_inputs: bool,
) -> RecordingResult {
    debug_assert!(
        inputs.paint_command_cache_read_write || source_frame.is_none(),
        "a recording that publishes nothing has no published frame to copy from"
    );
    let structural_epoch = paint_state.visual_context.structural_epoch();
    let paintable_rows = layout_arena.paintable_rows();
    let cache_inputs = PaintCacheInputs::from_recording_inputs(&paintable_rows, inputs, paint_state);
    // The published frame is copied from while every input its producers read is unchanged and
    // no push asked for everything; otherwise this frame records from scratch.
    let source_is_usable = source_frame
        .as_ref()
        .is_some_and(|frame| frame.cache_inputs == cache_inputs)
        && source_items.is_some()
        && !layout_arena.paint_damage_covers_everything()
        && !layout_arena.scroll_metadata_damaged_everywhere();
    let (source_frame, source_items) = if source_is_usable {
        (source_frame, source_items)
    } else {
        (None, None)
    };
    let force_dark_settings = inputs.force_dark_enabled.then_some(inputs.force_dark_settings);
    let mut recorder = PaintRecorder {
        layout_arena: &paintable_rows,
        paint_state,
        inputs,
        recorder: DisplayListRecorder::new(force_dark_settings),
        converter: DevicePixelConverter::new(inputs.device_pixels_per_css_pixel),
        svg_resource_walk: None,
        viewport,
        source_frame,
        source_items,
        live_producer: false,
        plan_from_prepared_inputs,
        moved_expansion: FastSet::default(),
        blocking_wheel_event_region_count: 0,
        observer: O::default(),
        list: HitTestList {
            item_capacity_hint_from_previous_list: paint_state
                .hit_test_list
                .as_ref()
                .map_or(0, |list| list.items.len()),
            ..HitTestList::default()
        },
        scratch,
        resources: RecordingResourceManifest::default(),
    };
    recorder
        .observer
        .observe(|log| log.damage = Some(layout_arena.paint_damage_summary()));
    recorder.record_canvas();
    let prologue_bytes = u32::try_from(recorder.recorder.byte_size()).expect("display list exceeds u32");
    let has_inspector_overlays = inputs.inspector_highlight.is_some()
        || inputs.grid_overlays.is_some()
        || !inputs.flex_overlays.is_empty()
        || inputs.caret_debug_rect.is_some();
    let root_scope = PaintScope::stacking_context(viewport);
    // Nothing was pushed and nothing records every frame: the published tape and items are
    // this frame, which lets the compositor skip its update as well.
    let unchanged_frame = recorder
        .source_frame
        .as_ref()
        .zip(recorder.source_items.as_ref())
        .filter(|(frame, _)| {
            frame_is_unchanged(tree, layout_arena.has_paint_damage())
                && !has_inspector_overlays
                && frame.prologue_bytes == prologue_bytes
                && recorder.recorder.bytes() == &frame.display_list.bytes[..prologue_bytes as usize]
        })
        .map(|(frame, items)| (frame.display_list.clone(), items.items.clone()));
    let display_list = match unchanged_frame {
        Some((display_list, items)) => {
            recorder
                .observer
                .observe(|log| log.leaf(Operation::Scope(root_scope), Action::Copy, false));
            recorder.list.items = items;
            recorder.blocking_wheel_event_region_count = tree.root_entry().output().blocking_wheel_event_regions;
            display_list
        }
        None => {
            let source_prologue_bytes = recorder.source_frame.as_ref().map(|frame| frame.prologue_bytes);
            Assembler::new(&mut recorder, tree, source_prologue_bytes).assemble_root(root_scope);
            if has_inspector_overlays {
                recorder.trace_paint(
                    Operation::Named(None, "inspector-overlays"),
                    crate::painting::record::paint::inspector_overlay::record_inspector_overlays,
                );
            }
            Arc::new(recorder.recorder.into_builder().finish())
        }
    };
    let mut hit_test_list = recorder.list;
    hit_test_list.generation = hit_test_list_generation;
    let output = RecordingOutput {
        recorded_structural_epoch: structural_epoch,
        cache_inputs,
        prologue_bytes,
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
    }
}

impl<O: Observer> PaintRecorder<'_, O> {
    // SVG content below an SVG root is recorded by the root's producer; every box inside
    // shows up nested under it.
    fn paint_svg_box(&mut self, svg_box: NodeSlotId, phase: PaintPhase) {
        self.trace_scope(Operation::Named(Some(svg_box), "svg"), Action::Record, |this| {
            this.paint_svg_box_impl(svg_box, phase);
        });
    }

    pub(crate) fn paint_svg_box_impl(&mut self, svg_box: NodeSlotId, phase: PaintPhase) {
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
            self.mark_live_producer();
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
                Operation::Producer(svg_box, ProducerKind::HitForeground),
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
            self.trace_paint(Operation::Producer(svg_box, ProducerKind::DrawBackground), |this| {
                crate::painting::record::paint::paint(this, svg_box, PaintPhase::Background);
            });
        }
        self.trace_paint(Operation::Producer(svg_box, ProducerKind::DrawForeground), |this| {
            crate::painting::record::paint::paint(this, svg_box, PaintPhase::Foreground);
        });
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

    pub(crate) fn context_for_phase(&self, paintable: NodeSlotId, phase: PaintPhase) -> ContextRef {
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

    pub(crate) fn paint(&mut self, paintable: NodeSlotId, phase: PaintPhase) {
        crate::painting::record::paint::paint(self, paintable, phase);
    }
}
