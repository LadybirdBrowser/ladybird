/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::painting::display_list::commands::{DisplayListCommandType, DisplayListResourceId, PaintNestedDisplayList};
use crate::painting::host::FfiRecordingPublishCallbacks;
use crate::painting::paint_state::PendingRecording;
use crate::painting::record::RecordingOutput;
use crate::painting::record::resources::RecordingResourceManifest;
use crate::painting::record::vector_images::{
    VectorImageRenderRequest, is_vector_image_placeholder, vector_image_placeholder_index,
};

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
    let display_list = std::rc::Rc::make_mut(&mut output.display_list);
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
        mut output,
        resources,
        recording_from_scratch,
        paint_command_cache_read_write,
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
    if let Some((mut recording_from_scratch, resources_from_scratch)) = recording_from_scratch {
        resolve_vector_image_placeholders(
            &mut recording_from_scratch,
            &resources_from_scratch.vector_image_render_requests,
            publish,
        );
        crate::painting::record::verify::verify_spliced_recording_matches_fresh(&output, &recording_from_scratch);
    }
    let mut paint_state = arena.paint_state().borrow_mut();
    output.is_identical_to_cache_source = paint_state
        .paint_command_cache_source
        .as_ref()
        .zip(paint_state.hit_test_item_cache_source.as_ref())
        .is_some_and(|(source, item_source)| {
            std::rc::Rc::ptr_eq(&output.display_list, &source.display_list)
                && std::rc::Rc::ptr_eq(&output.hit_test_list.items, &item_source.items)
                && output.recorded_structural_epoch == source.recorded_structural_epoch
                && output.wheel_event_listener_state_generation == source.wheel_event_listener_state_generation
                && output.has_blocking_wheel_event_listeners == source.has_blocking_wheel_event_listeners
        });
    let list = std::mem::take(&mut output.hit_test_list);
    let previous_list_is_the_source = paint_state
        .hit_test_list
        .as_ref()
        .zip(paint_state.hit_test_item_cache_source.as_ref())
        .is_some_and(|(list, source)| std::rc::Rc::ptr_eq(&list.items, &source.items));
    if output.is_identical_to_cache_source && previous_list_is_the_source {
        drop(list);
    } else {
        paint_state.hit_test_list_generation += 1;
        debug_assert_eq!(list.generation, paint_state.hit_test_list_generation);
        if paint_command_cache_read_write {
            paint_state.hit_test_item_cache_source = Some(std::rc::Rc::new(
                crate::painting::record::cache::HitTestItemCacheSource {
                    items: list.items.clone(),
                },
            ));
        }
        paint_state.hit_test_list = Some(list);
    }
    let output = std::rc::Rc::new(output);
    if paint_command_cache_read_write {
        paint_state.paint_command_cache_source = Some(output.clone());
        // Read-only recordings commit nothing and must not age dirty stamps out.
        arena.note_paint_record_completed_with_cache_writes();
        paint_state.visual_context.quarantined_slots_are_releasable = true;
    }
    paint_state.last_recording = Some(output);
    paint_state.hit_test_list.as_ref().map_or(0, |list| list.generation)
}
