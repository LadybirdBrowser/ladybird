/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PageSwapEvent, https://html.spec.whatwg.org/multipage/browsing-the-web.html#pageswapevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::Trace;

use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::css::css_rule::wrap_cpp_cell;
use crate::dom::event::EventInit;

/// The dictionary PageSwapEventInit of HTML/PageSwapEvent.idl.
pub(crate) struct PageSwapEventInit {
    pub(crate) parent: EventInit,
    pub(crate) activation: Option<CppInterface>,
    pub(crate) view_transition: Option<CppInterface>,
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#pageswapevent
#[derive(Trace)]
pub(crate) struct PageSwapEvent {
    event: CppEvent,
    activation: Option<CppCell>,
    view_transition: Option<CppCell>,
}

impl Wrappable for PageSwapEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl PageSwapEvent {
    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: PageSwapEventInit,
    ) -> WebIdlResult<Ref<PageSwapEvent>> {
        let event_init = event_init_dict.parent;
        let event = CppEvent::create_for_owner(
            cx.realm(),
            &r#type,
            event_init.bubbles,
            event_init.cancelable,
            event_init.composed,
        );
        let object = allocate(cx.vm(), |_| PageSwapEvent {
            event,
            activation: event_init_dict.activation.map(CppInterface::cell),
            view_transition: event_init_dict.view_transition.map(CppInterface::cell),
        });
        object.event.set_owner(object);
        Ok(object)
    }

    pub(crate) fn event(&self) -> &CppEvent {
        &self.event
    }

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#dom-pageswapevent-activation
    pub(crate) fn activation(&self, cx: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        Ok(self.activation.as_ref().map(|activation| wrap_cpp_cell(cx, activation)))
    }

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#dom-pageswapevent-viewtransition
    pub(crate) fn view_transition(&self, cx: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        Ok(self
            .view_transition
            .as_ref()
            .map(|view_transition| wrap_cpp_cell(cx, view_transition)))
    }
}
