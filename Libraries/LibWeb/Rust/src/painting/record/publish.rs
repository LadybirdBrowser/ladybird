/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::painting::display_list::chunks::Chunk;
use crate::painting::display_list::commands::{DisplayListCommandType, DisplayListResourceId, PaintVectorImage};
use crate::painting::display_list_resources::{DisplayListResources, summary_of};
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
use crate::painting::vector_image_cache::VectorImageKey;
use crate::painting::vector_image_records::VectorImageRecords;
use crate::stage::MainThread;
use std::collections::HashMap;
use std::sync::Arc;

fn resolve_vector_image_placeholders(
    output: &mut RecordingOutput,
    requests: &[VectorImageRenderRequest],
    main_thread: &MainThread,
    publish: &FfiRecordingPublishCallbacks,
) -> Vec<Arc<VectorImageRecords>> {
    if requests.is_empty() {
        return Vec::new();
    }
    let vector_images = render_vector_images(requests, main_thread, publish);
    patch_vector_image_placeholders(output, &vector_images);
    vector_images
}

/// Has the host render the SVG images of `requests`, and answers their records, one for each request: those the
/// image's document keeps for the request, or none where the image has nothing to show.
pub(crate) fn render_vector_images(
    requests: &[VectorImageRenderRequest],
    main_thread: &MainThread,
    publish: &FfiRecordingPublishCallbacks,
) -> Vec<Arc<VectorImageRecords>> {
    requests
        .iter()
        .map(|request| {
            let request = request.to_ffi();
            publish
                .render_vector_image(main_thread, &request)
                .and_then(|image_host| image_host.vector_image(&VectorImageKey::of_request(&request)))
                .unwrap_or_else(VectorImageRecords::empty)
        })
        .collect()
}

/// Patches the placeholders of SVG images in `output` with the ids of `vector_images`, one for each image: finishes
/// every chunk of the recording that draws one, and the display list's tape with it.
fn patch_vector_image_placeholders(output: &mut RecordingOutput, vector_images: &[Arc<VectorImageRecords>]) {
    let id_field_offset = std::mem::offset_of!(PaintVectorImage, vector_image_id);
    let chunks = Arc::clone(&output.chunks);
    let display_list = Arc::make_mut(&mut output.display_list);
    debug_assert_eq!(
        chunks.len(),
        display_list.chunks.len(),
        "a tape holds one chunk for each of the list's"
    );
    let mut chunk_start = 0;
    for (index, chunk) in chunks.iter().enumerate() {
        let chunk_end = display_list.chunks[index].byte_end as usize;
        let records = chunk_start..chunk_end;
        chunk_start = chunk_end;
        if !chunk.draws_vector_image_placeholders() || chunk.finished().is_some() {
            continue;
        }
        let mut patch_offsets = Vec::new();
        crate::painting::display_list::nested_records::for_each_command_including_nested(
            &display_list.bytes[records.clone()],
            &mut |command_type, payload_offset, payload| {
                if command_type != DisplayListCommandType::PaintVectorImage {
                    return;
                }
                let id =
                    crate::painting::display_list::builder::read_command::<PaintVectorImage>(payload).vector_image_id;
                if is_vector_image_placeholder(id) {
                    patch_offsets.push((
                        records.start + payload_offset + id_field_offset,
                        vector_images[vector_image_placeholder_index(id)].id(),
                    ));
                }
            },
        );
        for &(offset, resolved_id) in &patch_offsets {
            display_list.bytes[offset..offset + std::mem::size_of::<u64>()]
                .copy_from_slice(&DisplayListResourceId(resolved_id).0.to_ne_bytes());
        }
        let finished = chunk.finish(Chunk::named(
            display_list.bytes[records].to_vec(),
            chunk.recorded().runs.clone(),
        ));
        display_list.chunks[index].id = finished.id;
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
        hit_test_items_on_screen,
    } = publication;
    let publishes_recording = pending.publishes_recording;
    let output = publish_to_host(pending, recorder, main_thread, publish);
    take_in_presented_output(
        recorder,
        hit_test_items_on_screen,
        Arc::new(output),
        publishes_recording,
        |output| {
            host.queue_change(crate::render_state::ArenaChange::Paint(
                crate::painting::paint_changes::PaintChange::TakeInRecording {
                    output,
                    publishes_recording,
                },
            ));
        },
    );
    // A recording that landed behind the rows leaves nothing hit testing reads, as the boxes it shows may be gone.
    if behind_rows {
        hit_test_items_on_screen.forget();
    }
}

/// Makes a recording's output, with the host rendering its SVG images, reading nothing but the recording and the
/// recorder state it recorded with: what a publication does before the document takes the output in.
pub(crate) fn publish_to_host(
    pending: PendingRecording,
    recorder: &RecorderState,
    main_thread: &MainThread,
    publish: &FfiRecordingPublishCallbacks,
) -> RecordingOutput {
    publish_resources(pending, recorder, |output, requests| {
        resolve_vector_image_placeholders(output, requests, main_thread, publish)
    })
}

/// Whether a recording renders an SVG image, which only the host renders, so that only the host publishes it.
pub(crate) fn renders_vector_images(pending: &PendingRecording) -> bool {
    !pending.recording.resources.vector_image_render_requests.is_empty()
}

/// Makes the output of a recording that renders no SVG image, beside the event loop.
pub(crate) fn publish_to_presenter(pending: PendingRecording, recorder: &RecorderState) -> RecordingOutput {
    debug_assert!(!renders_vector_images(&pending), "only the host renders an SVG image");
    publish_resources(pending, recorder, |_, _| Vec::new())
}

/// Makes the output of a recording that renders SVG images, beside the event loop, with `vector_images`, those the host
/// rendered them as, one for each of the images.
pub(crate) fn publish_with_vector_images(
    pending: PendingRecording,
    recorder: &RecorderState,
    vector_images: &[Arc<VectorImageRecords>],
) -> RecordingOutput {
    publish_resources(pending, recorder, |output, _| {
        patch_vector_image_placeholders(output, vector_images);
        vector_images.to_vec()
    })
}

/// The records the host rendered the SVG images of `pending` as for the recording published last with `recorder`, those
/// of each of the images, or none where one of the images is new to that recording.
pub(crate) fn published_vector_image_records(
    pending: &PendingRecording,
    recorder: &RecorderState,
) -> Option<Vec<Arc<VectorImageRecords>>> {
    let published = recorder.published_recording.as_ref()?;
    pending
        .recording
        .resources
        .vector_image_render_requests
        .iter()
        .map(|request| {
            let id = published
                .vector_image_ids_by_request
                .iter()
                .find_map(|(published_request, id)| (published_request == request).then_some(*id))?;
            published.resources.vector_images.get(&id).cloned()
        })
        .collect()
}

/// The SVG images `output` draws, with the ids of their records: those of `requests`, whose records are `resolved_ids`,
/// and those of `published`, the recording published last, that the clean output `output` copied from it still draws.
fn drawn_vector_image_ids(
    output: &RecordingOutput,
    requests: &[VectorImageRenderRequest],
    resolved_ids: &[u64],
    published: Option<&RecordingOutput>,
) -> Vec<(VectorImageRenderRequest, u64)> {
    let mut drawn: Vec<(VectorImageRenderRequest, u64)> =
        requests.iter().copied().zip(resolved_ids.iter().copied()).collect();
    let carried = published.map_or(&[][..], |published| &published.vector_image_ids_by_request[..]);
    if carried.is_empty() {
        return drawn;
    }
    let mut drawn_ids = std::collections::HashSet::new();
    crate::painting::display_list::nested_records::for_each_command_including_nested(
        &output.display_list.bytes,
        &mut |command_type, _, payload| {
            if command_type == DisplayListCommandType::PaintVectorImage {
                drawn_ids.insert(
                    crate::painting::display_list::builder::read_command::<PaintVectorImage>(payload).vector_image_id,
                );
            }
        },
    );
    for &(request, id) in carried {
        if drawn_ids.contains(&DisplayListResourceId(id))
            && !drawn.iter().any(|(drawn_request, _)| *drawn_request == request)
        {
            drawn.push((request, id));
        }
    }
    drawn
}

/// Makes a recording's output: has `resolve_vector_images` patch in the SVG images it renders, and gathers everything
/// its display list draws besides its tape.
fn publish_resources(
    pending: PendingRecording,
    recorder: &RecorderState,
    resolve_vector_images: impl FnOnce(&mut RecordingOutput, &[VectorImageRenderRequest]) -> Vec<Arc<VectorImageRecords>>,
) -> RecordingOutput {
    let PendingRecording {
        recording: RecordingResult { mut output, resources },
        publishes_recording,
        svg_paint_resources,
    } = pending;
    let RecordingResourceManifest {
        fonts,
        image_frames,
        vector_image_render_requests,
        ..
    } = resources;
    let mut display_list_resources = DisplayListResources {
        fonts: fonts.into_iter().map(|(id, font)| (id.0, font)).collect(),
        image_frames,
        vector_images: HashMap::new(),
    };
    // The filters of the visual context tree's effects draw these.
    for frame in published_filter_image_frames_in(&svg_paint_resources) {
        display_list_resources.image_frames.entry(frame.id()).or_insert(frame);
    }
    let resolved = resolve_vector_images(&mut output, &vector_image_render_requests);
    let resolved_ids: Vec<u64> = resolved.iter().map(|vector_image| vector_image.id()).collect();
    for vector_image in resolved {
        display_list_resources
            .vector_images
            .insert(vector_image.id(), vector_image);
    }
    // What the recording copied from the one it read paint commands from draws what that one drew.
    if let Some(source) = recorder.published_recording.as_deref() {
        display_list_resources.complete_from(&summary_of(&output.display_list), &source.resources);
    }
    output.resources = display_list_resources;
    // Only a recording that publishes becomes the one the next recording copies from, so only it keeps the SVG images
    // it carried over from the one published last.
    let published = recorder.published_recording.as_deref().filter(|_| publishes_recording);
    output.vector_image_ids_by_request =
        drawn_vector_image_ids(&output, &vector_image_render_requests, &resolved_ids, published);
    output.is_identical_to_published_recording = recorder.published_recording.as_ref().is_some_and(|source| {
        Arc::ptr_eq(&output.display_list, &source.display_list)
            && output.hit_test_items_stamp == source.hit_test_items_stamp
            && output.recorded_structural_epoch == source.recorded_structural_epoch
            && output.wheel_event_listener_state_generation == source.wheel_event_listener_state_generation
            && output.has_blocking_wheel_event_listeners == source.has_blocking_wheel_event_listeners
    });
    // A recording that repeats the one published last publishes its display list again, under its id.
    output.display_list_id = match recorder.published_recording.as_deref() {
        Some(source) if output.is_identical_to_published_recording => source.display_list_id,
        _ => crate::painting::display_list::ids::allocate_display_list_id(),
    };
    output
}

/// What the hit-test items of the frame a document presented last are as of. Hit testing walks the rows, which place
/// other items once their hit-test items stamp or the visual context tree changes, and the host tells by a generation
/// whether the frame it hit tests is still the one on screen.
#[derive(Default)]
pub(crate) struct HitTestItemsOnScreen {
    /// The rows' hit-test items stamp and the tree's structural epoch of the frame, while one stands.
    as_of: Option<(u64, u64)>,
    generation: u64,
}

impl HitTestItemsOnScreen {
    /// Takes in the frame `output` presented: the generation moves on where it places other items than the frame
    /// before it.
    pub(crate) fn take_in(&mut self, output: &RecordingOutput) {
        let as_of = Some((output.hit_test_items_stamp, output.recorded_structural_epoch));
        if self.as_of != as_of {
            self.as_of = as_of;
            self.generation += 1;
        }
    }

    /// Forgets the frame on screen, whose boxes may be gone.
    pub(crate) fn forget(&mut self) {
        self.as_of = None;
    }

    /// The generation of the frame on screen, or zero where none stands.
    pub(crate) fn generation(&self) -> u64 {
        match self.as_of {
            Some(_) => self.generation,
            None => 0,
        }
    }
}

/// Takes the output of a recording that presented itself in: what hit testing reads of the frame on screen, and for a
/// recording that publishes, the source the next recording reuses from. Resource callbacks and verification have
/// finished, so the new recording may become the source.
pub(crate) fn take_in_presented_output(
    recorder: &mut RecorderState,
    hit_test_items_on_screen: &mut HitTestItemsOnScreen,
    output: Arc<RecordingOutput>,
    publishes_recording: bool,
    take_in: impl FnOnce(Arc<RecordingOutput>),
) {
    hit_test_items_on_screen.take_in(&output);
    if publishes_recording {
        recorder.published_recording = Some(Arc::clone(&output));
    }
    take_in(output);
}

/// Takes the recording `output` the host published in as the document's last, where `arena` is the document's arena.
pub(crate) fn take_in_recording(arena: &mut LayoutNodeArena, output: Arc<RecordingOutput>, publishes_recording: bool) {
    let paint_state = arena.paint_state_mut();
    if publishes_recording {
        paint_state.visual_context.quarantined_slots_are_releasable = true;
        paint_state.window_of_published_recording = output.window.clone().zip(output.visual_context_tree.clone());
    }
    paint_state.last_recording = Some(output);
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::painting::record::change_ticks::RowPaintChange;
    use std::sync::Arc;

    #[test]
    fn read_only_publication_keeps_the_source_recording_whose_generation_later_changes_are_newer_than() {
        let mut arena = LayoutNodeArena::new();
        let mut recorder = RecorderState::default();
        let mut hit_test_items_on_screen = HitTestItemsOnScreen::default();
        let row = arena.allocate_for_test().slot;
        arena.populate_paintable_row(row);
        let mut original_source = None;
        // Publish a source, a read-only recording, and then the frame after the change.
        for (structural_epoch, read_write) in [(1, true), (2, false), (3, true)] {
            // Each recording is of another tree, so that it places other hit-test items.
            let output = RecordingOutput {
                recorded_structural_epoch: structural_epoch,
                recorded_as_of: arena.take_paint_generation(),
                ..Default::default()
            };
            take_in_presented_output(
                &mut recorder,
                &mut hit_test_items_on_screen,
                Arc::new(output),
                read_write,
                |output| take_in_recording(&mut arena, output, read_write),
            );
            assert_eq!(hit_test_items_on_screen.generation(), structural_epoch);
            let source = recorder.published_recording.clone().unwrap();
            let changed_since_source = arena
                .paint_change_stamps(row)
                .row_or_anything_below_it_changed_since(source.recorded_as_of);
            match structural_epoch {
                1 => {
                    original_source = Some(source);
                    arena.note_paint_change(row, RowPaintChange::DRAWING);
                }
                2 => {
                    assert!(Arc::ptr_eq(original_source.as_ref().unwrap(), &source));
                    assert!(changed_since_source);
                }
                3 => {
                    assert!(!Arc::ptr_eq(original_source.as_ref().unwrap(), &source));
                    assert!(!changed_since_source);
                }
                _ => unreachable!(),
            }
        }
    }

    #[test]
    fn only_a_take_in_that_publishes_releases_what_it_replaced() {
        let mut arena = LayoutNodeArena::new();
        let take_in = |arena: &mut LayoutNodeArena, publishes_recording| {
            take_in_recording(arena, Arc::new(RecordingOutput::default()), publishes_recording);
        };
        take_in(&mut arena, true);
        assert!(arena.paint_state().visual_context.quarantined_slots_are_releasable);
        arena.paint_state_mut().visual_context.quarantined_slots_are_releasable = false;
        take_in(&mut arena, false);
        assert!(!arena.paint_state().visual_context.quarantined_slots_are_releasable);
        assert!(arena.paint_state().last_recording.is_some());
    }

    #[test]
    fn a_frame_of_the_same_hit_test_items_keeps_the_generation_of_the_frame_on_screen() {
        let mut hit_test_items_on_screen = HitTestItemsOnScreen::default();
        assert_eq!(hit_test_items_on_screen.generation(), 0);
        let frame = |hit_test_items_stamp, recorded_structural_epoch| RecordingOutput {
            hit_test_items_stamp,
            recorded_structural_epoch,
            ..Default::default()
        };
        // Frames of rows that place the same items in one tree keep the generation, and another stamp or another
        // tree moves it on.
        for (stamp, structural_epoch, expected_generation) in [(4, 7, 1), (4, 7, 1), (5, 7, 2), (5, 8, 3)] {
            hit_test_items_on_screen.take_in(&frame(stamp, structural_epoch));
            assert_eq!(hit_test_items_on_screen.generation(), expected_generation);
        }
        // A frame behind the rows leaves none, and the same frame after it is another.
        hit_test_items_on_screen.forget();
        assert_eq!(hit_test_items_on_screen.generation(), 0);
        hit_test_items_on_screen.take_in(&frame(5, 8));
        assert_eq!(hit_test_items_on_screen.generation(), 4);
    }
}
