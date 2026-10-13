/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CountQueuingStrategy, https://streams.spec.whatwg.org/#cqs-class

use libjs_rust::api::{
    FunctionObject, Gc, PropertyKey, RawNativeFunction, ThrowCompletionOr, Trace, Value, Vm, raw_native,
};

use crate::bindings::interface::Wrappable;
use crate::bindings::realm_data::realm_data;
use crate::bindings::{CallContext, WebIdlResult};
use crate::streams::queuing_strategy_init::QueuingStrategyInit;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CountQueuingStrategy {
    high_water_mark: f64,
}

impl Wrappable for CountQueuingStrategy {}

#[expect(
    clippy::unnecessary_wraps,
    reason = "the bindings call every method of an implementation for its WebIDL result, whether or not it can fail"
)]
impl CountQueuingStrategy {
    // https://streams.spec.whatwg.org/#cqs-constructor
    pub(crate) fn construct(_: &mut CallContext, init: QueuingStrategyInit) -> WebIdlResult<Self> {
        // 1. Set this.[[highWaterMark]] to init["highWaterMark"].
        Ok(Self {
            high_water_mark: init.high_water_mark,
        })
    }

    // https://streams.spec.whatwg.org/#cqs-high-water-mark
    pub(crate) fn high_water_mark(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The highWaterMark getter steps are:
        // 1. Return this.[[highWaterMark]].
        Ok(self.high_water_mark)
    }

    // https://streams.spec.whatwg.org/#cqs-size
    pub(crate) fn size(&self, cx: &mut CallContext) -> WebIdlResult<Gc<FunctionObject>> {
        // 1. Return this's relevant global object's count queuing strategy size function.
        Ok(count_queuing_strategy_size_function(cx))
    }
}

/// The count queuing strategy size function of the global object of the realm of the call context,
/// https://streams.spec.whatwg.org/#count-queuing-strategy-size-function, which the first use in the realm creates.
fn count_queuing_strategy_size_function(cx: &CallContext) -> Gc<FunctionObject> {
    let data = realm_data(cx.vm(), cx.realm());
    if let Some(function) = data.count_queuing_strategy_size_function.get() {
        return function;
    }

    // 1. Let steps be the following steps:
    #[expect(
        clippy::unnecessary_wraps,
        reason = "the steps of a native function return a completion, whether or not they can throw"
    )]
    fn steps(_: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return 1.
        Ok(Value::from_i32(1))
    }

    // 2. Let F be ! CreateBuiltinFunction(steps, 0, "size", « », globalObject's relevant Realm).
    let function = RawNativeFunction::create(
        cx.vm(),
        raw_native!(steps),
        0,
        &PropertyKey::from_utf8("size"),
        Some(cx.realm()),
        None,
        None,
    );

    // 3. Set globalObject's count queuing strategy size function to a Function that represents a reference to F,
    //    with callback context equal to globalObject's relevant settings object.
    let function: Gc<FunctionObject> = function.upcast();
    data.count_queuing_strategy_size_function.set(Some(function));
    function
}
