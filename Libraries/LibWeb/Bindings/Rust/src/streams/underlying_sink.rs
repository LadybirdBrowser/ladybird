/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! UnderlyingSink, https://streams.spec.whatwg.org/#underlying-sink-api

use libjs_rust::api::Value;
#[cfg(not(web_idl_bindings))]
use libjs_rust::api::{ThrowCompletionOr, Vm};

use crate::bindings::callback::{CallbackFunction, PromiseCallbackFunction};
#[cfg(not(web_idl_bindings))]
use crate::bindings::conversions::FromJs;

/// The dictionary UnderlyingSink of Streams/UnderlyingSink.idl, whose members the bindings convert.
pub(crate) struct UnderlyingSink {
    pub(crate) abort: Option<PromiseCallbackFunction>,
    pub(crate) close: Option<PromiseCallbackFunction>,
    pub(crate) start: Option<CallbackFunction>,
    pub(crate) r#type: Option<Value>,
    pub(crate) write: Option<PromiseCallbackFunction>,
}

/// Without the generated bindings, which a build of the crate on its own leaves out, nothing converts a JS value to the
/// dictionary. This stands in for the conversion so that the constructor that uses it compiles.
#[cfg(not(web_idl_bindings))]
impl FromJs for UnderlyingSink {
    fn from_js(_: &Vm, _: Value) -> ThrowCompletionOr<Self> {
        unreachable!("a JS value has no conversion without the bindings")
    }
}
