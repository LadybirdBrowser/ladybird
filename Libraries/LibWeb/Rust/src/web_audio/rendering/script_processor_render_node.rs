/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::needless_range_loop)]
use std::sync::Arc;

use super::render_graph::RenderGraph;
use super::render_node::*;
use super::render_nodes::{BusRef, NodeProcessor, node_handle};
use super::script_processor_pipe::ScriptProcessorPipe;

struct ScriptProcessorRenderNode {
    pipe: Arc<ScriptProcessorPipe>,
    block: usize,
    frame_in_block: usize,
    block_start_frame: u64,
    block_is_owned: bool,
}

impl NodeProcessor for ScriptProcessorRenderNode {
    unsafe fn process(&mut self, data: &RenderNodeData, graph: &RenderGraph, context: &RenderContext) {
        unsafe {
            let output_bus = BusRef(data.output(0));
            output_bus.set_channel_count(self.pipe.config.output_channel_count.max(1));
            output_bus.zero();
            if self.pipe.is_shut_down() {
                return;
            }

            // https://webaudio.github.io/web-audio-api/#ScriptProcessorNode
            // audioprocess events are only dispatched if the ScriptProcessorNode has at least one input or one output
            // connected.
            if (&*data.input_connections.get())[0].is_empty()
                && (&*data.outgoing_node_connections.get()).is_empty()
                && (&*data.outgoing_param_connections.get()).is_empty()
            {
                return;
            }
            let config = &self.pipe.config;
            if self.frame_in_block == 0 {
                self.block_start_frame = context.quantum_start_frame;
                self.block_is_owned = self.pipe.is_block_owned_by_rendering_thread(self.block);
            }
            if self.block_is_owned {
                if config.input_channel_count > 0 {
                    let input_bus = BusRef(data.pull_input(graph, context, 0).cast_mut());
                    for channel in 0..config.input_channel_count {
                        let (pointer, length) = self.pipe.input_channel(self.block, channel);
                        let destination = &mut ak::slice_from_raw_parts_mut(pointer, length)
                            [self.frame_in_block..self.frame_in_block + context.quantum_size];
                        if channel < input_bus.channel_count() {
                            destination.copy_from_slice(input_bus.channel(channel));
                        } else {
                            destination.fill(0.);
                        }
                    }
                }
                if self.pipe.has_output(self.block) {
                    for channel in 0..config.output_channel_count {
                        let (pointer, length) = self.pipe.output_channel(self.block, channel);
                        let source = &ak::slice_from_raw_parts(pointer, length)
                            [self.frame_in_block..self.frame_in_block + context.quantum_size];
                        output_bus.channel_mut(channel).copy_from_slice(source);
                    }
                }
            }
            self.frame_in_block += context.quantum_size;
            if self.frame_in_block < config.buffer_size {
                return;
            }
            if self.block_is_owned {
                self.pipe
                    .hand_block_to_control_thread(self.block, self.block_start_frame);
            }
            self.block = (self.block + 1) % config.block_count;
            self.frame_in_block = 0;
        }
    }
}

pub fn create_script_processor_render_node(
    node_id: u64,
    quantum_size: usize,
    pipe: Arc<ScriptProcessorPipe>,
) -> Box<NodeHandle> {
    let data = RenderNodeData::new(node_id, 1, 1, quantum_size, pipe.config.output_channel_count.max(1));
    assert!(pipe.config.buffer_size.is_multiple_of(quantum_size));
    node_handle(
        data,
        ScriptProcessorRenderNode {
            pipe,
            block: 0,
            frame_in_block: 0,
            block_start_frame: 0,
            block_is_owned: false,
        },
    )
}
