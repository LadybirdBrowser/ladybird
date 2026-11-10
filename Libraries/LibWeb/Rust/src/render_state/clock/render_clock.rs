/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the render clock's thread keeps of the contexts it hands display ticks and pointer moves on for, and what it
//! makes of the messages of the render clock channel. `Web::Compositor::RenderClock` owns it, and only reaches it on
//! the clock thread.

use super::hover::{PendingPointer, PointerAnswer};
use super::{ClockTicks, FfiScrollOffset};
use crate::css::ffi_support::ArcRef;
use libcompositing_rust::display_list::commands::CompositorContextId;
use libcompositing_rust::ipc_types::{Web_AsyncScrollNodeKind, Web_CompositorScrollOffset};
use libcompositing_rust::ipc_types::{render_clock_client, render_clock_server};
use libgfx_rust::FloatPoint;
use libipc_rust::Misbehavior;
use libipc_rust::cpp_connection::{FfiThreadSafeMessagePoster, RawMessage, ThreadSafeMessagePoster};
use std::collections::HashMap;
use std::sync::Arc;

struct ArmedContext {
    maximum_frames_per_second: f64,
    ticks: Arc<ClockTicks>,
}

struct PointerLane {
    maximum_frames_per_second: f64,
    ticks: Arc<ClockTicks>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum LanesAreTicked {
    ByDisplayTicks,
    ByHandForATest,
}

#[derive(Default)]
pub struct RenderClockLanes {
    channel_to_compositor: Option<ThreadSafeMessagePoster>,
    armed_contexts: HashMap<CompositorContextId, ArmedContext>,
    pointer_lanes: HashMap<CompositorContextId, PointerLane>,
}

impl RenderClockLanes {
    fn replace_channel(&mut self, channel_to_compositor: ThreadSafeMessagePoster) {
        // A new channel starts with nothing armed: the contexts armed on the previous one were armed for a Compositor
        // that is gone.
        self.drop_channel();
        self.channel_to_compositor = Some(channel_to_compositor);
    }

    fn drop_channel(&mut self) {
        self.armed_contexts.clear();
        self.pointer_lanes.clear();
        self.channel_to_compositor = None;
    }

    fn arm_lane(
        &mut self,
        context_id: CompositorContextId,
        maximum_frames_per_second: f64,
        ticks: Arc<ClockTicks>,
        tick_now_at: Option<i64>,
    ) {
        // Another document of the context hears of the pointer no more, and a tick armed for it would hand its ticks to
        // this one's.
        if self
            .pointer_lanes
            .get(&context_id)
            .is_some_and(|lane| !Arc::ptr_eq(&lane.ticks, &ticks))
        {
            self.armed_contexts.remove(&context_id);
        }
        // The display ticks of an armed context drive its ticks at their rate, however many tasks begin meanwhile.
        if let Some(tick_now_at) = tick_now_at
            && !self.armed_contexts.contains_key(&context_id)
        {
            let _ = ticks.tick(tick_now_at, &[]);
            self.armed_contexts.insert(
                context_id,
                ArmedContext {
                    maximum_frames_per_second,
                    ticks: Arc::clone(&ticks),
                },
            );
            self.request_clock_tick(context_id, maximum_frames_per_second);
        }
        self.pointer_lanes.insert(
            context_id,
            PointerLane {
                maximum_frames_per_second,
                ticks,
            },
        );
    }

    fn forget_context(&mut self, context_id: CompositorContextId) {
        self.armed_contexts.remove(&context_id);
        self.pointer_lanes.remove(&context_id);
    }

    fn pointer_moved(
        &mut self,
        context_id: CompositorContextId,
        pointer: PendingPointer,
        lanes_are_ticked: LanesAreTicked,
    ) {
        let Some(lane) = self.pointer_lanes.get(&context_id) else {
            return;
        };
        let answer = lane.ticks.pointer_moved(pointer);
        self.arm_lane_for_pointer_answer(context_id, answer, lanes_are_ticked);
    }

    fn pointer_rests_on_moving_content(&mut self, context_id: CompositorContextId, lanes_are_ticked: LanesAreTicked) {
        let Some(lane) = self.pointer_lanes.get(&context_id) else {
            return;
        };
        let answer = lane.ticks.pointer_rests_on_moving_content();
        self.arm_lane_for_pointer_answer(context_id, answer, lanes_are_ticked);
    }

    fn arm_lane_for_pointer_answer(
        &mut self,
        context_id: CompositorContextId,
        answer: PointerAnswer,
        lanes_are_ticked: LanesAreTicked,
    ) {
        match answer {
            PointerAnswer::Moves => return,
            PointerAnswer::Ticks => {}
        }
        // A test ticks the lanes by hand.
        if lanes_are_ticked == LanesAreTicked::ByHandForATest {
            return;
        }
        // A context armed for ticks already asked for the next one.
        if self.armed_contexts.contains_key(&context_id) {
            return;
        }
        let Some(lane) = self.pointer_lanes.get(&context_id) else {
            return;
        };
        let maximum_frames_per_second = lane.maximum_frames_per_second;
        self.armed_contexts.insert(
            context_id,
            ArmedContext {
                maximum_frames_per_second,
                ticks: Arc::clone(&lane.ticks),
            },
        );
        self.request_clock_tick(context_id, maximum_frames_per_second);
    }

    fn request_clock_tick(&self, context_id: CompositorContextId, maximum_frames_per_second: f64) {
        if let Some(channel_to_compositor) = &self.channel_to_compositor {
            let _ = channel_to_compositor.try_post(render_clock_server::messages::RequestClockTick {
                context_id,
                maximum_frames_per_second,
            });
        }
    }

    fn clock_tick(
        &mut self,
        context_id: CompositorContextId,
        frame_time_nanoseconds: i64,
        scroll_offsets: &[Web_CompositorScrollOffset],
    ) {
        // A tick for a context disarmed after its request went out, or armed on an earlier channel.
        let Some(armed_context) = self.armed_contexts.get(&context_id) else {
            return;
        };

        // A scroll timeline follows the scroll node of an element or a document's viewport, which its unique id names.
        let scrollers: Vec<FfiScrollOffset> = scroll_offsets
            .iter()
            .filter(|scroll_offset| scroll_offset.scroll_node.kind != Web_AsyncScrollNodeKind::PseudoElement)
            .map(|scroll_offset| FfiScrollOffset {
                scroller: scroll_offset.scroll_node.node_id.0,
                x: scroll_offset.offset.x.to_double(),
                y: scroll_offset.offset.y.to_double(),
            })
            .collect();

        // Handing a tick on only queues it, so the next request goes out right after: however long this tick takes,
        // the next one is already on its way, and the Compositor's pacing is the only limit on the rate.
        if !armed_context.ticks.tick(frame_time_nanoseconds, &scrollers) {
            self.armed_contexts.remove(&context_id);
            return;
        }
        let maximum_frames_per_second = armed_context.maximum_frames_per_second;
        self.request_clock_tick(context_id, maximum_frames_per_second);
    }
}

struct MessagesFromCompositor<'a> {
    lanes: &'a mut RenderClockLanes,
    lanes_are_ticked: LanesAreTicked,
}

impl render_clock_client::Stub for MessagesFromCompositor<'_> {
    fn clock_tick(&mut self, tick: render_clock_client::messages::ClockTick) -> Result<(), Misbehavior> {
        self.lanes
            .clock_tick(tick.context_id, tick.frame_time_nanoseconds, &tick.scroll_offsets);
        Ok(())
    }

    fn pointer_moved(&mut self, moved: render_clock_client::messages::PointerMoved) -> Result<(), Misbehavior> {
        self.lanes.pointer_moved(
            moved.context_id,
            PendingPointer {
                position: Some(FloatPoint {
                    x: moved.position.x as f32,
                    y: moved.position.y as f32,
                }),
                buttons: moved.buttons,
                scrolled_since_frame: moved.scrolled_since_frame,
                input_event_id: moved.input_event_id,
            },
            self.lanes_are_ticked,
        );
        Ok(())
    }

    fn pointer_left(&mut self, left: render_clock_client::messages::PointerLeft) -> Result<(), Misbehavior> {
        self.lanes.pointer_moved(
            left.context_id,
            PendingPointer {
                position: None,
                buttons: 0,
                scrolled_since_frame: false,
                input_event_id: left.input_event_id,
            },
            self.lanes_are_ticked,
        );
        Ok(())
    }

    fn pointer_rests_on_moving_content(
        &mut self,
        rests: render_clock_client::messages::PointerRestsOnMovingContent,
    ) -> Result<(), Misbehavior> {
        self.lanes
            .pointer_rests_on_moving_content(rests.context_id, self.lanes_are_ticked);
        Ok(())
    }
}

fn lanes_are_ticked(by_hand_for_a_test: bool) -> LanesAreTicked {
    if by_hand_for_a_test {
        LanesAreTicked::ByHandForATest
    } else {
        LanesAreTicked::ByDisplayTicks
    }
}

/// The lanes of the process's render clock, which lives as long as the process.
#[unsafe(no_mangle)]
pub extern "C" fn render_clock_lanes_create() -> *mut RenderClockLanes {
    Box::into_raw(Box::default())
}

/// # Safety
///
/// `channel_to_compositor` must hold one reference to a live `IPC::ThreadSafeMessagePoster`, which the caller gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_clock_lanes_replace_channel(
    lanes: &mut RenderClockLanes,
    channel_to_compositor: FfiThreadSafeMessagePoster,
) {
    // SAFETY: The caller gives up the reference the poster holds.
    lanes.replace_channel(unsafe { ThreadSafeMessagePoster::adopt(channel_to_compositor) });
}

#[unsafe(no_mangle)]
pub extern "C" fn render_clock_lanes_drop_channel(lanes: &mut RenderClockLanes) {
    lanes.drop_channel();
}

#[unsafe(no_mangle)]
pub extern "C" fn render_clock_lanes_arm_lane(
    lanes: &mut RenderClockLanes,
    context_id: u64,
    maximum_frames_per_second: f64,
    ticks: ArcRef<'_, ClockTicks>,
    ticks_now: bool,
    tick_now_at: i64,
) {
    lanes.arm_lane(
        CompositorContextId(context_id),
        maximum_frames_per_second,
        ticks.to_arc(),
        ticks_now.then_some(tick_now_at),
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn render_clock_lanes_forget_context(lanes: &mut RenderClockLanes, context_id: u64) {
    lanes.forget_context(CompositorContextId(context_id));
}

#[unsafe(no_mangle)]
pub extern "C" fn render_clock_lanes_disarm_all(lanes: &mut RenderClockLanes) {
    lanes.armed_contexts.clear();
}

#[unsafe(no_mangle)]
pub extern "C" fn render_clock_lanes_pointer_moved(
    lanes: &mut RenderClockLanes,
    context_id: u64,
    has_position: bool,
    x: f32,
    y: f32,
    buttons: u32,
    scrolled_since_frame: bool,
    input_event_id: u64,
    lanes_are_ticked_by_hand_for_a_test: bool,
) {
    lanes.pointer_moved(
        CompositorContextId(context_id),
        PendingPointer {
            position: has_position.then_some(FloatPoint { x, y }),
            buttons,
            scrolled_since_frame,
            input_event_id,
        },
        lanes_are_ticked(lanes_are_ticked_by_hand_for_a_test),
    );
}

/// # Safety
///
/// `message` must be one `IPC::RawMessage::hand_to_rust()` made.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_clock_lanes_handle_message(
    lanes: &mut RenderClockLanes,
    message: RawMessage<'_>,
    lanes_are_ticked_by_hand_for_a_test: bool,
) {
    let mut stub = MessagesFromCompositor {
        lanes,
        lanes_are_ticked: lanes_are_ticked(lanes_are_ticked_by_hand_for_a_test),
    };
    // SAFETY: The caller hands over a message of the render clock channel.
    unsafe { message.hand_to(|bytes, attachments| render_clock_client::dispatch(&mut stub, bytes, attachments)) };
}
