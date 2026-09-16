/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::painting::display_list::commands::{DisplayListCommandType, DisplayListResourceId, PaintNestedDisplayList};
use crate::painting::host::FfiRecordingPublishCallbacks;
use crate::painting::paint_state::PendingRecording;
use crate::painting::record::resources::RecordingResourceManifest;
use crate::painting::record::vector_images::{
    VectorImageRenderRequest, is_vector_image_placeholder, vector_image_placeholder_index,
};
use crate::painting::record::{RecordingOutput, RecordingResult};

fn resolve_vector_image_placeholders(
    output: &mut RecordingOutput,
    requests: &[VectorImageRenderRequest],
    publish: &FfiRecordingPublishCallbacks,
) {
    if requests.is_empty() {
        return;
    }
    let resolved_ids: Vec<u64> = requests
        .iter()
        .map(|request| publish.resolve_vector_image_display_list(&request.to_ffi()))
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

pub(crate) fn publish_recording(
    arena: &LayoutNodeArena,
    pending: PendingRecording,
    publish: &FfiRecordingPublishCallbacks,
) -> u64 {
    let PendingRecording {
        recording: RecordingResult { mut output, resources },
        recording_from_scratch,
        publishes_recording,
    } = pending;
    let RecordingResourceManifest {
        fonts,
        image_frames,
        video_sinks,
        vector_image_render_requests,
        ..
    } = resources;
    for font in fonts.values() {
        publish.add_font(font);
    }
    for frame in image_frames.values() {
        publish.add_image_frame(frame);
    }
    for frame in arena.svg_paint_resources().published_filter_image_frames() {
        publish.add_image_frame(&frame);
    }
    for (resource_id, sink_handle) in video_sinks {
        publish.add_video_sink(resource_id, sink_handle);
    }
    resolve_vector_image_placeholders(&mut output, &vector_image_render_requests, publish);
    if let Some(mut recording_from_scratch) = recording_from_scratch {
        resolve_vector_image_placeholders(
            &mut recording_from_scratch.output,
            &recording_from_scratch.resources.vector_image_render_requests,
            publish,
        );
        crate::painting::record::verify::verify_assembled_recording_matches_fresh(
            &output,
            &recording_from_scratch.output,
        );
    }
    publish_recording_output(arena, output, publishes_recording)
}

// Resource callbacks and verification must finish before the new frame becomes the source.
fn publish_recording_output(arena: &LayoutNodeArena, mut output: RecordingOutput, publishes_recording: bool) -> u64 {
    let mut paint_state = arena.paint_state().borrow_mut();
    output.is_identical_to_published_frame = paint_state
        .published_frame
        .as_ref()
        .zip(paint_state.published_hit_test_items.as_ref())
        .is_some_and(|(source, item_source)| {
            std::sync::Arc::ptr_eq(&output.display_list, &source.display_list)
                && std::rc::Rc::ptr_eq(&output.hit_test_list.items, &item_source.items)
                && output.recorded_structural_epoch == source.recorded_structural_epoch
                && output.wheel_event_listener_state_generation == source.wheel_event_listener_state_generation
                && output.has_blocking_wheel_event_listeners == source.has_blocking_wheel_event_listeners
        });
    let list = std::mem::take(&mut output.hit_test_list);
    let previous_list_is_the_source = paint_state
        .hit_test_list
        .as_ref()
        .zip(paint_state.published_hit_test_items.as_ref())
        .is_some_and(|(list, source)| std::rc::Rc::ptr_eq(&list.items, &source.items));
    if output.is_identical_to_published_frame && previous_list_is_the_source {
        drop(list);
    } else {
        paint_state.hit_test_list_generation += 1;
        debug_assert_eq!(list.generation, paint_state.hit_test_list_generation);
        if publishes_recording {
            paint_state.published_hit_test_items =
                Some(std::rc::Rc::new(crate::painting::record::PublishedHitTestItems {
                    items: list.items.clone(),
                }));
        }
        paint_state.hit_test_list = Some(list);
    }
    let output = std::rc::Rc::new(output);
    if publishes_recording {
        paint_state.published_frame = Some(output.clone());
        // Read-only recordings publish no frame and must not consume the damage.
        arena.clear_paint_damage_consumed_by_published_recording();
        paint_state.visual_context.quarantined_slots_are_releasable = true;
    }
    paint_state.last_recording = Some(output);
    paint_state.hit_test_list.as_ref().map_or(0, |list| list.generation)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::painting::hit_test::HitTestList;
    use crate::painting::record::damage::PaintDamage;
    use std::rc::Rc;

    #[test]
    fn read_only_publication_keeps_the_source_frame_and_the_pending_damage() {
        let mut arena = LayoutNodeArena::new();
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
            assert_eq!(
                publish_recording_output(&arena, output, read_write),
                hit_test_generation
            );
            let source = arena.paint_state().borrow().published_frame.clone().unwrap();
            match hit_test_generation {
                1 => {
                    original_source = Some(source);
                    arena.push_paint_damage(row, PaintDamage::DRAW_FOREGROUND);
                }
                2 => {
                    assert!(Rc::ptr_eq(original_source.as_ref().unwrap(), &source));
                    assert_eq!(arena.paint_damage_of_row(row), PaintDamage::DRAW_FOREGROUND);
                }
                3 => {
                    assert!(!Rc::ptr_eq(original_source.as_ref().unwrap(), &source));
                    assert_eq!(arena.paint_damage_of_row(row), PaintDamage::NONE);
                }
                _ => unreachable!(),
            }
        }
    }
}
