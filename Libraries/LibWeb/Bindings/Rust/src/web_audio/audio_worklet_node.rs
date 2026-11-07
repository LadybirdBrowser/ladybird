/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::collapsible_if, reason = "preserve the C++ branch structure")]

use std::cell::Cell;
use std::cell::RefCell;
use std::ffi::c_void;
use std::ptr::NonNull;
use std::rc::Rc;
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::{
    Array, EmbedderCell, EmbedderClass, EmbedderValue, ErrorKind, FunctionObject, GcWeak, MarkedVec, Object,
    PropertyKey, RawNativeFunctionResult, TypedArrayBase, TypedArrayKind, Value, Vm, call, construct,
    create_typed_array,
};
use libjs_rust::api::{Gc, Realm, Trace};
use libweb_rust::web_audio::rendering::audio_worklet_pipe::{
    AudioWorkletPipe, AudioWorkletPipeClient, Config, WorkletPipeState, ring_sizing_for_device_latency,
};
use libweb_rust::web_audio::rendering::audio_worklet_render_node::create_audio_worklet_render_node;
use libweb_rust::web_audio::rendering::render_node::RenderAudioParam;

use super::audio_array::{float32_array_length, overwrite_float32_array};
use super::audio_node::{
    AnyAudioNode, AudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::audio_param::{AudioParam, AutomationRate, FixedAutomationRate};
use super::audio_param_map::AudioParamMap;
use super::audio_worklet_processor::AudioWorkletProcessor;
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, Wrapped, allocate};
use crate::bindings::interface::{
    cell_into_abi, implementation_if_wrapper, with_temporary_execution_context_and_callbacks, wrap,
};
use crate::bindings::promise::{TaskSource, queue_a_global_task};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::html::message_port::MessagePort;

// https://webaudio.github.io/web-audio-api/#AudioWorkletNode
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AudioWorkletNode {
    node: AudioNode,

    // https://webaudio.github.io/web-audio-api/#dom-audioworkletnode-audioworkletnode-context-name-options-name
    name: Utf16String,
    number_of_inputs: u32,
    number_of_outputs: u32,
    parameter_map: Ref<AudioParamMap>,
    parameter_names: Vec<Utf16String>, // descriptor order; keys for the `parameters` argument
    port: Ref<MessagePort>,
    scope: CppCell,
    processor: Cell<Option<Ref<AudioWorkletProcessor>>>,
    #[gc(untraced)]
    pipe: RefCell<Option<Arc<AudioWorkletPipe>>>,
}

impl Inherits<AudioNode> for AudioWorkletNode {
    fn parent(&self) -> &AudioNode {
        &self.node
    }
}

impl Wrappable for AudioWorkletNode {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.node.cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(node: Ref<Self>) {
        if let Some(pipe) = &*node.pipe.borrow() {
            pipe.set_client(None);
            pipe.set_state(WorkletPipeState::ShutDown);
        }
        node.node.finalize_node();
    }
}

impl GetTheParent for AudioWorkletNode {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

pub(crate) struct AudioWorkletNodeOptions {
    pub parent: AudioNodeOptions,
    pub number_of_inputs: u32,
    pub number_of_outputs: u32,
    pub output_channel_count: Option<Vec<u32>>,
    pub parameter_data: Option<Vec<(Utf16String, f64)>>,
    pub processor_options: Option<Gc<Object>>,
}

struct AudioParamDescriptor {
    name: Utf16String,
    default_value: f32,
    min_value: f32,
    max_value: f32,
    automation_rate: AutomationRate,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct ProcessorConstructionTask {
    node: Ref<AudioWorkletNode>,
    scope: CppCell,
    processor_port: Ref<MessagePort>,
    options_object: Gc<Object>,
}

static PROCESSOR_CONSTRUCTION_TASK_CLASS: EmbedderClass<ProcessorConstructionTask> =
    EmbedderClass::new("AudioWorkletProcessorConstructionTask");
impl EmbedderValue for ProcessorConstructionTask {
    const CLASS: &'static EmbedderClass<Self> = &PROCESSOR_CONSTRUCTION_TASK_CLASS;
}

unsafe extern "C" {
    fn web_audio_worklet_find_definition(
        worklet: *mut c_void,
        name: usize,
        context: *mut c_void,
        descriptor: unsafe extern "C" fn(*mut c_void, usize, f32, f32, f32, AutomationRate),
    ) -> bool;
    fn web_audio_worklet_global_scope(worklet: *mut c_void) -> *mut c_void;
    fn web_audio_worklet_scope_realm(scope: *mut c_void) -> *mut c_void;
    fn web_audio_worklet_clone_options(
        vm: *mut c_void,
        realm: *mut c_void,
        error_realm: *mut c_void,
        options: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_audio_worklet_processor_constructor(scope: *mut c_void, name: usize) -> *mut c_void;
    fn web_audio_worklet_set_pending_data(
        scope: *mut c_void,
        node_reference: *mut c_void,
        transferred_port: *mut c_void,
    );
    fn web_audio_worklet_clear_pending_data(scope: *mut c_void);
    fn web_audio_worklet_set_current_frame(scope: *mut c_void, current_frame: u64);
    fn web_audio_worklet_queue_processor_error(node: *mut c_void, target: *mut c_void, global: *mut c_void, error: u64);
}

struct PumpClient {
    vm: NonNull<Vm>,

    // weak: the slot must not keep the node alive (see keep-alive notes)
    node: GcWeak<EmbedderCell<Wrapped<AudioWorkletNode>>>,
}

impl AudioWorkletPipeClient for PumpClient {
    fn pump(&self) {
        if let Some(node) = self.node.get() {
            // SAFETY: The VM of the control thread outlives its audio nodes.
            node.pump(unsafe { self.vm.as_ref() });
        }
    }
}

impl AudioWorkletNode {
    fn this(&self) -> Ref<Self> {
        let AnyAudioNode::AudioWorkletNode(node) = self.node.this() else {
            unreachable!()
        };
        node
    }

    // https://webaudio.github.io/web-audio-api/#dom-audioworkletnode-audioworkletnode
    pub(crate) fn construct(
        cx: &mut CallContext,
        context: AnyBaseAudioContext,
        name: Utf16String,
        options: AudioWorkletNodeOptions,
    ) -> WebIdlResult<Ref<Self>> {
        let vm = cx.vm();

        // 1. If nodeName does not exist as a key in the BaseAudioContext's node name to parameter descriptor
        //    map, throw an InvalidStateError exception and abort these steps.
        let worklet = context.audio_worklet_cell(vm);
        let mut descriptors = Vec::<AudioParamDescriptor>::new();
        unsafe extern "C" fn descriptor(
            context: *mut c_void,
            name: usize,
            default_value: f32,
            min_value: f32,
            max_value: f32,
            automation_rate: AutomationRate,
        ) {
            let name = unsafe { ak::Utf16FlyString::from_raw_borrowed(name) }.into();
            unsafe { &mut *context.cast::<Vec<AudioParamDescriptor>>() }.push(AudioParamDescriptor {
                name,
                default_value,
                min_value,
                max_value,
                automation_rate,
            });
        }
        if !unsafe {
            web_audio_worklet_find_definition(
                worklet.as_ptr(),
                name.raw_identity(),
                (&raw mut descriptors).cast(),
                descriptor,
            )
        } {
            let mut message: Vec<u16> = b"No AudioWorkletProcessor named '"
                .iter()
                .copied()
                .map(u16::from)
                .collect();
            message.extend_from_slice(&name.to_utf16());
            message.extend(b"' is registered".iter().copied().map(u16::from));
            return Err(Exception::Dom("InvalidStateError", Utf16String::from_utf16(&message)));
        }

        // https://webaudio.github.io/web-audio-api/#configuring-channels-with-audioworkletnodeoptions
        // If both numberOfInputs and numberOfOutputs are zero, throw a NotSupportedError.
        if options.number_of_inputs == 0 && options.number_of_outputs == 0 {
            return Err(Exception::Dom(
                "NotSupportedError",
                Utf16String::from_ascii(b"AudioWorkletNode must have at least one input or output"),
            ));
        }

        // If outputChannelCount exists: throw an IndexSizeError if its length does not equal numberOfOutputs;
        // throw a NotSupportedError if any entry is zero or greater than the supported maximum.
        if let Some(output_channel_count) = &options.output_channel_count {
            if output_channel_count.len() != options.number_of_outputs as usize {
                return Err(Exception::Dom(
                    "IndexSizeError",
                    Utf16String::from_ascii(b"outputChannelCount length must equal numberOfOutputs"),
                ));
            }
            for channel_count in output_channel_count {
                if *channel_count == 0 || *channel_count > BaseAudioContext::MAX_NUMBER_OF_CHANNELS {
                    return Err(Exception::Dom(
                        "NotSupportedError",
                        Utf16String::from_ascii(b"outputChannelCount entries must be between 1 and 32"),
                    ));
                }
            }
        }
        let scope = unsafe { CppCell::from_raw(web_audio_worklet_global_scope(worklet.as_ptr())) };
        let worklet_realm: Gc<Realm> =
            unsafe { Gc::from_non_null(NonNull::new(web_audio_worklet_scope_realm(scope.as_ptr()).cast()).unwrap()) };

        // 2. Let node be this value; initialize node's AudioNode base with context and options.
        let parameter_map = AudioParamMap::create(vm);
        let node_port = MessagePort::create(vm, context.realm());
        let node = allocate(vm, |defer_gc| Self {
            node: AudioNode::new(defer_gc, context, 2),
            name,
            number_of_inputs: options.number_of_inputs,
            number_of_outputs: options.number_of_outputs,
            parameter_map,
            parameter_names: descriptors.iter().map(|descriptor| descriptor.name.clone()).collect(),
            port: node_port,
            scope: scope.clone(),
            processor: Cell::new(None),
            pipe: RefCell::new(None),
        });
        node.node.set_this(AnyAudioNode::AudioWorkletNode(node));
        node.node.event_target().set_owner(node);
        let default_options = AudioNodeDefaultOptions {
            channel_count: 2,
            channel_count_mode: ChannelCountMode::Max,
            channel_interpretation: ChannelInterpretation::Speakers,
        };
        node.node
            .initialize_audio_node_options(cx, &options.parent, &default_options)?;

        // 3. Let map be a new AudioParamMap; for each descriptor in the parameter descriptors, create an
        //    AudioParam and append it to map; apply parameterData overrides.
        for descriptor in &descriptors {
            let param = AudioParam::create(
                vm,
                context,
                Some(AnyAudioNode::AudioWorkletNode(node)),
                descriptor.default_value,
                descriptor.min_value,
                descriptor.max_value,
                descriptor.automation_rate,
                FixedAutomationRate::No,
            );
            if let Some(parameter_data) = &options.parameter_data {
                if let Some(entry) = parameter_data.iter().find(|entry| entry.0 == descriptor.name) {
                    AudioParam::set_value(&param, cx, entry.1 as f32)?;
                }
            }
            parameter_map.set_entry(vm, descriptor.name.clone(), param);
        }

        // 4. Let messageChannel be a new MessageChannel (collapsed): node.port and the processor's port are
        //    entangled; node.port's message queue is enabled immediately. The processor-side port stays
        //    disabled until the processor constructor has returned, so messages posted before (or during)
        //    construction are queued and delivered to listeners the constructor registered — even ones added
        //    with addEventListener() and no explicit start().
        let processor_port = MessagePort::create(vm, worklet_realm);
        MessagePort::entangle(vm, node_port, processor_port);
        MessagePort::enable(node_port, vm);

        // 5. Let serializedProcessorOptions be StructuredSerialize(optionsObject's processorOptions).
        // 6-7. (Deserialization happens into the worklet realm when building the processor's options object.)
        let mut cloned_processor_options = None;
        if let Some(processor_options) = options.processor_options {
            let value = unsafe {
                web_audio_worklet_clone_options(
                    (vm as *const Vm).cast_mut().cast(),
                    worklet_realm.as_ptr().cast(),
                    cx.realm().as_ptr().cast(),
                    processor_options.as_ptr().cast(),
                )
            }
            .into_completion()?;
            assert!(value.is_object());
            cloned_processor_options = Some(value.as_object());
        }
        let options_object = Object::create(vm, worklet_realm, Some(worklet_realm.intrinsics().object_prototype(vm)));
        options_object
            .create_data_property(
                vm,
                &PropertyKey::from_utf8("numberOfInputs"),
                Value::from_f64(f64::from(options.number_of_inputs)),
                None,
                None,
            )
            .expect("a new options object accepts a property");
        options_object
            .create_data_property(
                vm,
                &PropertyKey::from_utf8("numberOfOutputs"),
                Value::from_f64(f64::from(options.number_of_outputs)),
                None,
                None,
            )
            .expect("a new options object accepts a property");
        if let Some(output_channel_count) = &options.output_channel_count {
            let array = Array::create(vm, worklet_realm, output_channel_count.len() as u64, None)
                .expect("an output-channel array is created");
            for (index, channel_count) in output_channel_count.iter().enumerate() {
                array
                    .create_data_property(
                        vm,
                        &PropertyKey::from(index as u32),
                        Value::from_f64(f64::from(*channel_count)),
                        None,
                        None,
                    )
                    .expect("a new array accepts a property");
            }
            options_object
                .create_data_property(
                    vm,
                    &PropertyKey::from_utf8("outputChannelCount"),
                    Value::from_object(array),
                    None,
                    None,
                )
                .expect("a new options object accepts a property");
        }
        if let Some(parameter_data) = &options.parameter_data {
            let data = Object::create(vm, worklet_realm, Some(worklet_realm.intrinsics().object_prototype(vm)));
            for (key, value) in parameter_data {
                data.create_data_property(
                    vm,
                    &PropertyKey::from_utf16_string(key),
                    Value::from_f64(*value),
                    None,
                    None,
                )
                .expect("a new parameter-data object accepts a property");
            }
            options_object
                .create_data_property(
                    vm,
                    &PropertyKey::from_utf8("parameterData"),
                    Value::from_object(data),
                    None,
                    None,
                )
                .expect("a new options object accepts a property");
        }

        // processorOptions is always present; when omitted by the caller, Web Audio specifies an empty object.
        let processor_options = cloned_processor_options.unwrap_or_else(|| {
            Object::create(vm, worklet_realm, Some(worklet_realm.intrinsics().object_prototype(vm)))
        });
        options_object
            .create_data_property(
                vm,
                &PropertyKey::from_utf8("processorOptions"),
                Value::from_object(processor_options),
                None,
                None,
            )
            .expect("a new options object accepts a property");

        // Reserve space for later input layout changes without allocating on the render thread.
        let input_channel_capacity = vec![BaseAudioContext::MAX_NUMBER_OF_CHANNELS; options.number_of_inputs as usize];
        let output_channel_count_matches_input =
            options.number_of_inputs == 1 && options.number_of_outputs == 1 && options.output_channel_count.is_none();
        let mut output_channel_capacity = Vec::new();
        for index in 0..options.number_of_outputs as usize {
            if let Some(counts) = &options.output_channel_count {
                output_channel_capacity.push(counts[index]);
            } else {
                output_channel_capacity.push(if output_channel_count_matches_input {
                    BaseAudioContext::MAX_NUMBER_OF_CHANNELS
                } else {
                    1
                });
            }
        }
        let mut param_is_a_rate = Vec::new();
        for descriptor in &descriptors {
            param_is_a_rate.push(descriptor.automation_rate == AutomationRate::ARate);
        }
        let sizing = ring_sizing_for_device_latency(
            100,
            context.sample_rate_value(),
            BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
        );
        let pipe = AudioWorkletPipe::new(Config {
            quantum_size: BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
            input_count: options.number_of_inputs as usize,
            output_count: options.number_of_outputs as usize,
            input_channel_capacity,
            output_channel_capacity: output_channel_capacity.clone(),
            output_channel_count_matches_input,
            param_count: descriptors.len(),
            param_is_a_rate,
            ring_capacity: sizing.ring_capacity,
            prime_level: sizing.prime_level,
        });
        unsafe { pipe.prime_outputs_with_silence(sizing.prime_level) };
        *node.pipe.borrow_mut() = Some(Arc::clone(&pipe));
        pipe.set_client(Some(Rc::new(PumpClient {
            vm: NonNull::from(vm),
            node: GcWeak::new(vm.heap(), node),
        })));

        // Create the render mirror. Channel configuration rides in the constructor (the graph drops
        // SetChannelConfig messages that precede AddNode).
        let render_params: Vec<Arc<RenderAudioParam>> = parameter_map
            .values(vm)
            .values()
            .iter()
            .map(|param| Arc::clone(param.render_param()))
            .collect();
        node.node.queue_render_node_creation(create_audio_worklet_render_node(
            node.node.node_id(),
            options.number_of_inputs as usize,
            options.number_of_outputs as usize,
            BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
            &output_channel_capacity,
            node.node.channel_count_value() as usize,
            node.node.channel_count_mode(cx)?,
            node.node.channel_interpretation(cx)?,
            Arc::clone(&pipe),
            render_params,
        ));

        // FIXME: Implement the spec's actively-processing keep-alive (drop this when the processor's
        //        process() return value has been false and inputs are silent). For now a live worklet
        //        node stays registered with the context until the context goes away.
        context.add_playing_source(vm, AnyAudioNode::AudioWorkletNode(node));

        // 8. Queue a control message to invoke the constructor of the corresponding AudioWorkletProcessor.
        // NOTE: The worklet shares the window agent's event loop, so the control message becomes a queued
        //       global task on the worklet global; the node constructor returns before user code runs.
        let task = PROCESSOR_CONSTRUCTION_TASK_CLASS.allocate(vm, |_| ProcessorConstructionTask {
            node,
            scope,
            processor_port,
            options_object,
        });
        queue_a_global_task(
            vm,
            worklet_realm.global_object(),
            TaskSource::DOMManipulation,
            task,
            |vm, task| {
                task.node
                    .invoke_processor_constructor(vm, &task.scope, task.processor_port, task.options_object);
            },
        );
        Ok(node)
    }

    // https://webaudio.github.io/web-audio-api/#invoking-processor-constructor
    fn invoke_processor_constructor(
        &self,
        vm: &Vm,
        scope: &CppCell,
        processor_port: Ref<MessagePort>,
        options_object: Gc<Object>,
    ) {
        // 1. Let constructionData be the processor construction data transferred from the control thread.
        // 2. Let processorName, nodeReference and serializedPort be constructionData’s name, node, and port respectively.
        // 3. Let serializedOptions be constructionData’s options.
        // 4. Let deserializedPort be the result of StructuredDeserialize(serializedPort, the current Realm).
        // 5. Let deserializedOptions be the result of StructuredDeserialize(serializedOptions, the current Realm).
        // NB: The queued task carries the construction data with the port and options already deserialized.
        let worklet_realm: Gc<Realm> =
            unsafe { Gc::from_non_null(NonNull::new(web_audio_worklet_scope_realm(scope.as_ptr()).cast()).unwrap()) };
        with_temporary_execution_context_and_callbacks(worklet_realm, || {
            // 6. Let processorCtor be the result of looking up processorName on the AudioWorkletGlobalScope’s node name to processor constructor map.
            let processor_ctor: Gc<FunctionObject> = unsafe {
                Gc::from_non_null(
                    NonNull::new(
                        web_audio_worklet_processor_constructor(scope.as_ptr(), self.name.raw_identity()).cast(),
                    )
                    .unwrap(),
                )
            };

            // 7. Store nodeReference and deserializedPort to node reference and transferred port of this AudioWorkletGlobalScope’s pending processor construction data respectively.
            unsafe {
                web_audio_worklet_set_pending_data(
                    scope.as_ptr(),
                    cell_into_abi(self.this()),
                    cell_into_abi(processor_port),
                );
            };

            // 8. Construct a callback function from processorCtor with the argument of deserializedOptions. If any exceptions are thrown in the callback, queue a task to the control thread to fire an event named processorerror at nodeReference using ErrorEvent.
            // AD-HOC: A constructor that returns an object other than an AudioWorkletProcessor fails with a TypeError.
            let constructed =
                construct(vm, processor_ctor, &[Value::from_object(options_object)], None).and_then(|object| {
                    implementation_if_wrapper::<AudioWorkletProcessor>(object).ok_or_else(|| {
                        vm.throw_completion_with_message::<()>(
                            ErrorKind::TypeError,
                            "The processor constructor did not return an AudioWorkletProcessor".to_string(),
                        )
                        .unwrap_err()
                    })
                });
            let processor = match constructed {
                Ok(processor) => Some(processor),
                Err(error) => {
                    self.pipe.borrow().as_ref().unwrap().set_state(WorkletPipeState::Failed);
                    self.fire_processor_error(error.value());
                    None
                }
            };

            // 9. Empty the pending processor construction data slot.
            unsafe { web_audio_worklet_clear_pending_data(scope.as_ptr()) };
            let Some(processor) = processor else {
                return;
            };
            self.processor.set(Some(processor));
            self.node
                .context_value()
                .add_audio_worklet_pipe(Arc::clone(self.pipe.borrow().as_ref().unwrap()));

            // NB: Defer incoming messages until the constructor can register listeners.
            MessagePort::enable(processor_port, vm);
        });
    }

    // Drains one node's input ring, runs its processor's process() per quantum, and pushes the
    // results back. Scheduled (coalesced) by the render thread via the pipe's wakeup callback.
    fn pump(&self, vm: &Vm) {
        let pipe = Arc::clone(self.pipe.borrow().as_ref().expect("a pumped node has a pipe"));

        // Every invocation consumes its scheduled wakeup, including stale callbacks for removed slots.
        pipe.clear_wakeup_flag();
        let Some(processor) = self.processor.get() else {
            return;
        };
        if pipe.state() == WorkletPipeState::ShutDown || !processor.callable_process.get() {
            return;
        }

        let realm: Gc<Realm> = unsafe {
            Gc::from_non_null(NonNull::new(web_audio_worklet_scope_realm(self.scope.as_ptr()).cast()).unwrap())
        };

        // Keep one execution context for the batch so its microtask checkpoint runs after all available quanta.
        with_temporary_execution_context_and_callbacks(realm, || {
            // 4.4.4.2. Let O be the ECMAScript object corresponding to processor.
            let processor_object = wrap(&CallContext::new(vm, realm), processor);
            let config = &pipe.config;
            loop {
                if pipe.output_occupancy() == config.ring_capacity {
                    break;
                }
                let mut processed_one = false;
                let mut pump_error = None;
                let mut output_full = false;

                let _ = unsafe {
                    pipe.try_read_input(|input_slot| {
                        processed_one = true;

                        // https://webaudio.github.io/web-audio-api/#rendering-loop — currentFrame/currentTime
                        // reflect the block being processed.
                        web_audio_worklet_set_current_frame(self.scope.as_ptr(), input_slot.start_frame());

                        // The processor may detach channel arrays, so they cannot be reused without checking their buffers.
                        // FIXME: Reuse arrays that remain attached.
                        let build_channel_array =
                            |channel_count: usize, fill_channel: &dyn Fn(usize, &TypedArrayBase)| {
                                let channels = Array::create(vm, realm, channel_count as u64, None)
                                    .expect("a channel array is created");
                                for channel in 0..channel_count {
                                    let float32_array = create_typed_array(
                                        vm,
                                        realm,
                                        TypedArrayKind::Float32Array,
                                        config.quantum_size,
                                    )
                                    .expect("a channel's Float32Array is created");
                                    fill_channel(channel, &float32_array);
                                    channels
                                        .create_data_property(
                                            vm,
                                            &PropertyKey::from(channel as u32),
                                            Value::from_object(float32_array),
                                            None,
                                            None,
                                        )
                                        .expect("a new array accepts a property");
                                }
                                channels
                            };

                        let inputs = Array::create(vm, realm, config.input_count as u64, None)
                            .expect("the inputs array is created");
                        for input in 0..config.input_count {
                            let channels = build_channel_array(
                                input_slot.actual_channel_count(input) as usize,
                                &|channel, float32_array| {
                                    overwrite_float32_array(float32_array, input_slot.input_channel(input, channel));
                                },
                            );
                            inputs
                                .create_data_property(
                                    vm,
                                    &PropertyKey::from(input as u32),
                                    Value::from_object(channels),
                                    None,
                                    None,
                                )
                                .expect("a new array accepts a property");
                        }

                        let output_channel_count = |output: usize| -> usize {
                            if config.output_channel_count_matches_input {
                                return (input_slot.actual_channel_count(0) as usize).max(1);
                            }
                            config.output_channel_capacity[output] as usize
                        };
                        let outputs = Array::create(vm, realm, config.output_count as u64, None)
                            .expect("the outputs array is created");
                        let output_channel_arrays = MarkedVec::with_capacity(vm, config.output_count);
                        for output in 0..config.output_count {
                            let channels = build_channel_array(output_channel_count(output), &|_, _| {});
                            output_channel_arrays.push(channels);
                            outputs
                                .create_data_property(
                                    vm,
                                    &PropertyKey::from(output as u32),
                                    Value::from_object(channels),
                                    None,
                                    None,
                                )
                                .expect("a new array accepts a property");
                        }

                        let parameters = Object::create(vm, realm, Some(realm.intrinsics().object_prototype(vm)));
                        for param_index in 0..config.param_count {
                            let block = input_slot.param_block(param_index);
                            let float32_array =
                                create_typed_array(vm, realm, TypedArrayKind::Float32Array, block.len())
                                    .expect("a parameter's Float32Array is created");
                            overwrite_float32_array(&float32_array, block);
                            parameters
                                .create_data_property(
                                    vm,
                                    &PropertyKey::from_utf16_string(&self.parameter_names[param_index]),
                                    Value::from_object(float32_array),
                                    None,
                                    None,
                                )
                                .expect("a new parameters object accepts a property");
                        }

                        // 4.4.4.7. Let getResult be Get(O, "process").
                        let process_value = match processor_object.get(vm, &PropertyKey::from_utf8("process")) {
                            Ok(value) if value.is_function() => value,
                            Ok(_) => {
                                let error = vm
                                    .throw_completion_with_message::<()>(
                                        ErrorKind::TypeError,
                                        "AudioWorkletProcessor.process must be a function".to_string(),
                                    )
                                    .unwrap_err();
                                pump_error = Some(error.value());
                                return;
                            }
                            Err(error) => {
                                pump_error = Some(error.value());
                                return;
                            }
                        };

                        // 4.4.4.11. Set [[callable process]] to true.
                        processor.callable_process.set(true);

                        let process_arguments = [
                            Value::from_object(inputs),
                            Value::from_object(outputs),
                            Value::from_object(parameters),
                        ];
                        let result = match call(
                            vm,
                            process_value,
                            Value::from_object(processor_object),
                            &process_arguments,
                        ) {
                            Ok(result) => result,
                            Err(error) => {
                                pump_error = Some(error.value());
                                return;
                            }
                        };

                        if pipe.state() == WorkletPipeState::ShutDown {
                            return;
                        }

                        let processor_active = result.to_boolean();

                        // Push the processed quantum back; read outputs defensively (detached → silence).
                        let pushed = pipe.try_write_output(|output_slot| {
                            output_slot.set_start_frame(input_slot.start_frame());
                            output_slot.set_processor_active(processor_active);
                            for output in 0..config.output_count {
                                let channel_count = output_channel_count(output);
                                output_slot.set_actual_channel_count(output, channel_count as u32);
                                let channels = output_channel_arrays.get(output).unwrap();
                                for channel in 0..channel_count {
                                    let destination = output_slot.output_channel(output, channel);
                                    let channel_value = channels
                                        .get(vm, &PropertyKey::from(channel as u32))
                                        .expect("a channel array's element is a data property");
                                    let float32_array = channel_value
                                        .is_object()
                                        .then(|| channel_value.as_object().downcast::<TypedArrayBase>())
                                        .flatten()
                                        .filter(|array| array.kind() == TypedArrayKind::Float32Array);
                                    let Some(float32_array) = float32_array.filter(|array| {
                                        !array.viewed_array_buffer().is_detached()
                                            && float32_array_length(array) >= config.quantum_size
                                    }) else {
                                        destination.fill(0.);
                                        continue;
                                    };
                                    let bytes = ak::slice_from_raw_parts_mut(
                                        destination.as_mut_ptr().cast::<u8>(),
                                        size_of_val(destination),
                                    );
                                    float32_array
                                        .viewed_array_buffer()
                                        .copy_to(float32_array.byte_offset(), bytes);
                                }
                            }
                        });
                        if !pushed {
                            output_full = true;
                        }
                    })
                };

                // 4.4.4.13.3. If completion is an abrupt completion:
                if let Some(error) = pump_error {
                    // 4.4.4.13.3.1. Set [[callable process]] to false.
                    processor.callable_process.set(false);

                    // 4.4.4.13.3.2. Set processor’s active source flag to false.
                    // 4.4.4.13.3.3. Make a silent output buffer available for reading.
                    // NB: A failed pipe renders silence.
                    pipe.set_state(WorkletPipeState::Failed);

                    // 4.4.4.13.3.4. Queue a task to the control thread to fire an event named processorerror at the associated AudioWorkletNode using ErrorEvent.
                    processor.node_reference.fire_processor_error(error);
                    return;
                }
                if pipe.state() == WorkletPipeState::ShutDown {
                    return;
                }
                if output_full {
                    break;
                }
                if !processed_one {
                    break;
                }
            }
        });
    }

    pub(crate) fn fire_processor_error(&self, error: Value) {
        // https://webaudio.github.io/web-audio-api/#dom-audioworkletnode-onprocessorerror
        // "...the processor will queue a task to fire an event named processorerror using ErrorEvent at the
        //  associated AudioWorkletNode."
        unsafe {
            web_audio_worklet_queue_processor_error(
                cell_into_abi(self.this()),
                self.node.event_target().as_ptr(),
                self.node.context_value().global_event_target().as_ptr(),
                error.to_raw(),
            );
        };
    }

    pub(crate) fn parameters(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioParamMap>> {
        Ok(self.parameter_map)
    }

    pub(crate) fn port(&self, _: &mut CallContext) -> WebIdlResult<Ref<MessagePort>> {
        Ok(self.port)
    }

    pub(crate) fn number_of_inputs_value(&self) -> u32 {
        self.number_of_inputs
    }

    pub(crate) fn number_of_outputs_value(&self) -> u32 {
        self.number_of_outputs
    }
}
