/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Transferable streams, https://streams.spec.whatwg.org/#transferrable-streams, which LibWeb's C++ MessagePort
//! carries.

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{
    EmbedderCell, EmbedderClass, EmbedderValue, Gc, Must, NativeFunction, Object, PrimitiveString, PropertyKey,
    RawNativeFunctionResult, Realm, Trace, Value, Vm,
};

use crate::bindings::interface::{Ref, allocate};
use crate::bindings::promise::{
    NativeSteps, WebIdlPromise, create_promise, create_rejected_promise_from_exception, create_resolved_promise,
    exception_value, reject_promise_with_exception, resolve_promise, upon_fulfillment,
};
use crate::bindings::structured_data::{TransferDataReader, TransferDataWriter};
use crate::bindings::{CallContext, Exception, MustNotThrow, WebIdlResult};
use crate::streams::algorithms::{
    AbortAlgorithm, CancelAlgorithm, CloseAlgorithm, PullAlgorithm, SizeAlgorithm, StartAlgorithm, WriteAlgorithm,
};
use crate::streams::cpp::CppCell;
use crate::streams::readable_stream::{ReadableStream, initialize_readable_stream};
use crate::streams::readable_stream_default_controller::{
    ReadableStreamDefaultController, readable_stream_default_controller_close,
    readable_stream_default_controller_enqueue, readable_stream_default_controller_error,
    set_up_readable_stream_default_controller,
};
use crate::streams::writable_stream::{WritableStream, initialize_writable_stream};
use crate::streams::writable_stream_default_controller::{
    WritableStreamDefaultController, set_up_writable_stream_default_controller,
    writable_stream_default_controller_error_if_needed,
};

unsafe extern "C" {
    /// Two new MessagePorts in the realm, entangled with each other.
    fn web_streams_create_entangled_ports(realm: *mut c_void, port1: &mut *mut c_void, port2: &mut *mut c_void);

    /// Adds handlers for the port's message and messageerror events that call the functions, native functions of the
    /// realm, with the event.
    fn web_streams_message_port_add_handlers(
        realm: *mut c_void,
        port: *mut c_void,
        on_message: *mut c_void,
        on_message_error: *mut c_void,
    );

    /// The data of a MessageEvent, whose wrapper the handlers of a port are called with.
    fn web_streams_message_event_data(event: u64) -> u64;

    /// Runs the message port post message steps providing the port with which the port is entangled, if any, the
    /// message and «[ "transfer" → « » ]».
    fn web_streams_message_port_post_message(
        realm: *mut c_void,
        port: *mut c_void,
        message: u64,
    ) -> RawNativeFunctionResult;

    /// Disentangles the port.
    fn web_streams_message_port_disentangle(port: *mut c_void);

    /// Enables the port's port message queue.
    fn web_streams_message_port_enable(port: *mut c_void);

    /// The MessagePort that a wrapper wraps.
    fn web_streams_message_port_from_object(object: *mut c_void) -> *mut c_void;

    /// The wrapper of the MessagePort in the realm.
    fn web_streams_message_port_wrapper(realm: *mut c_void, port: *mut c_void) -> *mut c_void;

    /// Sets the data holder to ! StructuredSerializeWithTransfer(value, « value »).
    fn web_streams_serialize_with_transfer(data_holder: *mut c_void, value: u64);

    /// ! StructuredDeserializeWithTransfer(dataHolder, the current Realm).[[Deserialized]], for the next value that the
    /// data holder holds.
    fn web_streams_deserialize_with_transfer(data_holder: *mut c_void) -> u64;
}

/// The steps of a port's message event handler, given the state of the transform and the data of the message.
type MessageSteps<C> = fn(&CallContext, C, Value);

/// The steps of a port's messageerror event handler, given the state of the transform.
type MessageErrorSteps<C> = fn(&CallContext, C);

/// A MessagePort that a cross-realm transform posts its messages to.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CrossRealmTransformPort(CppCell);

impl CrossRealmTransformPort {
    fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#disentangle
    fn disentangle(&self) {
        // SAFETY: The port is live.
        unsafe { web_streams_message_port_disentangle(self.as_ptr()) };
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-start
    fn enable(&self) {
        // SAFETY: The port is live.
        unsafe { web_streams_message_port_enable(self.as_ptr()) };
    }

    /// Adds the handlers for the port's message and messageerror events, which are given the state.
    fn add_handlers<C: Trace + Copy + 'static>(
        &self,
        cx: &CallContext,
        state: C,
        on_message: MessageSteps<C>,
        on_message_error: MessageErrorSteps<C>,
    ) {
        let realm = cx.realm();
        let on_message = NativeFunction::create(
            cx.vm(),
            NativeSteps {
                captures: (state, realm),
                steps: on_message,
            },
            |vm: &Vm, function: &NativeSteps<(C, Gc<Realm>), MessageSteps<C>>| {
                let (state, realm) = function.captures;
                // SAFETY: The port's message event handler is called with a MessageEvent.
                let data = Value(unsafe { web_streams_message_event_data(vm.argument(0).0) });
                (function.steps)(&CallContext::new(vm, realm), state, data);
                Ok(Value::UNDEFINED)
            },
            1,
            &PropertyKey::from_utf8(""),
            Some(realm),
            None,
            None,
        );
        let on_message_error = NativeFunction::create(
            cx.vm(),
            NativeSteps {
                captures: (state, realm),
                steps: on_message_error,
            },
            |vm: &Vm, function: &NativeSteps<(C, Gc<Realm>), MessageErrorSteps<C>>| {
                let (state, realm) = function.captures;
                (function.steps)(&CallContext::new(vm, realm), state);
                Ok(Value::UNDEFINED)
            },
            1,
            &PropertyKey::from_utf8(""),
            Some(realm),
            None,
            None,
        );
        // SAFETY: The realm, the port and the functions are live.
        unsafe {
            web_streams_message_port_add_handlers(
                realm.as_ptr().cast(),
                self.as_ptr(),
                on_message.as_ptr().cast(),
                on_message_error.as_ptr().cast(),
            );
        }
    }

    // https://streams.spec.whatwg.org/#abstract-opdef-packandpostmessage
    fn pack_and_post_message(&self, cx: &CallContext, r#type: &str, value: Value) -> WebIdlResult<()> {
        let vm = cx.vm();

        // 1. Let message be OrdinaryObjectCreate(null).
        let message = Object::create(vm, cx.realm(), None);

        // 2. Perform ! CreateDataProperty(message, "type", type).
        let r#type = Value::from_string(PrimitiveString::create_from_ascii(vm, r#type.as_bytes()));
        message
            .create_data_property(vm, &vm.names.type_, r#type, None, None)
            .must();

        // 3. Perform ! CreateDataProperty(message, "value", value).
        message
            .create_data_property(vm, &vm.names.value, value, None, None)
            .must();

        // 4. Let targetPort be the port with which port is entangled, if any; otherwise let it be null.
        // 5. Let options be «[ "transfer" → « » ]».
        // 6. Run the message port post message steps providing targetPort, message, and options.
        // SAFETY: The realm, the port and the message are live.
        unsafe {
            web_streams_message_port_post_message(
                cx.realm().as_ptr().cast(),
                self.as_ptr(),
                Value::from_object(message).0,
            )
        }
        .into_completion()?;
        Ok(())
    }

    // https://streams.spec.whatwg.org/#abstract-opdef-packandpostmessagehandlingerror
    fn pack_and_post_message_handling_error(&self, cx: &CallContext, r#type: &str, value: Value) -> WebIdlResult<()> {
        // 1. Let result be PackAndPostMessage(port, type, value).
        let result = self.pack_and_post_message(cx, r#type, value);

        // 2. If result is an abrupt completion,
        let Err(exception) = result else {
            // 3. Return result as a completion record.
            return Ok(());
        };
        let error = exception_value(cx.vm(), cx.realm(), exception);

        // 1. Perform ! CrossRealmTransformSendError(port, result.[[Value]]).
        self.cross_realm_transform_send_error(cx, error);

        // 3. Return result as a completion record.
        Err(Exception::Thrown(libjs_rust::api::Throw::new(error)))
    }

    // https://streams.spec.whatwg.org/#abstract-opdef-crossrealmtransformsenderror
    fn cross_realm_transform_send_error(&self, cx: &CallContext, error: Value) {
        // 1. Perform PackAndPostMessage(port, "error", error), discarding the result.
        let _ = self.pack_and_post_message(cx, "error", error);
    }

    /// The pullAlgorithm of SetUpCrossRealmTransformReadable.
    pub(crate) fn pull_algorithm(&self, cx: &CallContext) -> WebIdlPromise {
        // 1. Perform ! PackAndPostMessage(port, "pull", undefined).
        self.pack_and_post_message(cx, "pull", Value::UNDEFINED).must();

        // 2. Return a promise resolved with undefined.
        create_resolved_promise(cx.vm(), cx.realm(), Value::UNDEFINED)
    }

    /// The cancelAlgorithm of SetUpCrossRealmTransformReadable, and the abortAlgorithm of
    /// SetUpCrossRealmTransformWritable.
    pub(crate) fn cancel_algorithm(&self, cx: &CallContext, reason: Value) -> WebIdlPromise {
        // 1. Let result be PackAndPostMessageHandlingError(port, "error", reason).
        let result = self.pack_and_post_message_handling_error(cx, "error", reason);

        // 2. Disentangle port.
        self.disentangle();

        // 3. If result is an abrupt completion, return a promise rejected with result.[[Value]].
        if let Err(exception) = result {
            return create_rejected_promise_from_exception(cx.vm(), cx.realm(), exception);
        }

        // 4. Otherwise, return a promise resolved with undefined.
        create_resolved_promise(cx.vm(), cx.realm(), Value::UNDEFINED)
    }

    /// The closeAlgorithm of SetUpCrossRealmTransformWritable.
    pub(crate) fn close_algorithm(&self, cx: &CallContext) -> WebIdlPromise {
        // 1. Perform ! PackAndPostMessage(port, "close", undefined).
        self.pack_and_post_message(cx, "close", Value::UNDEFINED).must();

        // 2. Disentangle port.
        self.disentangle();

        // 3. Return a promise resolved with undefined.
        create_resolved_promise(cx.vm(), cx.realm(), Value::UNDEFINED)
    }

    /// The abortAlgorithm of SetUpCrossRealmTransformWritable.
    pub(crate) fn abort_algorithm(&self, cx: &CallContext, reason: Value) -> WebIdlPromise {
        // 1. Let result be PackAndPostMessageHandlingError(port, "error", reason).
        // 2. Disentangle port.
        // 3. If result is an abrupt completion, return a promise rejected with result.[[Value]].
        // 4. Otherwise, return a promise resolved with undefined.
        self.cancel_algorithm(cx, reason)
    }
}

/// The type and the value of a message that a cross-realm transform received, ! Get(data, "type") and
/// ! Get(data, "value").
fn type_and_value_of_message(vm: &Vm, data: Value) -> (Gc<PrimitiveString>, Value) {
    // 2. Assert: data is an Object.
    assert!(data.is_object());
    let data = data.as_object();

    // 3. Let type be ! Get(data, "type").
    let r#type = data.get(vm, &vm.names.type_).must();

    // 4. Let value be ! Get(data, "value").
    let value = data.get(vm, &vm.names.value).must();

    // 5. Assert: type is a String.
    assert!(r#type.is_string());
    (r#type.as_string(), value)
}

/// A new "DataCloneError" DOMException, which a cross-realm transform errors with when a message cannot be received.
fn data_clone_error(cx: &CallContext) -> Value {
    exception_value(
        cx.vm(),
        cx.realm(),
        Exception::Dom(
            "DataCloneError",
            ak::Utf16String::from_ascii(b"Unable to transfer stream"),
        ),
    )
}

/// The state that the handlers and the algorithms of a cross-realm transform share.
#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
struct ReadableHandlerState {
    port: Gc<EmbedderCell<PortCell>>,
    controller: Ref<ReadableStreamDefaultController>,
}

/// A cell that holds a MessagePort, which the handlers of its events capture.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct PortCell(CrossRealmTransformPort);

static PORT_CELL_CLASS: EmbedderClass<PortCell> = EmbedderClass::new("CrossRealmTransformPort");

impl EmbedderValue for PortCell {
    const CLASS: &'static EmbedderClass<Self> = &PORT_CELL_CLASS;
}

// https://streams.spec.whatwg.org/#abstract-opdef-setupcrossrealmtransformreadable
pub(crate) fn set_up_cross_realm_transform_readable(
    cx: &CallContext,
    stream: Ref<ReadableStream>,
    port: CrossRealmTransformPort,
) {
    stream.set_realm(cx.realm());

    // 1. Perform ! InitializeReadableStream(stream).
    initialize_readable_stream(stream);

    // 2. Let controller be a new ReadableStreamDefaultController.
    let controller = allocate(cx.vm(), ReadableStreamDefaultController::new());
    let state = ReadableHandlerState {
        port: PORT_CELL_CLASS.allocate(cx.vm(), PortCell(port.clone())),
        controller,
    };

    // 3. Add a handler for port’s message event with the following steps:
    fn on_message(cx: &CallContext, state: ReadableHandlerState, data: Value) {
        let port = &state.port.0;
        let controller = state.controller;

        // 1. Let data be the data of the message.
        // 2. Assert: data is an Object.
        // 3. Let type be ! Get(data, "type").
        // 4. Let value be ! Get(data, "value").
        // 5. Assert: type is a String.
        let (r#type, value) = type_and_value_of_message(cx.vm(), data);
        let r#type = r#type.to_utf8();

        // 6. If type is "chunk",
        if r#type == "chunk" {
            // 1. Perform ! ReadableStreamDefaultControllerEnqueue(controller, value).
            readable_stream_default_controller_enqueue(cx, controller, value).must();
        }
        // 7. Otherwise, if type is "close",
        else if r#type == "close" {
            // 1. Perform ! ReadableStreamDefaultControllerClose(controller).
            readable_stream_default_controller_close(cx, controller);

            // 2. Disentangle port.
            port.disentangle();
        }
        // 8. Otherwise, if type is "error",
        else if r#type == "error" {
            // 1. Perform ! ReadableStreamDefaultControllerError(controller, value).
            readable_stream_default_controller_error(cx, controller, value);

            // 2. Disentangle port.
            port.disentangle();
        }
    }

    // 4. Add a handler for port’s messageerror event with the following steps:
    fn on_message_error(cx: &CallContext, state: ReadableHandlerState) {
        let port = &state.port.0;

        // 1. Let error be a new "DataCloneError" DOMException.
        let error = data_clone_error(cx);

        // 2. Perform ! CrossRealmTransformSendError(port, error).
        port.cross_realm_transform_send_error(cx, error);

        // 3. Perform ! ReadableStreamDefaultControllerError(controller, error).
        readable_stream_default_controller_error(cx, state.controller, error);

        // 4. Disentangle port.
        port.disentangle();
    }
    port.add_handlers(cx, state, on_message, on_message_error);

    // 5. Enable port’s port message queue.
    port.enable();

    // 6. Let startAlgorithm be an algorithm that returns undefined.
    let start_algorithm = StartAlgorithm::ReturnsUndefined;

    // 7. Let pullAlgorithm be the following steps:
    let pull_algorithm = PullAlgorithm::CrossRealm(port.clone());

    // 8. Let cancelAlgorithm be the following steps, taking a reason argument:
    let cancel_algorithm = CancelAlgorithm::CrossRealm(port);

    // 9. Let sizeAlgorithm be an algorithm that returns 1.
    let size_algorithm = SizeAlgorithm::ReturnsOne;

    // 10. Perform ! SetUpReadableStreamDefaultController(stream, controller, startAlgorithm, pullAlgorithm,
    //     cancelAlgorithm, 0, sizeAlgorithm).
    set_up_readable_stream_default_controller(
        cx,
        stream,
        controller,
        start_algorithm,
        pull_algorithm,
        cancel_algorithm,
        0.0,
        size_algorithm,
    )
    .must();
}

/// The state that the handlers and the writeAlgorithm of SetUpCrossRealmTransformWritable share.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CrossRealmWritableState {
    port: CrossRealmTransformPort,
    controller: Ref<WritableStreamDefaultController>,
    // backpressurePromise
    backpressure_promise: Cell<Option<WebIdlPromise>>,
}

static CROSS_REALM_WRITABLE_STATE_CLASS: EmbedderClass<CrossRealmWritableState> =
    EmbedderClass::new("CrossRealmTransformWritable");

impl EmbedderValue for CrossRealmWritableState {
    const CLASS: &'static EmbedderClass<Self> = &CROSS_REALM_WRITABLE_STATE_CLASS;
}

impl CrossRealmWritableState {
    /// The writeAlgorithm of SetUpCrossRealmTransformWritable.
    pub(crate) fn write_algorithm(this: Gc<EmbedderCell<Self>>, cx: &CallContext, chunk: Value) -> WebIdlPromise {
        // 1. If backpressurePromise is undefined, set backpressurePromise to a promise resolved with undefined.
        let backpressure_promise = match this.backpressure_promise.get() {
            Some(backpressure_promise) => backpressure_promise,
            None => {
                let backpressure_promise = create_resolved_promise(cx.vm(), cx.realm(), Value::UNDEFINED);
                this.backpressure_promise.set(Some(backpressure_promise));
                backpressure_promise
            }
        };

        // AD-HOC: The steps below return a promise that they create when they run, but the error of
        //         PackAndPostMessageHandlingError has to reach the original ReadableStream, which WPT tests: the
        //         promise that reacting to backpressurePromise returns is the one that they reject or resolve.
        let reaction_promise = create_promise(cx.vm(), cx.realm());

        // 2. Return the result of reacting to backpressurePromise with the following fulfillment steps:
        upon_fulfillment(
            cx.vm(),
            backpressure_promise,
            (this, reaction_promise, chunk),
            |cx, (this, reaction_promise, chunk), _| {
                // 1. Set backpressurePromise to a new promise.
                this.backpressure_promise.set(Some(create_promise(cx.vm(), cx.realm())));

                // 2. Let result be PackAndPostMessageHandlingError(port, "chunk", chunk).
                let result = this.port.pack_and_post_message_handling_error(cx, "chunk", chunk);

                match result {
                    // 3. If result is an abrupt completion,
                    Err(exception) => {
                        // 1. Disentangle port.
                        this.port.disentangle();

                        // 2. Return a promise rejected with result.[[Value]].
                        reject_promise_with_exception(cx.vm(), reaction_promise, exception);
                    }
                    // 4. Otherwise, return a promise resolved with undefined.
                    Ok(()) => resolve_promise(cx.vm(), reaction_promise, Value::UNDEFINED),
                }
                Ok(Value::UNDEFINED)
            },
        );
        reaction_promise
    }

    /// Resolves backpressurePromise with undefined and sets it to undefined, if it is not undefined.
    fn resolve_backpressure_promise(&self, vm: &Vm) {
        // 1. If backpressurePromise is not undefined,
        if let Some(backpressure_promise) = self.backpressure_promise.take() {
            // 1. Resolve backpressurePromise with undefined.
            // 2. Set backpressurePromise to undefined.
            resolve_promise(vm, backpressure_promise, Value::UNDEFINED);
        }
    }
}

// https://streams.spec.whatwg.org/#abstract-opdef-setupcrossrealmtransformwritable
pub(crate) fn set_up_cross_realm_transform_writable(
    cx: &CallContext,
    stream: Ref<WritableStream>,
    port: CrossRealmTransformPort,
) {
    // 1. Perform ! InitializeWritableStream(stream).
    initialize_writable_stream(stream);

    // 2. Let controller be a new WritableStreamDefaultController.
    let controller = allocate(cx.vm(), WritableStreamDefaultController::new());

    // 3. Let backpressurePromise be a new promise.
    let state = CROSS_REALM_WRITABLE_STATE_CLASS.allocate(
        cx.vm(),
        CrossRealmWritableState {
            port: port.clone(),
            controller,
            backpressure_promise: Cell::new(Some(create_promise(cx.vm(), cx.realm()))),
        },
    );

    // 4. Add a handler for port’s message event with the following steps:
    fn on_message(cx: &CallContext, state: Gc<EmbedderCell<CrossRealmWritableState>>, data: Value) {
        // 1. Let data be the data of the message.
        // 2. Assert: data is an Object.
        // 3. Let type be ! Get(data, "type").
        // 4. Let value be ! Get(data, "value").
        // 5. Assert: type is a String.
        let (r#type, value) = type_and_value_of_message(cx.vm(), data);
        let r#type = r#type.to_utf8();

        // 6. If type is "pull",
        if r#type == "pull" {
            // 1. If backpressurePromise is not undefined,
            state.resolve_backpressure_promise(cx.vm());
        }
        // 7. Otherwise, if type is "error",
        else if r#type == "error" {
            // 1. Perform ! WritableStreamDefaultControllerErrorIfNeeded(controller, value).
            writable_stream_default_controller_error_if_needed(cx, state.controller, value);

            // 2. If backpressurePromise is not undefined,
            state.resolve_backpressure_promise(cx.vm());
        }
    }

    // 5. Add a handler for port’s messageerror event with the following steps:
    fn on_message_error(cx: &CallContext, state: Gc<EmbedderCell<CrossRealmWritableState>>) {
        // 1. Let error be a new "DataCloneError" DOMException.
        let error = data_clone_error(cx);

        // 2. Perform ! CrossRealmTransformSendError(port, error).
        state.port.cross_realm_transform_send_error(cx, error);

        // 3. Perform ! WritableStreamDefaultControllerErrorIfNeeded(controller, error).
        writable_stream_default_controller_error_if_needed(cx, state.controller, error);

        // 4. Disentangle port.
        state.port.disentangle();
    }
    port.add_handlers(cx, state, on_message, on_message_error);

    // 6. Enable port’s port message queue.
    port.enable();

    // 7. Let startAlgorithm be an algorithm that returns undefined.
    let start_algorithm = StartAlgorithm::ReturnsUndefined;

    // 8. Let writeAlgorithm be the following steps, taking a chunk argument:
    let write_algorithm = WriteAlgorithm::CrossRealm(state);

    // 9. Let closeAlgorithm be the following steps:
    let close_algorithm = CloseAlgorithm::CrossRealm(port.clone());

    // 10. Let abortAlgorithm be the following steps, taking a reason argument:
    let abort_algorithm = AbortAlgorithm::CrossRealm(port);

    // 11. Let sizeAlgorithm be an algorithm that returns 1.
    let size_algorithm = SizeAlgorithm::ReturnsOne;

    // 12. Perform ! SetUpWritableStreamDefaultController(stream, controller, startAlgorithm, writeAlgorithm,
    //     closeAlgorithm, abortAlgorithm, 1, sizeAlgorithm).
    set_up_writable_stream_default_controller(
        cx,
        stream,
        controller,
        start_algorithm,
        write_algorithm,
        close_algorithm,
        abort_algorithm,
        1.0,
        size_algorithm,
    )
    .must();
}

/// Two new MessagePorts in the current realm, entangled with each other.
pub(crate) fn create_entangled_ports(cx: &CallContext) -> (CrossRealmTransformPort, CrossRealmTransformPort) {
    let mut port1 = core::ptr::null_mut();
    let mut port2 = core::ptr::null_mut();
    // SAFETY: The realm is live, and LibWeb returns two live ports.
    unsafe {
        web_streams_create_entangled_ports(cx.realm().as_ptr().cast(), &mut port1, &mut port2);
        (
            CrossRealmTransformPort(CppCell::from_raw(port1)),
            CrossRealmTransformPort(CppCell::from_raw(port2)),
        )
    }
}

/// Sets the data holder to ! StructuredSerializeWithTransfer(port, « port »).
pub(crate) fn serialize_message_port(
    cx: &CallContext,
    data_holder: &mut TransferDataWriter,
    port: &CrossRealmTransformPort,
) {
    // SAFETY: The realm and the port are live, and LibWeb returns the wrapper of the port.
    let wrapper = unsafe { web_streams_message_port_wrapper(cx.realm().as_ptr().cast(), port.as_ptr()) };
    let wrapper = NonNull::new(wrapper.cast::<Object>()).expect("LibWeb returns a wrapper");
    // SAFETY: LibWeb returns a live wrapper.
    let wrapper = unsafe { Gc::from_non_null(wrapper) };
    serialize_with_transfer(data_holder, Value::from_object(wrapper));
}

/// The MessagePort of ! StructuredDeserializeWithTransfer(dataHolder.[[port]], the current Realm).
pub(crate) fn deserialize_message_port(
    _: &CallContext,
    data_holder: &mut TransferDataReader,
) -> CrossRealmTransformPort {
    let deserialized = deserialize_with_transfer(data_holder);
    assert!(deserialized.is_object());
    // SAFETY: The object is live, and LibWeb returns the MessagePort that it wraps.
    unsafe {
        let port = web_streams_message_port_from_object(deserialized.as_object().as_ptr().cast());
        CrossRealmTransformPort(CppCell::from_raw(port))
    }
}

/// Sets the data holder to ! StructuredSerializeWithTransfer(value, « value »).
pub(crate) fn serialize_with_transfer(data_holder: &mut TransferDataWriter, value: Value) {
    // SAFETY: The data holder is lent for the transfer steps, and the value is live.
    unsafe { web_streams_serialize_with_transfer(data_holder.as_raw(), value.0) };
}

/// ! StructuredDeserializeWithTransfer(dataHolder, the current Realm).[[Deserialized]]
pub(crate) fn deserialize_with_transfer(data_holder: &mut TransferDataReader) -> Value {
    // SAFETY: The data holder is lent for the transfer-receiving steps.
    Value(unsafe { web_streams_deserialize_with_transfer(data_holder.as_raw()) })
}
