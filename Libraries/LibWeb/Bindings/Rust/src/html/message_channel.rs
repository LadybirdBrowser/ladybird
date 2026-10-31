/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! MessageChannel, https://html.spec.whatwg.org/multipage/web-messaging.html#message-channels

use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::interface::{Ref, Wrappable};
use crate::bindings::{CallContext, WebIdlResult};
use crate::html::message_port::MessagePort;

// https://html.spec.whatwg.org/multipage/web-messaging.html#messagechannel
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct MessageChannel {
    realm: Gc<Realm>,
    // Each MessageChannel object has an associated port 1 and an associated port 2, both MessagePort objects.
    port1: Ref<MessagePort>,
    port2: Ref<MessagePort>,
}

impl Wrappable for MessageChannel {
    fn relevant_realm(&self, _: &Vm) -> Option<Gc<Realm>> {
        Some(self.realm)
    }
}

impl MessageChannel {
    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messagechannel
    pub(crate) fn construct(cx: &mut CallContext) -> WebIdlResult<Self> {
        let vm = cx.vm();
        let realm = cx.realm();

        // 1. Set this's port 1 to a new MessagePort in this's relevant realm.
        let port1 = MessagePort::create(vm, realm);

        // 2. Set this's port 2 to a new MessagePort in this's relevant realm.
        let port2 = MessagePort::create(vm, realm);

        // 3. Entangle this's port 1 and this's port 2.
        MessagePort::entangle(vm, port1, port2);

        Ok(Self { realm, port1, port2 })
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messagechannel-port1
    pub(crate) fn port1(&self, _: &mut CallContext) -> WebIdlResult<Ref<MessagePort>> {
        // The port1 getter steps are to return this's port 1.
        Ok(self.port1)
    }

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messagechannel-port2
    pub(crate) fn port2(&self, _: &mut CallContext) -> WebIdlResult<Ref<MessagePort>> {
        // The port2 getter steps are to return this's port 2.
        Ok(self.port2)
    }
}
