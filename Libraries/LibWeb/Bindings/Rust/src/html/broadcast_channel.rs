/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024-2026, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! BroadcastChannel, https://html.spec.whatwg.org/multipage/web-messaging.html#broadcasting-to-other-browsing-contexts

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;
use std::collections::VecDeque;
use std::rc::Rc;

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, Gc, GcWeak, MarkedVec, Realm, Trace, Value, Vm};
use liburl_rust::FfiOrigin;
use liburl_rust::origin::Origin;

use crate::bindings::event_target::{CppEvent, CppEventTarget, EventListenerListChanged, GetTheParent};
use crate::bindings::global_object::{
    relevant_global_object, window_from_global_object, worker_global_scope_from_global_object,
};
use crate::bindings::interface::{Ref, Wrappable, Wrapped, allocate};
use crate::bindings::promise::{TaskSource, queue_a_global_task};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::html::structured_serialize::cpp::{IPCSerializationRecord, ReceivedIPCSerializationRecord, SharedBuffers};
use crate::html::structured_serialize::{
    structured_deserialize_with_shared_buffers, structured_serialize_with_shared_buffers,
};
use crate::storage_api::storage_key::{StorageKey, obtain_a_storage_key_for_non_storage_purposes};

unsafe extern "C" {
    fn web_html_broadcast_channel_create_serialized_message() -> *mut c_void;
    fn web_html_broadcast_channel_destroy_serialized_message(message: *mut c_void);
    fn web_html_broadcast_channel_serialized_message_record(message: *mut c_void) -> *mut c_void;
    fn web_html_broadcast_channel_serialized_message_shared_buffers(message: *mut c_void) -> *mut c_void;
    fn web_html_broadcast_channel_relevant_settings_object_origin(
        realm: *mut c_void,
        ctx: *mut c_void,
        on_origin: unsafe extern "C" fn(*mut c_void, *const FfiOrigin),
    );
    fn web_html_broadcast_channel_set_strongly_referenced(
        realm: *mut c_void,
        event_target: *mut c_void,
        strongly_referenced: bool,
    );
    fn web_html_broadcast_channel_post_message_to_other_processes(
        realm: *mut c_void,
        channel_name: usize,
        message: *const c_void,
    );
}

/// The serialized message and the shared memory of its SharedArrayBuffers, which LibWeb's C++ code keeps as a
/// BroadcastChannelSerializedMessage that this owns.
pub(crate) struct SerializedMessage(NonNull<c_void>);

impl SerializedMessage {
    fn new() -> Self {
        // SAFETY: LibWeb returns a new message, which this owns.
        Self(NonNull::new(unsafe { web_html_broadcast_channel_create_serialized_message() }).expect("LibWeb allocates"))
    }

    /// # Safety
    ///
    /// `message` must be a message that LibWeb hands over.
    pub(crate) unsafe fn from_raw(message: *mut c_void) -> Self {
        Self(NonNull::new(message).expect("LibWeb hands over a message"))
    }

    fn record(&self) -> IPCSerializationRecord {
        // SAFETY: This owns the message, which lends its record for as long as it lives.
        unsafe {
            IPCSerializationRecord::from_raw(web_html_broadcast_channel_serialized_message_record(self.0.as_ptr()))
        }
    }

    fn received_record(&self) -> ReceivedIPCSerializationRecord<'_> {
        // SAFETY: This owns the message, which lends its record for as long as it lives.
        unsafe {
            ReceivedIPCSerializationRecord::from_raw(web_html_broadcast_channel_serialized_message_record(
                self.0.as_ptr(),
            ))
        }
    }

    fn shared_buffers(&self) -> SharedBuffers {
        // SAFETY: This owns the message, which lends its list for as long as it lives.
        unsafe {
            SharedBuffers::from_raw(web_html_broadcast_channel_serialized_message_shared_buffers(
                self.0.as_ptr(),
            ))
        }
    }
}

impl Drop for SerializedMessage {
    fn drop(&mut self) {
        // SAFETY: This owns the message.
        unsafe { web_html_broadcast_channel_destroy_serialized_message(self.0.as_ptr()) };
    }
}

/// A message that a BroadcastChannel posted, in this agent or in another process: what the tasks that steps 6 to 9 of
/// postMessage() queue read.
pub(crate) struct BroadcastChannelMessage {
    pub(crate) storage_key: StorageKey,
    pub(crate) channel_name: Utf16String,
    pub(crate) source_origin: Origin,
    pub(crate) serialized: SerializedMessage,
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#broadcastchannel
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct BroadcastChannel {
    event_target: CppEventTarget,
    realm: Gc<Realm>,

    // A BroadcastChannel object has a channel name and a closed flag.
    #[gc(untraced)]
    channel_name: Utf16String,
    closed_flag: Cell<bool>,

    #[gc(untraced)]
    origin: Origin,
    #[gc(untraced)]
    storage_key: StorageKey,

    // The messages whose tasks have not run yet. Each task takes the message at the front.
    #[gc(untraced)]
    queued_messages: RefCell<VecDeque<Rc<BroadcastChannelMessage>>>,
}

impl Wrappable for BroadcastChannel {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for BroadcastChannel {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl EventListenerListChanged for BroadcastChannel {
    fn event_listener_list_changed(this: Ref<Self>) {
        this.update_strong_reference_from_relevant_global_object();
    }
}

/// The channels of the thread's agent, in creation order: references that do not keep them alive, which a collected
/// channel leaves empty. The empty ones are pruned once the list has grown to twice what was live after the last
/// pruning.
struct BroadcastChannels {
    channels: Vec<GcWeak<EmbedderCell<Wrapped<BroadcastChannel>>>>,
    live_after_pruning: usize,
}

impl BroadcastChannels {
    const CHANNELS_BEFORE_FIRST_PRUNING: usize = 128;

    fn add(&mut self, vm: &Vm, channel: Ref<BroadcastChannel>) {
        if self.channels.len() >= (2 * self.live_after_pruning).max(Self::CHANNELS_BEFORE_FIRST_PRUNING) {
            self.channels.retain(|channel| channel.get().is_some());
            self.live_after_pruning = self.channels.len();
        }
        self.channels.push(GcWeak::new(vm.heap(), channel));
    }
}

thread_local! {
    static BROADCAST_CHANNELS: RefCell<BroadcastChannels> = const {
        RefCell::new(BroadcastChannels {
            channels: Vec::new(),
            live_after_pruning: 0,
        })
    };
}

fn relevant_settings_object_origin(realm: Gc<Realm>) -> Origin {
    unsafe extern "C" fn on_origin(ctx: *mut c_void, ffi_origin: *const FfiOrigin) {
        // SAFETY: ctx is the origin below, and LibWeb lends the origin for the call.
        unsafe { *ctx.cast::<Option<Origin>>() = Some((*ffi_origin).to_origin()) };
    }

    let mut origin: Option<Origin> = None;
    // SAFETY: The realm is live, and the origin outlives the call.
    unsafe {
        web_html_broadcast_channel_relevant_settings_object_origin(
            realm.as_ptr().cast(),
            (&raw mut origin).cast(),
            on_origin,
        );
    }
    origin.expect("LibWeb lends the origin")
}

impl BroadcastChannel {
    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-broadcastchannel
    pub(crate) fn construct(cx: &mut CallContext, name: Utf16String) -> WebIdlResult<Ref<Self>> {
        let vm = cx.vm();
        let realm = cx.realm();
        let channel = allocate(vm, |_| Self {
            event_target: CppEventTarget::new(),
            realm,
            // 1. Set this's channel name to name.
            channel_name: name,
            // 2. Set this's closed flag to false.
            closed_flag: Cell::new(false),
            origin: relevant_settings_object_origin(realm),
            storage_key: obtain_a_storage_key_for_non_storage_purposes(realm),
            queued_messages: RefCell::new(VecDeque::new()),
        });
        channel.event_target.set_owner(channel);
        channel.event_target.set_event_listener_list_changed_steps::<Self>();
        BROADCAST_CHANNELS.with_borrow_mut(|channels| channels.add(vm, channel));
        Ok(channel)
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-broadcastchannel-name
    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The name getter steps are to return this's channel name.
        Ok(self.channel_name.clone())
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#eligible-for-messaging
    fn is_eligible_for_messaging(&self) -> bool {
        // A BroadcastChannel object is said to be eligible for messaging when its relevant global object is either:
        let relevant_global_object = relevant_global_object(self);

        // * a Window object whose associated Document is fully active, or
        if let Some(window) = window_from_global_object(relevant_global_object) {
            return window.associated_document().is_fully_active();
        }

        // * a WorkerGlobalScope object whose closing flag is false and is not suspendable.
        // FIXME: Suspendable worker
        if let Some(worker_global_scope) = worker_global_scope_from_global_object(relevant_global_object) {
            return !worker_global_scope.is_closing();
        }

        false
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#broadcasting-to-other-browsing-contexts
    fn update_strong_reference_from_relevant_global_object(&self) {
        // While a BroadcastChannel object whose closed flag is false has an event listener registered for message or
        // messageerror events, there must be a strong reference from the BroadcastChannel object's relevant global object
        // to the BroadcastChannel object itself.
        let strongly_referenced = !self.closed_flag.get()
            && (self.event_target.has_event_listener("message")
                || self.event_target.has_event_listener("messageerror"));
        // SAFETY: The realm and the EventTarget are live.
        unsafe {
            web_html_broadcast_channel_set_strongly_referenced(
                self.realm.as_ptr().cast(),
                self.event_target.as_ptr(),
                strongly_referenced,
            );
        }
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-broadcastchannel-postmessage
    pub(crate) fn post_message(this: &Ref<Self>, cx: &mut CallContext, message: Value) -> WebIdlResult<()> {
        // 1. If this is not eligible for messaging, then return.
        if !this.is_eligible_for_messaging() {
            return Ok(());
        }

        // 2. If this's closed flag is true, then throw an "InvalidStateError" DOMException.
        if this.closed_flag.get() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"BroadcastChannel.postMessage() on a closed channel"),
            ));
        }

        // 3. Let serialized be StructuredSerialize(message). Rethrow any exceptions.
        // AD-HOC: This message reaches agents in other processes through the browser process — so a shared-memory-backed
        //         SharedArrayBuffer travels beside the record, by file descriptor, and every receiver references the same
        //         [[ArrayBufferData]] rather than a copy.
        let serialized = SerializedMessage::new();
        structured_serialize_with_shared_buffers(cx, message, serialized.record(), serialized.shared_buffers())?;

        // 4. Let sourceOrigin be this's relevant settings object's origin.
        let source_origin = this.origin.clone();

        // 5. Let sourceStorageKey be the result of running obtain a storage key for non-storage purposes with this's relevant settings object.
        let source_storage_key = this.storage_key.clone();

        let message = Rc::new(BroadcastChannelMessage {
            storage_key: source_storage_key,
            channel_name: this.channel_name.clone(),
            source_origin,
            serialized,
        });

        // Steps 6-9.
        Self::deliver_message_locally(cx.vm(), &message, Some(*this));

        // SAFETY: The realm is live, and the name and the message are lent for the call.
        unsafe {
            web_html_broadcast_channel_post_message_to_other_processes(
                this.realm.as_ptr().cast(),
                message.channel_name.raw_identity(),
                message.serialized.0.as_ptr(),
            );
        }

        Ok(())
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-broadcastchannel-postmessage
    pub(crate) fn deliver_message_locally(vm: &Vm, message: &Rc<BroadcastChannelMessage>, source: Option<Ref<Self>>) {
        // 6. Let destinations be a list of BroadcastChannel objects that match the following criteria:
        //    * They are eligible for messaging.
        //    * The result of running obtain a storage key for non-storage purposes with their relevant settings object equals sourceStorageKey.
        //    * Their channel name is this's channel name.
        // 7. Remove source from destinations.
        let destinations = MarkedVec::new(vm);
        BROADCAST_CHANNELS.with_borrow(|channels| {
            for channel in channels.channels.iter().filter_map(GcWeak::get) {
                if Some(channel) != source
                    && channel.is_eligible_for_messaging()
                    && channel.storage_key == message.storage_key
                    && channel.channel_name == message.channel_name
                {
                    destinations.push(channel);
                }
            }
        });

        // 8. Sort destinations such that all BroadcastChannel objects whose relevant agents are the same are sorted in creation order, oldest first.
        //    (This does not define a complete ordering. Within this constraint, user agents may sort the list in any implementation-defined manner.)
        // NB: The channels of the thread's agent are listed in creation order, so destinations already are.

        // 9. For each destination in destinations, queue a global task on the DOM manipulation task source given destination's relevant global object to perform the following steps:
        for destination in destinations.values().iter() {
            destination.queued_messages.borrow_mut().push_back(Rc::clone(message));
            queue_a_global_task(
                vm,
                destination.realm,
                TaskSource::DOMManipulation,
                *destination,
                |vm, destination| {
                    let Some(message) = destination.queued_messages.borrow_mut().pop_front() else {
                        return;
                    };
                    Self::message_task_steps(destination, vm, &message);
                },
            );
        }
    }

    fn message_task_steps(destination: Ref<Self>, vm: &Vm, message: &BroadcastChannelMessage) {
        // 1. If destination's closed flag is true, then abort these steps.
        if destination.closed_flag.get() {
            return;
        }

        // 2. Let targetRealm be destination's relevant realm.
        let target_realm = destination.realm;
        let cx = CallContext::new(vm, target_realm);

        // 3. Let data be StructuredDeserialize(serialized, targetRealm).
        let data = structured_deserialize_with_shared_buffers(
            &cx,
            message.serialized.received_record(),
            message.serialized.shared_buffers(),
        );
        let Ok(data) = data else {
            //    If this throws an exception, catch it, fire an event named messageerror at destination, using MessageEvent, with its
            //    origin initialized to sourceOrigin, and then abort these steps.
            let event = CppEvent::create_message_event_with_origin(
                target_realm,
                "messageerror",
                Value::NULL,
                &message.source_origin,
            );
            event.set_is_trusted(true);
            destination.event_target.dispatch_event(&event);
            return;
        };

        // 4. Fire an event named message at destination, using MessageEvent, with the data attribute initialized to data and
        //    its origin initialized to sourceOrigin.
        let event = CppEvent::create_message_event_with_origin(target_realm, "message", data, &message.source_origin);
        event.set_is_trusted(true);
        destination.event_target.dispatch_event(&event);
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-broadcastchannel-close
    pub(crate) fn close(&self, _: &mut CallContext) -> WebIdlResult<()> {
        // The close() method steps are to set this's closed flag to true.
        self.closed_flag.set(true);

        self.update_strong_reference_from_relevant_global_object();
        Ok(())
    }
}
