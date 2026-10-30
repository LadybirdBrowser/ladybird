/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! MessagePort, https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;
use std::collections::VecDeque;

use ak::Utf16String;
use libjs_rust::api::{
    CellRoot, EmbedderCell, Gc, GcRefCell, GcWeak, MarkedVec, Object, Realm, Trace, Value, Vm, is_host_instance_of,
};

use crate::bindings::event_target::{CppEvent, CppEventTarget, GetTheParent, set_event_handler_attribute};
use crate::bindings::interface::{
    Interface, Ref, Wrappable, Wrapped, allocate, implementation_if_wrapper, with_temporary_execution_context,
};
use crate::bindings::promise::{TaskSource, queue_a_global_task};
use crate::bindings::structured_data::{TransferDataReader, TransferDataWriter, Transferable};
use crate::bindings::transport::{MessagePortTransport, ShouldShutdown};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::html::structured_serialize::cpp::{OwnedSerializedTransferRecord, SerializedTransferRecord};
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
    #[gc(untraced)]
    port_message_queue: RefCell<VecDeque<OwnedSerializedTransferRecord>>,
    #[gc(untraced)]
    pending_outgoing_messages: RefCell<Vec<OwnedSerializedTransferRecord>>,
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
    fn relevant_realm(&self, _: &Vm) -> Option<Gc<Realm>> {
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
    fn new(realm: Gc<Realm>) -> Self {
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
            port_message_queue: RefCell::new(VecDeque::new()),
            pending_outgoing_messages: RefCell::new(Vec::new()),
            message_task_generation: Cell::new(0),
            should_shutdown_on_enable: Cell::new(false),
            enabled: Cell::new(false),
            keep_alive: RefCell::new(None),
            detached: Cell::new(false),
        }
    }

    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>) -> Ref<Self> {
        let port = allocate(vm, |_| Self::new(realm));
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
            return Ok(());
        }
        let Some(target_port) = target_port else {
            // AD-HOC: A port entangled over a transport has no targetPort in this agent, so it sends the message for the
            //         other end to add the task. A port that has never been entangled keeps the message until it is.
            if this.transport.borrow().is_some() {
                Self::post_port_message(this, serialize_with_transfer_result.record());
            } else if !this.detached.get() && !this.has_been_shipped.get() && !this.has_ever_been_entangled.get() {
                this.pending_outgoing_messages
                    .borrow_mut()
                    .push(serialize_with_transfer_result);
            }
            return Ok(());
        };

        // 7. Add a task that runs the following steps to the port message queue of targetPort:
        Self::add_message_task(target_port, vm, serialize_with_transfer_result);

        Ok(())
    }

    /// Adds the task that fires the message to the port message queue of the port. The event loop runs it only once the
    /// queue is enabled.
    fn add_message_task(this: Ref<Self>, vm: &Vm, serialize_with_transfer_result: OwnedSerializedTransferRecord) {
        this.port_message_queue
            .borrow_mut()
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
            this.realm,
            TaskSource::PostedMessage,
            (this, message_task_generation),
            |vm, (this, message_task_generation)| {
                if message_task_generation != this.message_task_generation.get() {
                    return;
                }
                let Some(serialize_with_transfer_result) = this.port_message_queue.borrow_mut().pop_front() else {
                    return;
                };
                Self::post_message_task_steps(this, vm, serialize_with_transfer_result);
            },
        );
    }

    fn flush_pending_outgoing_messages(this: Ref<Self>, vm: &Vm) {
        if let Some(remote_port) = this.remote_port.get() {
            let pending_outgoing_messages = core::mem::take(&mut *this.pending_outgoing_messages.borrow_mut());
            for pending_message in pending_outgoing_messages {
                Self::add_message_task(remote_port, vm, pending_message);
            }
            return;
        }

        if !this
            .transport
            .borrow()
            .as_ref()
            .is_some_and(MessagePortTransport::is_open)
        {
            return;
        }

        let pending_outgoing_messages = core::mem::take(&mut *this.pending_outgoing_messages.borrow_mut());
        for pending_message in &pending_outgoing_messages {
            Self::post_port_message(this, pending_message.record());
        }
    }

    /// Reads the messages that have arrived on the port's transport, and notes whether the other end went away.
    fn drain_transport(&self) -> Vec<OwnedSerializedTransferRecord> {
        let mut messages = Vec::new();
        let transport = self.transport.borrow();
        let Some(transport) = &*transport else {
            return messages;
        };

        let should_shutdown = transport.read_as_many_messages_as_possible_without_blocking(&mut messages);

        // The peer is gone, so nothing more arrives; the pending messages are delivered once the port is enabled.
        if should_shutdown == ShouldShutdown::Yes {
            self.should_shutdown_on_enable.set(true);
            transport.close();
        }
        messages
    }

    fn post_port_message(this: Ref<Self>, serialize_with_transfer_result: SerializedTransferRecord) {
        let sent = {
            let transport = this.transport.borrow();
            let Some(transport) = &*transport else {
                return;
            };
            if !transport.is_open() {
                return;
            }
            transport.send(serialize_with_transfer_result)
        };
        if !sent {
            this.disentangle();
        }
    }

    fn read_from_transport(this: Ref<Self>, vm: &Vm) {
        for message in this.drain_transport() {
            Self::add_message_task(this, vm, message);
        }

        if this.enabled.get() && this.should_shutdown_on_enable.replace(false) {
            queue_a_global_task(vm, this.realm, TaskSource::PostedMessage, this, |_, this| {
                this.close_steps();
            });
        }
    }

    fn post_message_task_steps(
        this: Ref<Self>,
        vm: &Vm,
        serialize_with_transfer_result: OwnedSerializedTransferRecord,
    ) {
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
            structured_deserialize_with_transfer(&cx, serialize_with_transfer_result.record())
        });
        let Ok(deserialize_record) = deserialize_record else {
            // If this throws an exception, catch it, fire an event named messageerror at messageEventTarget, using MessageEvent, and then return.
            let event = CppEvent::create_message_event(target_realm, "messageerror", Value::NULL, &[]);
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
        let event = CppEvent::create_message_event(target_realm, "message", message_clone, &new_ports.values());
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
        self.port_message_queue.borrow_mut().clear();
        self.message_task_generation.set(self.message_task_generation.get() + 1);
    }

    /// Drops every message the port holds, both ways, and forgets that its peer went away.
    fn clear_message_queues(&self) {
        self.discard_pending_messages();
        self.pending_outgoing_messages.borrow_mut().clear();
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

        // NB: The ports change only once every write to dataHolder has succeeded, so steps 1 and 3.1 run last.

        // NB: A port entangled over a transport hands its transport over. Stop the transport's IO thread before draining
        //     it below, so that no message arrives between the drain and the handle going to the new owner.
        let released_transport = this
            .transport
            .borrow()
            .as_ref()
            .map(MessagePortTransport::release_for_transfer);

        // 2. Set dataHolder.[[PortMessageQueue]] to value's port message queue.
        // NB: Drain the transport into the queue first, so that the messages it received and a pending shutdown move
        //     with the port instead of being left behind on the old transport.
        let received_messages = this.drain_transport();
        let mut port_message_queue = this.port_message_queue.borrow_mut();
        port_message_queue.extend(received_messages);
        data_holder.write_records(port_message_queue.make_contiguous())?;
        drop(port_message_queue);
        data_holder.write_records(&this.pending_outgoing_messages.borrow())?;
        data_holder.write_bool(this.should_shutdown_on_enable.get())?;

        // 3. If value is entangled with another port remotePort:
        // NB: Ports in the same agent need no transport until one of them is shipped. Keep remotePort in this agent, and
        //     entangle it over a new transport with the port that the transfer-receiving steps create.
        let (remote_port, remote_port_handle) = match this.remote_port.get() {
            Some(remote_port) => {
                let (transport, handle) = MessagePortTransport::create_paired();
                (Some((remote_port, transport)), Some(handle))
            }
            None => (None, released_transport),
        };

        //    2. Set dataHolder.[[RemotePort]] to remotePort.
        // 4. Otherwise, set dataHolder.[[RemotePort]] to null.
        data_holder.write_bool(remote_port_handle.is_some())?;
        if let Some(remote_port_handle) = remote_port_handle {
            if this.fail_next_transfer_for_testing.replace(false) {
                return Err(Exception::Dom(
                    "DataCloneError",
                    Utf16String::from_ascii(b"Forced MessagePort transfer failure"),
                ));
            }
            data_holder.write_transport_handle(remote_port_handle)?;
        }

        // 1. Set value's has been shipped flag to true.
        this.has_been_shipped.set(true);

        if let Some((remote_port, mut transport)) = remote_port {
            // 3.1. Set remotePort's has been shipped flag to true.
            remote_port.has_been_shipped.set(true);

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
        *value.port_message_queue.borrow_mut() = data_holder.read_records()?.into();
        *value.pending_outgoing_messages.borrow_mut() = data_holder.read_records()?;
        value.should_shutdown_on_enable.set(data_holder.read_bool()?);

        // 3. If dataHolder.[[RemotePort]] is not null, then entangle dataHolder.[[RemotePort]] and value.
        //     (This will disentangle dataHolder.[[RemotePort]] from the original port that was transferred.)
        if data_holder.read_bool()? {
            let mut transport = data_holder.read_transport_handle()?.create_transport();
            value.has_ever_been_entangled.set(true);

            transport.set_up_read_hook(ReadHookState::new(vm, value), ReadHookState::read_from_transport);
            *value.transport.borrow_mut() = Some(transport);
            Self::update_keep_alive(value);

            Self::flush_pending_outgoing_messages(value, vm);
        }

        Ok(value)
    }
}
