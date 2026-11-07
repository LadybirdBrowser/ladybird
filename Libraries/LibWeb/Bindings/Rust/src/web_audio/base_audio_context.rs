/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::collapsible_if, reason = "preserve the C++ branch structure")]

use std::cell::{Cell, RefCell};
use std::ffi::c_void;
use std::ops::Deref;
use std::ptr::NonNull;
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::DeferGc;
use libjs_rust::api::{ArrayBuffer, EmbedderClass, EmbedderValue, detach_array_buffer};
use libjs_rust::api::{
    EmbedderCell, ErrorKind, ErrorType, Gc, GcRefCell, GcWeak, Object, Realm, ThrowCompletionOr, Trace, Value, Visitor,
    Vm,
};
use libweb_rust::web_audio::control_message::ControlMessage;
use libweb_rust::web_audio::control_message_queue::ControlMessageQueue;
use libweb_rust::web_audio::rendering::audio_worklet_pipe::{AudioWorkletPipe, WorkletPipeState};

#[cfg(web_idl_bindings)]
use super::analyser_node::{AnalyserNode, AnalyserOptions};
use super::audio_buffer::AudioBuffer;
#[cfg(web_idl_bindings)]
use super::audio_buffer_source_node::{AudioBufferSourceNode, AudioBufferSourceOptions};
use super::audio_context::AudioContext;
use super::audio_destination_node::AudioDestinationNode;
use super::audio_listener::AudioListener;
use super::audio_node::AnyAudioNode;
#[cfg(web_idl_bindings)]
use super::biquad_filter_node::{BiquadFilterNode, BiquadFilterOptions};
#[cfg(web_idl_bindings)]
use super::channel_merger_node::{ChannelMergerNode, ChannelMergerOptions};
#[cfg(web_idl_bindings)]
use super::channel_splitter_node::{ChannelSplitterNode, ChannelSplitterOptions};
#[cfg(web_idl_bindings)]
use super::constant_source_node::{ConstantSourceNode, ConstantSourceOptions};
#[cfg(web_idl_bindings)]
use super::convolver_node::{ConvolverNode, ConvolverOptions};
#[cfg(web_idl_bindings)]
use super::delay_node::{DelayNode, DelayOptions};
#[cfg(web_idl_bindings)]
use super::dynamics_compressor_node::{DynamicsCompressorNode, DynamicsCompressorOptions};
#[cfg(web_idl_bindings)]
use super::gain_node::{GainNode, GainOptions};
use super::iir_filter_node::{IIRFilterNode, IIRFilterOptions};
use super::media_stream_audio_source_node::MediaStreamAudioSourceNode;
use super::offline_audio_context::OfflineAudioContext;
#[cfg(web_idl_bindings)]
use super::oscillator_node::{OscillatorNode, OscillatorOptions};
#[cfg(web_idl_bindings)]
use super::panner_node::{PannerNode, PannerOptions};
use super::periodic_wave::{PeriodicWave, PeriodicWaveConstraints, PeriodicWaveOptions};
use super::script_processor_node::ScriptProcessorNode;
#[cfg(web_idl_bindings)]
use super::stereo_panner_node::{StereoPannerNode, StereoPannerOptions};
#[cfg(web_idl_bindings)]
use super::wave_shaper_node::{WaveShaperNode, WaveShaperOptions};
use crate::bindings::callback::CallbackFunction;
#[cfg(web_idl_bindings)]
use crate::bindings::conversions::EmptyDictionary;
use crate::bindings::conversions::{CppInterface, ToJs};
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::{CppEvent, CppEventTarget, GetTheParent};
use crate::bindings::interface::with_temporary_execution_context_and_callbacks;
use crate::bindings::interface::{Inherits, Ref, Wrappable, Wrapped, cell_if_abi, cell_into_abi};
use crate::bindings::promise::{WebIdlPromise, pending_steps, run_pending_steps};
use crate::bindings::promise::{create_promise, exception_value, promise_realm, reject_promise, resolve_promise};
use crate::bindings::{CallContext, Exception, WebIdlResult};

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum AudioContextState {
    Suspended,
    Running,
    Closed,
}

struct MediaElementTaskSource(NonNull<c_void>);

impl Drop for MediaElementTaskSource {
    fn drop(&mut self) {
        unsafe { web_audio_context_destroy_task_source(self.0.as_ptr()) };
    }
}

// https://webaudio.github.io/web-audio-api/#BaseAudioContext
pub(crate) struct BaseAudioContext {
    event_target: CppEventTarget,
    pub(crate) destination: Cell<Option<Ref<AudioDestinationNode>>>,
    pub(crate) pending_promises: GcRefCell<Vec<WebIdlPromise>>,
    next_node_id: Cell<u64>,
    sample_rate: Cell<f32>,
    pub(crate) listener: Cell<Option<Ref<AudioListener>>>,
    global_object: CppCell,
    state: Cell<AudioContextState>,
    control_thread_state: Cell<AudioContextState>,
    rendering_thread_state: Cell<AudioContextState>,
    media_element_event_task_source: MediaElementTaskSource,
    document_observer: GcRefCell<Option<CppCell>>,
    audio_worklet: GcRefCell<Option<CppCell>>,
    control_message_queue: Arc<ControlMessageQueue>,
    playing_sources: GcRefCell<Vec<(u64, AnyAudioNode)>>,
    script_processor_nodes: RefCell<Vec<GcWeak<EmbedderCell<Wrapped<ScriptProcessorNode>>>>>,
    audio_worklet_pipes: RefCell<Vec<Arc<AudioWorkletPipe>>>,

    // Kept alive only while they can still produce audio; see visit_edges().
    media_stream_sources: RefCell<Vec<GcWeak<EmbedderCell<Wrapped<MediaStreamAudioSourceNode>>>>>,
    this: Cell<Option<AnyBaseAudioContext>>,
}

// SAFETY: This traces the context’s strong edges and the weak nodes it keeps alive while rendering.
unsafe impl Trace for BaseAudioContext {
    fn trace(&self, visitor: &mut Visitor) {
        self.event_target.trace(visitor);
        self.destination.trace(visitor);
        self.pending_promises.trace(visitor);
        self.listener.trace(visitor);
        self.global_object.trace(visitor);
        self.document_observer.trace(visitor);
        self.audio_worklet.trace(visitor);
        self.playing_sources.trace(visitor);
        self.this.trace(visitor);
        if self.control_thread_state.get() == AudioContextState::Closed {
            return;
        }

        // INTEROP: Blink keeps a ScriptProcessorNode with audioprocess listeners alive until its context is cleared
        //          (ScriptProcessorNode::HasPendingActivity()), whether it is connected or not, so that a node script only
        //          reaches through its listener keeps producing audio.
        for node in &*self.script_processor_nodes.borrow() {
            if let Some(node) = node.get() {
                if node.parent().event_target().has_event_listener("audioprocess") {
                    node.trace(visitor);
                }
            }
        }

        // Keep a source and the nodes it feeds alive while its track is live, even after script drops the source.
        // https://webaudio.github.io/web-audio-api/#AudioNode-actively-processing
        // NB: A muted or disabled track only pauses its source, so it stays alive as well.
        for source in &*self.media_stream_sources.borrow() {
            if let Some(source) = source.get() {
                if source.has_live_input_track() {
                    source.trace(visitor);
                }
            }
        }
    }
}

impl Wrappable for BaseAudioContext {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for BaseAudioContext {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

unsafe extern "C" {
    fn web_audio_context_create_task_source() -> *mut c_void;
    fn web_audio_context_destroy_task_source(source: *mut c_void);
    fn web_audio_context_queue_task(
        source: *mut c_void,
        global: *mut c_void,
        cell: *mut c_void,
        invoke: unsafe extern "C" fn(*mut c_void, *mut c_void),
    );
    fn web_audio_context_observe_document(global: *mut c_void, cell: *mut c_void) -> *mut c_void;
    fn web_audio_context_global_realm(global: *mut c_void) -> *mut c_void;
    fn web_audio_context_global_wrapper(global: *mut c_void) -> *mut c_void;
    fn web_audio_context_document_is_fully_active(global: *mut c_void) -> bool;
    fn web_audio_context_navigable_is_destroyed(global: *mut c_void) -> bool;
    fn web_audio_context_get_worklet(cell: *mut c_void) -> *mut c_void;
    fn web_audio_context_wrap_worklet(realm: *mut c_void, worklet: *mut c_void) -> *mut c_void;
}

impl BaseAudioContext {
    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffer-numberofchannels
    // > An implementation MUST support at least 32 channels.
    // Other browsers appear to only allow 32 channels - so let's limit ourselves to that too.
    pub(crate) const MAX_NUMBER_OF_CHANNELS: u32 = libweb_rust::web_audio::MAX_NUMBER_OF_CHANNELS as u32;

    // https://webaudio.github.io/web-audio-api/#sample-rates
    // Implementations MUST support sample rates between 3000 Hz and 768000 Hz, inclusive. A NotSupportedError MUST
    // be thrown if a sample rate outside this range is specified.
    pub(crate) const MIN_SAMPLE_RATE: f32 = 3000.;
    pub(crate) const MAX_SAMPLE_RATE: f32 = 768000.;

    // https://webaudio.github.io/web-audio-api/#render-quantum-size
    pub(crate) const RENDER_QUANTUM_SIZE: u32 = 128;

    pub(crate) fn new(_defer_gc: &DeferGc<'_>, global_object: CppCell, sample_rate: f32) -> Self {
        Self {
            event_target: CppEventTarget::new(),
            destination: Cell::new(None),
            pending_promises: GcRefCell::new(Vec::new()),
            next_node_id: Cell::new(0),
            sample_rate: Cell::new(sample_rate),
            listener: Cell::new(None),
            global_object,
            state: Cell::new(AudioContextState::Suspended),
            control_thread_state: Cell::new(AudioContextState::Suspended),
            rendering_thread_state: Cell::new(AudioContextState::Suspended),
            media_element_event_task_source: MediaElementTaskSource(
                NonNull::new(unsafe { web_audio_context_create_task_source() }).unwrap(),
            ),
            document_observer: GcRefCell::new(None),
            audio_worklet: GcRefCell::new(None),
            control_message_queue: Arc::new(ControlMessageQueue::default()),
            playing_sources: GcRefCell::new(Vec::new()),
            script_processor_nodes: RefCell::new(Vec::new()),
            audio_worklet_pipes: RefCell::new(Vec::new()),
            media_stream_sources: RefCell::new(Vec::new()),
            this: Cell::new(None),
        }
    }

    pub(crate) fn initialize(&self, vm: &Vm, this: AnyBaseAudioContext) {
        self.this.set(Some(this));
        let observer = unsafe { web_audio_context_observe_document(self.global_object.as_ptr(), this.as_abi()) };
        if !observer.is_null() {
            *self.document_observer.borrow_mut(vm) = Some(unsafe { CppCell::from_raw(observer) });
        }
    }

    pub(crate) fn this(&self) -> AnyBaseAudioContext {
        self.this.get().expect("a context knows itself after allocation")
    }

    pub(crate) fn event_target(&self) -> &CppEventTarget {
        &self.event_target
    }

    pub(crate) fn global_event_target(&self) -> &CppCell {
        &self.global_object
    }

    pub(crate) fn realm(&self) -> Gc<Realm> {
        unsafe {
            Gc::from_non_null(NonNull::new(web_audio_context_global_realm(self.global_object.as_ptr()).cast()).unwrap())
        }
    }

    pub(crate) fn global(&self) -> Gc<Object> {
        unsafe {
            Gc::from_non_null(
                NonNull::new(web_audio_context_global_wrapper(self.global_object.as_ptr()).cast()).unwrap(),
            )
        }
    }

    pub(crate) fn document_is_fully_active(&self) -> bool {
        unsafe { web_audio_context_document_is_fully_active(self.global_object.as_ptr()) }
    }

    pub(crate) fn navigable_is_destroyed(&self) -> bool {
        unsafe { web_audio_context_navigable_is_destroyed(self.global_object.as_ptr()) }
    }

    pub(crate) fn sample_rate_value(&self) -> f32 {
        self.sample_rate.get()
    }

    pub(crate) fn set_sample_rate(&self, sample_rate: f32) {
        self.sample_rate.set(sample_rate);
    }

    // https://webaudio.github.io/web-audio-api/#--nyquist-frequency
    pub(crate) fn nyquist_frequency(&self) -> f32 {
        self.sample_rate.get() / 2.
    }

    pub(crate) fn state_value(&self) -> AudioContextState {
        self.state.get()
    }

    pub(crate) fn set_state(&self, state: AudioContextState) {
        self.state.set(state);
    }

    pub(crate) fn control_state(&self) -> AudioContextState {
        self.control_thread_state.get()
    }

    pub(crate) fn set_control_state(&self, state: AudioContextState) {
        self.control_thread_state.set(state);
    }

    pub(crate) fn set_rendering_state(&self, state: AudioContextState) {
        self.rendering_thread_state.set(state);
    }

    pub(crate) fn destination_value(&self) -> Ref<AudioDestinationNode> {
        self.destination.get().expect("a context has a destination")
    }

    pub(crate) fn listener_value(&self) -> Ref<AudioListener> {
        self.listener.get().expect("a context has a listener")
    }

    pub(crate) fn control_message_queue(&self) -> &Arc<ControlMessageQueue> {
        &self.control_message_queue
    }

    pub(crate) fn next_node_id(&self) -> u64 {
        let id = self.next_node_id.get() + 1;
        self.next_node_id.set(id);
        id
    }

    pub(crate) fn queue_control_message(&self, message: ControlMessage) {
        self.control_message_queue.enqueue(message);

        // FIXME: Should signal the rendering thread when implemented
    }

    pub(crate) fn add_playing_source(&self, vm: &Vm, node: AnyAudioNode) {
        let mut sources = self.playing_sources.borrow_mut(vm);
        if let Some(entry) = sources.iter_mut().find(|entry| entry.0 == node.node_id()) {
            entry.1 = node;
        } else {
            sources.push((node.node_id(), node));
        }
    }

    pub(crate) fn add_script_processor_node(&self, vm: &Vm, node: Ref<ScriptProcessorNode>) {
        let mut nodes = self.script_processor_nodes.borrow_mut();
        nodes.retain(|node| node.get().is_some());
        if !nodes.iter().any(|entry| entry.get() == Some(node)) {
            nodes.push(GcWeak::new(vm.heap(), node));
        }
    }

    pub(crate) fn add_media_stream_source(&self, vm: &Vm, source: Ref<MediaStreamAudioSourceNode>) {
        let mut sources = self.media_stream_sources.borrow_mut();
        sources.retain(|source| source.get().is_some());
        if !sources.iter().any(|entry| entry.get() == Some(source)) {
            sources.push(GcWeak::new(vm.heap(), source));
        }
    }

    pub(crate) fn handle_ended_sources(&self, vm: &Vm, ended_nodes: &[u64]) {
        for node_id in ended_nodes {
            let node = {
                let mut sources = self.playing_sources.borrow_mut(vm);
                sources
                    .iter()
                    .position(|entry| entry.0 == *node_id)
                    .map(|index| sources.remove(index).1)
            };
            let Some(node) = node else {
                continue;
            };
            self.queue_a_media_element_task(vm, node, |_, node| {
                node.event_target().dispatch_event(&CppEvent::create(
                    node.context_value().realm(),
                    "ended",
                    false,
                    false,
                    false,
                ));
            });
        }
    }

    pub(crate) fn queue_a_media_element_task<C: Trace + Copy + 'static>(
        &self,
        vm: &Vm,
        captures: C,
        steps: fn(&Vm, C),
    ) {
        // Rendering completion callbacks run from the audio thread, outside any
        // JavaScript execution context. Resolve the task target from this context's
        // associated global rather than consulting the thread-local current ESO.
        let cell = pending_steps(vm, captures, steps);
        unsafe {
            web_audio_context_queue_task(
                self.media_element_event_task_source.0.as_ptr(),
                self.global_object.as_ptr(),
                cell,
                run_pending_steps,
            );
        };
    }

    pub(crate) fn dispatch_statechange(&self) {
        self.event_target
            .dispatch_event(&CppEvent::create(self.realm(), "statechange", false, false, false));
    }

    pub(crate) fn audio_worklet_cell(&self, vm: &Vm) -> CppCell {
        if self.audio_worklet.borrow().is_none() {
            let worklet = unsafe { CppCell::from_raw(web_audio_context_get_worklet(self.this().as_abi())) };
            *self.audio_worklet.borrow_mut(vm) = Some(worklet);
        }
        self.audio_worklet.borrow().as_ref().unwrap().clone()
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-audioworklet
    pub(crate) fn audio_worklet(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        let worklet = self.audio_worklet_cell(cx.vm());
        let wrapper = unsafe { web_audio_context_wrap_worklet(cx.realm().as_ptr().cast(), worklet.as_ptr()) };
        Ok(unsafe { CppInterface::from_wrapper(Gc::<Object>::from_non_null(NonNull::new(wrapper.cast()).unwrap())) })
    }

    pub(crate) fn add_audio_worklet_pipe(&self, pipe: Arc<AudioWorkletPipe>) {
        let mut pipes = self.audio_worklet_pipes.borrow_mut();
        pipes.retain(|pipe| pipe.state() != WorkletPipeState::ShutDown);
        pipes.push(pipe);
    }

    // Marks every worklet node's pipe shut down and drops the pump slots; called when the context
    // closes or its document stops being fully active.
    pub(crate) fn shutdown_audio_worklet(&self) {
        for pipe in self.audio_worklet_pipes.take() {
            pipe.set_client(None);
            pipe.set_state(WorkletPipeState::ShutDown);
        }
    }

    pub(crate) fn destination(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioDestinationNode>> {
        Ok(self.destination_value())
    }

    pub(crate) fn sample_rate(&self, _: &mut CallContext) -> WebIdlResult<f32> {
        Ok(self.sample_rate_value())
    }

    pub(crate) fn listener(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioListener>> {
        Ok(self.listener_value())
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-state
    pub(crate) fn state(&self, _: &mut CallContext) -> WebIdlResult<AudioContextState> {
        // Getting this attribute returns the contents of the [[control thread state]] slot.
        // AD-HOC: The tasks queued by resume(), suspend() and close() set the state attribute after the
        //         [[control thread state]] has changed, so the attribute has its own slot.
        Ok(self.state_value())
    }

    pub(crate) fn render_quantum_size(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(Self::RENDER_QUANTUM_SIZE)
    }

    pub(crate) fn current_time(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.current_time_value())
    }

    pub(crate) fn current_time_value(&self) -> f64 {
        match self.this() {
            AnyBaseAudioContext::AudioContext(context) => context.current_time_value(),
            AnyBaseAudioContext::OfflineAudioContext(context) => context.current_time_value(),
            AnyBaseAudioContext::BaseAudioContext(_) => 0.,
        }
    }

    pub(crate) fn verify_sample_rate(sample_rate: f32) -> WebIdlResult<()> {
        if sample_rate < Self::MIN_SAMPLE_RATE || sample_rate > Self::MAX_SAMPLE_RATE {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"Sample rate is outside of allowed range"),
            ));
        }
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffer
    pub(crate) fn verify_audio_options_inside_nominal_range(
        number_of_channels: u32,
        length: u32,
        sample_rate: f32,
    ) -> WebIdlResult<()> {
        // A NotSupportedError exception MUST be thrown if any of the arguments is negative, zero, or outside its nominal range.
        if number_of_channels == 0 {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"Number of channels must not be '0'"),
            ));
        }
        if number_of_channels > Self::MAX_NUMBER_OF_CHANNELS {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"Number of channels is greater than allowed range"),
            ));
        }
        if length == 0 {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"Length of buffer must be at least 1"),
            ));
        }
        Self::verify_sample_rate(sample_rate)
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffer
    pub(crate) fn create_buffer(
        &self,
        cx: &mut CallContext,
        number_of_channels: u32,
        length: u32,
        sample_rate: f32,
    ) -> WebIdlResult<Ref<AudioBuffer>> {
        // Creates an AudioBuffer of the given size. The audio data in the buffer will be zero-initialized (silent).
        // A NotSupportedError exception MUST be thrown if any of the arguments is negative, zero, or outside its nominal range.
        Self::verify_audio_options_inside_nominal_range(number_of_channels, length, sample_rate)?;
        if length as usize > i32::MAX as usize / size_of::<f32>() {
            return Err(cx
                .vm()
                .throw_completion::<()>(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"typed array"])
                .unwrap_err()
                .into());
        }
        AudioBuffer::create(cx.vm(), number_of_channels, length, sample_rate).map_err(|_| {
            cx.vm()
                .throw_completion::<()>(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                .unwrap_err()
                .into()
        })
    }
}

#[cfg(web_idl_bindings)]
impl BaseAudioContext {
    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createanalyser
    pub(crate) fn create_analyser(&self, cx: &mut CallContext) -> WebIdlResult<Ref<AnalyserNode>> {
        AnalyserNode::create(cx, self.this(), &AnalyserOptions::empty())
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbiquadfilter
    pub(crate) fn create_biquad_filter(&self, cx: &mut CallContext) -> WebIdlResult<Ref<BiquadFilterNode>> {
        // Factory method for a BiquadFilterNode representing a second order filter which can be configured as one of several common filter types.
        BiquadFilterNode::create(cx, self.this(), &BiquadFilterOptions::empty())
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffersource
    pub(crate) fn create_buffer_source(&self, cx: &mut CallContext) -> WebIdlResult<Ref<AudioBufferSourceNode>> {
        // Factory method for a AudioBufferSourceNode.
        AudioBufferSourceNode::create(cx, self.this(), &AudioBufferSourceOptions::empty())
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createconstantsource
    pub(crate) fn create_constant_source(&self, cx: &mut CallContext) -> WebIdlResult<Ref<ConstantSourceNode>> {
        ConstantSourceNode::create(cx, self.this(), &ConstantSourceOptions::empty())
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createconvolver
    pub(crate) fn create_convolver(&self, cx: &mut CallContext) -> WebIdlResult<Ref<ConvolverNode>> {
        ConvolverNode::create(cx, self.this(), &ConvolverOptions::empty())
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createoscillator
    pub(crate) fn create_oscillator(&self, cx: &mut CallContext) -> WebIdlResult<Ref<OscillatorNode>> {
        // Factory method for an OscillatorNode.
        OscillatorNode::create(cx, self.this(), &OscillatorOptions::empty())
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createdynamicscompressor
    pub(crate) fn create_dynamics_compressor(&self, cx: &mut CallContext) -> WebIdlResult<Ref<DynamicsCompressorNode>> {
        // Factory method for a DynamicsCompressorNode.
        DynamicsCompressorNode::create(cx, self.this(), &DynamicsCompressorOptions::empty())
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-creategain
    pub(crate) fn create_gain(&self, cx: &mut CallContext) -> WebIdlResult<Ref<GainNode>> {
        // Factory method for GainNode.
        GainNode::create(cx, self.this(), &GainOptions::empty())
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createpanner
    pub(crate) fn create_panner(&self, cx: &mut CallContext) -> WebIdlResult<Ref<PannerNode>> {
        // Factory method for a PannerNode.
        PannerNode::create(cx, self.this(), &PannerOptions::empty())
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createstereopanner
    pub(crate) fn create_stereo_panner(&self, cx: &mut CallContext) -> WebIdlResult<Ref<StereoPannerNode>> {
        // Factory method for a StereoPannerNode.
        StereoPannerNode::create(cx, self.this(), &StereoPannerOptions::empty())
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createwaveshaper
    pub(crate) fn create_wave_shaper(&self, cx: &mut CallContext) -> WebIdlResult<Ref<WaveShaperNode>> {
        // Factory method for a WaveShaperNode representing a non-linear distortion.
        WaveShaperNode::create(cx, self.this(), &WaveShaperOptions::empty())
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createchannelmerger
    pub(crate) fn create_channel_merger(
        &self,
        cx: &mut CallContext,
        number_of_inputs: u32,
    ) -> WebIdlResult<Ref<ChannelMergerNode>> {
        let mut options = ChannelMergerOptions::empty();
        options.number_of_inputs = number_of_inputs;
        ChannelMergerNode::validate_options(&options)?;
        ChannelMergerNode::create(cx, self.this(), &options)
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createchannelsplitter
    pub(crate) fn create_channel_splitter(
        &self,
        cx: &mut CallContext,
        number_of_outputs: u32,
    ) -> WebIdlResult<Ref<ChannelSplitterNode>> {
        let mut options = ChannelSplitterOptions::empty();
        options.number_of_outputs = number_of_outputs;
        ChannelSplitterNode::validate_options(&options)?;
        ChannelSplitterNode::create(cx, self.this(), &options)
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createdelay
    pub(crate) fn create_delay(&self, cx: &mut CallContext, max_delay_time: f64) -> WebIdlResult<Ref<DelayNode>> {
        let mut options = DelayOptions::empty();
        options.max_delay_time = max_delay_time;
        DelayNode::validate_options(&options)?;
        DelayNode::create(cx, self.this(), &options)
    }
}

impl BaseAudioContext {
    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createiirfilter
    pub(crate) fn create_iir_filter(
        &self,
        cx: &mut CallContext,
        feedforward: Vec<f64>,
        feedback: Vec<f64>,
    ) -> WebIdlResult<Ref<IIRFilterNode>> {
        // Factory method for an IIRFilterNode representing a general IIR Filter.
        let options = IIRFilterOptions {
            parent: super::audio_node::AudioNodeOptions::default(),
            feedforward,
            feedback,
        };
        IIRFilterNode::create(cx, self.this(), &options)
    }

    pub(crate) fn create_periodic_wave(
        &self,
        cx: &mut CallContext,
        real: Vec<f32>,
        imag: Vec<f32>,
        constraints: PeriodicWaveConstraints,
    ) -> WebIdlResult<Ref<PeriodicWave>> {
        // 1. If real and imag are not of the same length, an IndexSizeError MUST be thrown.
        // NB: The PeriodicWave constructor checks the lengths.
        // 2. Let o be a new object of type PeriodicWaveOptions.
        // 3. Respectively set the real and imag parameters passed to this factory method to the attributes of the same name on o.
        // 4. Set the disableNormalization attribute on o to the value of the disableNormalization attribute of the constraints attribute passed to the factory method.
        let options = PeriodicWaveOptions {
            parent: constraints,
            real: Some(real),
            imag: Some(imag),
        };

        // 5. Construct a new PeriodicWave p, passing the BaseAudioContext this factory method has been called on as a first argument, and o.
        // 6. Return p.
        PeriodicWave::create(cx, options)
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createscriptprocessor
    pub(crate) fn create_script_processor(
        &self,
        cx: &mut CallContext,
        mut buffer_size: u32,
        number_of_input_channels: u32,
        number_of_output_channels: u32,
    ) -> WebIdlResult<Ref<ScriptProcessorNode>> {
        // The bufferSize parameter determines the buffer size in units of sample-frames. If it’s not passed in, or if the
        // value is 0, then the implementation will choose the best buffer size for the given environment, which will be
        // constant power of 2 throughout the lifetime of the node.
        if buffer_size == 0 {
            buffer_size = ScriptProcessorNode::DEFAULT_BUFFER_SIZE as u32;
        }
        ScriptProcessorNode::validate_options(buffer_size as i32, number_of_input_channels, number_of_output_channels)?;
        ScriptProcessorNode::create(
            cx,
            self.this(),
            buffer_size as i32,
            number_of_input_channels,
            number_of_output_channels,
        )
    }
}

#[derive(Clone, Copy, PartialEq, Trace)]
#[gc(crate = libjs_rust)]
#[allow(clippy::enum_variant_names, reason = "the variants have the interfaces’ names")]
pub(crate) enum AnyBaseAudioContext {
    BaseAudioContext(Ref<BaseAudioContext>),
    AudioContext(Ref<AudioContext>),
    OfflineAudioContext(Ref<OfflineAudioContext>),
}

impl Deref for AnyBaseAudioContext {
    type Target = BaseAudioContext;

    fn deref(&self) -> &BaseAudioContext {
        match self {
            Self::BaseAudioContext(context) => context,
            Self::AudioContext(context) => context.parent(),
            Self::OfflineAudioContext(context) => context.parent(),
        }
    }
}

impl ToJs for AnyBaseAudioContext {
    fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value> {
        match self {
            Self::BaseAudioContext(context) => context.to_js(cx),
            Self::AudioContext(context) => context.to_js(cx),
            Self::OfflineAudioContext(context) => context.to_js(cx),
        }
    }
}

impl AnyBaseAudioContext {
    pub(crate) fn as_abi(self) -> *mut c_void {
        match self {
            Self::BaseAudioContext(context) => cell_into_abi(context),
            Self::AudioContext(context) => cell_into_abi(context),
            Self::OfflineAudioContext(context) => cell_into_abi(context),
        }
    }

    pub(crate) unsafe fn from_abi(cell: *mut c_void) -> Self {
        if let Some(context) = unsafe { cell_if_abi::<BaseAudioContext>(cell) } {
            return Self::BaseAudioContext(context);
        }
        if let Some(context) = unsafe { cell_if_abi::<AudioContext>(cell) } {
            return Self::AudioContext(context);
        }
        if let Some(context) = unsafe { cell_if_abi::<OfflineAudioContext>(cell) } {
            return Self::OfflineAudioContext(context);
        }
        unreachable!("C++ holds a context cell")
    }
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_rust_audio_context_sample_rate(cell: *mut c_void) -> f32 {
    unsafe { AnyBaseAudioContext::from_abi(cell) }.sample_rate_value()
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_rust_audio_context_global(cell: *mut c_void) -> *mut c_void {
    unsafe { AnyBaseAudioContext::from_abi(cell) }.global_object.as_ptr()
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_rust_audio_context_global_wrapper(cell: *mut c_void) -> *mut c_void {
    unsafe { AnyBaseAudioContext::from_abi(cell) }.global().as_ptr().cast()
}

// Called when the associated document is destroyed or otherwise stops being fully active, so rendering resources
// can be released.
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_rust_audio_context_document_inactive(cell: *mut c_void) {
    match unsafe { AnyBaseAudioContext::from_abi(cell) } {
        AnyBaseAudioContext::AudioContext(context) => context.document_became_inactive(),
        AnyBaseAudioContext::OfflineAudioContext(context) => context.document_became_inactive(),
        AnyBaseAudioContext::BaseAudioContext(_) => {}
    }
}

struct DecodedAudioData(NonNull<c_void>);

impl Drop for DecodedAudioData {
    fn drop(&mut self) {
        unsafe { web_audio_context_decoded_destroy(self.0.as_ptr()) };
    }
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct DecodingOperation {
    context: AnyBaseAudioContext,
    promise: WebIdlPromise,
    success_callback: Option<CallbackFunction>,
    error_callback: Option<CallbackFunction>,
    #[gc(untraced)]
    decoded: RefCell<Option<DecodedAudioData>>,
}

static DECODING_OPERATION_CLASS: EmbedderClass<DecodingOperation> = EmbedderClass::new("WebAudioDecodingOperation");

impl EmbedderValue for DecodingOperation {
    const CLASS: &'static EmbedderClass<Self> = &DECODING_OPERATION_CLASS;
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct DetachedAudioDataError {
    realm: Gc<Realm>,
    callback: CallbackFunction,
    error: Value,
}

static DETACHED_AUDIO_DATA_ERROR_CLASS: EmbedderClass<DetachedAudioDataError> =
    EmbedderClass::new("DetachedAudioDataError");
impl EmbedderValue for DetachedAudioDataError {
    const CLASS: &'static EmbedderClass<Self> = &DETACHED_AUDIO_DATA_ERROR_CLASS;
}

unsafe extern "C" {
    fn web_audio_context_decode(data: ak::Span<'_, u8>, sample_rate: u32, operation: *mut c_void);
    fn web_audio_context_decoded_destroy(decoded: *mut c_void);
    fn web_audio_context_decoded_metadata(
        decoded: *mut c_void,
        channels: *mut u32,
        frames: *mut u64,
        sample_rate: *mut u32,
    );
    fn web_audio_context_decoded_channel(decoded: *mut c_void, channel: u32, samples: *mut ak::Span<'_, f32>);
    fn web_audio_context_report_exception(realm: *mut c_void, error: u64);
}

impl BaseAudioContext {
    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-decodeaudiodata
    pub(crate) fn decode_audio_data(
        &self,
        cx: &mut CallContext,
        audio_data: Gc<ArrayBuffer>,
        success_callback: Option<Option<CallbackFunction>>,
        error_callback: Option<Option<CallbackFunction>>,
    ) -> WebIdlResult<WebIdlPromise> {
        let vm = cx.vm();
        let promise = create_promise(vm, self.realm());
        if audio_data.is_detached() {
            let error = exception_value(
                vm,
                promise_realm(promise),
                Exception::Dom("DataCloneError", Utf16String::from_ascii(b"Audio data is detached.")),
            );
            reject_promise(vm, promise, error);
            if let Some(callback) = error_callback.flatten() {
                let state = DETACHED_AUDIO_DATA_ERROR_CLASS.allocate(vm, |_| DetachedAudioDataError {
                    realm: cx.realm(),
                    callback,
                    error,
                });
                self.queue_a_media_element_task(vm, state, |_, state| {
                    if let Err(error) = state.callback.invoke(None, &[state.error]) {
                        unsafe {
                            web_audio_context_report_exception(state.realm.as_ptr().cast(), error.value().to_raw());
                        };
                    }
                });
            }
            return Ok(promise);
        }
        let mut bytes = Vec::new();
        bytes.try_reserve_exact(audio_data.byte_length()).map_err(|_| {
            Exception::from(
                vm.throw_completion::<()>(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
                    .unwrap_err(),
            )
        })?;
        bytes.resize(audio_data.byte_length(), 0);
        audio_data.copy_to(0, &mut bytes);
        detach_array_buffer(vm, audio_data, None)?;

        // FIXME: When decodeAudioData is called, the following steps MUST be performed on the control thread:
        // 1. If this's relevant global object's associated Document is not fully active then return a
        //    promise rejected with "InvalidStateError" DOMException.
        if !self.document_is_fully_active() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"The document is not fully active."),
            ));
        }

        // 3. Append promise to [[pending promises]] and queue a decoding operation.
        self.pending_promises.borrow_mut(vm).push(promise);
        let operation = DECODING_OPERATION_CLASS.allocate(vm, |_| DecodingOperation {
            context: self.this(),
            promise,
            success_callback: success_callback.flatten(),
            error_callback: error_callback.flatten(),
            decoded: RefCell::new(None),
        });
        self.queue_a_decoding_operation(vm, operation, &bytes);
        Ok(promise)
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-decodeaudiodata
    fn queue_a_decoding_operation(&self, vm: &Vm, operation: Gc<EmbedderCell<DecodingOperation>>, audio_data: &[u8]) {
        // NB: The decoding thread is in the media server, which answers on this thread. Promise settlement and callbacks
        //     are posted to the media element task source below.
        if audio_data.is_empty() {
            queue_decode_error(vm, operation);
            return;
        }

        // The answer arrives outside any task, so the context and callbacks are rooted until it does.
        unsafe {
            web_audio_context_decode(
                ak::Span::new(audio_data),
                self.sample_rate_value() as u32,
                operation.as_ptr().cast(),
            );
        };
    }
}

fn queue_decode_error(vm: &Vm, operation: Gc<EmbedderCell<DecodingOperation>>) {
    operation
        .context
        .queue_a_media_element_task(vm, operation, |vm, operation| {
            let realm = promise_realm(operation.promise);
            with_temporary_execution_context_and_callbacks(realm, || {
                let error = exception_value(
                    vm,
                    realm,
                    Exception::Dom("EncodingError", Utf16String::from_ascii(b"Unable to decode.")),
                );
                reject_promise(vm, operation.promise, error);
                {
                    let mut pending = operation.context.pending_promises.borrow_mut(vm);
                    if let Some(index) = pending.iter().position(|promise| *promise == operation.promise) {
                        pending.remove(index);
                    }
                }
                if let Some(callback) = &operation.error_callback {
                    if let Err(error) = callback.invoke(None, &[error]) {
                        unsafe { web_audio_context_report_exception(realm.as_ptr().cast(), error.value().to_raw()) };
                    }
                }
            });
        });
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_rust_audio_context_decoded(vm: &Vm, operation: *mut c_void, decoded: *mut c_void) {
    let operation: Gc<EmbedderCell<DecodingOperation>> =
        unsafe { Gc::from_non_null(NonNull::new(operation.cast()).unwrap()) };
    let Some(decoded) = NonNull::new(decoded) else {
        queue_decode_error(vm, operation);
        return;
    };
    let decoded = DecodedAudioData(decoded);
    let mut channel_count = 0;
    let mut frame_count = 0;
    let mut sample_rate = 0;
    unsafe {
        web_audio_context_decoded_metadata(
            decoded.0.as_ptr(),
            &raw mut channel_count,
            &raw mut frame_count,
            &raw mut sample_rate,
        );
    };
    if frame_count > u64::from(u32::MAX) {
        queue_decode_error(vm, operation);
        return;
    }
    *operation.decoded.borrow_mut() = Some(decoded);
    operation.context.queue_a_media_element_task(
        vm,
        (operation, channel_count, frame_count, sample_rate),
        |vm, (operation, channel_count, frame_count, sample_rate)| {
            let buffer = AudioBuffer::create(vm, channel_count, frame_count as u32, sample_rate as f32);
            let Ok(buffer) = buffer else {
                queue_decode_error(vm, operation);
                return;
            };
            let decoded = operation.decoded.borrow_mut().take().unwrap();
            for channel in 0..channel_count {
                let (channel_data, length) = buffer
                    .channel_data(channel)
                    .unwrap_or_else(|_| panic!("the decoded channel exists"));
                let mut samples = ak::Span::default();
                unsafe { web_audio_context_decoded_channel(decoded.0.as_ptr(), channel, &raw mut samples) };
                let samples = samples.as_slice();
                let byte_length = size_of_val(samples);
                assert!(byte_length <= length);

                // SAFETY: The new buffer's channel has no views yet.
                let destination = unsafe { ak::slice_from_raw_parts_mut(channel_data, length) };
                let source = unsafe { ak::slice_from_raw_parts(samples.as_ptr().cast::<u8>(), byte_length) };
                destination[..byte_length].copy_from_slice(source);
            }
            let realm = promise_realm(operation.promise);
            with_temporary_execution_context_and_callbacks(realm, || {
                let value = buffer
                    .to_js(&CallContext::new(vm, realm))
                    .expect("an AudioBuffer wraps");
                resolve_promise(vm, operation.promise, value);
                {
                    let mut pending = operation.context.pending_promises.borrow_mut(vm);
                    if let Some(index) = pending.iter().position(|promise| *promise == operation.promise) {
                        pending.remove(index);
                    }
                }
                if let Some(callback) = &operation.success_callback {
                    if let Err(error) = callback.invoke(None, &[value]) {
                        unsafe { web_audio_context_report_exception(realm.as_ptr().cast(), error.value().to_raw()) };
                    }
                }
            });
        },
    );
}
