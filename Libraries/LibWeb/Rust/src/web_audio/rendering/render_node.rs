/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::UnsafeCell;
use std::ffi::c_void;
use std::sync::Arc;
use std::sync::atomic::{AtomicU8, Ordering};

use ak::SpanMut;

use super::audio_bus::AudioBus;
use crate::web_audio::audio_param_timeline::AudioParamTimeline;

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
    input_scratch: Option<Box<AudioBus>>,
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

pub type PullOutput = unsafe extern "C" fn(*mut c_void, u64, usize, *const RenderContext) -> *const AudioBus;

struct ForeignRenderGraph {
    graph: *mut c_void,
    pull_output: PullOutput,
}

impl RenderAudioParam {
    pub(crate) fn new(
        timeline: Arc<AudioParamTimeline>,
        min_value: f32,
        max_value: f32,
        automation_rate: AutomationRate,
    ) -> Self {
        Self {
            timeline,
            min_value,
            max_value,
            automation_rate: AtomicU8::new(automation_rate as u8),
            state: UnsafeCell::new(RenderAudioParamState {
                inputs: Vec::new(),
                input_scratch: None,
            }),
        }
    }

    fn set_automation_rate(&self, automation_rate: AutomationRate) {
        self.automation_rate.store(automation_rate as u8, Ordering::SeqCst);
    }

    fn automation_rate(&self) -> AutomationRate {
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
        //    from these events.
        // NB: An a-rate parameter takes the exact value of the signal at each sample frame of the render quantum, while a
        //     k-rate parameter uses the value of the very first sample frame for the entire quantum.
        let rate = self.automation_rate();
        if rate == AutomationRate::KRate {
            let value = self.timeline.value_at_time(context.quantum_start_time());
            output.fill(value);
        } else {
            self.timeline
                .sample(context.quantum_start_time(), context.sample_period(), output);
        }

        // 2. Set [[current value]] to the value of paramIntrinsicValue at the beginning of this render quantum.
        // NB: The render param keeps no [[current value]] slot of its own; the AudioParam's value getter reads the shared
        //     automation timeline directly on the control thread.

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
            if input_scratch
                .as_ref()
                .is_none_or(|scratch| scratch.frame_count() != context.quantum_size)
            {
                *input_scratch = Some(Box::new(AudioBus::new(1, context.quantum_size)));
            }
            let input_scratch = input_scratch.as_mut().unwrap();
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

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_render_param_create(
    timeline: *const AudioParamTimeline,
    min_value: f32,
    max_value: f32,
    automation_rate: AutomationRate,
) -> *const RenderAudioParam {
    unsafe {
        Arc::increment_strong_count(timeline);
    }
    let timeline = unsafe { Arc::from_raw(timeline) };
    Arc::into_raw(Arc::new(RenderAudioParam::new(
        timeline,
        min_value,
        max_value,
        automation_rate,
    )))
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_render_param_destroy(param: *const RenderAudioParam) {
    drop(unsafe { Arc::from_raw(param) });
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_render_param_set_automation_rate(
    param: *const RenderAudioParam,
    rate: AutomationRate,
) {
    unsafe { &*param }.set_automation_rate(rate);
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_render_param_automation_rate(
    param: *const RenderAudioParam,
) -> AutomationRate {
    unsafe { &*param }.automation_rate()
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_render_param_clear_inputs(param: *const RenderAudioParam) {
    unsafe { &mut *(*param).state.get() }.inputs.clear();
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_render_param_add_input(
    param: *const RenderAudioParam,
    source: u64,
    output_index: usize,
) {
    unsafe { &mut *(*param).state.get() }
        .inputs
        .push(Input { source, output_index });
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_render_param_compute(
    param: *const RenderAudioParam,
    graph: *mut c_void,
    pull_output: PullOutput,
    context: *const RenderContext,
    output: SpanMut<'_, f32>,
) {
    let graph = ForeignRenderGraph { graph, pull_output };
    unsafe {
        (&*param).compute(&graph, &*context, output.into_slice());
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
    pulled_source_buses: UnsafeCell<smallvec::SmallVec<[*const AudioBus; 4]>>,
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
            input_buses.push(AudioBus::new(1, quantum_size));
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
            pulled_source_buses: UnsafeCell::new(smallvec::SmallVec::new()),
            outputs: UnsafeCell::new(outputs),
            outgoing_node_connections: UnsafeCell::new(Vec::new()),
            outgoing_param_connections: UnsafeCell::new(Vec::new()),
            processed_quantum: std::cell::Cell::new(u64::MAX),
            is_processing: std::cell::Cell::new(false),
        }
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
        let source_buses = unsafe { &mut *self.pulled_source_buses.get() };
        source_buses.clear();
        for connection in connections {
            let bus = graph.pull_output(connection.source, connection.output_index, context);
            if !bus.is_null() {
                source_buses.push(bus);
            }
        }
        if source_buses.is_empty() {
            unsafe {
                (&mut *input_bus).set_channel_count(1);
                (&mut *input_bus).zero();
            }
            return input_bus;
        }

        let mut computed_number_of_channels = 0;
        match self.channel_count_mode.get() {
            ChannelCountMode::Max | ChannelCountMode::ClampedMax => {
                // "max": computedNumberOfChannels is the maximum of the number of channels of all connections to an input.
                //        In this mode channelCount is ignored.
                // "clamped-max": computedNumberOfChannels is determined as for "max" and then clamped to a maximum value of
                //                the given channelCount.
                for &bus in source_buses.iter() {
                    computed_number_of_channels = computed_number_of_channels.max(unsafe { &*bus }.channel_count());
                }
                if self.channel_count_mode.get() == ChannelCountMode::ClampedMax {
                    computed_number_of_channels = computed_number_of_channels.min(self.channel_count.get());
                }
            }
            ChannelCountMode::Explicit => {
                // "explicit": computedNumberOfChannels is the exact value as specified by the channelCount.
                computed_number_of_channels = self.channel_count.get();
            }
        }
        unsafe {
            (&mut *input_bus).set_channel_count(computed_number_of_channels);
            (&mut *input_bus).zero();
        }
        for &bus in source_buses.iter() {
            unsafe {
                (&mut *input_bus).sum_from(
                    &*bus,
                    self.channel_interpretation.get() == ChannelInterpretation::Speakers,
                );
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
}

pub struct NodeHandle {
    pub(crate) node: Box<dyn RenderNode>,
}

impl NodeHandle {
    pub fn data(&self) -> &RenderNodeData {
        self.node.data()
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct ExternalNodeCallbacks {
    pub process: unsafe extern "C" fn(*mut c_void, *const super::render_graph::RenderGraph, *const RenderContext),
    pub set_ring: unsafe extern "C" fn(*mut c_void, *mut c_void, u32),
    pub for_each_param:
        unsafe extern "C" fn(*mut c_void, *mut c_void, unsafe extern "C" fn(*mut c_void, *const RenderAudioParam)),
    pub ended: unsafe extern "C" fn(*mut c_void) -> bool,
    pub destroy: unsafe extern "C" fn(*mut c_void),
}

pub struct ExternalRenderNode {
    pub data: RenderNodeData,
    pub cpp_node: *mut c_void,
    pub callbacks: ExternalNodeCallbacks,
}

// SAFETY: The C++ node is transferred to the rendering thread together with its Rust base data.
unsafe impl Send for ExternalRenderNode {}
impl Drop for ExternalRenderNode {
    fn drop(&mut self) {
        unsafe {
            (self.callbacks.destroy)(self.cpp_node);
        }
    }
}

impl RenderNode for ExternalRenderNode {
    unsafe fn take_ended_notification(&self) -> bool {
        unsafe { (self.callbacks.ended)(self.cpp_node) }
    }

    fn data(&self) -> &RenderNodeData {
        &self.data
    }

    unsafe fn process(&self, graph: &super::render_graph::RenderGraph, context: &RenderContext) {
        unsafe {
            (self.callbacks.process)(self.cpp_node, graph, context);
        }
    }

    unsafe fn handle_message(&self, message: &super::node_message::NodeMessage) {
        if let super::node_message::NodeMessage::SetMediaStreamSourceRing(message) = message {
            unsafe {
                (self.callbacks.set_ring)(
                    self.cpp_node,
                    message.ring.as_ref().map_or(std::ptr::null_mut(), |ring| ring.pointer),
                    message.channel_count,
                );
            }
        }
    }

    fn for_each_param(&self, callback: &mut dyn FnMut(&RenderAudioParam)) {
        unsafe extern "C" fn call(context: *mut c_void, param: *const RenderAudioParam) {
            let callback = unsafe { &mut *context.cast::<&mut dyn FnMut(&RenderAudioParam)>() };
            callback(unsafe { &*param });
        }
        let mut callback = callback;
        unsafe {
            (self.callbacks.for_each_param)(self.cpp_node, (&raw mut callback).cast(), call);
        }
    }
}

pub trait OutputGraph {
    fn pull_output(&self, source: u64, output_index: usize, context: &RenderContext) -> *const AudioBus;
}

impl OutputGraph for ForeignRenderGraph {
    fn pull_output(&self, source: u64, output_index: usize, context: &RenderContext) -> *const AudioBus {
        unsafe { (self.pull_output)(self.graph, source, output_index, context) }
    }
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

#[unsafe(no_mangle)]
pub(crate) extern "C" fn web_audio_node_create_external(
    node_id: u64,
    input_count: usize,
    output_count: usize,
    quantum_size: usize,
    output_channel_count: usize,
    cpp_node: *mut c_void,
    callbacks: ExternalNodeCallbacks,
) -> *mut NodeHandle {
    Box::into_raw(Box::new(NodeHandle {
        node: Box::new(ExternalRenderNode {
            data: RenderNodeData::new(node_id, input_count, output_count, quantum_size, output_channel_count),
            cpp_node,
            callbacks,
        }),
    }))
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_node_destroy(node: *mut NodeHandle) {
    drop(unsafe { Box::from_raw(node) });
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_node_id(node: *const NodeHandle) -> u64 {
    unsafe { &*node }.data().node_id
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_node_input_count(node: *const NodeHandle) -> usize {
    unsafe { &*node }.data().input_count()
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_node_output_count(node: *const NodeHandle) -> usize {
    unsafe { &*node }.data().output_count()
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_node_output(node: *const NodeHandle, index: usize) -> *mut AudioBus {
    unsafe { (&*node).data().output(index) }
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_node_pull_input(
    node: *const NodeHandle,
    graph: *const super::render_graph::RenderGraph,
    context: *const RenderContext,
    input_index: usize,
) -> *const AudioBus {
    unsafe { (&*node).data().pull_input(&*graph, &*context, input_index) }
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_node_channel_count(node: *const NodeHandle) -> usize {
    unsafe { &*node }.data().channel_count.get()
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_node_channel_count_mode(node: *const NodeHandle) -> ChannelCountMode {
    unsafe { &*node }.data().channel_count_mode.get()
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_node_channel_interpretation(
    node: *const NodeHandle,
) -> ChannelInterpretation {
    unsafe { &*node }.data().channel_interpretation.get()
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_node_set_channel_count(node: *const NodeHandle, value: usize) {
    unsafe { &*node }.data().channel_count.set(value);
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_node_set_channel_count_mode(
    node: *const NodeHandle,
    value: ChannelCountMode,
) {
    unsafe { &*node }.data().channel_count_mode.set(value);
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_node_set_channel_interpretation(
    node: *const NodeHandle,
    value: ChannelInterpretation,
) {
    unsafe { &*node }.data().channel_interpretation.set(value);
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_node_has_input_connections(node: *const NodeHandle, input: usize) -> bool {
    !(unsafe { &*(&*node).data().input_connections.get() })[input].is_empty()
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_node_outgoing_node_connection_count(node: *const NodeHandle) -> usize {
    unsafe { &*(&*node).data().outgoing_node_connections.get() }.len()
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_audio_node_outgoing_param_connection_count(node: *const NodeHandle) -> usize {
    unsafe { &*(&*node).data().outgoing_param_connections.get() }.len()
}
