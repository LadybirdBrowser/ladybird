/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::audio_bus::AudioBus;
use super::audio_frame_ring::{AudioFrameRing, AudioFrameRingCallbacks};
use super::render_graph::RenderGraph;
use super::render_node::*;
use super::render_nodes::{BusRef, NodeProcessor, node_handle};
use crate::web_audio::MAX_NUMBER_OF_CHANNELS;
use crate::web_audio::control_message::FfiSharedCppObject;

// https://webaudio.github.io/web-audio-api/#AnalyserNode
struct AnalyserRenderNode {
    time_domain_ring: AudioFrameRing,
    mono_scratch: AudioBus,
    discard_scratch: Vec<f32>,
}

impl NodeProcessor for AnalyserRenderNode {
    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            let input = BusRef(data.pull_input(graph, context, 0).cast_mut());

            // The AnalyserNode passes its input through unprocessed.
            BusRef(data.output(0)).copy_from(&input);

            // https://webaudio.github.io/web-audio-api/#current-time-domain-data
            // When the current time-domain data are computed, the input signal must be down-mixed to mono as if
            // channelCount is 1, channelCountMode is "max" and channelInterpretation is "speakers". This is independent
            // of the settings for the AnalyserNode itself.
            // AD-HOC: Down-mix the input as mixed by the AnalyserNode's own channel settings, like other engines.
            self.mono_scratch.zero();
            self.mono_scratch.sum_from(&*input.0, true);

            // The ring must always end up holding the most recent samples, so a full ring (the page
            // is not currently polling any of the analyser getters) drops its oldest quantum first.
            // NB: Popping from the producing side bends the SPSC discipline. It can only race with
            //     the control thread while the ring is simultaneously full and being drained by a
            //     getter, in which case the worst outcome is one repeated or skipped quantum in the
            //     analysis window; the ring's masked indices keep the race memory-safe.
            if self.time_domain_ring.frames_free() < context.quantum_size {
                self.time_domain_ring.try_pop(&mut self.discard_scratch);
            }
            self.time_domain_ring.try_push(self.mono_scratch.channel(0));
        }
    }
}

/// # Safety
/// The ring handle and callbacks must refer to the same live LibAudio ring.
pub unsafe fn create_analyser_render_node(
    node_id: u64,
    quantum_size: usize,
    ring: FfiSharedCppObject,
    callbacks: AudioFrameRingCallbacks,
) -> Box<NodeHandle> {
    let time_domain_ring = AudioFrameRing {
        owner: unsafe { ring.acquire() }.unwrap(),
        callbacks,
    };
    assert_eq!(time_domain_ring.channel_count(), 1);
    let mut data = RenderNodeData::new(node_id, 1, 1, quantum_size, 1);
    data.reserve_output_channels(0, MAX_NUMBER_OF_CHANNELS);

    // NB: The control thread queues SetChannelConfig before AddNode and messages addressed to
    //     unknown nodes are dropped, so the constructor must establish the node's channel
    //     configuration itself.
    // https://webaudio.github.io/web-audio-api/#AnalyserNode
    data.channel_count.set(2);
    data.channel_count_mode.set(ChannelCountMode::Max);
    data.channel_interpretation.set(ChannelInterpretation::Speakers);
    node_handle(
        data,
        AnalyserRenderNode {
            time_domain_ring,
            mono_scratch: AudioBus::new(1, quantum_size),
            discard_scratch: vec![0.; quantum_size],
        },
    )
}
