/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Transformer, https://streams.spec.whatwg.org/#transformer-api

use libjs_rust::api::Value;
#[cfg(not(web_idl_bindings))]
use libjs_rust::api::{ThrowCompletionOr, Vm};

use crate::bindings::callback::{CallbackFunction, PromiseCallbackFunction};
#[cfg(not(web_idl_bindings))]
use crate::bindings::conversions::FromJs;

/// The dictionary Transformer of Streams/Transformer.idl, whose members the bindings convert.
pub(crate) struct Transformer {
    pub(crate) cancel: Option<PromiseCallbackFunction>,
    pub(crate) flush: Option<PromiseCallbackFunction>,
    pub(crate) readable_type: Option<Value>,
    pub(crate) start: Option<CallbackFunction>,
    pub(crate) transform: Option<PromiseCallbackFunction>,
    pub(crate) writable_type: Option<Value>,
}

/// Without the generated bindings, which a build of the crate on its own leaves out, nothing converts a JS value to the
/// dictionary. This stands in for the conversion so that the constructor that uses it compiles.
#[cfg(not(web_idl_bindings))]
impl FromJs for Transformer {
    fn from_js(_: &Vm, _: Value) -> ThrowCompletionOr<Self> {
        unreachable!("a JS value has no conversion without the bindings")
    }
}
