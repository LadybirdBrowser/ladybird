/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Exception, https://webassembly.github.io/spec/js-api/#exceptions

use ak::Utf16String;
use libjs_rust::api::{CompactTraceback, Error, Gc, MarkedVec, Realm, Trace, Value, Vm};
use libwasm_engine::runtime::TagInstance;

use crate::bindings::interface::{Ref, Wrappable, allocate, wrap};
use crate::bindings::{CallContext, Exception as WebIdlException, WebIdlResult};
use crate::web_assembly::tag::Tag;
use crate::web_assembly::values::{to_js_value, to_webassembly_value};
use crate::web_assembly::{
    Agent, CellReference, EntityRef, WasmValues, host_object, implementation_of_value, set_host_object,
};

/// https://webassembly.github.io/spec/js-api/#dictdef-exceptionoptions
pub(crate) struct ExceptionOptions {
    pub(crate) trace_stack: bool,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Exception {
    // [[Address]]
    address: CellReference,
    // [[Type]]
    // NB: The payload of an exception never changes, so getArg() reads [[Payload]] from the store.
    tag: EntityRef<TagInstance>,
    // [[Stack]]
    #[gc(untraced)]
    stack: Option<Utf16String>,
}

impl Wrappable for Exception {}

impl Exception {
    // https://webassembly.github.io/spec/js-api/#dom-exception-exception
    pub(crate) fn construct(
        cx: &mut CallContext,
        exception_tag: Ref<Tag>,
        payload: MarkedVec<'_, Value>,
        options: ExceptionOptions,
    ) -> WebIdlResult<Ref<Self>> {
        let vm = cx.vm();
        let agent = Agent::the(vm);

        // 1. Let JSTagAddr be the result of getting the JavaScript exception tag.
        // 2. If exceptionTag.[[Address]] is equal to JSTagAddr,
        //     1. Throw a TypeError.
        if exception_tag.address() == agent.js_tag() {
            return Err(WebIdlException::type_error(
                "Cannot construct an exception with the JavaScript tag",
            ));
        }

        // 3. Let store be the surrounding agent's associated store.
        // 4. Let [types] → [] be tag_type(store, exceptionTag.[[Address]]).
        let types = agent.store().tag(exception_tag.address()).ty.params.clone();

        // 5. If types's size is not payload's size,
        //     1. Throw a TypeError.
        if types.len() != payload.len() {
            return Err(WebIdlException::TypeError(Utf16String::from_utf8(&format!(
                "The tag takes {} values, not {}",
                types.len(),
                payload.len()
            ))));
        }

        // 6. Let wasmPayload be « ».
        // NB: The converted values may refer to new cells, which the cell of the values keeps alive until they are in
        //     the exception.
        let wasm_payload = WasmValues::allocate(vm);

        // 7. For each value and resultType of payload and types, paired linearly,
        for (index, &result_type) in types.iter().enumerate() {
            // 1. If resultType is v128 or exnref,
            //     1. Throw a TypeError.
            // 2. Append ? ToWebAssemblyValue(value, resultType) to wasmPayload.
            // NB: ToWebAssemblyValue throws the TypeError for v128 and exnref.
            let value = to_webassembly_value(
                vm,
                payload.get(index).expect("the payload has a value per type"),
                result_type,
            )?;
            wasm_payload.0.borrow_mut().push(value);
        }

        // 8. Let (store, exceptionAddr) be exn_alloc(store, exceptionTag.[[Address]], wasmPayload).
        // 9. Set the surrounding agent's associated store to store.
        let exception = agent
            .store()
            .allocate_exception_with_values(exception_tag.address(), &wasm_payload.0.borrow());
        // NB: Using the Gc after the allocation keeps the payload where the conservative scan finds it until it is in
        //     the exception.
        core::hint::black_box(wasm_payload);

        // 10. Initialize this from exceptionAddr.
        // 11. If options["traceStack"] is true,
        //     1. Set this.[[Stack]] to either a DOMString representation of the current call stack or undefined.
        // 12. Otherwise,
        //     1. Set this.[[Stack]] to undefined.
        let stack = options.trace_stack.then(|| {
            let realm = vm.current_realm().expect("a constructor runs in a realm");
            Error::create(vm, realm).stack_string(CompactTraceback::No)
        });
        Ok(Self::initialize(vm, exception, stack))
    }

    // https://webassembly.github.io/spec/js-api/#initialize-an-exception-object
    fn initialize(vm: &Vm, exception: u64, stack: Option<Utf16String>) -> Ref<Self> {
        // 1. Let map be the surrounding agent's associated exception object cache.
        // 2. Assert: map[exnAddr] doesn't exist.
        let cell = exception as *mut u8;
        assert!(host_object::<()>(cell).is_none());

        // 3. Set exn.[[Address]] to exnAddr.
        // 4. Set map[exnAddr] to exn.
        // 5. Let store be the surrounding agent's associated store.
        // 6. Let tagaddr be exn_tag(store, exnAddr).
        // 7. Let payload be exn_read(store, exnAddr).
        // 8. Set exn.[[Type]] to tagaddr.
        // 9. Set exn.[[Payload]] to payload.
        let tag = Agent::the(vm).store().exception(exception).tag;
        let object = allocate(vm, |_| Self {
            address: CellReference(exception),
            tag: EntityRef(tag),
            stack,
        });
        set_host_object(cell, object);
        object
    }

    /// The wrapper of the Exception object of the exception in the realm,
    /// https://webassembly.github.io/spec/js-api/#create-an-exception-object
    pub(crate) fn wrapper_of_address(vm: &Vm, realm: Gc<Realm>, exception: u64) -> Value {
        // 1. Let map be the surrounding agent's associated exception object cache.
        // 2. If map[exnAddr] exists,
        //     1. Return map[exnAddr].
        // 3. Let exn be a new Exception.
        // 4. Initialize exn from exnAddr.
        let object = host_object(exception as *mut u8).unwrap_or_else(|| Self::initialize(vm, exception, None));
        Value::from_object(wrap(&CallContext::new(vm, realm), object))
    }

    /// The [[Address]] of the Exception object that the value is, if it is the wrapper of one.
    pub(crate) fn address_of_value(value: Value) -> Option<u64> {
        implementation_of_value::<Self>(value).map(|exception| exception.address.0)
    }

    // https://webassembly.github.io/spec/js-api/#dom-exception-getarg
    pub(crate) fn get_arg(&self, cx: &mut CallContext, exception_tag: Ref<Tag>, index: u32) -> WebIdlResult<Value> {
        let vm = cx.vm();
        let store = Agent::the(vm).store();

        // 1. If this.[[Type]] is not equal to exceptionTag.[[Address]],
        //     1. Throw a TypeError.
        if self.tag.0 != exception_tag.address() {
            return Err(WebIdlException::type_error("The exception does not have this tag"));
        }

        // 2. Let payload be this.[[Payload]].
        let payload = store.exception_values(self.address.0);

        // 3. If index ≥ payload's size,
        //     1. Throw a RangeError.
        let Some(&value) = payload.get(index as usize) else {
            return Err(WebIdlException::RangeError(Utf16String::from_utf8(&format!(
                "The exception has {} values",
                payload.len()
            ))));
        };

        // 4. Let [types] → [] be tag_type(store, exceptionTag.[[Address]]).
        // 5. If types[index] matches v128 or exnref,
        //     1. Throw a TypeError.
        // 6. Return ToJSValue(payload[index]).
        // NB: ToJSValue throws the TypeError for v128 and exnref.
        let ty = store.tag(exception_tag.address()).ty.params[index as usize];
        Ok(to_js_value(vm, value, ty)?)
    }

    // https://webassembly.github.io/spec/js-api/#dom-exception-is
    pub(crate) fn is(&self, _: &mut CallContext, exception_tag: Ref<Tag>) -> WebIdlResult<bool> {
        // 1. If this.[[Type]] is not equal to exceptionTag.[[Address]],
        //     1. Return false.
        // 2. Return true.
        Ok(self.tag.0 == exception_tag.address())
    }

    // https://webassembly.github.io/spec/js-api/#dom-exception-stack
    pub(crate) fn stack(&self, _: &mut CallContext) -> WebIdlResult<Option<Utf16String>> {
        // 1. Return this.[[Stack]].
        Ok(self.stack.clone())
    }
}
