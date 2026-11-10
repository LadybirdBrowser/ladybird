/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! MessagePort, https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports

use core::any::Any;
use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;
use core::sync::atomic::{AtomicU64, Ordering};
use std::collections::{HashMap, VecDeque};
use std::sync::Arc;

use ak::Utf16String;
use libipc_rust::{Attachment, DecodeError, Decoder, EncodeError, Encoder};
use libjs_rust::api::{
    CellRoot, DeferGc, EmbedderCell, Gc, GcRefCell, GcWeak, MarkedVec, Object, Realm, Trace, Value, Vm,
    is_host_instance_of,
};

use crate::bindings::event_target::{CppEvent, CppEventTarget, GetTheParent, set_event_handler_attribute};
use crate::bindings::interface::{
    Interface, Ref, Wrappable, Wrapped, allocate, implementation_if_wrapper, with_temporary_execution_context,
};
use crate::bindings::promise::{TaskSource, queue_a_global_task};
use crate::bindings::structured_data::{TransferDataReader, TransferDataWriter, Transferable};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::html::port_channel::{PeerState, PortChannelEnd};
use crate::html::structured_serialize::transfer_data_holder::{
    RawAttachment, ReceivePayload, TransferPayload, append_attachments,
};
use crate::html::structured_serialize::transfer_record::{PortMessage, TransferRecord, TransferRecordRef};
use crate::html::structured_serialize::{structured_deserialize_with_transfer, structured_serialize_with_transfer};

/// The dictionary StructuredSerializeOptions of HTML/MessagePort.idl, whose members the bindings convert.
pub(crate) struct StructuredSerializeOptions<'vm> {
    pub(crate) transfer: MarkedVec<'vm, Gc<Object>>,
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct MessagePort {
    event_target: CppEventTarget,
    realm: Gc<Realm>,

    // https://html.spec.whatwg.org/multipage/web-messaging.html#message-event-target
    message_event_target: GcRefCell<CppEventTarget>,

    // The port this port is entangled with, if that port is in this agent. A port entangled over a transport has none.
    remote_port: Cell<Option<Ref<MessagePort>>>,

    // https://html.spec.whatwg.org/multipage/web-messaging.html#has-been-shipped
    has_been_shipped: Cell<bool>,
    has_ever_been_entangled: Cell<bool>,
    fail_next_transfer_for_testing: Cell<bool>,

    #[gc(untraced)]
    transport: RefCell<Option<MessagePortTransport>>,

    // https://html.spec.whatwg.org/multipage/web-messaging.html#port-message-queue
    // NB: Holds the messages whose tasks have not run yet. Each task takes the message at the front.
    port_message_queue: GcRefCell<VecDeque<TransferRecordRef>>,
    pending_outgoing_messages: GcRefCell<Vec<TransferRecordRef>>,
    message_task_generation: Cell<u64>,
    should_shutdown_on_enable: Cell<bool>,
    enabled: Cell<bool>,
    // The root that keeps the port alive while it has a transport and its port message queue is enabled, which
    // update_keep_alive() creates and clears.
    #[gc(untraced)]
    #[cfg_attr(
        ladybird_gc_lints,
        expect(
            root_in_traced_type,
            reason = "the port roots itself only while it has a transport and its port message queue is enabled, and clears the root when either goes"
        )
    )]
    keep_alive: RefCell<Option<CellRoot<EmbedderCell<Wrapped<MessagePort>>>>>,

    // https://html.spec.whatwg.org/multipage/structured-data.html#detached
    detached: Cell<bool>,
}

impl Wrappable for MessagePort {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }

    const FINALIZES: bool = true;

    fn finalize(port: Ref<Self>) {
        port.disentangle();
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for MessagePort {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

/// The ports of the thread's agent, which disentangle_ports_of_realm() visits: references that do not keep them alive,
/// which a collected port leaves empty. The empty ones are pruned once the list has grown to twice what was live after
/// the last pruning.
struct MessagePorts {
    ports: Vec<GcWeak<EmbedderCell<Wrapped<MessagePort>>>>,
    live_after_pruning: usize,
}

impl MessagePorts {
    const PORTS_BEFORE_FIRST_PRUNING: usize = 128;

    fn add(&mut self, vm: &Vm, port: Ref<MessagePort>) {
        if self.ports.len() >= (2 * self.live_after_pruning).max(Self::PORTS_BEFORE_FIRST_PRUNING) {
            self.ports.retain(|port| port.get().is_some());
            self.live_after_pruning = self.ports.len();
        }
        self.ports.push(GcWeak::new(vm.heap(), port));
    }
}

thread_local! {
    static MESSAGE_PORTS: RefCell<MessagePorts> = const {
        RefCell::new(MessagePorts {
            ports: Vec::new(),
            live_after_pruning: 0,
        })
    };
}

/// What the read hook of a port's transport runs with: the port, which the hook must not keep alive, and the VM.
struct ReadHookState {
    vm: NonNull<Vm>,
    port: GcWeak<EmbedderCell<Wrapped<MessagePort>>>,
}

impl ReadHookState {
    fn new(vm: &Vm, port: Ref<MessagePort>) -> Self {
        Self {
            vm: NonNull::from(vm),
            port: GcWeak::new(vm.heap(), port),
        }
    }

    /// The read hook, which runs on the owner thread when messages arrive.
    fn read_from_transport(&self) {
        let Some(port) = self.port.get() else {
            return;
        };
        // SAFETY: The VM outlives the transport, which the port, a cell of the VM's heap, owns.
        let vm = unsafe { self.vm.as_ref() };
        MessagePort::read_from_transport(port, vm);
    }
}

impl MessagePort {
    fn new(_: &DeferGc<'_>, realm: Gc<Realm>) -> Self {
        let event_target = CppEventTarget::new();
        Self {
            // Unless otherwise specified, it defaults to the MessagePort object itself.
            message_event_target: GcRefCell::new(event_target.clone()),
            event_target,
            realm,
            remote_port: Cell::new(None),
            has_been_shipped: Cell::new(false),
            has_ever_been_entangled: Cell::new(false),
            fail_next_transfer_for_testing: Cell::new(false),
            transport: RefCell::new(None),
            port_message_queue: GcRefCell::new(VecDeque::new()),
            pending_outgoing_messages: GcRefCell::new(Vec::new()),
            message_task_generation: Cell::new(0),
            should_shutdown_on_enable: Cell::new(false),
            enabled: Cell::new(false),
            keep_alive: RefCell::new(None),
            detached: Cell::new(false),
        }
    }

    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>) -> Ref<Self> {
        let port = allocate(vm, |defer_gc| Self::new(defer_gc, realm));
        port.event_target.set_owner(port);
        MESSAGE_PORTS.with_borrow_mut(|message_ports| message_ports.add(vm, port));
        port
    }

    pub(crate) fn event_target(&self) -> &CppEventTarget {
        &self.event_target
    }

    pub(crate) fn entangled_port(&self) -> Option<Ref<Self>> {
        self.remote_port.get()
    }

    pub(crate) fn pending_outgoing_message_count(&self) -> usize {
        self.pending_outgoing_messages.borrow().len()
    }

    /// How this port reaches the port it is entangled with, for tests.
    pub(crate) fn entanglement_kind(&self) -> &'static str {
        if self.remote_port.get().is_some() {
            return "same agent";
        }
        match &*self.transport.borrow() {
            Some(transport) if transport.is_channel() => "channel",
            Some(_) => "socket",
            None => "none",
        }
    }

    /// Whether the port's transport closed, as it does once the port sees that its peer went, for tests.
    pub(crate) fn transport_is_closed_for_testing(&self) -> bool {
        self.transport
            .borrow()
            .as_ref()
            .is_some_and(|transport| transport.is_closed.get())
    }

    pub(crate) fn fail_next_transfer_for_testing(&self) {
        self.fail_next_transfer_for_testing.set(true);
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#message-event-target
    pub(crate) fn set_message_event_target(&self, target: CppEventTarget) {
        self.message_event_target.replace(target);
    }

    /// Disentangles every port whose relevant realm is the realm, for a document that is destroyed.
    pub(crate) fn disentangle_ports_of_realm(vm: &Vm, realm: Gc<Realm>) {
        let ports = MarkedVec::new(vm);
        MESSAGE_PORTS.with_borrow(|message_ports| {
            for port in message_ports.ports.iter().filter_map(GcWeak::get) {
                if port.realm == realm {
                    ports.push(port);
                }
            }
        });
        for port in ports.values().iter() {
            port.disentangle();
        }
    }

    /// Disentangles every port of the thread's agent and forgets them, for an agent that goes away. A port roots itself
    /// while it can receive messages, and that root has to go before the heap does.
    pub(crate) fn shut_down_ports_of_agent(vm: &Vm) {
        let ports = MarkedVec::new(vm);
        MESSAGE_PORTS.with_borrow_mut(|message_ports| {
            for port in message_ports.ports.drain(..).filter_map(|port| port.get()) {
                ports.push(port);
            }
            message_ports.live_after_pruning = 0;
        });
        for port in ports.values().iter() {
            port.disentangle();
        }
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#ports-and-garbage-collection
    // When a MessagePort object o is entangled, user agents must either act as if o's entangled MessagePort object has a
    // strong reference to o, or as if o's relevant global object has a strong reference to o.
    // NB: Same-agent entanglement does the former, through the remote_port edges ordinary GC traversal visits.
    // AD-HOC: A port entangled over a transport has no edge to the port it's entangled with, so it isn't kept alive as if
    //         its relevant global object referenced it: It's kept alive only while its port message queue is enabled,
    //         since no message ever reaches script through a port whose queue isn't. So once script drops such a port,
    //         collecting it is unobservable, and disentangling it then closes its transport, which lets the port at the
    //         other end of the channel go too. Keeping every such port alive would keep the transport, and its IO thread,
    //         of every channel that script ever transferred and dropped without close() for the life of the process.
    fn update_keep_alive(this: Ref<Self>) {
        let mut keep_alive = this.keep_alive.borrow_mut();
        if this.transport.borrow().is_some() && this.enabled.get() {
            if keep_alive.is_none() {
                *keep_alive = Some(CellRoot::new(this));
            }
            return;
        }
        *keep_alive = None;
    }

    fn is_entangled(&self) -> bool {
        self.remote_port.get().is_some() || self.transport.borrow().is_some()
    }

    fn transfer_list_contains(transfer: &[Gc<Object>], port: Ref<Self>) -> bool {
        transfer
            .iter()
            .any(|object| implementation_if_wrapper::<Self>(*object) == Some(port))
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-postmessage
    pub(crate) fn post_message(
        this: &Ref<Self>,
        cx: &mut CallContext,
        message: Value,
        transfer: MarkedVec<'_, Gc<Object>>,
    ) -> WebIdlResult<()> {
        // 1. Let targetPort be the port with which this is entangled, if any; otherwise let it be null.
        let target_port = this.remote_port.get();

        // 2. Let options be «[ "transfer" → transfer ]».
        // 3. Run the message port post message steps providing this, targetPort, message and options.
        Self::message_port_post_message_steps(*this, cx, target_port, message, &transfer.values())
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-postmessage-options
    pub(crate) fn post_message_with_options(
        this: &Ref<Self>,
        cx: &mut CallContext,
        message: Value,
        options: StructuredSerializeOptions<'_>,
    ) -> WebIdlResult<()> {
        // 1. Let targetPort be the port with which this is entangled, if any; otherwise let it be null.
        let target_port = this.remote_port.get();

        // 2. Run the message port post message steps providing this, targetPort, message and options.
        Self::message_port_post_message_steps(*this, cx, target_port, message, &options.transfer.values())
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#message-port-post-message-steps
    pub(crate) fn message_port_post_message_steps(
        this: Ref<Self>,
        cx: &CallContext,
        target_port: Option<Ref<Self>>,
        message: Value,
        transfer: &[Gc<Object>],
    ) -> WebIdlResult<()> {
        let vm = cx.vm();

        // 1. Let transfer be options["transfer"].

        // 2. If transfer contains sourcePort, then throw a "DataCloneError" DOMException.
        if Self::transfer_list_contains(transfer, this) {
            return Err(Exception::Dom(
                "DataCloneError",
                Utf16String::from_ascii(b"Cannot transfer a MessagePort to itself"),
            ));
        }

        // 3. Let doomed be false.
        let mut doomed = false;

        // 4. If targetPort is not null and transfer contains targetPort, then set doomed to true and optionally report to a developer console that the target port was posted to itself, causing the communication channel to be lost.
        if let Some(target_port) = target_port
            && Self::transfer_list_contains(transfer, target_port)
        {
            doomed = true;
            // FIXME: Report to a developer console that the target port was posted to itself, causing the communication
            //        channel to be lost.
        }

        // 5. Let serializeWithTransferResult be StructuredSerializeWithTransfer(message, transfer). Rethrow any exceptions.
        let serialize_with_transfer_result = structured_serialize_with_transfer(cx, message, transfer)?;

        // 6. If targetPort is null, or if doomed is true, then return.
        if doomed {
            TransferRecord::discard(serialize_with_transfer_result);
            return Ok(());
        }
        let Some(target_port) = target_port else {
            // AD-HOC: A port entangled over a transport has no targetPort in this agent, so it sends the message for the
            //         other end to add the task. A port that has never been entangled keeps the message until it is.
            if this.transport.borrow().is_some() {
                Self::post_port_message(this, vm, serialize_with_transfer_result);
            } else if !this.detached.get() && !this.has_been_shipped.get() && !this.has_ever_been_entangled.get() {
                this.pending_outgoing_messages
                    .borrow_mut(vm)
                    .push(serialize_with_transfer_result);
            } else {
                TransferRecord::discard(serialize_with_transfer_result);
            }
            return Ok(());
        };

        // 7. Add a task that runs the following steps to the port message queue of targetPort:
        Self::add_message_task(target_port, vm, serialize_with_transfer_result);

        Ok(())
    }

    /// Adds the task that fires the message to the port message queue of the port. The event loop runs it only once the
    /// queue is enabled.
    fn add_message_task(this: Ref<Self>, vm: &Vm, serialize_with_transfer_result: TransferRecordRef) {
        this.port_message_queue
            .borrow_mut(vm)
            .push_back(serialize_with_transfer_result);
        if this.enabled.get() {
            Self::queue_message_task(this, vm);
        }
    }

    /// Queues the task for the message at the front of the port message queue, which fires nothing if the queue was
    /// emptied since.
    fn queue_message_task(this: Ref<Self>, vm: &Vm) {
        let message_task_generation = this.message_task_generation.get();
        queue_a_global_task(
            vm,
            this.realm.global_object(),
            TaskSource::PostedMessage,
            (this, message_task_generation),
            |vm, (this, message_task_generation)| {
                if message_task_generation != this.message_task_generation.get() {
                    return;
                }
                let Some(serialize_with_transfer_result) = this.port_message_queue.borrow_mut(vm).pop_front() else {
                    return;
                };
                Self::post_message_task_steps(this, vm, serialize_with_transfer_result);
            },
        );
    }

    fn flush_pending_outgoing_messages(this: Ref<Self>, vm: &Vm) {
        let remote_port = this.remote_port.get();
        if remote_port.is_none()
            && !this
                .transport
                .borrow()
                .as_ref()
                .is_some_and(MessagePortTransport::is_open)
        {
            return;
        }
        let pending = MarkedVec::new(vm);
        for record in this.pending_outgoing_messages.borrow_mut(vm).drain(..) {
            pending.push(record);
        }
        for &record in pending.values().iter() {
            if let Some(remote_port) = remote_port {
                Self::add_message_task(remote_port, vm, record);
            } else {
                Self::post_port_message(this, vm, record);
            }
        }
    }

    /// Reads the messages that have arrived on the port's transport, and notes whether the other end went away.
    fn drain_transport<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, TransferRecordRef> {
        let messages = MarkedVec::new(vm);
        let transport = self.transport.borrow();
        let Some(transport) = &*transport else {
            return messages;
        };

        let should_shutdown = transport.read_as_many_messages_as_possible_without_blocking(vm, &messages);

        // The peer is gone, so nothing more arrives; the pending messages are delivered once the port is enabled.
        if should_shutdown == ShouldShutdown::Yes {
            self.should_shutdown_on_enable.set(true);
            transport.close();
        }
        messages
    }

    fn post_port_message(this: Ref<Self>, vm: &Vm, serialize_with_transfer_result: TransferRecordRef) {
        let sent = {
            let transport = this.transport.borrow();
            let Some(transport) = &*transport else {
                TransferRecord::discard(serialize_with_transfer_result);
                return;
            };
            if !transport.is_open() {
                TransferRecord::discard(serialize_with_transfer_result);
                return;
            }
            transport.send(vm, serialize_with_transfer_result)
        };
        if !sent {
            this.disentangle();
        }
    }

    fn read_from_transport(this: Ref<Self>, vm: &Vm) {
        for &message in this.drain_transport(vm).values().iter() {
            Self::add_message_task(this, vm, message);
        }

        if this.enabled.get() && this.should_shutdown_on_enable.replace(false) {
            queue_a_global_task(
                vm,
                this.realm.global_object(),
                TaskSource::PostedMessage,
                this,
                |_, this| {
                    this.close_steps();
                },
            );
        }
    }

    fn post_message_task_steps(this: Ref<Self>, vm: &Vm, serialize_with_transfer_result: TransferRecordRef) {
        // 1. Let finalTargetPort be the MessagePort in whose port message queue the task now finds itself.
        // NOTE: This can be different from targetPort, if targetPort itself was transferred and thus all its tasks moved along with it.
        let final_target_port = this;

        // 2. Let messageEventTarget be finalTargetPort's message event target.
        let message_event_target = final_target_port.message_event_target.borrow().clone();

        // 3. Let targetRealm be finalTargetPort's relevant realm.
        let target_realm = final_target_port.realm;
        let cx = CallContext::new(vm, target_realm);

        // 4. Let deserializeRecord be StructuredDeserializeWithTransfer(serializeWithTransferResult, targetRealm).
        // NB: The execution context must not stay on the stack while the event is dispatched, so that a microtask
        //     checkpoint can be performed after each listener.
        let deserialize_record = with_temporary_execution_context(target_realm, || {
            structured_deserialize_with_transfer(&cx, serialize_with_transfer_result)
        });
        TransferRecord::discard(serialize_with_transfer_result);
        let Ok(deserialize_record) = deserialize_record else {
            // If this throws an exception, catch it, fire an event named messageerror at messageEventTarget, using MessageEvent, and then return.
            let event = CppEvent::create_message_event(target_realm, "messageerror", Value::NULL, &[], None, None);
            event.set_is_trusted(true);
            message_event_target.dispatch_event(&event);
            return;
        };

        // 5. Let messageClone be deserializeRecord.[[Deserialized]].
        let message_clone = deserialize_record.deserialized;

        // 6. Let newPorts be a new frozen array consisting of all MessagePort objects in deserializeRecord.[[TransferredValues]], if any, maintaining their relative order.
        // FIXME: Use a FrozenArray
        let new_ports = MarkedVec::new(vm);
        for value in deserialize_record.transferred_values.values().iter() {
            if is_host_instance_of(value, Self::wrapper_class()) {
                new_ports.push(*value);
            }
        }

        // 7. Fire an event named message at messageEventTarget, using MessageEvent, with the data attribute initialized to messageClone and the ports attribute initialized to newPorts.
        let event =
            CppEvent::create_message_event(target_realm, "message", message_clone, &new_ports.values(), None, None);
        event.set_is_trusted(true);
        message_event_target.dispatch_event(&event);
    }

    pub(crate) fn enable(this: Ref<Self>, vm: &Vm) {
        if this.enabled.replace(true) {
            return;
        }
        Self::update_keep_alive(this);

        let queued_messages = this.port_message_queue.borrow().len();
        for _ in 0..queued_messages {
            Self::queue_message_task(this, vm);
        }
        Self::read_from_transport(this, vm);
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-start
    pub(crate) fn start(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<()> {
        // The start() method steps are to enable this's port message queue, if it is not already enabled.
        Self::enable(*this, cx.vm());
        Ok(())
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-close
    pub(crate) fn close(&self, _: &mut CallContext) -> WebIdlResult<()> {
        self.close_steps();
        Ok(())
    }

    /// The close() method steps, which a port whose peer went away runs once it is enabled as well.
    pub(crate) fn close_steps(&self) {
        // 1. Set this's [[Detached]] internal slot value to true.
        self.detached.set(true);

        // 2. If this is entangled, disentangle it.
        if self.is_entangled() {
            self.disentangle();
        }

        self.clear_message_queues();
    }

    /// Empties the port message queue, so that the message tasks already queued fire nothing.
    pub(crate) fn discard_pending_messages(&self) {
        for &record in self.port_message_queue.borrow().iter() {
            TransferRecord::discard(record);
        }
        self.port_message_queue.replace(VecDeque::new());
        self.message_task_generation.set(self.message_task_generation.get() + 1);
    }

    /// Drops every message the port holds, both ways, and forgets that its peer went away.
    fn clear_message_queues(&self) {
        self.discard_pending_messages();
        for &record in self.pending_outgoing_messages.borrow().iter() {
            TransferRecord::discard(record);
        }
        self.pending_outgoing_messages.replace(Vec::new());
        self.should_shutdown_on_enable.set(false);
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#disentangle
    pub(crate) fn disentangle(&self) {
        // 1. Let otherPort be the MessagePort which initiatorPort was entangled with.
        // 2. Assert: otherPort exists.
        // NB: A port is also disentangled when it is collected, when its document is destroyed and when a send fails,
        //     whether it is entangled or not. A port entangled over a transport has no otherPort in this agent.

        // 3. Disentangle initiatorPort and otherPort, so that they are no longer entangled or associated with each other.
        if let Some(other_port) = self.remote_port.take() {
            other_port.remote_port.set(None);
        }
        let transport = self.transport.borrow_mut().take();
        if let Some(transport) = transport {
            transport.close_after_sending_all_pending_messages();
        }
        self.keep_alive.replace(None);

        self.clear_message_queues();
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#entangle
    pub(crate) fn entangle(vm: &Vm, this: Ref<Self>, remote_port: Ref<Self>) {
        // 1. If one of the ports is already entangled, then disentangle it and the port that it was entangled with.
        if this.is_entangled() {
            this.disentangle();
        }
        if remote_port.is_entangled() {
            remote_port.disentangle();
        }

        // 2. Associate the two ports to be entangled, so that they form the two parts of a new channel.
        //    (There is no MessageChannel object that represents this channel.)
        remote_port.remote_port.set(Some(this));
        this.remote_port.set(Some(remote_port));
        remote_port.has_ever_been_entangled.set(true);
        this.has_ever_been_entangled.set(true);

        Self::flush_pending_outgoing_messages(this, vm);
        Self::flush_pending_outgoing_messages(remote_port, vm);
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#handler-messageeventtarget-onmessage
    pub(crate) fn set_onmessage(this: &Ref<Self>, cx: &mut CallContext, value: Value) {
        let port: &Self = this;
        set_event_handler_attribute(port, "message", value);

        // https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports:handler-messageeventtarget-onmessage
        // The first time a MessagePort object's onmessage IDL attribute is set, the port's port message queue must be enabled,
        // as if the start() method had been called.
        Self::enable(*this, cx.vm());
    }
}

impl Transferable for MessagePort {
    fn detached(&self) -> &Cell<bool> {
        &self.detached
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports:transfer-steps
    fn transfer_steps(
        this: &Ref<Self>,
        cx: &mut CallContext,
        data_holder: &mut TransferDataWriter,
    ) -> WebIdlResult<()> {
        let this = *this;
        let vm = cx.vm();

        if this.is_entangled() && this.fail_next_transfer_for_testing.replace(false) {
            return Err(Exception::Dom(
                "DataCloneError",
                Utf16String::from_ascii(b"Forced MessagePort transfer failure"),
            ));
        }

        // 1. Set value's has been shipped flag to true.
        this.has_been_shipped.set(true);

        // NB: A port entangled over a transport hands its transport over. Stop the transport's IO thread before draining
        //     it below, so that no message arrives between the drain and the handle going to the new owner. A channel
        //     keeps the messages that wait for its end, for whoever owns that next.
        let released_transport = this
            .transport
            .borrow()
            .as_ref()
            .and_then(MessagePortTransport::release_for_transfer);

        // 2. Set dataHolder.[[PortMessageQueue]] to value's port message queue.
        // NB: Drain the transport into the queue first, so that the messages it received and a pending shutdown move
        //     with the port instead of being left behind on the old transport.
        let received_messages = this.drain_transport(vm);
        let mut port_message_queue = this.port_message_queue.borrow_mut(vm);
        port_message_queue.extend(received_messages.values().iter().copied());
        data_holder.write_records(cx, port_message_queue.make_contiguous());
        drop(port_message_queue);
        data_holder.write_records(cx, &this.pending_outgoing_messages.borrow());
        data_holder.write_bool(this.should_shutdown_on_enable.get());

        // 3. If value is entangled with another port remotePort:
        // NB: Ports in the same agent need no transport until one of them is shipped. Keep remotePort in this agent, and
        //     entangle it over a new transport with the port that the transfer-receiving steps create.
        let (remote_port, remote_port_handle) = match this.remote_port.get() {
            Some(remote_port) => {
                //    1. Set remotePort's has been shipped flag to true.
                remote_port.has_been_shipped.set(true);

                let (transport, transferred) = MessagePortTransport::create_channel();
                (Some((remote_port, transport)), Some(transferred))
            }
            None => (None, released_transport),
        };

        //    2. Set dataHolder.[[RemotePort]] to remotePort.
        // 4. Otherwise, set dataHolder.[[RemotePort]] to null.
        data_holder.write_bool(remote_port_handle.is_some());
        if let Some(remote_port_handle) = remote_port_handle {
            data_holder.write_transport(remote_port_handle);
        }

        if let Some((remote_port, mut transport)) = remote_port {
            remote_port.remote_port.set(None);
            this.remote_port.set(None);
            transport.set_up_read_hook(ReadHookState::new(vm, remote_port), ReadHookState::read_from_transport);
            *remote_port.transport.borrow_mut() = Some(transport);
            Self::update_keep_alive(remote_port);
            Self::flush_pending_outgoing_messages(remote_port, vm);
        }

        // NB: The port's transport and messages went with it.
        *this.transport.borrow_mut() = None;
        Self::update_keep_alive(this);
        this.clear_message_queues();

        Ok(())
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports:transfer-receiving-steps
    fn transfer_receiving_steps(cx: &mut CallContext, data_holder: &mut TransferDataReader) -> WebIdlResult<Ref<Self>> {
        let vm = cx.vm();
        let value = Self::create(vm, cx.realm());

        // 1. Set value's has been shipped flag to true.
        value.has_been_shipped.set(true);

        // 2. Move all the tasks that are to fire message events in dataHolder.[[PortMessageQueue]] to the port message queue of value,
        //    if any, leaving value's port message queue in its initial disabled state, and, if value's relevant global object is a Window,
        //    associating the moved tasks with value's relevant global object's associated Document.
        let queued = data_holder.read_records(vm)?;
        value
            .port_message_queue
            .borrow_mut(vm)
            .extend(queued.values().iter().copied());
        let pending = data_holder.read_records(vm)?;
        value
            .pending_outgoing_messages
            .borrow_mut(vm)
            .extend(pending.values().iter().copied());
        value.should_shutdown_on_enable.set(data_holder.read_bool()?);

        // 3. If dataHolder.[[RemotePort]] is not null, then entangle dataHolder.[[RemotePort]] and value.
        //     (This will disentangle dataHolder.[[RemotePort]] from the original port that was transferred.)
        if data_holder.read_bool()? {
            let mut transport = data_holder.read_transport()?.create_transport();
            value.has_ever_been_entangled.set(true);

            transport.set_up_read_hook(ReadHookState::new(vm, value), ReadHookState::read_from_transport);
            *value.transport.borrow_mut() = Some(transport);
            Self::update_keep_alive(value);

            Self::flush_pending_outgoing_messages(value, vm);
        }

        Ok(value)
    }
}

unsafe extern "C" {
    fn web_bindings_transport_handle_take(handle: *mut c_void, context: *mut c_void, receive: ReceivePayload);
    fn web_bindings_transport_handle_create_transport(
        bytes: ak::Span<'_, u8>,
        attachments: *const RawAttachment,
        count: usize,
    ) -> *mut c_void;
    fn web_bindings_message_port_transport_create_paired(out_remote_handle: &mut *mut c_void) -> *mut c_void;
    fn web_bindings_message_port_current_event_loop() -> *mut c_void;
    fn web_bindings_message_port_release_event_loop(event_loop: *mut c_void);
    fn web_bindings_message_port_post_to_event_loop(
        event_loop: *mut c_void,
        steps: unsafe extern "C" fn(u64),
        argument: u64,
    );
    fn web_bindings_message_port_transport_destroy(transport: *mut c_void);
    fn web_bindings_message_port_transport_release_for_transfer(transport: *mut c_void) -> *mut c_void;
    fn web_bindings_message_port_transport_set_up_read_hook(
        transport: *mut c_void,
        state: *mut c_void,
        steps: unsafe extern "C" fn(*mut c_void),
    );
    fn web_bindings_message_port_transport_is_open(transport: *mut c_void) -> bool;
    fn web_bindings_message_port_transport_close(transport: *mut c_void);
    fn web_bindings_message_port_transport_close_after_sending_all_pending_messages(transport: *mut c_void);
    fn web_bindings_message_port_transport_send(
        transport: *mut c_void,
        bytes: ak::Span<'_, u8>,
        attachments: *const RawAttachment,
        count: usize,
    ) -> bool;
    fn web_bindings_message_port_transport_read_messages(
        transport: *mut c_void,
        records: *mut c_void,
        append: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>, *const RawAttachment, usize) -> bool,
    ) -> bool;
}

/// The rights of one end of an IPC transport, which this owns until a data holder carries them to another agent, or a
/// transport takes them. The rest is the start of a message that the last owner read off the transport: IPC::TransportHandle.
pub(crate) struct TransportHandle {
    attachments: Vec<Attachment>,
    unprocessed_bytes: Vec<u8>,
    unprocessed_attachments: Vec<Attachment>,
}

impl TransportHandle {
    /// # Safety
    ///
    /// `handle` must be a handle that LibWeb hands over.
    pub(crate) unsafe fn from_raw(handle: *mut c_void) -> Self {
        let mut payload = None;
        // SAFETY: The call takes the native handle and fills the lent payload slot.
        unsafe {
            web_bindings_transport_handle_take(handle, (&raw mut payload).cast(), TransferPayload::receive);
        };
        let payload: TransferPayload = payload.expect("C++ hands over the handle's payload");
        let mut decoder = Decoder::new(&payload.bytes, payload.attachments);
        let handle = Self::decode(&mut decoder).expect("a native handle has the transport's rights");
        assert!(decoder.is_at_end() && decoder.remaining_attachments() == 0);
        handle
    }

    pub(crate) fn encode_into(self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        append_attachments(encoder, self.attachments);
        encoder.encode(self.unprocessed_bytes.as_slice())?;
        encoder.encode_size(self.unprocessed_attachments.len())?;
        append_attachments(encoder, self.unprocessed_attachments);
        Ok(())
    }

    pub(crate) fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let count = if cfg!(target_vendor = "apple") { 2 } else { 1 };
        if decoder.remaining_attachments() < count {
            return Err(DecodeError::MissingAttachment);
        }
        let mut attachments = Vec::with_capacity(count);
        for index in 0..count {
            let attachment = decoder.take_attachment()?;
            #[cfg(target_vendor = "apple")]
            let attachment = {
                use libipc_rust::mach::{
                    MACH_MSG_TYPE_MOVE_RECEIVE, MACH_MSG_TYPE_MOVE_SEND, MACH_PORT_DEAD, MACH_PORT_NULL,
                };
                let (port, disposition) = attachment.into_mach_port();
                let expected = if index == 0 {
                    MACH_MSG_TYPE_MOVE_RECEIVE
                } else {
                    MACH_MSG_TYPE_MOVE_SEND
                };
                if disposition != expected
                    || port.name() == MACH_PORT_NULL
                    || (index == 0 && port.name() == MACH_PORT_DEAD)
                {
                    return Err(DecodeError::InvalidValue("invalid transport handle right"));
                }
                Attachment::from_mach_port(port, disposition)
            };
            #[cfg(not(target_vendor = "apple"))]
            let _ = index;
            attachments.push(attachment);
        }
        let unprocessed_bytes = decoder.decode::<Vec<u8>>()?;
        let unprocessed_count = decoder.decode_size()?;
        if decoder.remaining_attachments() < unprocessed_count {
            return Err(DecodeError::MissingAttachment);
        }
        let unprocessed_attachments = (0..unprocessed_count)
            .map(|_| decoder.take_attachment())
            .collect::<Result<_, _>>()?;
        Ok(Self {
            attachments,
            unprocessed_bytes,
            unprocessed_attachments,
        })
    }

    /// The IPC transport of the end that the handle holds the rights of, which it gives up.
    fn create_socket(self) -> SocketTransport {
        let mut encoder = Encoder::new();
        self.encode_into(&mut encoder).expect("a transport handle encodes");
        let (bytes, attachments) = encoder.finish();
        TransferPayload::new(bytes, attachments).with_raw(|bytes, attachments| {
            // SAFETY: The call takes every right, and LibWeb hands over the transport.
            unsafe {
                SocketTransport::from_raw(web_bindings_transport_handle_create_transport(
                    bytes,
                    attachments.as_ptr(),
                    attachments.len(),
                ))
            }
        })
    }
}

/// What a data holder carries for a port to reach its peer with.
pub(crate) enum TransferredTransport {
    Socket(TransportHandle),
    Channel(PortChannelEnd),
}

impl TransferredTransport {
    /// The transport of the end, which it gives up.
    pub(crate) fn create_transport(self) -> MessagePortTransport {
        MessagePortTransport::new(match self {
            Self::Socket(handle) => Connection::Socket(handle.create_socket()),
            Self::Channel(end) => Connection::Channel(end),
        })
    }
}

/// Whether the peer of a transport is gone, so that nothing more arrives: IPC::Transport::ShouldShutdown.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum ShouldShutdown {
    No,
    Yes,
}

/// The read hook of a transport: its steps, and the state they run with.
struct ReadHook<S> {
    state: S,
    steps: fn(&S),
}

/// The hook of a transport as its IPC transport or the wake of its channel calls it, with what the hook is called with.
#[derive(Clone, Copy)]
struct RawReadHook {
    hook: *mut c_void,
    run: unsafe extern "C" fn(*mut c_void),
}

thread_local! {
    /// The read hooks of the transports of this thread that are on a channel, by the id that their wakes carry. A wake
    /// can outlive its transport, and the agent of its thread, so this is what it goes through to reach either.
    static CHANNEL_READ_HOOKS: RefCell<HashMap<u64, RawReadHook>> = RefCell::new(HashMap::new());
}

/// Runs on the owner thread of the transport, from its event loop.
unsafe extern "C" fn run_channel_read_hook(id: u64) {
    let Some(hook) = CHANNEL_READ_HOOKS.with_borrow(|hooks| hooks.get(&id).copied()) else {
        return;
    };
    // SAFETY: The transport removes the hook from the list before it destroys the hook.
    unsafe { (hook.run)(hook.hook) };
}

/// The event loop of a thread as any thread may reach it: Core::WeakEventLoopReference.
struct EventLoopReference(NonNull<c_void>);

// SAFETY: The reference counts atomically, and it finds out under a lock whether its event loop is still there.
unsafe impl Send for EventLoopReference {}
unsafe impl Sync for EventLoopReference {}

impl EventLoopReference {
    fn current() -> Self {
        // SAFETY: The call hands over a reference.
        let reference = unsafe { web_bindings_message_port_current_event_loop() };
        Self(NonNull::new(reference).expect("the thread of a port has an event loop"))
    }

    /// Has the event loop run the read hook with the id, if the event loop is still there.
    fn post_read_hook(&self, id: u64) {
        // SAFETY: This holds a reference.
        unsafe { web_bindings_message_port_post_to_event_loop(self.0.as_ptr(), run_channel_read_hook, id) };
    }
}

impl Drop for EventLoopReference {
    fn drop(&mut self) {
        // SAFETY: This holds a reference.
        unsafe { web_bindings_message_port_release_event_loop(self.0.as_ptr()) };
    }
}

/// An IPC::Transport that this owns.
struct SocketTransport {
    raw: NonNull<c_void>,
}

impl SocketTransport {
    /// # Safety
    ///
    /// `transport` must be a transport that LibWeb hands over.
    unsafe fn from_raw(transport: *mut c_void) -> Self {
        Self {
            raw: NonNull::new(transport).expect("LibWeb hands over a transport"),
        }
    }

    fn set_up_read_hook(&self, hook: RawReadHook) {
        // SAFETY: This owns the transport, and the hook outlives it.
        unsafe {
            web_bindings_message_port_transport_set_up_read_hook(self.raw.as_ptr(), hook.hook, hook.run);
        }
    }

    /// Sends the message to the peer. False if the transport failed, which LibWeb logs.
    fn send(&self, message: PortMessage) -> bool {
        let mut encoder = Encoder::new();
        message.encode_into(&mut encoder).expect("a message record encodes");
        let (bytes, attachments) = encoder.finish();
        TransferPayload::new(bytes, attachments).with_raw(|bytes, attachments| {
            // SAFETY: This owns the transport. The call copies the bytes and takes every native attachment.
            unsafe {
                web_bindings_message_port_transport_send(
                    self.raw.as_ptr(),
                    bytes,
                    attachments.as_ptr(),
                    attachments.len(),
                )
            }
        })
    }

    fn read_as_many_messages_as_possible_without_blocking(
        &self,
        vm: &Vm,
        records: &MarkedVec<'_, TransferRecordRef>,
    ) -> ShouldShutdown {
        unsafe extern "C" fn append(
            context: *mut c_void,
            bytes: ak::Span<'_, u8>,
            attachments: *const RawAttachment,
            count: usize,
        ) -> bool {
            // SAFETY: The read lends its VM, rooted result list and bytes, and hands over every attachment.
            let (vm, records) = unsafe { &*context.cast::<(&Vm, &MarkedVec<'_, TransferRecordRef>)>() };
            let attachments = unsafe { RawAttachment::take_all(ak::slice_from_raw_parts(attachments, count)) };
            let mut decoder = Decoder::new(bytes.as_slice(), attachments);
            match TransferRecord::decode(vm, &mut decoder) {
                Ok(record) => {
                    records.push(record);
                    true
                }
                Err(error) => {
                    eprintln!("Failed to decode a MessagePort message: {error:?}");
                    false
                }
            }
        }
        let mut context = (vm, records);
        // SAFETY: This owns the transport, and lends the records for the call.
        let should_shutdown = unsafe {
            web_bindings_message_port_transport_read_messages(self.raw.as_ptr(), (&raw mut context).cast(), append)
        };
        if should_shutdown {
            ShouldShutdown::Yes
        } else {
            ShouldShutdown::No
        }
    }
}

impl Drop for SocketTransport {
    fn drop(&mut self) {
        // SAFETY: This owns the transport. Its read hook, which the state outlives, goes with it.
        unsafe { web_bindings_message_port_transport_destroy(self.raw.as_ptr()) };
    }
}

/// What a transport reaches its peer over: a channel if the peer is in this process, as far as anybody knows, and
/// otherwise an IPC transport.
enum Connection {
    Socket(SocketTransport),
    Channel(PortChannelEnd),
    /// The channel end went to a new owner, along with the port.
    Released,
}

/// The transport of one end of the channel between two MessagePorts entangled across agents, whose messages are the
/// records of structured serializations with transfer. Its read hook runs on the owner thread when they arrive.
pub(crate) struct MessagePortTransport {
    connection: RefCell<Connection>,
    /// What this took off the channel as it changed over to a socket, which comes first.
    channel_leftovers: RefCell<VecDeque<PortMessage>>,
    is_closed: Cell<bool>,
    raw_read_hook: Option<RawReadHook>,
    channel_read_hook_id: Option<u64>,
    /// The read hook, which the transport is lent until it is destroyed: this goes after the connection.
    read_hook: Option<Box<dyn Any>>,
}

impl MessagePortTransport {
    fn new(connection: Connection) -> Self {
        Self {
            connection: RefCell::new(connection),
            channel_leftovers: RefCell::new(VecDeque::new()),
            is_closed: Cell::new(false),
            raw_read_hook: None,
            channel_read_hook_id: None,
            read_hook: None,
        }
    }

    /// The transport of one end of a new channel in this process, and the other end.
    pub(crate) fn create_channel() -> (Self, TransferredTransport) {
        let (end, other_end) = PortChannelEnd::create_pair();
        (
            Self::new(Connection::Channel(end)),
            TransferredTransport::Channel(other_end),
        )
    }

    /// IPC::Transport::create_paired(), for both ends to go elsewhere.
    pub(crate) fn create_paired_handles() -> (TransportHandle, TransportHandle) {
        let mut remote_handle = core::ptr::null_mut();
        // SAFETY: LibWeb hands over a new transport and the handle.
        unsafe {
            let transport =
                SocketTransport::from_raw(web_bindings_message_port_transport_create_paired(&mut remote_handle));
            let local_handle = TransportHandle::from_raw(web_bindings_message_port_transport_release_for_transfer(
                transport.raw.as_ptr(),
            ));
            (local_handle, TransportHandle::from_raw(remote_handle))
        }
    }

    pub(crate) fn is_channel(&self) -> bool {
        matches!(&*self.connection.borrow(), Connection::Channel(_))
    }

    /// Takes this end back, for a data holder to carry to another agent. The IO thread of a socket stops first, so that
    /// what the peer sends from then on arrives at the new owner, while what it sent before can still be read. A transport
    /// that closed as its peer went has nothing to hand over.
    pub(crate) fn release_for_transfer(&self) -> Option<TransferredTransport> {
        if self.is_closed.get() {
            return None;
        }
        let mut connection = self.connection.borrow_mut();
        if let Connection::Socket(socket) = &*connection {
            // SAFETY: This owns the transport, and LibWeb hands over the handle.
            return Some(TransferredTransport::Socket(unsafe {
                TransportHandle::from_raw(web_bindings_message_port_transport_release_for_transfer(
                    socket.raw.as_ptr(),
                ))
            }));
        }
        let Connection::Channel(end) = core::mem::replace(&mut *connection, Connection::Released) else {
            unreachable!("a transport is released once");
        };
        end.set_wake(None);
        Some(TransferredTransport::Channel(end))
    }

    /// Runs the steps with the state on the owner thread whenever messages arrive, for as long as the transport lives.
    pub(crate) fn set_up_read_hook<S: 'static>(&mut self, state: S, steps: fn(&S)) {
        unsafe extern "C" fn run<S>(hook: *mut c_void) {
            // SAFETY: The transport calls its hook with the one that set_up_read_hook() lent it, which outlives the
            //         transport.
            let hook = unsafe { &*hook.cast::<ReadHook<S>>() };
            (hook.steps)(&hook.state);
        }
        let hook = Box::new(ReadHook { state, steps });
        let raw_hook = RawReadHook {
            hook: (&raw const *hook).cast_mut().cast(),
            run: run::<S>,
        };
        self.raw_read_hook = Some(raw_hook);
        self.read_hook = Some(hook);
        match &*self.connection.borrow() {
            Connection::Socket(socket) => socket.set_up_read_hook(raw_hook),
            Connection::Channel(end) => {
                static NEXT_ID: AtomicU64 = AtomicU64::new(1);
                let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
                CHANNEL_READ_HOOKS.with_borrow_mut(|hooks| hooks.insert(id, raw_hook));
                self.channel_read_hook_id = Some(id);
                let event_loop = EventLoopReference::current();
                end.set_wake(Some(Arc::new(move || event_loop.post_read_hook(id))));
            }
            Connection::Released => unreachable!("a released transport has no owner to read"),
        }
    }

    /// Changes over to the socket of the handle, keeping the messages that wait on the channel for the next read, and has
    /// the read hook run for them.
    fn change_over_to_socket(&self, handle: TransportHandle, messages: VecDeque<PortMessage>) {
        let socket = handle.create_socket();
        if let Some(hook) = self.raw_read_hook {
            socket.set_up_read_hook(hook);
        }
        self.channel_leftovers.borrow_mut().extend(messages);
        *self.connection.borrow_mut() = Connection::Socket(socket);
        if let Some(id) = self.channel_read_hook_id
            && !self.channel_leftovers.borrow().is_empty()
        {
            EventLoopReference::current().post_read_hook(id);
        }
    }

    pub(crate) fn is_open(&self) -> bool {
        if self.is_closed.get() {
            return false;
        }
        match &*self.connection.borrow() {
            // SAFETY: This owns the transport.
            Connection::Socket(socket) => unsafe { web_bindings_message_port_transport_is_open(socket.raw.as_ptr()) },
            Connection::Channel(end) => end.is_open(),
            Connection::Released => false,
        }
    }

    pub(crate) fn close(&self) {
        self.is_closed.set(true);
        if let Connection::Socket(socket) = &*self.connection.borrow() {
            // SAFETY: This owns the transport.
            unsafe { web_bindings_message_port_transport_close(socket.raw.as_ptr()) };
        }
    }

    pub(crate) fn close_after_sending_all_pending_messages(&self) {
        if let Connection::Socket(socket) = &*self.connection.borrow() {
            // SAFETY: This owns the transport.
            unsafe {
                web_bindings_message_port_transport_close_after_sending_all_pending_messages(socket.raw.as_ptr());
            }
        }
    }

    /// Sends the record to the peer as one message. False if the transport failed, which LibWeb logs.
    pub(crate) fn send(&self, vm: &Vm, record: TransferRecordRef) -> bool {
        let mut message = TransferRecord::into_port_message(record, vm);
        if let Connection::Channel(end) = &*self.connection.borrow() {
            match end.send(message) {
                Ok(()) => return true,
                Err(returned) => message = returned,
            }
        }
        // NB: The peer left the process, which the channel says only after everything that waits on it.
        let change_over = match &*self.connection.borrow() {
            Connection::Channel(end) => Some(end.read()),
            _ => None,
        };
        if let Some((messages, PeerState::Transport(handle))) = change_over {
            self.change_over_to_socket(handle, messages);
        }
        match &*self.connection.borrow() {
            Connection::Socket(socket) => socket.send(message),
            _ => false,
        }
    }

    /// Appends every message that has arrived, each a record, and tells whether the peer is gone.
    pub(crate) fn read_as_many_messages_as_possible_without_blocking(
        &self,
        vm: &Vm,
        records: &MarkedVec<'_, TransferRecordRef>,
    ) -> ShouldShutdown {
        for message in self.channel_leftovers.borrow_mut().drain(..) {
            records.push(TransferRecord::from_port_message(vm, message));
        }
        let channel_read = match &*self.connection.borrow() {
            Connection::Channel(end) => Some(end.read()),
            Connection::Socket(_) | Connection::Released => None,
        };
        if let Some((messages, state)) = channel_read {
            for message in messages {
                records.push(TransferRecord::from_port_message(vm, message));
            }
            match state {
                PeerState::Connected => return ShouldShutdown::No,
                PeerState::Gone => return ShouldShutdown::Yes,
                PeerState::Transport(handle) => self.change_over_to_socket(handle, VecDeque::new()),
            }
        }
        match &*self.connection.borrow() {
            Connection::Socket(socket) => socket.read_as_many_messages_as_possible_without_blocking(vm, records),
            _ => ShouldShutdown::No,
        }
    }
}

impl Drop for MessagePortTransport {
    fn drop(&mut self) {
        if let Some(id) = self.channel_read_hook_id {
            // NB: A thread that goes away has no hooks left to run, and has no list left to speak of.
            let _ = CHANNEL_READ_HOOKS.try_with(|hooks| hooks.borrow_mut().remove(&id));
        }
        // NB: The connection goes first, as the hook is lent to it.
        *self.connection.borrow_mut() = Connection::Released;
    }
}
