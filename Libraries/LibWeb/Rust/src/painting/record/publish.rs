/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::painting::display_list::commands::{DisplayListCommandType, DisplayListResourceId, PaintNestedDisplayList};
use crate::painting::hit_test::HitTestList;
use crate::painting::host::FfiRecordingPublishCallbacks;
use crate::painting::paint_state::PendingRecording;
use crate::painting::record::recorder_state::RecorderState;
use crate::painting::record::resources::RecordingResourceManifest;
use crate::painting::record::vector_images::{
    VectorImageRenderRequest, is_vector_image_placeholder, vector_image_placeholder_index,
};
use crate::painting::record::{RecordingOutput, RecordingResult};
use crate::painting::recording_slot::Publication;
use crate::painting::svg_paint_resources::published_filter_image_frames_in;
use crate::stage::MainThread;

fn resolve_vector_image_placeholders(
    output: &mut RecordingOutput,
    requests: &[VectorImageRenderRequest],
    main_thread: &MainThread,
    publish: &FfiRecordingPublishCallbacks,
) {
    if requests.is_empty() {
        return;
    }
    let resolved_ids: Vec<u64> = requests
        .iter()
        .map(|request| publish.resolve_vector_image_display_list(main_thread, &request.to_ffi()))
        .collect();
    let display_list = std::sync::Arc::make_mut(&mut output.display_list);
    let id_field_offset = std::mem::offset_of!(PaintNestedDisplayList, display_list_id);
    let mut patch_offsets = Vec::new();
    crate::painting::display_list::nested_records::for_each_command_including_nested(
        &display_list.bytes,
        &mut |command_type, payload_offset, payload| {
            if command_type != DisplayListCommandType::PaintNestedDisplayList {
                return;
            }
            let id =
                crate::painting::display_list::builder::read_command::<PaintNestedDisplayList>(payload).display_list_id;
            if is_vector_image_placeholder(id) {
                patch_offsets.push((
                    payload_offset + id_field_offset,
                    resolved_ids[vector_image_placeholder_index(id)],
                ));
            }
        },
    );
    for (offset, resolved_id) in patch_offsets {
        display_list.bytes[offset..offset + std::mem::size_of::<u64>()]
            .copy_from_slice(&DisplayListResourceId(resolved_id).0.to_ne_bytes());
    }
}

/// Publishes a pending recording from the document: hands its resources to the host and takes
/// its output in.
pub(crate) fn publish_recording(
    host: &crate::render_state::DocumentHost,
    publication: Publication<'_>,
    main_thread: &MainThread,
    publish: &FfiRecordingPublishCallbacks,
) {
    let Publication {
        pending,
        behind_rows,
        recorder,
        hit_test_list,
    } = publication;
    let publishes_recording = pending.publishes_recording;
    let output = publish_to_host(pending, recorder, main_thread, publish);
    take_in_published_output(
        recorder,
        hit_test_list,
        output,
        publishes_recording,
        |output, hit_test_list_changed| {
            host.queue_change(crate::render_state::ArenaChange::Paint(
                crate::painting::paint_changes::PaintChange::TakeInRecording {
                    output,
                    hit_test_list_changed: hit_test_list_changed || behind_rows,
                    publishes_recording,
                },
            ));
        },
    );
    // A recording that landed behind the rows keeps no hit-test list, as the boxes it names may be gone.
    if behind_rows {
        *hit_test_list = None;
    }
}

/// Where a published recording's fonts, image frames and video sinks go: the resource storage of the presenter that
/// presents it.
pub(crate) trait RecordingResourceSink {
    fn add_font(&mut self, font: &libgfx_rust::font::FontHandle);
    fn add_image_frame(&mut self, frame: &libgfx_rust::image_frame::ImageFrameHandle);
    fn add_video_sink(&mut self, resource_id: u64, sink_handle: u64);
}

/// The host's resource storage, which it reaches on the main thread through its callbacks.
struct HostResourceSink<'a, 'host> {
    main_thread: &'a MainThread<'host>,
    publish: &'a FfiRecordingPublishCallbacks,
}

impl RecordingResourceSink for HostResourceSink<'_, '_> {
    fn add_font(&mut self, font: &libgfx_rust::font::FontHandle) {
        self.publish.add_font(self.main_thread, font);
    }

    fn add_image_frame(&mut self, frame: &libgfx_rust::image_frame::ImageFrameHandle) {
        self.publish.add_image_frame(self.main_thread, frame);
    }

    fn add_video_sink(&mut self, resource_id: u64, sink_handle: u64) {
        self.publish.add_video_sink(self.main_thread, resource_id, sink_handle);
    }
}

/// Hands a recording's resources to the host and makes its output, reading nothing but the
/// recording and the recorder state it recorded with: what a publication does before the
/// document takes the output in.
pub(crate) fn publish_to_host(
    pending: PendingRecording,
    recorder: &RecorderState,
    main_thread: &MainThread,
    publish: &FfiRecordingPublishCallbacks,
) -> RecordingOutput {
    publish_resources(
        pending,
        recorder,
        &mut HostResourceSink { main_thread, publish },
        |output, requests| resolve_vector_image_placeholders(output, requests, main_thread, publish),
    )
}

/// Whether a recording renders an SVG image, which only the host renders, so that only the host publishes it.
pub(crate) fn renders_vector_images(pending: &PendingRecording) -> bool {
    !pending.recording.resources.vector_image_render_requests.is_empty()
}

/// Hands the resources of a recording that renders no SVG image to `presenter` and makes its output, beside the event
/// loop.
pub(crate) fn publish_to_presenter(
    pending: PendingRecording,
    recorder: &RecorderState,
    presenter: &mut crate::painting::presentation::PresenterBox,
) -> RecordingOutput {
    debug_assert!(!renders_vector_images(&pending), "only the host renders an SVG image");
    publish_resources(pending, recorder, presenter, |_, _| {})
}

/// Hands a recording's resources to `sink`, has `resolve_vector_images` patch in the SVG images it renders, and makes
/// its output.
fn publish_resources(
    pending: PendingRecording,
    recorder: &RecorderState,
    sink: &mut impl RecordingResourceSink,
    resolve_vector_images: impl FnOnce(&mut RecordingOutput, &[VectorImageRenderRequest]),
) -> RecordingOutput {
    let PendingRecording {
        recording: RecordingResult { mut output, resources },
        publishes_recording: _,
        svg_paint_resources,
    } = pending;
    let RecordingResourceManifest {
        fonts,
        image_frames,
        video_sinks,
        vector_image_render_requests,
        ..
    } = resources;
    for font in fonts.values() {
        sink.add_font(font);
    }
    for frame in image_frames.values() {
        sink.add_image_frame(frame);
    }
    for frame in published_filter_image_frames_in(&svg_paint_resources) {
        sink.add_image_frame(&frame);
    }
    for (resource_id, sink_handle) in video_sinks {
        sink.add_video_sink(resource_id, sink_handle);
    }
    resolve_vector_images(&mut output, &vector_image_render_requests);
    output.is_identical_to_published_recording = recorder
        .published_recording
        .as_ref()
        .zip(recorder.published_hit_test_items.as_ref())
        .is_some_and(|(source, item_source)| {
            std::sync::Arc::ptr_eq(&output.display_list, &source.display_list)
                && std::sync::Arc::ptr_eq(&output.hit_test_list.items, &item_source.items)
                && output.recorded_structural_epoch == source.recorded_structural_epoch
                && output.wheel_event_listener_state_generation == source.wheel_event_listener_state_generation
                && output.has_blocking_wheel_event_listeners == source.has_blocking_wheel_event_listeners
        });
    output
}

/// Takes a published recording's output in: its hit-test list, and for a recording that publishes,
/// the source the next recording copies from and the damage it consumed. Resource callbacks and
/// verification have finished, so the new recording may become the source. Returns the generation
/// of the document's hit-test list.
pub(crate) fn take_in_published_output(
    recorder: &mut RecorderState,
    hit_test_list: &mut Option<HitTestList>,
    mut output: RecordingOutput,
    publishes_recording: bool,
    take_in: impl FnOnce(std::sync::Arc<RecordingOutput>, bool),
) {
    let list = std::mem::take(&mut output.hit_test_list);
    let previous_list_is_the_source = hit_test_list
        .as_ref()
        .zip(recorder.published_hit_test_items.as_ref())
        .is_some_and(|(list, source)| std::sync::Arc::ptr_eq(&list.items, &source.items));
    let hit_test_list_changed = !(output.is_identical_to_published_recording && previous_list_is_the_source);
    if hit_test_list_changed {
        if publishes_recording {
            recorder.published_hit_test_items =
                Some(std::sync::Arc::new(crate::painting::record::PublishedHitTestItems {
                    items: list.items.clone(),
                }));
        }
        *hit_test_list = Some(list);
    }
    let output = std::sync::Arc::new(output);
    if publishes_recording {
        recorder.published_recording = Some(output.clone());
    }
    take_in(output, hit_test_list_changed);
}

/// Takes the recording `output` the host published in as the document's last, where `arena` is the document's arena:
/// the hit-test list generation moves on where the list changed, and a recording that publishes consumes the damage it
/// painted. Answers the hit-test list generation.
pub(crate) fn take_in_recording(
    arena: &LayoutNodeArena,
    output: std::sync::Arc<RecordingOutput>,
    hit_test_list_changed: bool,
    publishes_recording: bool,
) -> u64 {
    let mut paint_state = arena.paint_state().borrow_mut();
    if hit_test_list_changed {
        paint_state.hit_test_list_generation += 1;
    }
    if publishes_recording {
        // Read-only recordings publish nothing and must not consume the damage.
        arena.clear_paint_damage_consumed_by_published_recording();
        paint_state.visual_context.quarantined_slots_are_releasable = true;
    }
    paint_state.last_recording = Some(output);
    paint_state.hit_test_list_generation
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::painting::record::damage::PaintDamage;
    use std::sync::Arc;

    #[test]
    fn read_only_publication_keeps_the_source_recording_and_the_pending_damage() {
        let mut arena = LayoutNodeArena::new();
        let mut recorder = RecorderState::default();
        let mut hit_test_list = None;
        let row = arena.allocate_for_test().slot;
        arena.populate_paintable_row(row);
        let mut original_source = None;
        // Publish a source, a read-only recording, and then the pending repaint.
        for (hit_test_generation, read_write) in [(1, true), (2, false), (3, true)] {
            if read_write {
                arena.note_publishing_paint_recording_started();
            }
            let output = RecordingOutput {
                hit_test_list: HitTestList {
                    generation: hit_test_generation,
                    ..Default::default()
                },
                ..Default::default()
            };
            let mut generation = 0;
            take_in_published_output(
                &mut recorder,
                &mut hit_test_list,
                output,
                read_write,
                |output, changed| {
                    generation = take_in_recording(&arena, output, changed, read_write);
                },
            );
            assert_eq!(generation, hit_test_generation);
            assert_eq!(
                hit_test_list.as_ref().map_or(0, |list| list.generation),
                hit_test_generation
            );
            let source = recorder.published_recording.clone().unwrap();
            match hit_test_generation {
                1 => {
                    original_source = Some(source);
                    arena.push_paint_damage(row, PaintDamage::DRAW_FOREGROUND);
                }
                2 => {
                    assert!(Arc::ptr_eq(original_source.as_ref().unwrap(), &source));
                    assert_eq!(arena.paint_damage_of_row(row), PaintDamage::DRAW_FOREGROUND);
                }
                3 => {
                    assert!(!Arc::ptr_eq(original_source.as_ref().unwrap(), &source));
                    assert_eq!(arena.paint_damage_of_row(row), PaintDamage::NONE);
                }
                _ => unreachable!(),
            }
        }
    }
}
