/*
 * Copyright (c) 2025, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CommandEvent, https://html.spec.whatwg.org/multipage/interaction.html#commandevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::{CppCell, LentCppCell, cpp_class};
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::lent_string::{LentUtf16FlyString, LentUtf16String};
use crate::bindings::{CallContext, WebIdlResult};
use crate::css::css_rule::wrap_cpp_cell;
use crate::dom::event::EventInit;

/// The dictionary CommandEventInit of HTML/CommandEvent.idl.
pub(crate) struct CommandEventInit {
    pub(crate) parent: EventInit,
    pub(crate) source: Option<CppInterface>,
    pub(crate) command: Utf16String,
}

// https://html.spec.whatwg.org/multipage/interaction.html#commandevent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CommandEvent {
    event: CppEvent,
    source: Option<CppCell>,
    command: Utf16String,
}

impl Wrappable for CommandEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl CommandEvent {
    fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        event_name: &Utf16String,
        event_init: &EventInit,
        source: Option<CppCell>,
        command: Utf16String,
    ) -> Ref<CommandEvent> {
        let event = CppEvent::create_for_owner(
            realm,
            event_name,
            event_init.bubbles,
            event_init.cancelable,
            event_init.composed,
        );
        let object = allocate(vm, |_| CommandEvent { event, source, command });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: CommandEventInit,
    ) -> WebIdlResult<Ref<CommandEvent>> {
        let source = event_init_dict.source.map(CppInterface::cell);
        Ok(Self::create(
            cx.vm(),
            cx.realm(),
            &r#type,
            &event_init_dict.parent,
            source,
            event_init_dict.command,
        ))
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    // https://html.spec.whatwg.org/multipage/interaction.html#dom-commandevent-source
    pub(crate) fn source(&self, cx: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        // The source getter steps are to return the result of retargeting source against this's currentTarget.
        Ok(self.source.as_ref().map(|source| {
            let source = self.event.retarget_against_current_target(source);
            wrap_cpp_cell(cx, &source)
        }))
    }

    // https://html.spec.whatwg.org/multipage/interaction.html#dom-commandevent-command
    pub(crate) fn command(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.command.clone())
    }
}

/// The Event of a new trusted CommandEvent in the realm, for the implementation to fire.
#[allow(clippy::too_many_arguments, reason = "C++ passes each member of the dictionary")]
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_command_event_create(
    vm: &Vm,
    realm: Gc<Realm>,
    r#type: LentUtf16FlyString<'_>,
    bubbles: bool,
    cancelable: bool,
    composed: bool,
    source: &LentCppCell<cpp_class::Element>,
    command: LentUtf16String<'_>,
) -> *mut c_void {
    let event_init = EventInit {
        bubbles,
        cancelable,
        composed,
    };
    let r#type: Utf16String = r#type.to_utf16_fly_string().into();
    let source = CppCell::from_lent(source);
    let event = CommandEvent::create(vm, realm, &r#type, &event_init, Some(source), command.to_utf16_string());
    event.event().as_ptr()
}
