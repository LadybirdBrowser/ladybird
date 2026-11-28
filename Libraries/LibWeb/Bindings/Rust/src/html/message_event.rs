/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! MessageEvent, https://html.spec.whatwg.org/multipage/comms.html#messageevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, GcRefCell, MarkedVec, Must, Object, Realm, Trace, Value, Vm};
use liburl_rust::origin::Origin;

use crate::bindings::conversions::{CppInterface, UsvString, frozen_array_to_js, rooted_sequence_to_js};
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate, wrap};
use crate::bindings::setlike::WorldEntries;
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;
use crate::html::message_port::MessagePort;

/// The union MessageEventSource of HTML/MessageEvent.idl, whose members the bindings convert.
#[derive(Clone, Copy, Trace)]
pub(crate) enum MessageEventSource {
    WindowProxy(CppInterface),
    MessagePort(Ref<MessagePort>),
}

/// The dictionary MessageEventInit of HTML/MessageEvent.idl, whose members the bindings convert.
pub(crate) struct MessageEventInit<'vm> {
    pub(crate) parent: EventInit,
    pub(crate) data: Value,
    pub(crate) origin: UsvString,
    pub(crate) last_event_id: Utf16String,
    pub(crate) source: Option<MessageEventSource>,
    pub(crate) ports: MarkedVec<'vm, Ref<MessagePort>>,
}

impl<'vm> MessageEventInit<'vm> {
    /// The dictionary with the default value of each member.
    pub(crate) fn new(vm: &'vm Vm) -> Self {
        Self {
            parent: EventInit {
                bubbles: false,
                cancelable: false,
                composed: false,
            },
            data: Value::NULL,
            origin: UsvString::from_utf8(""),
            last_event_id: Utf16String::from_utf8(""),
            source: None,
            ports: MarkedVec::new(vm),
        }
    }
}

// https://html.spec.whatwg.org/multipage/comms.html#concept-messageevent-origin
// Each MessageEvent has an origin (an origin, a string, or null), initially null.
enum MessageEventOrigin {
    Origin(Origin),
    String(UsvString),
    Null,
}

#[derive(Trace)]
struct MessageEventState {
    data: Value,
    #[gc(untraced)]
    origin: MessageEventOrigin,
    #[gc(untraced)]
    last_event_id: Utf16String,
    source: Option<MessageEventSource>,
    ports: Vec<Ref<MessagePort>>,
}

// https://html.spec.whatwg.org/multipage/comms.html#messageevent
#[derive(Trace)]
pub(crate) struct MessageEvent {
    event: CppEvent,
    state: GcRefCell<MessageEventState>,
    ports_arrays: WorldEntries<Object>,
}

impl Wrappable for MessageEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl MessageEvent {
    /// A new MessageEvent in the realm, whose origin is the origin if there is one, and the origin member of the
    /// dictionary otherwise.
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: MessageEventInit<'_>,
        origin: Option<Origin>,
    ) -> Ref<MessageEvent> {
        let origin = match origin {
            Some(origin) => MessageEventOrigin::Origin(origin),
            None => MessageEventOrigin::String(event_init.origin),
        };
        Self::create_with_origin(
            vm,
            realm,
            event_name,
            event_init.parent,
            event_init.data,
            origin,
            event_init.last_event_id,
            event_init.source,
            &event_init.ports,
        )
    }

    /// A new MessageEvent in the realm whose type is the empty string, for createEvent().
    pub(crate) fn create_for_create_event(vm: &Vm, realm: Gc<Realm>) -> Ref<MessageEvent> {
        let event_init = MessageEventInit::new(vm);
        Self::create_with_origin(
            vm,
            realm,
            &Utf16String::from_utf8(""),
            event_init.parent,
            event_init.data,
            MessageEventOrigin::Null,
            event_init.last_event_id,
            event_init.source,
            &event_init.ports,
        )
    }

    #[allow(clippy::too_many_arguments, reason = "the event has these attributes")]
    fn create_with_origin(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: EventInit,
        data: Value,
        origin: MessageEventOrigin,
        last_event_id: Utf16String,
        source: Option<MessageEventSource>,
        ports: &MarkedVec<'_, Ref<MessagePort>>,
    ) -> Ref<MessageEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.bubbles,
            event_init.cancelable,
            event_init.composed,
        );
        let object = allocate(vm, |_| MessageEvent {
            event,
            state: GcRefCell::new(MessageEventState {
                data,
                origin,
                last_event_id,
                source,
                ports: ports.to_vec(),
            }),
            ports_arrays: WorldEntries::default(),
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: MessageEventInit<'_>,
    ) -> WebIdlResult<Ref<MessageEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, event_init_dict, None))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    /// The data attribute, for the implementation.
    pub(crate) fn data_value(&self) -> Value {
        self.state.borrow().data
    }

    pub(crate) fn data(&self, _: &mut CallContext) -> WebIdlResult<Value> {
        Ok(self.data_value())
    }

    // https://html.spec.whatwg.org/multipage/comms.html#dom-messageevent-origin
    pub(crate) fn origin(&self, _: &mut CallContext) -> WebIdlResult<UsvString> {
        Ok(match &self.state.borrow().origin {
            // 1. If this's origin is an origin, then return the serialization of this's origin.
            MessageEventOrigin::Origin(origin) => UsvString::from_utf8(&origin.serialize()),
            // 2. If this's origin is null, then return the empty string.
            MessageEventOrigin::Null => UsvString::from_utf8(""),
            // 3. Return this's origin.
            MessageEventOrigin::String(origin) => origin.clone(),
        })
    }

    pub(crate) fn last_event_id(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.state.borrow().last_event_id.clone())
    }

    pub(crate) fn source(&self, _: &mut CallContext) -> WebIdlResult<Option<MessageEventSource>> {
        Ok(self.state.borrow().source)
    }

    pub(crate) fn ports(&self, cx: &mut CallContext) -> WebIdlResult<Value> {
        let ports = self.ports_arrays.get_or_create(cx, || {
            let ports = MarkedVec::new(cx.vm());
            for port in &self.state.borrow().ports {
                ports.push(*port);
            }
            let array = rooted_sequence_to_js(cx, ports, |port| Ok(Value::from_object(wrap(cx, *port))));
            frozen_array_to_js(cx, array).must().as_object()
        });
        Ok(Value::from_object(ports))
    }

    // https://html.spec.whatwg.org/multipage/comms.html#dom-messageevent-initmessageevent
    #[allow(clippy::too_many_arguments, reason = "the operation has these arguments")]
    pub(crate) fn init_message_event(
        &self,
        cx: &mut CallContext,
        r#type: Utf16String,
        bubbles: bool,
        cancelable: bool,
        data: Value,
        origin: UsvString,
        last_event_id: Utf16String,
        source: Option<MessageEventSource>,
        ports: MarkedVec<'_, Ref<MessagePort>>,
    ) -> WebIdlResult<()> {
        // The initMessageEvent(type, bubbles, cancelable, data, origin, lastEventId, source, ports) method must
        // initialize the event in a manner analogous to the similarly-named initEvent() method.

        // https://dom.spec.whatwg.org/#dom-event-initevent
        // 1. If this’s dispatch flag is set, then return.
        if self.event.dispatched() {
            return Ok(());
        }

        // 2. Initialize this with type, bubbles, and cancelable.
        self.event.initialize_event(&r#type, bubbles, cancelable);

        *self.state.borrow_mut(cx.vm()) = MessageEventState {
            data,
            origin: MessageEventOrigin::String(origin),
            last_event_id,
            source,
            ports: ports.to_vec(),
        };

        self.ports_arrays.clear();
        Ok(())
    }

    // https://html.spec.whatwg.org/multipage/comms.html#the-messageevent-interface:extract-an-origin
    pub(crate) fn extract_an_origin(&self) -> Option<Origin> {
        // Objects implementing the MessageEvent interface's extract an origin steps are to return this's origin if it
        // is an origin; otherwise null.
        match &self.state.borrow().origin {
            MessageEventOrigin::Origin(origin) => Some(origin.clone()),
            MessageEventOrigin::String(_) | MessageEventOrigin::Null => None,
        }
    }
}
