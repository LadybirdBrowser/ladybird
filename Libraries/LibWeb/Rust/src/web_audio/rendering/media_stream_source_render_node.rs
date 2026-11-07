/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::needless_range_loop)]
use super::audio_frame_ring::{AudioFrameRing, AudioFrameRingCallbacks};
use super::node_message::NodeMessage;
use super::render_graph::RenderGraph;
use super::render_node::*;
use super::render_nodes::{BusRef, NodeProcessor, node_handle};
use crate::web_audio::MAX_NUMBER_OF_CHANNELS;

// https://webaudio.github.io/web-audio-api/#MediaStreamAudioSourceNode
struct MediaStreamSourceRenderNode {
    ring: Option<AudioFrameRing>,
    ring_callbacks: AudioFrameRingCallbacks,
    channel_count: u32,
    interleaved_scratch: Vec<f32>,
}

impl NodeProcessor for MediaStreamSourceRenderNode {
    fn handle_message(&mut self, message: &NodeMessage) {
        if let NodeMessage::SetMediaStreamSourceRing(set_ring) = message {
            self.ring = set_ring.ring.as_ref().map(|ring| AudioFrameRing {
                owner: ring.clone(),
                callbacks: self.ring_callbacks,
            });

            // NB: The ring's own channel count wins over the message field so the interleaved
            //     stride below can never disagree with the ring's layout.
            self.channel_count = if let Some(ring) = &self.ring {
                ring.channel_count()
            } else {
                set_ring.channel_count.max(1)
            };
        }
    }

    unsafe fn process(&mut self, data: &RenderNodeData, _graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            let source_output = BusRef(data.output(0));

            // The number of channels of the output corresponds to the number of channels of the track.
            source_output.set_channel_count(self.channel_count as usize);
            source_output.zero();
            let Some(ring) = &self.ring else {
                return;
            };

            // Drain the ring sequentially, up to one render quantum. When the producer has not
            // delivered enough frames yet (startup, scheduling jitter), the tail of the quantum stays
            // silent; skipping ahead to "the most recent frames" instead would drift against the
            // producer's cadence and repeat or drop content.
            let frames_to_pop = context.quantum_size.min(ring.frames_available());
            if frames_to_pop == 0 {
                return;
            }
            self.interleaved_scratch
                .resize(frames_to_pop * self.channel_count as usize, 0.);
            let frames_popped = ring.try_pop(&mut self.interleaved_scratch);
            for channel_index in 0..self.channel_count as usize {
                let output_samples = source_output.channel_mut(channel_index);
                for frame in 0..frames_popped {
                    output_samples[frame] =
                        self.interleaved_scratch[frame * self.channel_count as usize + channel_index];
                }
            }

            // The producer converts track samples to the context's sample rate before filling the ring.
        }
    }
}

/// # Safety
/// The callbacks must accept the ring handles supplied in later node messages.
pub unsafe fn create_media_stream_source_render_node(
    node_id: u64,
    quantum_size: usize,
    callbacks: AudioFrameRingCallbacks,
) -> Box<NodeHandle> {
    let mut data = RenderNodeData::new(node_id, 0, 1, quantum_size, 2);
    data.reserve_output_channels(0, MAX_NUMBER_OF_CHANNELS);

    // NB: The control thread queues SetChannelConfig before AddNode and messages addressed to
    //     unknown nodes are dropped, so the constructor must establish the node's channel
    //     configuration itself.
    // https://webaudio.github.io/web-audio-api/#MediaStreamAudioSourceNode
    data.channel_count.set(2);
    data.channel_count_mode.set(ChannelCountMode::Max);
    data.channel_interpretation.set(ChannelInterpretation::Speakers);
    node_handle(
        data,
        MediaStreamSourceRenderNode {
            ring: None,
            ring_callbacks: callbacks,
            channel_count: 1,
            interleaved_scratch: Vec::with_capacity(quantum_size * MAX_NUMBER_OF_CHANNELS),
        },
    )
}
