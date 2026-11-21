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
use crate::painting::display_list::recorder::{DisplayListRecorder, RecordNotes};
use crate::painting::node_painting;
use crate::painting::paint_order_plan::PaintScope;
use crate::painting::paint_read::{GeometryRead, PaintSource};
use crate::painting::record::RecordingInputs;
use crate::painting::record::assemble::{Assembler, AssemblyHost, Coverage, frame_is_unchanged};
use crate::painting::record::frame_inputs::FrameInputs;
use crate::painting::record::order_tree::{ProducerKind, RecordedChunk};
use crate::painting::record::paint::background_resolution::root_background_canvas_rect;
use crate::painting::record::resources::RecordingResourceManifest;
use crate::painting::record::scratch::RecordingScratch;
use crate::painting::record::svg_resources::MaskLayerSet;
use crate::painting::record::trace::{Action, Operation};
use crate::painting::record::window::{RecordedWindow, WindowSpaces};
use crate::painting::record::{RecordingOutput, RecordingResult};
use libcompositing_rust::display_list::window::spaces_place_alike;
use std::sync::Arc;

pub(crate) fn record_display_list(
    source: &PaintSource<'_>,
    scratch: &mut RecordingScratch,
    viewport: NodeSlotId,
    inputs: &RecordingInputs,
    source_recording: Option<Arc<RecordingOutput>>,
    window: Option<Arc<RecordedWindow>>,
    trace: bool,
) -> RecordingResult {
    scratch.begin_recording(source.frame().paintable_row_capacity());
    macro_rules! record {
        ($observer:ty) => {
            record_display_list_impl::<$observer>(source, scratch, viewport, inputs, source_recording, window)
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
    window: Option<Arc<RecordedWindow>>,
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
    // Only output that read an accumulated scale depends on one, so only a published recording that holds such
    // output is asked which scales it read.
    let accumulated_scales_changed = source_recording
        .as_ref()
        .and_then(|frame| frame.accumulated_scales)
        .is_some_and(|published_scales| accumulated_scales_of(paint_state) != Some(published_scales));
    let force_dark_settings = inputs.force_dark_enabled.then_some(inputs.force_dark_settings);
    // A recording of a frame without a visual context tree holds all of it.
    let window = window.filter(|_| paint_state.visual_context_tree.is_some());
    // The published recording left out what this one leaves out where it was made for the same window and every space
    // places what it holds where it did then.
    let published_holds_what_this_holds = |window: &RecordedWindow, published: &RecordingOutput| {
        published
            .window
            .as_ref()
            .is_some_and(|published_window| **published_window == *window)
            && published
                .visual_context_tree
                .as_deref()
                .zip(paint_state.visual_context_tree.as_deref())
                .is_some_and(|(published_tree, tree)| spaces_place_alike(published_tree, tree))
    };
    let coverage = match (&window, source_recording.as_deref()) {
        (None, _) => Coverage::WholeDocument,
        (Some(window), Some(published)) if published_holds_what_this_holds(window, published) => {
            Coverage::PublishedWindow
        }
        (Some(_), _) => Coverage::AnotherWindow,
    };
    let window_spaces = window
        .clone()
        .zip(paint_state.visual_context_tree.as_deref())
        .map(|(window, tree)| WindowSpaces::new(window, tree));
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
        producer_reads_accumulated_scale: std::cell::Cell::new(false),
        line_root_positions: None,
        blocking_wheel_event_region_count: 0,
        records_per_box_wheel_hit_test_targets: frame_inputs.records_per_box_wheel_hit_test_targets(),
        observer: O::default(),
        scratch,
        resources: RecordingResourceManifest::default(),
        window_spaces,
        record_notes_of_chunks: Vec::new(),
    };
    if inputs.notes_record_origins {
        recorder.recorder.note_record_origins();
    }
    let root_group_color = recorder.record_canvas();
    let prologue = recorder.take_run_chunk();
    let prologue_id = prologue.as_ref().map(|chunk| chunk.recorded().id);
    let has_inspector_overlays = inputs.inspector_highlight.is_some()
        || inputs.grid_overlays.is_some()
        || !inputs.flex_overlays.is_empty()
        || inputs.caret_debug_rect.is_some();
    let find_in_page_current_match_bounces =
        inputs.dims_page_for_find_in_page && inputs.find_in_page_current_match_scale != 1.0;
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
            frame_is_unchanged(
                root,
                anything_changed,
                window_focus_changed,
                accumulated_scales_changed,
                coverage,
            ) && !has_inspector_overlays
                && frame.prologue_id == prologue_id
                && !find_in_page_current_match_bounces
                && !frame.find_in_page_current_match_bounces
                && frame.find_in_page_dim_opacity == inputs.find_in_page_dim_opacity
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
            let recorded_root = Assembler::new(
                &mut recorder,
                window_focus_changed,
                accumulated_scales_changed,
                coverage,
            )
            .assemble_root(root_scope, published_root.as_ref());
            let mut chunks: Vec<Arc<RecordedChunk>> = prologue.into_iter().collect();
            recorded_root.for_each_chunk(&mut |chunk| chunks.push(Arc::clone(chunk)));
            if inputs.dims_page_for_find_in_page {
                chunks.extend(
                    crate::painting::record::paint::find_in_page_overlay::record_find_in_page_overlay(&mut recorder),
                );
            }
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
    let record_notes = recorder
        .recorder
        .notes_record_origins()
        .then(|| Arc::new(record_notes_of_tape(&chunks, &mut recorder.record_notes_of_chunks)));
    let accumulated_scales = recorded_root
        .as_ref()
        .filter(|root| root.facts.reads_accumulated_scale)
        .and_then(|_| accumulated_scales_of(paint_state));
    let output = RecordingOutput {
        recorded_structural_epoch: structural_epoch,
        recorded_as_of: source.frame().paint_generation(),
        visual_context_tree: paint_state.visual_context_tree.clone(),
        frame_inputs,
        root_background_canvas_rect,
        prologue_id,
        find_in_page_current_match_bounces,
        find_in_page_dim_opacity: inputs.find_in_page_dim_opacity,
        root_group_color,
        hit_test_items_stamp: source.hit_test_items_stamp(),
        chunks,
        display_list,
        recorded_root,
        window: window.clone(),
        has_blocking_wheel_event_listeners,
        wheel_event_listener_state_generation: inputs.wheel_event_listener_state_generation,
        is_identical_to_published_recording: false,
        window_is_focused: inputs.window_is_focused,
        accumulated_scales,
        capture_log: recorder.observer.finish(),
        vector_image_ids_by_request: Vec::new(),
        resources: Default::default(),
        display_list_id: 0,
        surface_clear_color: None,
        async_scrolling_metadata: None,
        record_notes,
    };
    let resources = recorder.resources;
    let mut record_from_scratch = |window: Option<Arc<RecordedWindow>>| {
        record_display_list_impl::<super::trace::NoTrace>(source, scratch, viewport, inputs, None, window).output
    };
    let verifies_reused_output = reuses_from_published_recording && reused_output_is_verified();
    let window_to_verify = window
        .clone()
        .filter(|_| reuses_from_published_recording && left_out_content_is_verified());
    let from_scratch =
        (verifies_reused_output || window_to_verify.is_some()).then(|| record_from_scratch(window.clone()));
    if verifies_reused_output && let Some(from_scratch) = &from_scratch {
        require_same_recording(&output, from_scratch);
    }
    if let Some(window) = window_to_verify
        && let Some(tree) = paint_state.visual_context_tree.as_deref()
    {
        let of_whole_document = record_from_scratch(None);
        require_only_what_misses_the_window_is_left_out(
            from_scratch.as_ref().unwrap_or(&output),
            &of_whole_document,
            &WindowSpaces::new(window, tree),
        );
    }
    RecordingResult { output, resources }
}

/// The notes of the chunks of a tape, by the offsets of their records in it. A chunk the recording copied has none.
fn record_notes_of_tape(
    chunks: &[Arc<RecordedChunk>],
    record_notes_of_chunks: &mut Vec<(Arc<RecordedChunk>, RecordNotes)>,
) -> RecordNotes {
    let mut notes = RecordNotes::default();
    let mut chunk_start = 0;
    for chunk in chunks {
        let chunk_size = chunk.latest().records.len();
        if chunk_size == 0 {
            continue;
        }
        if let Some(index) = record_notes_of_chunks
            .iter()
            .position(|(noted_chunk, _)| Arc::ptr_eq(noted_chunk, chunk))
        {
            let (_, chunk_notes) = record_notes_of_chunks.swap_remove(index);
            notes.append_shifted_by(
                chunk_notes,
                u32::try_from(chunk_start).expect("display list exceeds u32"),
            );
        }
        chunk_start += chunk_size;
    }
    notes
}

fn left_out_content_is_verified() -> bool {
    static VERIFIED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *VERIFIED.get_or_init(|| cfg!(debug_assertions) || std::env::var_os("LIBWEB_VERIFY_PAINT_WINDOW").is_some())
}

/// Requires a recording of a window, made from scratch, to lack only what misses the window: its tape is the tape
/// of a recording of the whole document without some of its chunks, and no command in those draws where the window
/// reaches. The commands that only describe the document to the compositor draw nothing.
fn require_only_what_misses_the_window_is_left_out(
    of_window: &RecordingOutput,
    of_whole_document: &RecordingOutput,
    spaces: &WindowSpaces<'_>,
) {
    use crate::painting::display_list::builder::{bounding_rect_computed_from_payload, for_each_command};
    use crate::painting::display_list::chunks::chunks_of;
    let (windowed, whole) = (&of_window.display_list, &of_whole_document.display_list);
    let mut chunks_of_window = chunks_of(&windowed.bytes, &windowed.command_runs, &windowed.chunks).peekable();
    for (chunk, bytes, runs) in chunks_of(&whole.bytes, &whole.command_runs, &whole.chunks) {
        let is_the_same_chunk = chunks_of_window
            .next_if(|(kept, kept_bytes, kept_runs)| {
                kept.id == chunk.id
                    || (kept_bytes.len() == bytes.len()
                        && kept_runs.len() == runs.len()
                        && without_vector_image_ids(kept_bytes) == without_vector_image_ids(bytes))
            })
            .is_some();
        if is_the_same_chunk {
            continue;
        }
        let chunk_start = chunk.byte_end - bytes.len() as u32;
        for run in runs {
            let run_start = (run.offset - chunk_start) as usize;
            let run_bytes = &bytes[run_start..run_start + run.size as usize];
            for_each_command(run_bytes, |header, _, payload| {
                if header.command_type.is_compositor_metadata() {
                    return;
                }
                let Some(bounding_rect) = bounding_rect_computed_from_payload(header, payload) else {
                    return;
                };
                assert!(
                    spaces.shows_in_window(bounding_rect.to_float(), run.context) != Some(true),
                    "A recording of a window left out {} at {:?} under {:?}, which shows in the window",
                    header.command_type.name(),
                    bounding_rect,
                    run.context
                );
            });
        }
    }
    assert!(
        chunks_of_window.next().is_none(),
        "A recording of a window holds a chunk that a recording of the whole document lacks"
    );
}

/// The bytes of a tape without the ids of the SVG image records it draws. A command that paints an SVG image is
/// recorded with a placeholder for the image's records, which publication replaces with the id of the records the host
/// rendered, and the placeholder follows how many images the recording met before. A reused command names the records
/// already.
fn without_vector_image_ids(bytes: &[u8]) -> Vec<u8> {
    use crate::painting::display_list::commands::{DisplayListCommandType, PaintVectorImage};
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
}

/// A digest of the scale that each space of the tree accumulates, or none without a tree.
fn accumulated_scales_of(paint_state: &crate::painting::published_frame::PublishedPaintState) -> Option<u64> {
    use crate::painting::display_list::commands::SpatialNodeIndex;
    use crate::painting::visual_context::IncludeVisualViewportTransform;
    use std::hash::Hasher;
    let tree = paint_state.visual_context_tree.as_deref()?;
    let world_transforms = paint_state.unscrolled_world_transforms();
    let mut digest = crate::fast_hash::fast_hasher();
    for index in 0..tree.spatial_nodes.len() {
        let scale =
            world_transforms.accumulated_2d_scale(SpatialNodeIndex(index as u32), IncludeVisualViewportTransform::No);
        digest.write_u32(scale.width.to_bits());
        digest.write_u32(scale.height.to_bits());
    }
    Some(digest.finish())
}

fn reused_output_is_verified() -> bool {
    static VERIFIED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *VERIFIED.get_or_init(|| cfg!(debug_assertions) || std::env::var_os("LIBWEB_VERIFY_PAINT_REUSE").is_some())
}

/// Requires a recording that reused clean output of the published one to be the recording the same frame gives when
/// everything is recorded: a difference is output that was reused though what it shows changed.
fn require_same_recording(assembled: &RecordingOutput, from_scratch: &RecordingOutput) {
    use crate::painting::display_list::builder::{bounding_rect_computed_from_payload, for_each_command};
    use crate::painting::display_list::chunks::{chunk_id, chunks_of};
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
            for_each_command(bytes, |header, offset, payload| {
                if offset <= first_differing_byte {
                    found = Some((
                        offset,
                        header.command_type.name(),
                        bounding_rect_computed_from_payload(header, payload),
                    ));
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
        // A text content element paints in document order: its text that follows a child element paints after that
        // child.
        // FIXME: Hit testing still tests all of the element's own text after its child elements.
        let is_text_box =
            self.source.node_kind_if_live(paintable) == Some(crate::layout::node_data::NodeKind::SVGTextBox);
        let mut next_text_part = if is_text_box {
            crate::painting::record::paint::svg::text_parts_before_children(self, paintable)
        } else {
            None
        };
        let mut next_child = crate::painting::paint_order::first_paint_child(self.source, paintable);
        while let Some(child) = next_child {
            next_child = crate::painting::paint_order::next_paint_sibling(self.source, child);
            // A child that establishes a stacking context is painted by that context.
            if !self.has_stacking_context(child) {
                self.paint_svg_box(child);
            }
            if let Some(next_text_part) = &mut next_text_part {
                let context = self.own_context(paintable);
                self.recorder.set_accumulated_visual_context(context);
                crate::painting::record::paint::svg::paint_text_after_child(self, paintable, child, next_text_part);
            }
        }
        if let Some(next_text_part) = next_text_part {
            let context = self.own_context(paintable);
            self.recorder.set_accumulated_visual_context(context);
            crate::painting::record::paint::svg::paint_text_after_children(self, paintable, next_text_part);
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
