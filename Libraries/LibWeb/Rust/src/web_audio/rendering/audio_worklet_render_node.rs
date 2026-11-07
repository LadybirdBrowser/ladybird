/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::needless_range_loop)]
use std::sync::Arc;

use super::audio_worklet_pipe::{AudioWorkletPipe, WorkletPipeState};
use super::render_graph::RenderGraph;
use super::render_node::*;
use super::render_nodes::{BusRef, NodeProcessor, node_handle};

// Script runs on the main-thread pump, `prime_level` quanta behind this render node; this side
// only exchanges preallocated pipe slots and renders silence on underrun.
struct AudioWorkletRenderNode {
    pipe: Arc<AudioWorkletPipe>,
    params: Vec<Arc<RenderAudioParam>>,
    param_scratch: Vec<f32>,
    output_channel_capacities: Vec<u32>,
}

impl NodeProcessor for AudioWorkletRenderNode {
    fn for_each_param(&self, callback: &mut dyn FnMut(&RenderAudioParam)) {
        for param in &self.params {
            callback(param);
        }
    }

    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            // A failed (processor threw) or shut-down (node finalized / context closed) worklet renders
            // silence forever; the pump side owns the state transitions.
            if self.pipe.state() != WorkletPipeState::Running {
                for output in 0..data.output_count() {
                    BusRef(data.output(output)).zero();
                }
                return;
            }
            self.pipe.try_push_input(|slot| {
                AudioWorkletPipe::set_start_frame(slot, context.quantum_start_frame);
                for input in 0..data.input_count() {
                    // Unlike the render graph's silent mono bus, an unconnected worklet input
                    // is represented by an empty channel array in process().
                    if (&*data.input_connections.get())[input].is_empty() {
                        *self.pipe.channel_count(slot, false, input) = 0;
                        continue;
                    }
                    let input_bus = BusRef(data.pull_input(graph, context, input).cast_mut());
                    let capacity_channels = self.pipe.config.input_channel_capacity[input];
                    let channels_to_copy = input_bus.channel_count().min(capacity_channels as usize);
                    *self.pipe.channel_count(slot, false, input) = channels_to_copy as u32;
                    for channel in 0..channels_to_copy {
                        let (pointer, length) = self.pipe.channel(slot, false, input, channel);
                        let destination = ak::slice_from_raw_parts_mut(pointer, length);
                        let source = input_bus.channel(channel);
                        assert_eq!(destination.len(), source.len());
                        destination.copy_from_slice(source);
                    }
                }
                for param_index in 0..self.params.len() {
                    let (pointer, length) = self.pipe.param_block(slot, param_index);
                    let block = ak::slice_from_raw_parts_mut(pointer, length);

                    // a-rate params fill the whole block; k-rate blocks are a single value. compute() handles
                    // both through the shared timeline + any audio-rate param connections.
                    self.param_scratch.resize(block.len(), 0.);
                    self.params[param_index].compute(graph, context, &mut self.param_scratch[..block.len()]);
                    block.copy_from_slice(&self.param_scratch[..block.len()]);
                }
            });
            self.pipe.request_wakeup();
            let popped = self.pipe.try_pop_output(|slot| {
                for output in 0..data.output_count() {
                    let output_bus = BusRef(data.output(output));
                    let channel_count = (*self.pipe.channel_count(slot, true, output) as usize)
                        .min(self.output_channel_capacities[output] as usize);
                    output_bus.set_channel_count(channel_count.max(1));
                    output_bus.zero();
                    for channel in 0..channel_count {
                        let (pointer, length) = self.pipe.channel(slot, true, output, channel);
                        let source = ak::slice_from_raw_parts(pointer, length);
                        let destination = output_bus.channel_mut(channel);
                        let frames = source.len().min(destination.len());
                        destination[..frames].copy_from_slice(&source[..frames]);
                    }
                }
            });
            if !popped {
                for output in 0..data.output_count() {
                    BusRef(data.output(output)).zero();
                }
                return;
            }

            // Latency clamp: after a main-thread stall, the pump refills the output ring past the configured
            // lookahead (the audio side emitted silence meanwhile). Discard the excess so the stall does not
            // permanently ratchet up this node's latency.
            if self.pipe.output_occupancy() > self.pipe.config.prime_level {
                self.pipe.discard_outputs_down_to(self.pipe.config.prime_level);
            }
        }
    }
}

#[allow(clippy::too_many_arguments, reason = "mirror the C++ render node constructor")]
pub fn create_audio_worklet_render_node(
    node_id: u64,
    input_count: usize,
    output_count: usize,
    quantum_size: usize,
    output_channel_capacities: &[u32],
    channel_count: usize,
    channel_count_mode: ChannelCountMode,
    channel_interpretation: ChannelInterpretation,
    pipe: Arc<AudioWorkletPipe>,
    params: Vec<Arc<RenderAudioParam>>,
) -> Box<NodeHandle> {
    let mut data = RenderNodeData::new(node_id, input_count, output_count, quantum_size, 1);
    for (output, &channel_capacity) in output_channel_capacities.iter().enumerate() {
        data.reserve_output_channels(output, channel_capacity as usize);
    }

    // Channel configuration must be set here: the SetChannelConfig messages queued during node
    // construction arrive before our AddNode and are dropped by the graph.
    data.channel_count.set(channel_count);
    data.channel_count_mode.set(channel_count_mode);
    data.channel_interpretation.set(channel_interpretation);
    node_handle(
        data,
        AudioWorkletRenderNode {
            pipe,
            params,
            param_scratch: vec![0.; quantum_size],
            output_channel_capacities: output_channel_capacities.to_vec(),
        },
    )
}
