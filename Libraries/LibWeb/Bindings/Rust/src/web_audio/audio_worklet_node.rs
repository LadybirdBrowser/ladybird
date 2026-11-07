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
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::{
    Array, EmbedderClass, EmbedderValue, FunctionObject, Object, PropertyKey, RawNativeFunctionResult, Value, Vm,
    construct,
};
use libjs_rust::api::{Gc, Realm, Trace};
use libweb_rust::web_audio::control_message::FfiSharedCppObject;
use libweb_rust::web_audio::rendering::audio_worklet_pipe::{AudioWorkletPipe, WorkletPipeConfig, WorkletPipeState};
use libweb_rust::web_audio::rendering::audio_worklet_render_node::create_audio_worklet_render_node;
use libweb_rust::web_audio::rendering::node_message::SharedCppObject;
use libweb_rust::web_audio::rendering::render_node::RenderAudioParam;

use super::audio_node::{
    AnyAudioNode, AudioNode, AudioNodeDefaultOptions, AudioNodeOptions, ChannelCountMode, ChannelInterpretation,
};
use super::audio_param::{AudioParam, AutomationRate, FixedAutomationRate};
use super::audio_param_map::AudioParamMap;
use super::base_audio_context::{AnyBaseAudioContext, BaseAudioContext};
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::interface::{cell_from_abi, cell_into_abi, with_temporary_execution_context_and_callbacks};
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
    port: Ref<MessagePort>,
    processor: Cell<Option<Gc<Object>>>,
    processor_errored: Cell<bool>,
    #[gc(untraced)]
    pipe: RefCell<Option<Arc<AudioWorkletPipe>>>,
    #[gc(untraced)]
    pipe_service: RefCell<Option<SharedCppObject>>,
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
        if let Some(service) = &*node.pipe_service.borrow() {
            unsafe { web_audio_worklet_clear_pump(service.pointer) };
        }
        if let Some(pipe) = &*node.pipe.borrow() {
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
    fn web_audio_worklet_create_pipe(
        config: WorkletPipeConfig<'_>,
        sample_rate: f32,
        native_pipe: *mut *const c_void,
    ) -> FfiSharedCppObject;
    fn web_audio_worklet_set_pump(pipe: *mut c_void, scope: *mut c_void, node_id: u64);
    fn web_audio_worklet_clear_pump(pipe: *mut c_void);
    fn web_audio_worklet_processor_constructor(scope: *mut c_void, name: usize) -> *mut c_void;
    fn web_audio_worklet_set_pending_port(scope: *mut c_void, port: *mut c_void);
    fn web_audio_worklet_clear_pending_data(scope: *mut c_void);
    fn web_audio_worklet_add_processor_slot(
        scope: *mut c_void,
        node_id: u64,
        processor: *mut c_void,
        port: *mut c_void,
        node: *mut c_void,
        pipe: *mut c_void,
        names: ak::Span<'_, usize>,
    );
    fn web_audio_worklet_queue_processor_error(node: *mut c_void, target: *mut c_void, global: *mut c_void, error: u64);
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
            port: node_port,
            processor: Cell::new(None),
            processor_errored: Cell::new(false),
            pipe: RefCell::new(None),
            pipe_service: RefCell::new(None),
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
        let sizing = libweb_rust::web_audio::rendering::audio_worklet_pipe::ring_sizing_for_device_latency(
            100,
            context.sample_rate_value(),
            BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
        );
        let config = WorkletPipeConfig {
            quantum_size: BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
            input_count: options.number_of_inputs,
            output_count: options.number_of_outputs,
            input_channel_capacity: ak::Span::new(&input_channel_capacity),
            output_channel_capacity: ak::Span::new(&output_channel_capacity),
            output_channel_count_matches_input,
            param_count: descriptors.len() as u32,
            param_is_a_rate: ak::Span::new(&param_is_a_rate),
            ring_capacity: sizing.ring_capacity,
            prime_level: sizing.prime_level,
        };
        let mut native_pipe = std::ptr::null();
        let service =
            unsafe { web_audio_worklet_create_pipe(config, context.sample_rate_value(), &raw mut native_pipe) };
        let native_pipe = native_pipe.cast::<AudioWorkletPipe>();
        unsafe { Arc::increment_strong_count(native_pipe) };
        let pipe = unsafe { Arc::from_raw(native_pipe) };
        unsafe { pipe.prime_outputs_with_silence(sizing.prime_level) };
        *node.pipe.borrow_mut() = Some(Arc::clone(&pipe));
        *node.pipe_service.borrow_mut() = Some(SharedCppObject {
            pointer: service.pointer,
            retain: service.retain,
            release: service.release,
        });
        unsafe { web_audio_worklet_set_pump(service.pointer, scope.as_ptr(), node.node.node_id()) };
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

    // https://webaudio.github.io/web-audio-api/#instantiation-of-audioworkletprocessor
    // "invoke processor constructor" — deferred to a queued task so the node constructor returns
    // before user code runs (and pre-constructor port messages queue up for delivery afterwards).
    fn invoke_processor_constructor(
        &self,
        vm: &Vm,
        scope: &CppCell,
        processor_port: Ref<MessagePort>,
        options_object: Gc<Object>,
    ) {
        let worklet_realm: Gc<Realm> =
            unsafe { Gc::from_non_null(NonNull::new(web_audio_worklet_scope_realm(scope.as_ptr()).cast()).unwrap()) };
        with_temporary_execution_context_and_callbacks(worklet_realm, || {
            let constructor: Gc<FunctionObject> = unsafe {
                Gc::from_non_null(
                    NonNull::new(
                        web_audio_worklet_processor_constructor(scope.as_ptr(), self.name.raw_identity()).cast(),
                    )
                    .unwrap(),
                )
            };

            // 1. Set the pending processor construction data: the processor-side port.
            unsafe { web_audio_worklet_set_pending_port(scope.as_ptr(), cell_into_abi(processor_port)) };

            // 2. Construct the processor: Construct(processorCtor, « optionsObject »).
            let processor_or_error = construct(vm, constructor, &[Value::from_object(options_object)], None);

            // 3. Empty the pending processor construction data slot (a constructor that never chained to
            //    super() leaves it populated).
            unsafe { web_audio_worklet_clear_pending_data(scope.as_ptr()) };
            let processor = match processor_or_error {
                Ok(processor) => processor,
                Err(error) => {
                    self.processor_errored.set(true);
                    self.pipe.borrow().as_ref().unwrap().set_state(WorkletPipeState::Failed);
                    self.fire_processor_error(error.value());
                    return;
                }
            };
            self.processor.set(Some(processor));
            let worklet = self.node.context_value().audio_worklet_cell(vm);
            let mut names = Vec::<Utf16String>::new();
            unsafe extern "C" fn append(context: *mut c_void, name: usize, _: f32, _: f32, _: f32, _: AutomationRate) {
                unsafe { &mut *context.cast::<Vec<Utf16String>>() }
                    .push(unsafe { ak::Utf16FlyString::from_raw_borrowed(name) }.into());
            }
            unsafe {
                web_audio_worklet_find_definition(
                    worklet.as_ptr(),
                    self.name.raw_identity(),
                    (&raw mut names).cast(),
                    append,
                )
            };
            let raw_names: Vec<usize> = names.iter().map(Utf16String::raw_identity).collect();
            unsafe {
                web_audio_worklet_add_processor_slot(
                    scope.as_ptr(),
                    self.node.node_id(),
                    processor.as_ptr().cast(),
                    cell_into_abi(processor_port),
                    cell_into_abi(self.this()),
                    self.pipe_service.borrow().as_ref().unwrap().pointer,
                    ak::Span::new(&raw_names),
                );
            };
            MessagePort::enable(processor_port, vm);
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

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn web_rust_audio_worklet_node_fire_error(cell: *mut c_void, error: u64) {
    let node: Ref<AudioWorkletNode> = unsafe { cell_from_abi(cell) };
    node.fire_processor_error(unsafe { Value::from_raw(error) });
}
