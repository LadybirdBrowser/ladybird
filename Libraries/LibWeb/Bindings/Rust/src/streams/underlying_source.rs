/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! UnderlyingSource, https://streams.spec.whatwg.org/#underlying-source-api

#[cfg(not(web_idl_bindings))]
use libjs_rust::api::{ThrowCompletionOr, Vm};
use libjs_rust::api::{Trace, Value};

use crate::bindings::CallContext;
use crate::bindings::callback::{CallbackFunction, PromiseCallbackFunction};
#[cfg(not(web_idl_bindings))]
use crate::bindings::conversions::FromJs;
use crate::bindings::interface::{Ref, wrap};
use crate::streams::readable_byte_stream_controller::ReadableByteStreamController;
use crate::streams::readable_stream_default_controller::ReadableStreamDefaultController;

/// The dictionary UnderlyingSource of Streams/UnderlyingSource.idl, whose members the bindings convert.
pub(crate) struct UnderlyingSource {
    pub(crate) auto_allocate_chunk_size: Option<u64>,
    pub(crate) cancel: Option<PromiseCallbackFunction>,
    pub(crate) pull: Option<PromiseCallbackFunction>,
    pub(crate) start: Option<CallbackFunction>,
    pub(crate) r#type: Option<ReadableStreamType>,
}

/// Without the generated bindings, which a build of the crate on its own leaves out, nothing converts a JS value to the
/// dictionary. This stands in for the conversion so that the constructor that uses it compiles.
#[cfg(not(web_idl_bindings))]
impl FromJs for UnderlyingSource {
    fn from_js(_: &Vm, _: Value) -> ThrowCompletionOr<Self> {
        unreachable!("a JS value has no conversion without the bindings")
    }
}

// https://streams.spec.whatwg.org/#enumdef-readablestreamtype
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum ReadableStreamType {
    Bytes,
}

/// https://streams.spec.whatwg.org/#typedefdef-readablestreamcontroller
#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum ReadableStreamController {
    ReadableStreamDefaultController(Ref<ReadableStreamDefaultController>),
    ReadableByteStreamController(Ref<ReadableByteStreamController>),
}

impl ReadableStreamController {
    /// The controller converted to a JS value, as the underlying source's callbacks are invoked with it.
    pub(crate) fn to_js(self, cx: &CallContext) -> Value {
        Value::from_object(match self {
            Self::ReadableStreamDefaultController(controller) => wrap(cx, controller),
            Self::ReadableByteStreamController(controller) => wrap(cx, controller),
        })
    }
}
