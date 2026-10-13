/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ByteLengthQueuingStrategy, https://streams.spec.whatwg.org/#blqs-class

use libjs_rust::api::{
    FunctionObject, Gc, PropertyKey, RawNativeFunction, ThrowCompletionOr, Trace, Value, Vm, raw_native,
};

use crate::bindings::interface::Wrappable;
use crate::bindings::realm_data::realm_data;
use crate::bindings::{CallContext, WebIdlResult};
use crate::streams::queuing_strategy_init::QueuingStrategyInit;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ByteLengthQueuingStrategy {
    high_water_mark: f64,
}

impl Wrappable for ByteLengthQueuingStrategy {}

#[expect(
    clippy::unnecessary_wraps,
    reason = "the bindings call every method of an implementation for its WebIDL result, whether or not it can fail"
)]
impl ByteLengthQueuingStrategy {
    // https://streams.spec.whatwg.org/#blqs-constructor
    pub(crate) fn construct(_: &mut CallContext, init: QueuingStrategyInit) -> WebIdlResult<Self> {
        // 1. Set this.[[highWaterMark]] to init["highWaterMark"].
        Ok(Self {
            high_water_mark: init.high_water_mark,
        })
    }

    // https://streams.spec.whatwg.org/#blqs-high-water-mark
    pub(crate) fn high_water_mark(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        // The highWaterMark getter steps are:
        // 1. Return this.[[highWaterMark]].
        Ok(self.high_water_mark)
    }

    // https://streams.spec.whatwg.org/#blqs-size
    pub(crate) fn size(&self, cx: &mut CallContext) -> WebIdlResult<Gc<FunctionObject>> {
        // 1. Return this's relevant global object's byte length queuing strategy size function.
        Ok(byte_length_queuing_strategy_size_function(cx))
    }
}

/// The byte length queuing strategy size function of the global object of the realm of the call context,
/// https://streams.spec.whatwg.org/#byte-length-queuing-strategy-size-function, which the first use in the realm
/// creates.
fn byte_length_queuing_strategy_size_function(cx: &CallContext) -> Gc<FunctionObject> {
    let data = realm_data(cx.vm(), cx.realm());
    if let Some(function) = data.byte_length_queuing_strategy_size_function.get() {
        return function;
    }

    // 1. Let steps be the following steps, given chunk:
    fn steps(vm: &Vm) -> ThrowCompletionOr<Value> {
        let chunk = vm.argument(0);

        // 1. Return ? GetV(chunk, "byteLength").
        chunk.get(vm, &vm.names.byteLength)
    }

    // 2. Let F be ! CreateBuiltinFunction(steps, 1, "size", « », globalObject's relevant Realm).
    let function = RawNativeFunction::create(
        cx.vm(),
        raw_native!(steps),
        1,
        &PropertyKey::from_utf8("size"),
        Some(cx.realm()),
        None,
        None,
    );

    // 3. Set globalObject's byte length queuing strategy size function to a Function that represents a reference
    //    to F, with callback context equal to globalObject's relevant settings object.
    let function: Gc<FunctionObject> = function.upcast();
    data.byte_length_queuing_strategy_size_function.set(Some(function));
    function
}
