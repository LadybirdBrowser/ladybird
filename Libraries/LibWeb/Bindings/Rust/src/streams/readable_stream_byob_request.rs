/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ReadableStreamBYOBRequest, https://streams.spec.whatwg.org/#rs-byob-request-class

use core::cell::Cell;

use libjs_rust::api::{Gc, Trace, TypedArrayBase};

use crate::bindings::conversions::{ArrayBufferView, BufferSource};
use crate::bindings::interface::{Ref, Wrappable};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::streams::readable_byte_stream_controller::{
    ReadableByteStreamController, readable_byte_stream_controller_respond,
    readable_byte_stream_controller_respond_with_new_view,
};

// https://streams.spec.whatwg.org/#readablestreambyobrequest
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ReadableStreamBYOBRequest {
    // https://streams.spec.whatwg.org/#readablestreambyobrequest-controller
    controller: Cell<Option<Ref<ReadableByteStreamController>>>,
    // https://streams.spec.whatwg.org/#readablestreambyobrequest-view
    view: Cell<Option<Gc<TypedArrayBase>>>,
}

impl Wrappable for ReadableStreamBYOBRequest {}

impl ReadableStreamBYOBRequest {
    pub(crate) fn new() -> Self {
        Self {
            controller: Cell::new(None),
            view: Cell::new(None),
        }
    }

    pub(crate) fn controller(&self) -> Option<Ref<ReadableByteStreamController>> {
        self.controller.get()
    }

    pub(crate) fn set_controller(&self, controller: Option<Ref<ReadableByteStreamController>>) {
        self.controller.set(controller);
    }

    /// The request's [[view]], a Uint8Array.
    pub(crate) fn current_view(&self) -> Option<Gc<TypedArrayBase>> {
        self.view.get()
    }

    pub(crate) fn set_view(&self, view: Option<Gc<TypedArrayBase>>) {
        self.view.set(view);
    }

    // https://streams.spec.whatwg.org/#rs-byob-request-view
    pub(crate) fn view(&self, _: &mut CallContext) -> WebIdlResult<Option<Gc<TypedArrayBase>>> {
        Ok(self.current_view())
    }

    // https://streams.spec.whatwg.org/#rs-byob-request-respond
    pub(crate) fn respond(this: &Ref<Self>, cx: &mut CallContext, bytes_written: u64) -> WebIdlResult<()> {
        // 1. If this.[[controller]] is undefined, throw a TypeError exception.
        let Some(controller) = this.controller() else {
            return Err(Exception::type_error("Controller is undefined"));
        };

        // 2. If ! IsDetachedBuffer(this.[[view]].[[ArrayBuffer]]) is true, throw a TypeError exception.
        let view = this
            .current_view()
            .expect("a BYOB request with a controller has a view");
        if view.viewed_array_buffer().is_detached() {
            return Err(Exception::type_error("Unable to respond to detached ArrayBuffer"));
        }

        // 3. Assert: this.[[view]].[[ByteLength]] > 0.
        assert!(BufferSource::TypedArray(view).extent().byte_length > 0);

        // 4. Assert: this.[[view]].[[ViewedArrayBuffer]].[[ByteLength]] > 0.
        assert!(view.viewed_array_buffer().byte_length() > 0);

        // 5. Perform ? ReadableByteStreamControllerRespond(this.[[controller]], bytesWritten).
        readable_byte_stream_controller_respond(cx, controller, bytes_written)
    }

    // https://streams.spec.whatwg.org/#rs-byob-request-respond-with-new-view
    pub(crate) fn respond_with_new_view(
        this: &Ref<Self>,
        cx: &mut CallContext,
        view: ArrayBufferView,
    ) -> WebIdlResult<()> {
        // 1. If this.[[controller]] is undefined, throw a TypeError exception.
        let Some(controller) = this.controller() else {
            return Err(Exception::type_error("Controller is undefined"));
        };

        // 2. If ! IsDetachedBuffer(view.[[ViewedArrayBuffer]]) is true, throw a TypeError exception.
        if BufferSource::from(view).extent().buffer.is_detached() {
            return Err(Exception::type_error("Unable to respond with a detached ArrayBuffer"));
        }

        // 3. Return ? ReadableByteStreamControllerRespondWithNewView(this.[[controller]], view).
        readable_byte_stream_controller_respond_with_new_view(cx, controller, view)
    }
}
