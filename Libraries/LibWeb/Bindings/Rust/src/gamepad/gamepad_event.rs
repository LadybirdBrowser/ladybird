/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! GamepadEvent, https://w3c.github.io/gamepad/#dom-gamepadevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;
use crate::gamepad::gamepad::Gamepad;

/// The dictionary GamepadEventInit of Gamepad/GamepadEvent.idl.
pub(crate) struct GamepadEventInit {
    pub(crate) parent: EventInit,
    pub(crate) gamepad: Option<Ref<Gamepad>>,
}

// https://w3c.github.io/gamepad/#dom-gamepadevent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct GamepadEvent {
    event: CppEvent,
    gamepad: Option<Ref<Gamepad>>,
}

impl Wrappable for GamepadEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl GamepadEvent {
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: GamepadEventInit,
    ) -> Ref<GamepadEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.parent.bubbles,
            event_init.parent.cancelable,
            event_init.parent.composed,
        );
        let object = allocate(vm, |_| GamepadEvent {
            event,
            gamepad: event_init.gamepad,
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: GamepadEventInit,
    ) -> WebIdlResult<Ref<GamepadEvent>> {
        Ok(Self::create(cx.vm(), cx.realm(), &r#type, event_init_dict))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    pub(crate) fn gamepad(&self, _: &mut CallContext) -> WebIdlResult<Option<Ref<Gamepad>>> {
        Ok(self.gamepad)
    }
}
