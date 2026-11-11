/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::UnsafeCell;
use std::sync::Arc;
use std::sync::atomic::{AtomicU8, Ordering};

use super::audio_bus::AudioBus;
use crate::web_audio::{MAX_NUMBER_OF_CHANNELS, audio_param_timeline::AudioParamTimeline};

// State for a single render quantum that is passed to every node being processed.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct RenderContext {
    pub sample_rate: f64,
    pub quantum_size: usize,
    pub quantum_start_frame: u64,
}

impl RenderContext {
    pub fn quantum_start_time(&self) -> f64 {
        self.quantum_start_frame as f64 / self.sample_rate
    }

    pub fn sample_period(&self) -> f64 {
        1. / self.sample_rate
    }

    pub fn frame_time(&self, frame: usize) -> f64 {
        (self.quantum_start_frame + frame as u64) as f64 / self.sample_rate
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum AutomationRate {
    ARate,
    KRate,
}

struct Input {
    source: u64,
    output_index: usize,
}

struct RenderAudioParamState {
    inputs: Vec<Input>,
    input_scratch: AudioBus,
}

// The rendering thread's view of an AudioParam. The automation timeline is shared with the AudioParam on the control
// thread; input connections are only manipulated by the rendering thread.
// https://webaudio.github.io/web-audio-api/#computedvalue
pub struct RenderAudioParam {
    timeline: Arc<AudioParamTimeline>,
    min_value: f32,
    max_value: f32,
    automation_rate: AtomicU8,
    state: UnsafeCell<RenderAudioParamState>,
}

// SAFETY: Only the rendering thread accesses state, and other threads use only the atomic automation rate.
unsafe impl Send for RenderAudioParam {}
unsafe impl Sync for RenderAudioParam {}

impl RenderAudioParam {
    pub fn new(
        timeline: Arc<AudioParamTimeline>,
        min_value: f32,
        max_value: f32,
        automation_rate: AutomationRate,
        quantum_size: usize,
    ) -> Self {
        Self {
            timeline,
            min_value,
            max_value,
            automation_rate: AtomicU8::new(automation_rate as u8),
            state: UnsafeCell::new(RenderAudioParamState {
                inputs: Vec::new(),
                input_scratch: AudioBus::new(1, quantum_size),
            }),
        }
    }

    pub fn set_automation_rate(&self, automation_rate: AutomationRate) {
        self.automation_rate.store(automation_rate as u8, Ordering::SeqCst);
    }

    pub(super) fn automation_rate(&self) -> AutomationRate {
        if self.automation_rate.load(Ordering::SeqCst) == AutomationRate::ARate as u8 {
            AutomationRate::ARate
        } else {
            AutomationRate::KRate
        }
    }

    // https://webaudio.github.io/web-audio-api/#computation-of-value
    pub(crate) unsafe fn compute(&self, graph: &dyn OutputGraph, context: &RenderContext, output: &mut [f32]) {
        let state = self.state.get();

        // 1. paramIntrinsicValue will be calculated at each time, which is either the value set directly to the value
        //    attribute, or, if there are any automation events with times before or at this time, the value as calculated
        //    from these events. If automation events are removed from a given time range, then the paramIntrinsicValue
        //    value will remain unchanged and stay at its previous value until either the value attribute is directly
        //    set, or automation events are added for the time range.
        // NB: A k-rate parameter uses the first sample frame for the entire quantum.
        let rate = self.automation_rate();
        if rate == AutomationRate::KRate {
            self.timeline.sample(context.quantum_start_time(), 0., &mut output[..1]);
            let value = output[0];
            output.fill(value);
        } else {
            self.timeline
                .sample(context.quantum_start_time(), context.sample_period(), output);
        }

        // 2. Set [[current value]] to the value of paramIntrinsicValue at the beginning of this render quantum.
        // NB: Sampling updates the slot for the snapshot it rendered.

        // 3. paramComputedValue is the sum of the paramIntrinsicValue value and the value of the input AudioParam
        //    buffer. If the sum is NaN, replace the sum with the defaultValue.
        for input in unsafe { &(*state).inputs } {
            let bus = graph.pull_output(input.source, input.output_index, context);
            let Some(bus) = (unsafe { bus.as_ref() }) else {
                continue;
            };

            // The AudioParam will take the rendered audio data from any AudioNode output connected to it and convert it to
            // mono by down-mixing if it is not already mono.
            let input_scratch = unsafe { &mut (*state).input_scratch };
            input_scratch.zero();
            input_scratch.sum_from(bus, true);

            let input_samples = input_scratch.channel(0);
            if rate == AutomationRate::KRate {
                for value in output.iter_mut() {
                    *value += input_samples[0];
                }
            } else {
                for frame in 0..output.len() {
                    output[frame] += input_samples[frame];
                }
            }
        }
        let default_value = self.timeline.default_value();
        for value in output.iter_mut() {
            if value.is_nan() {
                *value = default_value;
            }
        }

        // 4. If this AudioParam is a compound parameter, compute its final value with other AudioParams.
        // NB: None of the AudioParams backed by a render node are compound parameters.

        // 5. Set computedValue to paramComputedValue.

        // NB: The computedValue is clamped to the simple nominal range for this parameter.
        // https://webaudio.github.io/web-audio-api/#simple-nominal-range
        for value in output {
            assert!(self.max_value >= self.min_value);
            *value = if *value > self.max_value {
                self.max_value
            } else if *value < self.min_value {
                self.min_value
            } else {
                *value
            };
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ChannelCountMode {
    Max,
    ClampedMax,
    Explicit,
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ChannelInterpretation {
    Speakers,
    Discrete,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct Connection {
    pub source: u64,
    pub output_index: usize,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct OutgoingNodeConnection {
    pub destination: u64,
    pub output_index: usize,
    pub input_index: usize,
}

pub struct OutgoingParamConnection {
    pub destination: Arc<RenderAudioParam>,
    pub output_index: usize,
}

pub struct RenderNodeData {
    pub node_id: u64,
    pub channel_count: std::cell::Cell<usize>,
    pub channel_count_mode: std::cell::Cell<ChannelCountMode>,
    pub channel_interpretation: std::cell::Cell<ChannelInterpretation>,
    pub input_connections: UnsafeCell<Vec<Vec<Connection>>>,
    input_buses: UnsafeCell<Vec<AudioBus>>,
    outputs: UnsafeCell<Vec<AudioBus>>,
    pub outgoing_node_connections: UnsafeCell<Vec<OutgoingNodeConnection>>,
    pub outgoing_param_connections: UnsafeCell<Vec<OutgoingParamConnection>>,

    // Bookkeeping used by RenderGraph to process every node at most once per render quantum.
    pub processed_quantum: std::cell::Cell<u64>,
    pub is_processing: std::cell::Cell<bool>,
}

// SAFETY: Render-node data is transferred to one rendering thread before it is accessed.
unsafe impl Send for RenderNodeData {}
impl RenderNodeData {
    pub fn new(
        node_id: u64,
        input_count: usize,
        output_count: usize,
        quantum_size: usize,
        output_channel_count: usize,
    ) -> Self {
        let input_connections = (0..input_count).map(|_| Vec::new()).collect();
        let mut input_buses = Vec::with_capacity(input_count);
        for _input_index in 0..input_count {
            let mut bus = AudioBus::new(1, quantum_size);
            bus.reserve_channels(MAX_NUMBER_OF_CHANNELS);
            input_buses.push(bus);
        }
        let mut outputs = Vec::with_capacity(output_count);
        for _output_index in 0..output_count {
            outputs.push(AudioBus::new(output_channel_count, quantum_size));
        }
        Self {
            node_id,
            channel_count: std::cell::Cell::new(2),
            channel_count_mode: std::cell::Cell::new(ChannelCountMode::Max),
            channel_interpretation: std::cell::Cell::new(ChannelInterpretation::Speakers),
            input_connections: UnsafeCell::new(input_connections),
            input_buses: UnsafeCell::new(input_buses),
            outputs: UnsafeCell::new(outputs),
            outgoing_node_connections: UnsafeCell::new(Vec::new()),
            outgoing_param_connections: UnsafeCell::new(Vec::new()),
            processed_quantum: std::cell::Cell::new(u64::MAX),
            is_processing: std::cell::Cell::new(false),
        }
    }

    pub(crate) fn reserve_output_channels(&mut self, output: usize, channel_capacity: usize) {
        self.outputs.get_mut()[output].reserve_channels(channel_capacity);
    }

    pub fn input_count(&self) -> usize {
        unsafe { &*self.input_connections.get() }.len()
    }

    pub fn output_count(&self) -> usize {
        unsafe { &*self.outputs.get() }.len()
    }

    pub(crate) unsafe fn output(&self, index: usize) -> *mut AudioBus {
        let outputs = self.outputs.get();
        assert!(index < unsafe { &*outputs }.len());
        unsafe { (*outputs).as_mut_ptr().add(index) }
    }

    pub(crate) unsafe fn set_outgoing_connections(
        &self,
        node_connections: Vec<OutgoingNodeConnection>,
        param_connections: Vec<OutgoingParamConnection>,
    ) {
        unsafe {
            *self.outgoing_node_connections.get() = node_connections;
            *self.outgoing_param_connections.get() = param_connections;
        }
    }

    // Pulls and mixes all connections to the given input into a single bus with the input's computed number of channels.
    // Unconnected inputs are mixed as a single channel of silence.
    // https://webaudio.github.io/web-audio-api/#computednumberofchannels
    pub(crate) unsafe fn pull_input(
        &self,
        graph: &super::render_graph::RenderGraph,
        context: &RenderContext,
        input_index: usize,
    ) -> *const AudioBus {
        let connections = &unsafe { &*self.input_connections.get() }[input_index];
        let input_bus = unsafe { (*self.input_buses.get()).as_mut_ptr().add(input_index) };
        let mut maximum_source_channels = 0;
        for connection in connections {
            let bus = graph.pull_output(connection.source, connection.output_index, context);
            if let Some(bus) = unsafe { bus.as_ref() } {
                maximum_source_channels = maximum_source_channels.max(bus.channel_count());
            }
        }

        // AD-HOC: Treat a zero-channel ScriptProcessorNode buffer as a disconnected input.
        if maximum_source_channels == 0 {
            unsafe {
                (&mut *input_bus).set_channel_count(1);
                (&mut *input_bus).zero();
            }
            return input_bus;
        }

        let computed_number_of_channels = match self.channel_count_mode.get() {
            // "max": computedNumberOfChannels is the maximum of the number of channels of all connections to an input.
            //        In this mode channelCount is ignored.
            ChannelCountMode::Max => maximum_source_channels,
            // "clamped-max": computedNumberOfChannels is determined as for "max" and then clamped to a maximum value of
            //                the given channelCount.
            ChannelCountMode::ClampedMax => maximum_source_channels.min(self.channel_count.get()),
            // "explicit": computedNumberOfChannels is the exact value as specified by the channelCount.
            ChannelCountMode::Explicit => self.channel_count.get(),
        };
        let input_bus_mut = unsafe { &mut *input_bus };
        input_bus_mut.set_channel_count(computed_number_of_channels);
        input_bus_mut.zero();

        // The first pass already processed every dependency, including any re-entrant pulls.
        let speakers = self.channel_interpretation.get() == ChannelInterpretation::Speakers;
        for connection in connections {
            let bus = graph.pull_output(connection.source, connection.output_index, context);
            if let Some(bus) = unsafe { bus.as_ref() } {
                input_bus_mut.sum_from(bus, speakers);
            }
        }
        input_bus
    }
}

// Base class for the rendering thread's counterparts of AudioNodes. Render nodes are constructed on the control thread
// and transferred to the rendering thread through the control message queue; after that, all access happens on the
// rendering thread.
pub(crate) trait RenderNode: Send {
    fn data(&self) -> &RenderNodeData;

    unsafe fn process(&self, graph: &super::render_graph::RenderGraph, context: &RenderContext);

    unsafe fn handle_message(&self, _message: &super::node_message::NodeMessage) {}
    fn for_each_param(&self, _callback: &mut dyn FnMut(&RenderAudioParam)) {}

    // Returns true exactly once after this node has stopped playing, so the main thread can fire an ended event.
    unsafe fn take_ended_notification(&self) -> bool {
        false
    }

    // Whether this node may still produce output for input it has already received: the tail of a node whose tail-time
    // is yes. Such a node keeps playing after its AudioNode is gone, until the tail has played out.
    // https://webaudio.github.io/web-audio-api/#tail-time
    unsafe fn has_tail_remaining(&self) -> bool {
        false
    }
}

pub struct NodeHandle {
    pub(crate) node: Box<dyn RenderNode>,
}

impl NodeHandle {
    pub fn data(&self) -> &RenderNodeData {
        self.node.data()
    }
}

pub(crate) trait OutputGraph {
    fn pull_output(&self, source: u64, output_index: usize, context: &RenderContext) -> *const AudioBus;
}

impl OutputGraph for super::render_graph::RenderGraph {
    fn pull_output(&self, source: u64, output_index: usize, context: &RenderContext) -> *const AudioBus {
        self.pull_output(source, output_index, context)
    }
}

impl RenderAudioParam {
    pub(crate) unsafe fn clear_inputs(&self) {
        unsafe { &mut (*self.state.get()).inputs }.clear();
    }

    pub(crate) unsafe fn add_input(&self, source: u64, output_index: usize) {
        unsafe { &mut (*self.state.get()).inputs }.push(Input { source, output_index });
    }
}
