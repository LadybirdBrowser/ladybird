/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::collapsible_if, reason = "preserve the C++ branch structure")]

use std::cell::{Cell, RefCell};
use std::ffi::c_void;
use std::ptr::NonNull;
use std::rc::Rc;
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::{CellRoot, EmbedderCell, GcRefCell, PromiseCapability, Trace, Value, Vm};
use libweb_rust::web_audio::rendering::offline_audio_renderer::{OfflineAudioRenderer, OfflineAudioRendererClient};

use super::audio_array::overwrite_float32_array;
use super::audio_buffer::AudioBuffer;
use super::audio_destination_node::AudioDestinationNode;
use super::audio_listener::AudioListener;
use super::base_audio_context::{AnyBaseAudioContext, AudioContextState, BaseAudioContext};
use super::offline_audio_completion_event::OfflineAudioCompletionEvent;
use crate::bindings::conversions::ToJs;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::{CppEvent, CppEventTarget, GetTheParent};
use crate::bindings::global_object::window_from_global_object;
use crate::bindings::interface::{
    Inherits, Ref, Wrappable, Wrapped, allocate, with_temporary_execution_context_and_callbacks,
};
use crate::bindings::promise::{WebIdlPromise, promise_realm, reject_promise_with_exception, resolve_promise};
use crate::bindings::{CallContext, Exception, WebIdlResult};

pub(crate) struct OfflineAudioContextOptions {
    pub number_of_channels: u32,
    pub length: u32,
    pub sample_rate: f32,
}

// https://webaudio.github.io/web-audio-api/#OfflineAudioContext
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct OfflineAudioContext {
    context: BaseAudioContext,
    length: u32,
    number_of_channels: u32,
    rendering_started: Cell<bool>,
    rendered_buffer: Cell<Option<Ref<AudioBuffer>>>,
    #[gc(untraced)]
    renderer: RefCell<Option<Arc<OfflineAudioRenderer>>>,

    // https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-suspend, keyed by suspend frame.
    suspend_promises: GcRefCell<Vec<(u64, WebIdlPromise)>>,
}

impl Inherits<BaseAudioContext> for OfflineAudioContext {
    fn parent(&self) -> &BaseAudioContext {
        &self.context
    }
}

impl Wrappable for OfflineAudioContext {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.context.cpp_parent()
    }
}

impl GetTheParent for OfflineAudioContext {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

struct RendererClient {
    vm: NonNull<Vm>,
    context: CellRoot<EmbedderCell<Wrapped<OfflineAudioContext>>>,
    promise: CellRoot<PromiseCapability>,
}

impl RendererClient {
    fn vm(&self) -> &Vm {
        // SAFETY: The VM of the control thread outlives its audio contexts.
        unsafe { self.vm.as_ref() }
    }
}

impl OfflineAudioRendererClient for RendererClient {
    fn rendering_suspended(&self, suspend_time: f64) {
        self.context.cell().handle_suspended(self.vm(), suspend_time);
    }

    fn rendering_completed(&self, rendered_channels: &[Vec<f32>]) {
        self.context
            .cell()
            .finish_rendering(self.vm(), self.promise.cell(), rendered_channels);
    }

    fn sources_ended(&self, nodes: &[u64]) {
        self.context.cell().context.handle_ended_sources(self.vm(), nodes);
    }
}

impl OfflineAudioContext {
    fn this(&self) -> Ref<Self> {
        let AnyBaseAudioContext::OfflineAudioContext(context) = self.context.this() else {
            unreachable!()
        };
        context
    }

    // https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-offlineaudiocontext
    pub(crate) fn construct_options(
        cx: &mut CallContext,
        context_options: OfflineAudioContextOptions,
    ) -> WebIdlResult<Ref<Self>> {
        // AD-HOC: This spec text is currently only mentioned in the constructor overload that takes separate arguments,
        //         but these parameters should be validated for both constructors.
        // A NotSupportedError exception MUST be thrown if any of the arguments is negative, zero, or outside its nominal range.
        BaseAudioContext::verify_audio_options_inside_nominal_range(
            context_options.number_of_channels,
            context_options.length,
            context_options.sample_rate,
        )?;
        let window = window_from_global_object(cx.realm().global_object())
            .expect("an OfflineAudioContext is constructed on a Window");
        let global = unsafe { CppCell::from_raw(window.as_event_target().as_ptr()) };

        // Let c be a new OfflineAudioContext object. Initialize c as follows:
        let c = allocate(cx.vm(), |defer_gc| Self {
            context: BaseAudioContext::new(defer_gc, global, context_options.sample_rate),
            length: context_options.length,
            number_of_channels: context_options.number_of_channels,
            rendering_started: Cell::new(false),
            rendered_buffer: Cell::new(None),
            renderer: RefCell::new(None),
            suspend_promises: GcRefCell::new(Vec::new()),
        });
        c.context
            .initialize(cx.vm(), AnyBaseAudioContext::OfflineAudioContext(c));
        c.context.event_target().set_owner(c);
        c.context.listener.set(Some(AudioListener::create(
            cx.vm(),
            AnyBaseAudioContext::OfflineAudioContext(c),
        )));

        // 1. Set the [[control thread state]] for c to "suspended".
        c.context.set_control_state(AudioContextState::Suspended);
        c.context.set_state(AudioContextState::Suspended);

        // 2. Set the [[rendering thread state]] for c to "suspended".
        c.context.set_rendering_state(AudioContextState::Suspended);

        // FIXME: 3. Determine the [[render quantum size]] for this OfflineAudioContext, based on the value of the renderSizeHint:
        // 4. Construct an AudioDestinationNode with its channelCount set to contextOptions.numberOfChannels.
        c.context.destination.set(Some(AudioDestinationNode::create(
            cx,
            AnyBaseAudioContext::OfflineAudioContext(c),
            context_options.number_of_channels,
        )?));

        // FIXME: 5. Let messageChannel be a new MessageChannel.
        // FIXME: 6. Let controlSidePort be the value of messageChannel’s port1 attribute.
        // FIXME: 7. Let renderingSidePort be the value of messageChannel’s port2 attribute.
        // FIXME: 8. Let serializedRenderingSidePort be the result of StructuredSerializeWithTransfer(renderingSidePort, « renderingSidePort »).
        // FIXME: 9. Set this audioWorklet's port to controlSidePort.
        // FIXME: 10. Queue a control message to set the MessagePort on the AudioContextGlobalScope, with serializedRenderingSidePort.
        Ok(c)
    }

    // https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-offlineaudiocontext-numberofchannels-length-samplerate
    pub(crate) fn construct_channels(
        cx: &mut CallContext,
        number_of_channels: u32,
        length: u32,
        sample_rate: f32,
    ) -> WebIdlResult<Ref<Self>> {
        Self::construct_options(
            cx,
            OfflineAudioContextOptions {
                number_of_channels,
                length,
                sample_rate,
            },
        )
    }

    // https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-startrendering
    pub(crate) fn start_rendering(&self, cx: &mut CallContext, promise: WebIdlPromise) -> WebIdlResult<()> {
        // 1. If this’s relevant global object’s associated Document is not fully active then return a promise rejected with "InvalidStateError" DOMException.
        if !self.context.document_is_fully_active() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"Document is not fully active"),
            ));
        }

        // AD-HOC: Not in spec explicitly, but this should account for detached iframes too. See /the-offlineaudiocontext-interface/startrendering-after-discard.html WPT.
        if self.context.navigable_is_destroyed() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"The iframe has been detached"),
            ));
        }

        // 2. Let promise be a new promise.
        // NB: The bindings create the promise.

        // FIXME: 3. If the [[control thread state]] on the OfflineAudioContext is closed, reject promise with InvalidStateError and abort these steps.
        // 4. If the OfflineAudioContext’s length is not null and [[committed frames]] is greater than or equal to OfflineAudioContext’s length, reject promise with InvalidStateError and abort these steps.
        // NB: One-shot rendering commits the entire length on success.
        if self.rendering_started.get() {
            reject_promise_with_exception(
                cx.vm(),
                promise,
                Exception::Dom(
                    "InvalidStateError",
                    Utf16String::from_ascii(b"Rendering is already started"),
                ),
            );
            return Ok(());
        }

        // FIXME: 5. Let bufferLength be a number initialized to the value of the chunkSize.
        // FIXME: 5.1. If the OfflineAudioContext’s length is null:
        // FIXME: 5.1.1. If chunkSize is null, set bufferLength to render quantum size.
        // FIXME: 5.2. Otherwise, if the OfflineAudioContext’s length is not null:
        // FIXME: 5.2.1. If chunkSize is not null and [[committed frames]] + chunkSize is less than the OfflineAudioContext’s length, set bufferLength to chunkSize.
        // FIXME: 5.2.2. Otherwise, set bufferLength to length - [[committed frames]].
        // FIXME: 6. If bufferLength is not a multiple of render quantum size, round it up to the next multiple of the render quantum size.

        // 7. Create a new AudioBuffer buffer, with a number of channels and sample rate equal respectively to the numberOfChannels and sampleRate values passed to this instance’s constructor in the contextOptions parameter; and length equal to bufferLength.
        // NB: Without chunked rendering, bufferLength is the context’s length.
        let buffer_result = self.context.create_buffer(
            cx,
            self.number_of_channels,
            self.length,
            self.context.sample_rate_value(),
        );

        // 8. If an exception was thrown during the preceding AudioBuffer constructor call, reject promise with this exception and return promise.
        let buffer = match buffer_result {
            Ok(buffer) => buffer,
            Err(error) => {
                reject_promise_with_exception(cx.vm(), promise, error);
                return Ok(());
            }
        };

        // 9. Set [[rendering started]] to true.
        self.rendering_started.set(true);

        // 10. Set [[committed frames]] to the sum of [[committed frames]] and the buffer’s length.
        // NB: The one-shot rendering guard also records that all frames have been committed.

        // 11. Append buffer to [[rendered buffers]] and begin offline rendering for buffer.
        // NB: The one-shot renderer stores a single buffer.
        self.rendered_buffer.set(Some(buffer));
        self.begin_offline_rendering(cx.vm(), promise);

        // 12. Append promise to [[pending promises]].
        self.context.pending_promises.borrow_mut(cx.vm()).push(promise);

        // 13. Return promise.
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#begin-offline-rendering
    fn begin_offline_rendering(&self, vm: &Vm, promise: WebIdlPromise) {
        // 1. Given the current connections and scheduled changes, start rendering buffer’s length sample-frames of audio into buffer.
        // 2. For every render quantum, check and suspend rendering if necessary.
        // 3. If a suspended context is resumed, continue to render the buffer.
        let renderer = OfflineAudioRenderer::new(
            Arc::clone(self.context.control_message_queue()),
            self.context.destination_value().parent().node_id(),
            self.number_of_channels as usize,
            self.length as usize,
            self.context.sample_rate_value(),
            BaseAudioContext::RENDER_QUANTUM_SIZE as usize,
        );
        *self.renderer.borrow_mut() = Some(Arc::clone(&renderer));
        renderer.set_client(Some(Rc::new(RendererClient {
            vm: NonNull::from(vm),
            context: CellRoot::new(self.this()),
            promise: CellRoot::new(promise),
        })));
        for (frame, _) in &*self.suspend_promises.borrow() {
            renderer.request_suspend(*frame);
        }

        // AD-HOC: Other engines transition the context to "running" once rendering starts.
        self.context.set_control_state(AudioContextState::Running);
        self.context.set_state(AudioContextState::Running);
        self.context.set_rendering_state(AudioContextState::Running);
        self.queue_a_statechange_event(vm);
        renderer.start_rendering();
    }

    fn finish_rendering(&self, vm: &Vm, promise: WebIdlPromise, rendered_channels: &[Vec<f32>]) {
        // NB: Copy the rendered samples into [[rendered buffer]].
        let buffer = self.rendered_buffer.get().expect("offline rendering has a buffer");
        let mut cx = CallContext::new(vm, self.context.realm());
        for channel_index in 0..self.number_of_channels {
            let channel_data = AudioBuffer::get_channel_data(&buffer, &mut cx, channel_index)
                .unwrap_or_else(|_| panic!("the rendered channel exists"));
            overwrite_float32_array(&channel_data, &rendered_channels[channel_index as usize]);
        }

        // AD-HOC: Other engines transition the context to "closed" once rendering completes.
        self.context.set_control_state(AudioContextState::Closed);
        self.context.set_state(AudioContextState::Closed);
        self.context.set_rendering_state(AudioContextState::Closed);
        self.queue_a_statechange_event(vm);

        // NB: Break the reference cycle between the renderer's callbacks and this context.
        self.renderer.borrow().as_ref().unwrap().set_client(None);

        // 4. Once the rendering is complete, queue a media element task to execute the following steps:
        self.context
            .queue_a_media_element_task(vm, (self.this(), promise), |vm, (context, promise)| {
                let realm = promise_realm(promise);
                with_temporary_execution_context_and_callbacks(realm, || {
                    let buffer = context.rendered_buffer.get().unwrap();

                    // 4.1. Resolve the promise created by startRendering() with buffer.
                    resolve_promise(
                        vm,
                        promise,
                        buffer
                            .to_js(&CallContext::new(vm, realm))
                            .expect("an AudioBuffer wraps"),
                    );

                    // 4.2. Remove the promise from [[pending promises]].
                    context
                        .context
                        .pending_promises
                        .borrow_mut(vm)
                        .retain(|pending| *pending != promise);

                    // FIXME: 4.3. Remove buffer from [[rendered buffers]].
                    // FIXME: 4.4. Set [[rendered frames]] to the sum of [[rendered frames]] and the length of buffer.
                    // 4.5. If length is not null:
                    // NB: One-shot rendering always has a length.
                    // 4.5.1. If [[rendered frames]] is greater than or equal to length, Queue a control message to close the OfflineAudioContext.
                    // NB: The context was closed when rendering completed.
                    // 4.5.2. Queue a media element task to fire an event named complete at the OfflineAudioContext using OfflineAudioCompletionEvent whose renderedBuffer property is set to buffer.
                    context.context.queue_a_media_element_task(vm, context, |vm, context| {
                        let event = CppEvent::create_for_owner(
                            context.context.realm(),
                            &Utf16String::from_ascii(b"complete"),
                            false,
                            false,
                            false,
                        );
                        let event =
                            OfflineAudioCompletionEvent::create(vm, event, context.rendered_buffer.get().unwrap());
                        context.context.event_target().dispatch_event(&event.event);
                    });
                });
            });
    }

    fn handle_suspended(&self, vm: &Vm, suspend_time: f64) {
        self.context.set_control_state(AudioContextState::Suspended);
        self.context.set_state(AudioContextState::Suspended);
        self.context.set_rendering_state(AudioContextState::Suspended);
        let frame = (suspend_time * f64::from(self.context.sample_rate_value())).round() as u64;

        // Suspensions take effect at the beginning of the first render quantum at
        // or after the requested time. Keep the promise keyed by that same frame
        // so the rendering-thread callback can resolve it when it reaches the
        // boundary.
        let quantum_size = u64::from(BaseAudioContext::RENDER_QUANTUM_SIZE);
        let frame = (frame.wrapping_add(quantum_size - 1) / quantum_size) * quantum_size;
        let suspend_promise = {
            let mut promises = self.suspend_promises.borrow_mut(vm);
            promises
                .iter()
                .position(|entry| entry.0 == frame)
                .map(|index| promises.remove(index).1)
        };
        self.context.queue_a_media_element_task(
            vm,
            (self.this(), suspend_promise),
            |vm, (context, suspend_promise)| {
                with_temporary_execution_context_and_callbacks(context.context.realm(), || {
                    if let Some(promise) = suspend_promise {
                        resolve_promise(vm, promise, Value::UNDEFINED);
                    }
                    context.context.dispatch_statechange();
                });
            },
        );
    }

    fn queue_a_statechange_event(&self, vm: &Vm) {
        self.context.queue_a_media_element_task(vm, self.this(), |_, context| {
            context.context.dispatch_statechange();
        });
    }

    // https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-resume
    pub(crate) fn resume(&self, cx: &mut CallContext, promise: WebIdlPromise) -> WebIdlResult<()> {
        // FIXME: 1. If this’s relevant global object’s associated Document is not fully active then return a promise rejected with "InvalidStateError" DOMException.
        // 2. Let promise be a new Promise.
        // NB: The binding creates promise before calling this method.
        // 3. Abort these steps and reject promise with InvalidStateError when any of following conditions is true:
        if self.renderer.borrow().is_none() || self.context.state_value() != AudioContextState::Suspended {
            reject_promise_with_exception(
                cx.vm(),
                promise,
                Exception::Dom(
                    "InvalidStateError",
                    Utf16String::from_ascii(b"The OfflineAudioContext is not suspended"),
                ),
            );
            return Ok(());
        }
        self.renderer.borrow().as_ref().unwrap().resume();

        // 4. Set the [[control thread state]] flag on the OfflineAudioContext to running.
        self.context.set_control_state(AudioContextState::Running);
        self.context.set_state(AudioContextState::Running);
        self.context.set_rendering_state(AudioContextState::Running);
        self.queue_a_statechange_event(cx.vm());

        // 5. Queue a control message to resume the OfflineAudioContext.
        resolve_promise(cx.vm(), promise, Value::UNDEFINED);

        // 6. Return promise.
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-suspend
    pub(crate) fn suspend(&self, cx: &mut CallContext, promise: WebIdlPromise, suspend_time: f64) -> WebIdlResult<()> {
        if !suspend_time.is_finite() || suspend_time < 0. {
            return Err(Exception::range_error(
                "Suspend time must be a finite, non-negative number",
            ));
        }
        let frame = (suspend_time * f64::from(self.context.sample_rate_value())).round() as u64;
        let quantum_size = u64::from(BaseAudioContext::RENDER_QUANTUM_SIZE);
        let frame = (frame.wrapping_add(quantum_size - 1) / quantum_size) * quantum_size;
        if frame >= u64::from(self.length) {
            return Err(Exception::range_error(
                "Suspend time must be less than the duration of the context",
            ));
        }
        if self.suspend_promises.borrow().iter().any(|entry| entry.0 == frame) {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"A suspension is already scheduled at this time"),
            ));
        }
        self.suspend_promises.borrow_mut(cx.vm()).push((frame, promise));
        if let Some(renderer) = &*self.renderer.borrow() {
            if !renderer.request_suspend(frame) {
                self.suspend_promises
                    .borrow_mut(cx.vm())
                    .retain(|entry| entry.0 != frame);
                return Err(Exception::Dom(
                    "InvalidStateError",
                    Utf16String::from_ascii(b"The rendering thread has already passed the requested suspend time"),
                ));
            }
        }
        Ok(())
    }

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-currenttime
    pub(crate) fn current_time_value(&self) -> f64 {
        // This is the time in seconds of the sample frame immediately following the last sample-frame in the block of
        // audio most recently processed by the context's rendering graph.
        if let Some(renderer) = &*self.renderer.borrow() {
            return renderer.frames_rendered() as f64 / f64::from(self.context.sample_rate_value());
        }
        0.
    }

    // https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-length
    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        // The size of the buffer in sample-frames. This is the same as the value of the length parameter for the constructor.
        Ok(self.length)
    }

    pub(crate) fn document_became_inactive(&self) {
        if let Some(renderer) = &*self.renderer.borrow() {
            renderer.stop();
            renderer.set_client(None);
        }
    }
}
