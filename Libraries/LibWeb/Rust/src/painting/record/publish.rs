/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::painting::host::FfiRecordingPublishCallbacks;
use crate::painting::paint_state::PendingRecording;

pub(crate) fn publish_recording(
    arena: &LayoutNodeArena,
    pending: PendingRecording,
    publish: &FfiRecordingPublishCallbacks,
) -> u64 {
    let PendingRecording {
        mut output,
        recording_from_scratch,
        paint_command_cache_read_write,
    } = pending;
    for font in &output.newly_referenced_fonts {
        publish.add_font(font);
    }
    for frame in &output.newly_referenced_image_frames {
        publish.add_image_frame(frame);
    }
    if let Some(recording_from_scratch) = &recording_from_scratch {
        crate::painting::record::verify::verify_spliced_recording_matches_fresh(arena, &output, recording_from_scratch);
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
