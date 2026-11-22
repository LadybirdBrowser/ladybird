/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! BufferedChangeEvent, https://w3c.github.io/media-source/#bufferedchangeevent-interface

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::Trace;

use crate::bindings::conversions::CppInterface;
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

/// The dictionary BufferedChangeEventInit of MediaSourceExtensions/BufferedChangeEvent.idl.
pub(crate) struct BufferedChangeEventInit {
    pub(crate) parent: EventInit,
    #[allow(dead_code, reason = "the attributes that return the ranges are not implemented")]
    pub(crate) added_ranges: Option<CppInterface>,
    #[allow(dead_code, reason = "the attributes that return the ranges are not implemented")]
    pub(crate) removed_ranges: Option<CppInterface>,
}

// https://w3c.github.io/media-source/#bufferedchangeevent-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct BufferedChangeEvent {
    event: CppEvent,
}

impl Wrappable for BufferedChangeEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl BufferedChangeEvent {
    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: BufferedChangeEventInit,
    ) -> WebIdlResult<Ref<BufferedChangeEvent>> {
        let event_init = event_init_dict.parent;
        let event = CppEvent::create_for_owner(
            cx.realm(),
            &r#type,
            event_init.bubbles,
            event_init.cancelable,
            event_init.composed,
        );
        let object = allocate(cx.vm(), |_| BufferedChangeEvent { event });
        object.event.set_owner(object);
        Ok(object)
    }
}
