/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::record::trace::Observer;

use super::{PaintPhase, PaintRecorder};
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::builder::RecordedDisplayList;
use crate::painting::display_list::commands::ContextRef;
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::display_list::recorder::DisplayListRecorder;
use crate::painting::node_painting;
use crate::painting::paint_order_plan::PaintScope;
use crate::painting::paint_read::{GeometryRead, PaintSource};
use crate::painting::record::RecordingInputs;
use crate::painting::record::assemble::{Assembler, AssemblyHost, frame_is_unchanged};
use crate::painting::record::frame_inputs::FrameInputs;
use crate::painting::record::order_tree::{ProducerKind, RecordedChunk};
use crate::painting::record::paint::background_resolution::root_background_canvas_rect;
use crate::painting::record::resources::RecordingResourceManifest;
use crate::painting::record::scratch::RecordingScratch;
use crate::painting::record::svg_resources::MaskLayerSet;
use crate::painting::record::trace::{Action, Operation};
use crate::painting::record::{RecordingOutput, RecordingResult};
use std::sync::Arc;

pub(crate) fn record_display_list(
    source: &PaintSource<'_>,
    scratch: &mut RecordingScratch,
    viewport: NodeSlotId,
    inputs: &RecordingInputs,
    source_recording: Option<Arc<RecordingOutput>>,
    trace: bool,
) -> RecordingResult {
    scratch.begin_recording(source.frame().paintable_row_capacity());
    macro_rules! record {
        ($observer:ty) => {
            record_display_list_impl::<$observer>(source, scratch, viewport, inputs, source_recording)
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

/// The tape of a display list of `chunks`, each as its recording or the publication of that recording left it.
fn tape_of(chunks: &[Arc<RecordedChunk>]) -> Arc<RecordedDisplayList> {
    Arc::new(RecordedDisplayList::of_chunks(
        chunks.iter().map(|chunk| &**chunk.latest()),
    ))
}

fn record_display_list_impl<O: Observer>(
    source: &PaintSource<'_>,
    scratch: &mut RecordingScratch,
    viewport: NodeSlotId,
    inputs: &RecordingInputs,
    source_recording: Option<Arc<RecordingOutput>>,
) -> RecordingResult {
    let paint_state = source.frame().paint_state();
    let structural_epoch = paint_state.structural_epoch();
    let frame_inputs = FrameInputs::from_recording_inputs(inputs, paint_state);
    let root_background_canvas_rect = root_background_canvas_rect(
        source,
        inputs.uncaptured.root_background_source.root_layout_node,
        inputs.css_viewport_rect,
    );
    // The published recording is reused from while every input its producers read is unchanged and
    // nothing asked for everything to paint anew since; otherwise this frame records from scratch.
    let source_is_usable = source_recording.as_ref().is_some_and(|published| {
        published.frame_inputs == frame_inputs && !source.frame().everything_paints_anew_since(published.recorded_as_of)
    });
    let source_recording = source_recording.filter(|_| source_is_usable);
    let window_focus_changed = source_recording
        .as_ref()
        .is_some_and(|frame| frame.window_is_focused != inputs.window_is_focused);
    let force_dark_settings = inputs.force_dark_enabled.then_some(inputs.force_dark_settings);
    let mut recorder = PaintRecorder {
        source,
        paint_state,
        inputs,
        recorder: DisplayListRecorder::new(force_dark_settings),
        converter: DevicePixelConverter::new(inputs.device_pixels_per_css_pixel),
        svg_resource_walk: None,
        viewport,
        ink_bounds_audit: super::ink_bounds::InkBoundsAudit::new(),
        source_recording,
        run_draws_vector_image_placeholders: false,
        live_producer: false,
        producer_reads_window_focus: std::cell::Cell::new(false),
        line_root_positions: None,
        blocking_wheel_event_region_count: 0,
        records_per_box_wheel_hit_test_targets: frame_inputs.records_per_box_wheel_hit_test_targets(),
        observer: O::default(),
        scratch,
        resources: RecordingResourceManifest::default(),
    };
    let root_group_color = recorder.record_canvas();
    let prologue = recorder.take_run_chunk();
    let prologue_id = prologue.as_ref().map(|chunk| chunk.recorded().id);
    let has_inspector_overlays = inputs.inspector_highlight.is_some()
        || inputs.grid_overlays.is_some()
        || !inputs.flex_overlays.is_empty()
        || inputs.caret_debug_rect.is_some();
    let root_scope = PaintScope::stacking_context(viewport);
    // The scopes the published recording walked, where it can be reused from.
    let published_root = recorder
        .source_recording
        .as_ref()
        .and_then(|frame| frame.recorded_root.clone());
    // Nothing changed and nothing records every frame: the published chunks are this frame's, which lets the
    // compositor skip its update as well.
    let unchanged_frame = recorder
        .source_recording
        .as_ref()
        .zip(published_root.as_ref())
        .filter(|(frame, root)| {
            let anything_changed = source
                .paint_change_stamps(viewport)
                .row_or_anything_below_it_changed_since(frame.recorded_as_of);
            frame_is_unchanged(root, anything_changed, window_focus_changed)
                && !has_inspector_overlays
                && frame.prologue_id == prologue_id
        })
        .map(|(frame, root)| {
            (
                Arc::clone(&frame.chunks),
                Arc::clone(&frame.display_list),
                Arc::clone(root),
            )
        });
    let reuses_from_published_recording = published_root.is_some() && unchanged_frame.is_none();
    let (chunks, display_list, recorded_root) = match unchanged_frame {
        Some((chunks, display_list, root)) => {
            recorder
                .observer
                .observe(|log| log.leaf(Operation::Scope(root_scope), Action::Copy, false));
            (chunks, display_list, Some(root))
        }
        None if inputs.records_without_visual_contexts => {
            recorder.trace_paint(Operation::Named(None, "svg-image-document"), |recorder| {
                recorder.record_svg_image_document(viewport);
            });
            let chunks: Arc<[Arc<RecordedChunk>]> = prologue.into_iter().chain(recorder.take_run_chunk()).collect();
            let display_list = tape_of(&chunks);
            (chunks, display_list, None)
        }
        None => {
            let recorded_root =
                Assembler::new(&mut recorder, window_focus_changed).assemble_root(root_scope, published_root.as_ref());
            let mut chunks: Vec<Arc<RecordedChunk>> = prologue.into_iter().collect();
            recorded_root.for_each_chunk(&mut |chunk| chunks.push(Arc::clone(chunk)));
            if has_inspector_overlays {
                recorder.trace_paint(
                    Operation::Named(None, "inspector-overlays"),
                    crate::painting::record::paint::inspector_overlay::record_inspector_overlays,
                );
                chunks.extend(recorder.take_run_chunk());
            }
            let chunks: Arc<[Arc<RecordedChunk>]> = chunks.into();
            let display_list = tape_of(&chunks);
            (chunks, display_list, Some(recorded_root))
        }
    };
    recorder.ink_bounds_audit.report();
    let has_blocking_wheel_event_listeners = match &recorded_root {
        Some(root) => root.facts.blocking_wheel_event_regions > 0,
        None => recorder.blocking_wheel_event_region_count > 0,
    };
    let output = RecordingOutput {
        recorded_structural_epoch: structural_epoch,
        recorded_as_of: source.frame().paint_generation(),
        visual_context_tree: paint_state.visual_context_tree.clone(),
        frame_inputs,
        root_background_canvas_rect,
        prologue_id,
        root_group_color,
        hit_test_items_stamp: source.hit_test_items_stamp(),
        chunks,
        display_list,
        recorded_root,
        has_blocking_wheel_event_listeners,
        wheel_event_listener_state_generation: inputs.wheel_event_listener_state_generation,
        is_identical_to_published_recording: false,
        window_is_focused: inputs.window_is_focused,
        capture_log: recorder.observer.finish(),
        vector_image_ids_by_request: Vec::new(),
        resources: Default::default(),
        display_list_id: 0,
        surface_clear_color: None,
        async_scrolling_metadata: None,
    };
    let resources = recorder.resources;
    if reuses_from_published_recording && reused_output_is_verified() {
        let from_scratch =
            record_display_list_impl::<super::trace::NoTrace>(source, scratch, viewport, inputs, None).output;
        require_same_recording(&output, &from_scratch);
    }
    RecordingResult { output, resources }
}

fn reused_output_is_verified() -> bool {
    static VERIFIED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *VERIFIED.get_or_init(|| cfg!(debug_assertions) || std::env::var_os("LIBWEB_VERIFY_PAINT_REUSE").is_some())
}

/// Requires a recording that reused clean output of the published one to be the recording the same frame gives when
/// everything is recorded: a difference is output that was reused though what it shows changed.
fn require_same_recording(assembled: &RecordingOutput, from_scratch: &RecordingOutput) {
    use crate::painting::display_list::builder::for_each_command;
    use crate::painting::display_list::chunks::{chunk_id, chunks_of};
    use crate::painting::display_list::commands::{DisplayListCommandType, PaintVectorImage};
    // A command that paints an SVG image is recorded with a placeholder for the image's records, which publication
    // replaces with the id of the records the host rendered. A reused command names those records already.
    let without_vector_image_ids = |bytes: &[u8]| {
        let mut bytes = bytes.to_vec();
        let id_field = std::mem::offset_of!(PaintVectorImage, vector_image_id);
        let mut id_offsets = Vec::new();
        crate::painting::display_list::nested_records::for_each_command_including_nested(
            &bytes,
            &mut |command_type, payload_offset, _| {
                if command_type == DisplayListCommandType::PaintVectorImage {
                    id_offsets.push(payload_offset + id_field);
                }
            },
        );
        for offset in id_offsets {
            bytes[offset..offset + std::mem::size_of::<u64>()].fill(0);
        }
        bytes
    };
    let (assembled_list, scratch_list) = (&assembled.display_list, &from_scratch.display_list);
    let assembled_bytes = without_vector_image_ids(&assembled_list.bytes);
    let scratch_bytes = without_vector_image_ids(&scratch_list.bytes);
    if assembled_bytes != scratch_bytes {
        let first_differing_byte = assembled_bytes
            .iter()
            .zip(&scratch_bytes)
            .position(|(copied, recorded)| copied != recorded)
            .unwrap_or(assembled_bytes.len().min(scratch_bytes.len()));
        let command_at = |bytes: &[u8]| {
            let mut found = None;
            for_each_command(bytes, |header, offset, _| {
                if offset <= first_differing_byte {
                    found = Some((offset, header.command_type.name(), header.bounding_rect));
                }
            });
            found
        };
        panic!(
            "A recording reused output that recording from scratch replaces: {} bytes against {}, differing from byte {first_differing_byte}, in {:?} where recording from scratch has {:?}",
            assembled_list.bytes.len(),
            scratch_list.bytes.len(),
            command_at(&assembled_list.bytes),
            command_at(&scratch_list.bytes)
        );
    }
    assert!(
        assembled_list.command_runs == scratch_list.command_runs,
        "A recording reused command runs that recording from scratch replaces"
    );
    let chunk_ends = |list: &RecordedDisplayList| -> Vec<(u32, u32)> {
        list.chunks
            .iter()
            .map(|chunk| (chunk.byte_end, chunk.run_end))
            .collect()
    };
    assert!(
        chunk_ends(assembled_list) == chunk_ends(scratch_list),
        "A recording cut its tape into other chunks than recording from scratch does"
    );
    for (chunk, bytes, runs) in chunks_of(
        &assembled_list.bytes,
        &assembled_list.command_runs,
        &assembled_list.chunks,
    ) {
        let chunk_start = chunk.byte_end - bytes.len() as u32;
        assert!(
            chunk.id == chunk_id(bytes, runs, chunk_start),
            "A recording reused a chunk under a name that is not the name of what it holds"
        );
    }
    assert!(
        assembled.has_blocking_wheel_event_listeners == from_scratch.has_blocking_wheel_event_listeners,
        "A recording reused wheel regions that recording from scratch replaces"
    );
}

impl<O: Observer> PaintRecorder<'_, O> {
    // SVG content below an SVG root is recorded by the root's producer; every box inside
    // shows up nested under it.
    fn paint_svg_box(&mut self, svg_box: NodeSlotId) {
        self.trace_scope(Operation::Named(Some(svg_box), "svg"), Action::Record, |this| {
            this.paint_svg_box_impl(svg_box);
        });
    }

    pub(crate) fn paint_svg_box_impl(&mut self, svg_box: NodeSlotId) {
        let context = self.own_context(svg_box);
        self.recorder.set_accumulated_visual_context(context);

        // For elements with SVG filters, emit a transparent FillRect to trigger filter application.
        // This ensures content-generating filters (feFlood, feImage) work even with empty source.
        if let Some(svg_filter_bounds) = self.source.committed_side_data(svg_box).svg_filter_bounds {
            let device_rect = self
                .converter
                .enclosing_device_rect(crate::css::css_pixels::CssPixelRect::from(svg_filter_bounds));
            self.recorder.fill_rect_transparent(device_rect);
        }

        if self.declare_mask_contents(svg_box, MaskLayerSet::SvgOnly) {
            return;
        }
        self.trace_paint(Operation::Producer(svg_box, ProducerKind::DrawForeground), |this| {
            crate::painting::record::paint::paint(this, svg_box, PaintPhase::Foreground);
        });
        self.svg_paint_descendants(svg_box);
    }

    fn svg_paint_descendants(&mut self, paintable: NodeSlotId) {
        let mut next_child = crate::painting::paint_order::first_paint_child(self.source, paintable);
        while let Some(child) = next_child {
            next_child = crate::painting::paint_order::next_paint_sibling(self.source, child);
            // A child that establishes a stacking context is painted by that context.
            if self.has_stacking_context(child) {
                continue;
            }
            self.paint_svg_box(child);
        }
    }

    pub(crate) fn for_descendants_context(&self, paintable: NodeSlotId) -> ContextRef {
        self.data(paintable).accumulated_visual_context_for_descendants
    }

    pub(crate) fn context_for_phase(&self, paintable: NodeSlotId, phase: PaintPhase) -> ContextRef {
        // Text fragments are content of the block container (or of a self-painting inline box).
        // They need the descendants' visual context, not the element's own visual context.
        let foreground_paints_descendant_content = node_painting::has_lines(self.source, paintable)
            || node_painting::is_fragmented_inline(self.source, paintable);
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
