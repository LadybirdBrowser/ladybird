/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::Cell;

use super::audio_bus::AudioBus;
use super::node_message::node_message_target;
use super::render_node::{NodeHandle, RenderContext};
use crate::web_audio::control_message::ControlMessage;

// The rendering thread's mirror of the audio graph. Nodes and connections are added and updated exclusively through
// control messages, which are applied at the start of each render quantum.
// https://webaudio.github.io/web-audio-api/#rendering-loop
pub struct RenderGraph {
    nodes: NodeMap,
    // Nodes whose AudioNode is gone, but which still have a tail to play. Each is removed once its tail has played out.
    nodes_finishing_tails: Vec<u64>,
    destination_id: u64,
    current_quantum: Cell<u64>,
    incoming_connections_dirty: Cell<bool>,
}

impl Default for RenderGraph {
    fn default() -> Self {
        Self {
            nodes: NodeMap::default(),
            nodes_finishing_tails: Vec::new(),
            destination_id: 0,
            current_quantum: Cell::new(0),
            incoming_connections_dirty: Cell::new(false),
        }
    }
}

impl RenderGraph {
    pub fn apply_control_messages(&mut self, messages: Vec<ControlMessage>) {
        self.remove_nodes_whose_tails_have_played_out();
        for message in messages {
            match message {
                ControlMessage::AddNode(add_node) => {
                    let node_id = add_node.node.data().node_id;
                    self.nodes.insert(node_id, add_node.node);
                    self.incoming_connections_dirty.set(true);
                }
                ControlMessage::RemoveNode(remove_node) => {
                    // A node that still has a tail keeps its connections and goes on rendering it, as nothing can
                    // reach its AudioNode to stop it.
                    let has_tail_remaining = self
                        .node(remove_node.node_id)
                        .is_some_and(|node| unsafe { node.node.has_tail_remaining() });
                    if has_tail_remaining {
                        self.nodes_finishing_tails.push(remove_node.node_id);
                    } else if self.nodes.remove(&remove_node.node_id).is_some() {
                        self.incoming_connections_dirty.set(true);
                    }
                }
                ControlMessage::ReplaceConnections(replace_connections) => {
                    if let Some(source_node) = self.node(replace_connections.source) {
                        unsafe {
                            source_node.data().set_outgoing_connections(
                                replace_connections.node_connections,
                                replace_connections.param_connections,
                            );
                        }
                        self.incoming_connections_dirty.set(true);
                    }
                }
                ControlMessage::SetChannelConfig(config) => {
                    if let Some(target_node) = self.node(config.node_id) {
                        target_node.data().channel_count.set(config.channel_count);
                        target_node.data().channel_count_mode.set(config.channel_count_mode);
                        target_node
                            .data()
                            .channel_interpretation
                            .set(config.channel_interpretation);
                    }
                }
                ControlMessage::NodeMessage(node_message) => {
                    if let Some(target_node) = self.node(node_message_target(&node_message)) {
                        unsafe {
                            target_node.node.handle_message(&node_message);
                        }
                    }
                }
            }
        }
    }

    fn remove_nodes_whose_tails_have_played_out(&mut self) {
        if self.nodes_finishing_tails.is_empty() {
            return;
        }
        let nodes = &mut self.nodes;
        let mut removed_any = false;
        self.nodes_finishing_tails.retain(|node_id| {
            let has_tail_remaining = nodes
                .get(node_id)
                .is_some_and(|node| unsafe { node.node.has_tail_remaining() });
            if !has_tail_remaining {
                removed_any |= nodes.remove(node_id).is_some();
            }
            has_tail_remaining
        });
        if removed_any {
            self.incoming_connections_dirty.set(true);
        }
    }

    // Renders a single quantum by pulling the destination node and any active source nodes.
    pub fn render_quantum(&self, context: &RenderContext) {
        self.current_quantum.set(self.current_quantum.get().wrapping_add(1));
        if self.incoming_connections_dirty.get() {
            unsafe {
                self.rebuild_incoming_connections();
            }
            self.incoming_connections_dirty.set(false);
        }

        // Process the destination first so the main graph is processed in dependency order.
        if let Some(destination_node) = self.destination() {
            self.process_node(destination_node, context);
        }
        // Advance unconnected sources and process sinks with no outputs.
        for graph_node in self.nodes.values() {
            self.process_node(graph_node, context);
        }
    }

    // Returns the source node's output bus, processing the node first if it has not been processed in this quantum.
    // Re-entrant pulls caused by cycles return the output rendered in the previous quantum.
    pub fn pull_output(&self, source: u64, output_index: usize, context: &RenderContext) -> *mut AudioBus {
        let Some(source_node) = self.node(source) else {
            return std::ptr::null_mut();
        };
        let data = source_node.data();
        if output_index >= data.output_count() {
            return std::ptr::null_mut();
        }
        self.process_node(source_node, context);
        unsafe { data.output(output_index) }
    }

    fn process_node(&self, node: &NodeHandle, context: &RenderContext) {
        let data = node.data();
        if data.processed_quantum.get() == self.current_quantum.get() || data.is_processing.get() {
            return;
        }
        data.is_processing.set(true);
        unsafe {
            node.node.process(self, context);
        }
        data.is_processing.set(false);
        data.processed_quantum.set(self.current_quantum.get());
    }

    pub fn node(&self, node_id: u64) -> Option<&NodeHandle> {
        self.nodes.get(&node_id)
    }

    pub fn set_destination(&mut self, node_id: u64) {
        self.destination_id = node_id;
    }

    pub fn destination(&self) -> Option<&NodeHandle> {
        self.node(self.destination_id)
    }

    // Drains the set of nodes that stopped playing since the last call and retires their permanently silent render nodes.
    pub fn take_ended_nodes(&mut self) -> Vec<u64> {
        let mut ended_nodes = Vec::new();
        for (&node_id, graph_node) in self.nodes.iter() {
            if unsafe { graph_node.node.take_ended_notification() } {
                ended_nodes.push(node_id);
            }
        }
        for node_id in &ended_nodes {
            self.nodes.remove(node_id);
        }
        if !ended_nodes.is_empty() {
            self.incoming_connections_dirty.set(true);
        }
        ended_nodes
    }

    unsafe fn rebuild_incoming_connections(&self) {
        for graph_node in self.nodes.values() {
            let data = graph_node.data();
            for input_index in 0..data.input_count() {
                (unsafe { &mut *data.input_connections.get() })[input_index].clear();
            }
            graph_node.node.for_each_param(&mut |param| unsafe {
                param.clear_inputs();
            });
        }
        for (&node_id, graph_node) in self.nodes.iter() {
            let data = graph_node.data();
            for connection in unsafe { &*data.outgoing_node_connections.get() } {
                if let Some(destination_node) = self.node(connection.destination)
                    && connection.input_index < destination_node.data().input_count()
                {
                    (unsafe { &mut *destination_node.data().input_connections.get() })[connection.input_index].push(
                        super::render_node::Connection {
                            source: node_id,
                            output_index: connection.output_index,
                        },
                    );
                }
            }
            for connection in unsafe { &*data.outgoing_param_connections.get() } {
                unsafe {
                    connection.destination.add_input(node_id, connection.output_index);
                }
            }
        }
    }
}

#[derive(Default)]
struct NodeMap {
    buckets: Vec<Option<(u64, Box<NodeHandle>)>>,
    size: usize,
}

impl NodeMap {
    fn hash(mut key: u64) -> usize {
        key ^= key >> 33;
        key = key.wrapping_mul(0xff51afd7ed558ccd);
        key ^= key >> 33;
        key = key.wrapping_mul(0xc4ceb9fe1a85ec53);
        key ^= key >> 33;
        key as u32 as usize
    }

    fn probe_length(&self, key: u64, index: usize) -> usize {
        index.wrapping_sub(Self::hash(key)) & (self.buckets.len() - 1)
    }

    fn insert(&mut self, key: u64, value: Box<NodeHandle>) {
        if (self.size + 1) * 100 >= self.buckets.len() * 70 {
            let capacity = (self.buckets.len() * 2).max(8);
            let old_buckets = std::mem::replace(&mut self.buckets, (0..capacity).map(|_| None).collect());
            self.size = 0;
            for (key, value) in old_buckets.into_iter().flatten() {
                self.write_value(key, value);
            }
        }
        self.write_value(key, value);
    }

    fn write_value(&mut self, mut key: u64, mut value: Box<NodeHandle>) {
        let mask = self.buckets.len() - 1;
        let mut index = Self::hash(key) & mask;
        let mut probe_length = 0;
        loop {
            let Some((target_key, _)) = &self.buckets[index] else {
                self.buckets[index] = Some((key, value));
                self.size += 1;
                return;
            };
            if *target_key == key {
                self.buckets[index] = Some((key, value));
                return;
            }
            let target_probe_length = self.probe_length(*target_key, index);
            if probe_length > target_probe_length {
                let displaced = self.buckets[index].replace((key, value)).unwrap();
                (key, value) = displaced;
                probe_length = target_probe_length;
            }
            index = (index + 1) & mask;
            probe_length += 1;
        }
    }

    fn index(&self, key: u64) -> Option<usize> {
        if self.size == 0 {
            return None;
        }
        let mask = self.buckets.len() - 1;
        let mut index = Self::hash(key) & mask;
        let mut probe_length = 0;
        loop {
            let (target_key, _) = self.buckets[index].as_ref()?;
            if *target_key == key {
                return Some(index);
            }
            if probe_length > self.probe_length(*target_key, index) {
                return None;
            }
            index = (index + 1) & mask;
            probe_length += 1;
        }
    }

    fn get(&self, key: &u64) -> Option<&NodeHandle> {
        self.index(*key)
            .map(|index| self.buckets[index].as_ref().unwrap().1.as_ref())
    }

    fn remove(&mut self, key: &u64) -> Option<Box<NodeHandle>> {
        let mut shift_to_index = self.index(*key)?;
        let (_, value) = self.buckets[shift_to_index].take().unwrap();
        self.size -= 1;
        let mask = self.buckets.len() - 1;
        loop {
            let shift_from_index = (shift_to_index + 1) & mask;
            let Some((next_key, _)) = self.buckets[shift_from_index].as_ref() else {
                break;
            };
            if self.probe_length(*next_key, shift_from_index) == 0 {
                break;
            }
            self.buckets[shift_to_index] = self.buckets[shift_from_index].take();
            shift_to_index = shift_from_index;
        }
        Some(value)
    }

    fn iter(&self) -> impl Iterator<Item = (&u64, &NodeHandle)> {
        self.buckets
            .iter()
            .filter_map(Option::as_ref)
            .map(|(key, value)| (key, value.as_ref()))
    }

    fn values(&self) -> impl Iterator<Item = &NodeHandle> {
        self.iter().map(|(_, value)| value)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::style::pipeline::allocation_count::count_allocations;
    use crate::web_audio::audio_param_timeline::AudioParamTimeline;
    use crate::web_audio::control_message::*;
    use crate::web_audio::control_message_queue::ControlMessageQueue;
    use crate::web_audio::rendering::node_message::{NodeMessage, StartSource, StopSource};
    use crate::web_audio::rendering::render_node::{
        AutomationRate, ChannelCountMode, ChannelInterpretation, OutgoingNodeConnection, OutgoingParamConnection,
        RenderAudioParam,
    };
    use crate::web_audio::rendering::render_nodes::{
        create_channel_merger_render_node, create_constant_source_render_node, create_destination_render_node,
        create_gain_render_node,
    };
    use std::sync::Arc;

    #[test]
    fn control_queue_rendering_and_ended_node_retirement() {
        let timeline = Arc::new(AudioParamTimeline::new(0.75));
        let param = Arc::new(RenderAudioParam::new(timeline, -1., 1., AutomationRate::ARate, 4));
        let queue = ControlMessageQueue::default();
        queue.enqueue(ControlMessage::SetChannelConfig(SetChannelConfig {
            node_id: 1,
            channel_count: 42,
            channel_count_mode: ChannelCountMode::Explicit,
            channel_interpretation: ChannelInterpretation::Discrete,
        }));
        for id in [1, 3] {
            let node = create_constant_source_render_node(id, 4, Arc::clone(&param));
            queue.enqueue(ControlMessage::AddNode(AddNode { node }));
            queue.enqueue(ControlMessage::NodeMessage(NodeMessage::StartSource(StartSource {
                node_id: id,
                when: 1. / 48000.,
            })));
            queue.enqueue(ControlMessage::NodeMessage(NodeMessage::StopSource(StopSource {
                node_id: id,
                when: 3. / 48000.,
            })));
        }
        let destination = create_destination_render_node(2, 4, 2);
        queue.enqueue(ControlMessage::AddNode(AddNode { node: destination }));
        queue.enqueue(ControlMessage::ReplaceConnections(ReplaceConnections {
            source: 1,
            node_connections: vec![OutgoingNodeConnection {
                destination: 2,
                output_index: 0,
                input_index: 0,
            }],
            param_connections: Vec::new(),
        }));
        let mut graph = RenderGraph::default();
        graph.set_destination(2);
        graph.apply_control_messages(queue.drain());
        assert!(queue.drain().is_empty());
        assert_eq!(graph.node(1).unwrap().data().channel_count.get(), 2);
        assert_eq!(Arc::strong_count(&param), 3);
        let mut context = RenderContext {
            sample_rate: 48000.,
            quantum_size: 4,
            quantum_start_frame: 0,
        };
        graph.render_quantum(&context);
        let bus = unsafe { &*graph.destination().unwrap().data().output(0) };
        assert_eq!(bus.channel(0), &[0., 0.75, 0.75, 0.]);
        assert_eq!(bus.channel(1), &[0., 0.75, 0.75, 0.]);
        let mut ended = graph.take_ended_nodes();
        ended.sort();
        assert_eq!(ended, [1, 3]);
        assert_eq!(Arc::strong_count(&param), 1);
        context.quantum_start_frame = 4;
        graph.render_quantum(&context);
        assert!(unsafe { &*graph.destination().unwrap().data().output(0) }.is_silent());
        assert!(graph.take_ended_nodes().is_empty());
    }

    #[test]
    fn connected_params_and_bus_layout_changes_do_not_allocate_during_processing() {
        let context = RenderContext {
            sample_rate: 48000.,
            quantum_size: 4,
            quantum_start_frame: 0,
        };
        let source_param = Arc::new(RenderAudioParam::new(
            Arc::new(AudioParamTimeline::new(0.5)),
            -1.,
            1.,
            AutomationRate::ARate,
            context.quantum_size,
        ));
        let gain_param = Arc::new(RenderAudioParam::new(
            Arc::new(AudioParamTimeline::new(1.)),
            f32::MIN,
            f32::MAX,
            AutomationRate::ARate,
            context.quantum_size,
        ));
        let mut graph = RenderGraph::default();
        graph.set_destination(3);
        graph.apply_control_messages(vec![
            ControlMessage::AddNode(AddNode {
                node: create_constant_source_render_node(1, context.quantum_size, source_param),
            }),
            ControlMessage::AddNode(AddNode {
                node: create_channel_merger_render_node(2, context.quantum_size, 6),
            }),
            ControlMessage::AddNode(AddNode {
                node: create_gain_render_node(3, context.quantum_size, Arc::clone(&gain_param)),
            }),
            ControlMessage::NodeMessage(NodeMessage::StartSource(StartSource { node_id: 1, when: 0. })),
        ]);
        for channel_count in [1, 6, 1, 6] {
            let input = if channel_count == 1 { 3 } else { 2 };
            graph.apply_control_messages(vec![
                ControlMessage::ReplaceConnections(ReplaceConnections {
                    source: 1,
                    node_connections: vec![OutgoingNodeConnection {
                        destination: input,
                        output_index: 0,
                        input_index: 0,
                    }],
                    param_connections: vec![OutgoingParamConnection {
                        destination: Arc::clone(&gain_param),
                        output_index: 0,
                    }],
                }),
                ControlMessage::ReplaceConnections(ReplaceConnections {
                    source: 2,
                    node_connections: if channel_count == 1 {
                        Vec::new()
                    } else {
                        vec![OutgoingNodeConnection {
                            destination: 3,
                            output_index: 0,
                            input_index: 0,
                        }]
                    },
                    param_connections: Vec::new(),
                }),
            ]);
            // Apply topology changes before counting DSP allocations.
            unsafe { graph.rebuild_incoming_connections() };
            graph.incoming_connections_dirty.set(false);
            let (_, allocations) = count_allocations(|| graph.render_quantum(&context));
            assert_eq!(allocations.everywhere, 0);
            let output = unsafe { &*graph.destination().unwrap().data().output(0) };
            assert_eq!(output.channel_count(), channel_count);
            assert_eq!(output.channel(0), &[0.75; 4]);
        }
    }
}
