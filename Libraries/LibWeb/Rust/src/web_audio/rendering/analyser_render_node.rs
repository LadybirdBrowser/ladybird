/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::needless_range_loop)]
use super::audio_frame_ring::{AudioFrameRing, AudioFrameRingCallbacks};
use super::render_graph::RenderGraph;
use super::render_node::*;
use super::render_nodes::{BusRef, NodeProcessor, node_handle};
use crate::web_audio::control_message::FfiSharedCppObject;

// https://webaudio.github.io/web-audio-api/#AnalyserNode
struct AnalyserRenderNode {
    time_domain_ring: AudioFrameRing,
    mono_scratch: Vec<f32>,
    discard_scratch: Vec<f32>,
}

impl NodeProcessor for AnalyserRenderNode {
    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            let input = BusRef(data.pull_input(graph, context, 0).cast_mut());

            // The AnalyserNode passes its input through unprocessed.
            BusRef(data.output(0)).copy_from(&input);

            // https://webaudio.github.io/web-audio-api/#current-time-domain-data
            // The input signal must be down-mixed to mono as if channelCount is 1, channelCountMode
            // is "max" and channelInterpretation is "speakers".
            // AD-HOC: The down-mix is an unweighted channel average, which matches the speakers rules
            //         for mono and stereo input; inputs with more channels would need the weighted
            //         speakers down-mix.
            let channel_count = input.channel_count();
            for frame in 0..context.quantum_size {
                let mut sum = 0.;
                for channel_index in 0..channel_count {
                    sum += input.channel(channel_index)[frame];
                }
                self.mono_scratch[frame] = if channel_count > 0 {
                    sum / channel_count as f32
                } else {
                    0.
                };
            }

            // The ring must always end up holding the most recent samples, so a full ring (the page
            // is not currently polling any of the analyser getters) drops its oldest quantum first.
            // NB: Popping from the producing side bends the SPSC discipline. It can only race with
            //     the control thread while the ring is simultaneously full and being drained by a
            //     getter, in which case the worst outcome is one repeated or skipped quantum in the
            //     analysis window; the ring's masked indices keep the race memory-safe.
            if self.time_domain_ring.frames_free() < context.quantum_size {
                self.time_domain_ring.try_pop(&mut self.discard_scratch);
            }
            self.time_domain_ring.try_push(&self.mono_scratch);
        }
    }
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_node_create_analyser(
    node_id: u64,
    quantum_size: usize,
    ring: FfiSharedCppObject,
    callbacks: AudioFrameRingCallbacks,
) -> *mut NodeHandle {
    let time_domain_ring = AudioFrameRing {
        owner: unsafe { ring.acquire() }.unwrap(),
        callbacks,
    };
    assert_eq!(time_domain_ring.channel_count(), 1);
    let data = RenderNodeData::new(node_id, 1, 1, quantum_size, 1);

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
            mono_scratch: vec![0.; quantum_size],
            discard_scratch: vec![0.; quantum_size],
        },
    )
}
