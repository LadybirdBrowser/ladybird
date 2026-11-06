/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The window post message steps of Window,
//! https://html.spec.whatwg.org/multipage/web-messaging.html#window-post-message-steps

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{
    EmbedderCell, EmbedderClass, EmbedderValue, Gc, MarkedVec, Object, Realm, Trace, Value, Vm, is_host_instance_of,
};
use liburl_rust::FfiOrigin;
use liburl_rust::origin::Origin;

use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::{CppEvent, CppEventTarget};
use crate::bindings::interface::{Interface, with_temporary_execution_context_and_callbacks};
use crate::bindings::promise::{TaskSource, queue_a_global_task};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::html::message_port::MessagePort;
use crate::html::structured_serialize::transfer_record::{TransferRecord, TransferRecordRef};
use crate::html::structured_serialize::{structured_deserialize_with_transfer, structured_serialize_with_transfer};

type ReceiveOrigin = unsafe extern "C" fn(*mut c_void, *const FfiOrigin);

unsafe extern "C" {
    fn web_window_post_message_incumbent_settings() -> *mut c_void;
    fn web_window_post_message_settings_origin(settings: *mut c_void, context: *mut c_void, receive: ReceiveOrigin);
    fn web_window_post_message_source(settings: *mut c_void) -> *mut c_void;
    fn web_window_post_message_parse_target_origin(target: usize, context: *mut c_void, receive: ReceiveOrigin);
    fn web_window_post_message_realm(window: *mut c_void) -> *mut c_void;
    fn web_window_post_message_document_origin(window: *mut c_void, context: *mut c_void, receive: ReceiveOrigin);
    fn web_window_post_message_send_remote(
        target: *mut c_void,
        settings: *mut c_void,
        record: *mut c_void,
        target_origin: *const FfiOrigin,
        source_origin: *const FfiOrigin,
        source: *mut c_void,
    );
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PostedMessage {
    pub(crate) target_window: CppCell,
    pub(crate) serialize_with_transfer_result: TransferRecordRef,
    #[gc(untraced)]
    pub(crate) target_origin: Option<Origin>,
    #[gc(untraced)]
    pub(crate) origin: Origin,
    pub(crate) source: Option<CppCell>,
}

static POSTED_MESSAGE_CLASS: EmbedderClass<PostedMessage> = EmbedderClass::new("WindowPostedMessage");

impl EmbedderValue for PostedMessage {
    const CLASS: &'static EmbedderClass<Self> = &POSTED_MESSAGE_CLASS;
}

unsafe extern "C" fn receive_origin(context: *mut c_void, origin: *const FfiOrigin) {
    // SAFETY: Each caller lends its result slot, and C++ lends a valid origin for the call.
    unsafe { *context.cast::<Option<Origin>>() = Some((*origin).to_origin()) };
}

fn origin_of(cell: &CppCell, query: unsafe extern "C" fn(*mut c_void, *mut c_void, ReceiveOrigin)) -> Origin {
    let mut origin = None;
    // SAFETY: The cell is live, and the callback writes to the result slot while it is lent.
    unsafe { query(cell.as_ptr(), (&raw mut origin).cast(), receive_origin) };
    origin.expect("C++ lends the origin")
}

fn realm_of(window: &CppCell) -> Gc<Realm> {
    // SAFETY: The window is live, and C++ returns its live principal realm.
    unsafe {
        Gc::from_non_null(
            NonNull::new(web_window_post_message_realm(window.as_ptr()).cast()).expect("a Window has a realm"),
        )
    }
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#window-post-message-steps
pub(crate) fn window_post_message_steps(
    cx: &CallContext,
    target_window: CppCell,
    remote: bool,
    message: Value,
    target_origin: &Utf16String,
    transfer: &[Gc<Object>],
) -> WebIdlResult<()> {
    // 1. Let targetRealm be targetWindow's realm.
    // NB: Taken from targetWindow when the queued task delivers the message.

    // 2. Let incumbentSettings be the incumbent settings object.
    // SAFETY: C++ returns the live incumbent settings object.
    let incumbent_settings = unsafe { CppCell::from_raw(web_window_post_message_incumbent_settings()) };

    // 3. Let targetOrigin be options["targetOrigin"].
    let target_origin_units = target_origin.to_utf16();
    let target_origin = if target_origin_units.as_ref() == [u16::from(b'/')] {
        // 4. If targetOrigin is a single U+002F SOLIDUS character (/), then set targetOrigin to incumbentSettings's origin.
        Some(origin_of(&incumbent_settings, web_window_post_message_settings_origin))
    }
    // 5. Otherwise, if targetOrigin is not a single U+002A ASTERISK character (*):
    else if target_origin_units.as_ref() != [u16::from(b'*')] {
        // 1. Let parsedURL be the result of running the URL parser on targetOrigin.
        let mut parsed_url_origin = None;
        // SAFETY: The string and the result slot are lent for the call.
        unsafe {
            web_window_post_message_parse_target_origin(
                target_origin.raw_identity(),
                (&raw mut parsed_url_origin).cast(),
                receive_origin,
            );
        }

        // 2. If parsedURL is failure, then throw a "SyntaxError" DOMException.
        let Some(parsed_url_origin) = parsed_url_origin else {
            return Err(Exception::Dom(
                "SyntaxError",
                Utf16String::from_utf8(&format!(
                    "Invalid URL for targetOrigin: '{}'",
                    String::from_utf16_lossy(&target_origin_units)
                )),
            ));
        };

        // 3. Set targetOrigin to parsedURL's origin.
        Some(parsed_url_origin)
    } else {
        None
    };

    // 6. Let transfer be options["transfer"].
    // 7. Let serializeWithTransferResult be StructuredSerializeWithTransfer(message, transfer). Rethrow any exceptions.
    let serialize_with_transfer_result = structured_serialize_with_transfer(cx, message, transfer)?;

    // NB: The task queued by step 8 reads incumbentSettings in its steps 2 and 3. Snapshot those values here.
    // 8.2. Let origin be incumbentSettings's origin.
    let origin = origin_of(&incumbent_settings, web_window_post_message_settings_origin);

    // 8.3. Let source be the WindowProxy object corresponding to incumbentSettings's global object (a Window object).
    // SAFETY: The settings object is live, and C++ returns its live WindowProxy.
    let source = unsafe { CppCell::from_raw(web_window_post_message_source(incumbent_settings.as_ptr())) };

    // 8. Queue a global task on the posted message task source given targetWindow to run the following steps:
    if remote {
        // NB: targetWindow lives in the process hosting this navigable's document, so the task is a request to the UI
        //     process, which forwards it there. That process represents the source's navigable, which names the source.
        //     A source whose [[Window]] has no navigable is null there.
        let serialize_with_transfer_result = TransferRecord::into_cpp(serialize_with_transfer_result, cx.vm());
        FfiOrigin::with_origin(&origin, |origin| {
            let send = |target_origin| {
                // SAFETY: Every cell and origin is live, and the call takes over the record's contents.
                unsafe {
                    web_window_post_message_send_remote(
                        target_window.as_ptr(),
                        incumbent_settings.as_ptr(),
                        serialize_with_transfer_result.record().as_raw(),
                        target_origin,
                        origin,
                        source.as_ptr(),
                    );
                }
            };
            match &target_origin {
                Some(target_origin) => FfiOrigin::with_origin(target_origin, |target_origin| send(target_origin)),
                None => send(core::ptr::null()),
            }
        });
        return Ok(());
    }
    queue_posted_message(
        cx.vm(),
        PostedMessage {
            target_window,
            serialize_with_transfer_result,
            target_origin,
            origin,
            source: Some(source),
        },
    );
    Ok(())
}

pub(crate) fn queue_posted_message(vm: &Vm, posted_message: PostedMessage) {
    let posted_message = POSTED_MESSAGE_CLASS.allocate(vm, |_| posted_message);
    queue_a_global_task(
        vm,
        realm_of(&posted_message.target_window).global_object(),
        TaskSource::PostedMessage,
        posted_message,
        posted_message_task_steps,
    );
}

fn posted_message_task_steps(vm: &Vm, posted_message: Gc<EmbedderCell<PostedMessage>>) {
    let serialize_with_transfer_result = posted_message.serialize_with_transfer_result;

    // 1. Let targetRealm be targetWindow's realm.
    let target_realm = realm_of(&posted_message.target_window);

    // 1. If the targetOrigin argument is not a single literal U+002A ASTERISK character (*) and targetWindow's
    //    associated Document's origin is not same origin with targetOrigin, then return.
    // NB: Due to step 4 and 5 above, the only time it's not '*' is if target_origin contains an Origin.
    if let Some(target_origin) = &posted_message.target_origin
        && !origin_of(&posted_message.target_window, web_window_post_message_document_origin)
            .is_same_origin(target_origin)
    {
        TransferRecord::discard(serialize_with_transfer_result);
        return;
    }

    // 2. Let origin be incumbentSettings's origin.
    // 3. Let source be the WindowProxy object corresponding to incumbentSettings's global object (a Window object).
    // NB: Both were snapshotted before the task was queued.
    let origin = &posted_message.origin;
    let source = posted_message.source.as_ref();

    // 4. Let deserializeRecord be StructuredDeserializeWithTransfer(serializeWithTransferResult, targetRealm).
    // NB: The execution context must not stay on the stack while the event is dispatched, so that a microtask
    //     checkpoint can be performed after each listener.
    let cx = CallContext::new(vm, target_realm);
    let deserialize_record = with_temporary_execution_context_and_callbacks(target_realm, || {
        structured_deserialize_with_transfer(&cx, serialize_with_transfer_result)
    });
    TransferRecord::discard(serialize_with_transfer_result);

    // SAFETY: The target is a live Window, which is an EventTarget.
    let target_window = unsafe { CppEventTarget::from_raw(posted_message.target_window.as_ptr()) };

    //    If this throws an exception, catch it, fire an event named messageerror at targetWindow, using MessageEvent,
    //    with its origin initialized to origin and the source attribute initialized to source, and then return.
    let Ok(deserialize_record) = deserialize_record else {
        let event =
            CppEvent::create_message_event(target_realm, "messageerror", Value::NULL, &[], Some(origin), source);
        event.set_is_trusted(true);
        target_window.dispatch_event(&event);
        return;
    };

    // 5. Let messageClone be deserializeRecord.[[Deserialized]].
    let message_clone = deserialize_record.deserialized;

    // 6. Let newPorts be a new frozen array consisting of all MessagePort objects in deserializeRecord.[[TransferredValues]],
    //    if any, maintaining their relative order.
    // FIXME: Use a FrozenArray
    let new_ports = MarkedVec::new(vm);
    for &value in deserialize_record.transferred_values.values().iter() {
        if is_host_instance_of(&value, MessagePort::wrapper_class()) {
            new_ports.push(value);
        }
    }

    // 7. Fire an event named message at targetWindow, using MessageEvent, with its origin initialized to origin,
    //    the source attribute initialized to source, the data attribute initialized to messageClone, and the ports
    //    attribute initialized to newPorts.
    let event = CppEvent::create_message_event(
        target_realm,
        "message",
        message_clone,
        &new_ports.values(),
        Some(origin),
        source,
    );
    event.set_is_trusted(true);
    target_window.dispatch_event(&event);
}
