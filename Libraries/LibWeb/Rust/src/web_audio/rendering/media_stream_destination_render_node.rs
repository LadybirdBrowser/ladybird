/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::needless_range_loop)]
use std::ffi::c_void;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use super::audio_bus::AudioBus;
use super::audio_frame_ring::{AudioFrameRing, AudioFrameRingCallbacks};
use super::pipe_callbacks::{CallbackService, PipeCallbacks};
use super::render_graph::RenderGraph;
use super::render_node::*;
use super::render_nodes::{NodeProcessor, node_handle};
use crate::web_audio::control_message::FfiSharedCppObject;

// The render thread only touches the ring and pending flag; the control thread owns the drain
// callback so its captured GC roots never cross threads.
struct SharedState {
    ring: AudioFrameRing,

    // Set by the rendering thread when it schedules a drain on the control thread and cleared
    // by the control thread when the drain runs, coalescing the notifications down to one
    // wakeup per rendered burst.
    drain_pending: AtomicBool,
    callbacks: CallbackService,
}

// https://webaudio.github.io/web-audio-api/#MediaStreamAudioDestinationNode
struct MediaStreamDestinationRenderNode {
    shared_state: Arc<SharedState>,
    mix_bus: AudioBus,
    interleaved_scratch: Vec<f32>,
}

impl NodeProcessor for MediaStreamDestinationRenderNode {
    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            let input = &*data.pull_input(graph, context, 0);

            // Mix the input to the track's channel count and interleave it into the shared ring. If
            // the control thread has stalled and the ring is full, the newest frames are dropped; the
            // consumer resynchronizes when it drains.
            self.mix_bus.zero();
            self.mix_bus.sum_from(
                input,
                data.channel_interpretation.get() == ChannelInterpretation::Speakers,
            );
            let channel_count = self.mix_bus.channel_count();
            for frame in 0..context.quantum_size {
                for channel_index in 0..channel_count {
                    self.interleaved_scratch[frame * channel_count + channel_index] =
                        self.mix_bus.channel(channel_index)[frame];
                }
            }
            self.shared_state.ring.try_push(&self.interleaved_scratch);

            // Coalesced render→control notification, following the renderer's ended-sources pattern:
            // at most one queued drain per rendered burst, so the control thread wakes up ~10 times
            // per second rather than once per quantum.
            if !self.shared_state.drain_pending.swap(true, Ordering::AcqRel) {
                unsafe extern "C" fn invoke(task: *mut c_void) {
                    let shared_state = unsafe { Arc::from_raw(task.cast::<SharedState>()) };
                    shared_state.drain_pending.store(false, Ordering::Release);
                    unsafe {
                        (shared_state.callbacks.callbacks.on_wakeup)(shared_state.callbacks.owner.pointer);
                    }
                }
                let task = Arc::into_raw(Arc::clone(&self.shared_state));
                self.shared_state.callbacks.post(task.cast_mut().cast(), invoke);
            }
        }
    }
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_node_create_media_stream_destination(
    node_id: u64,
    quantum_size: usize,
    channel_count: usize,
    ring: FfiSharedCppObject,
    ring_callbacks: AudioFrameRingCallbacks,
    callbacks: PipeCallbacks,
) -> *mut NodeHandle {
    let data = RenderNodeData::new(node_id, 1, 0, quantum_size, channel_count);

    // NB: The control thread queues SetChannelConfig before AddNode and messages addressed to
    //     unknown nodes are dropped, so the constructor must establish the node's channel
    //     configuration itself.
    // https://webaudio.github.io/web-audio-api/#MediaStreamAudioDestinationNode
    data.channel_count.set(channel_count);
    data.channel_count_mode.set(ChannelCountMode::Explicit);
    data.channel_interpretation.set(ChannelInterpretation::Speakers);
    Box::into_raw(node_handle(
        data,
        MediaStreamDestinationRenderNode {
            shared_state: Arc::new(SharedState {
                ring: AudioFrameRing {
                    owner: unsafe { ring.acquire() }.unwrap(),
                    callbacks: ring_callbacks,
                },
                drain_pending: AtomicBool::new(false),
                callbacks: unsafe { CallbackService::new(callbacks) },
            }),
            mix_bus: AudioBus::new(channel_count, quantum_size),
            interleaved_scratch: vec![0.; quantum_size * channel_count],
        },
    ))
}
