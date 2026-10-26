/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Global, https://webassembly.github.io/spec/js-api/#globals

use libjs_rust::api::{Trace, Value, Vm};
use libwasm_engine::runtime::{GlobalAddress, GlobalInstance};
use libwasm_engine::types::{GlobalType, HeapType, RefType, ValType};

use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::web_assembly::values::{default_webassembly_value, to_js_value, to_webassembly_value};
use crate::web_assembly::{Agent, EntityRef, host_object, implementation_of_value, set_host_object};

/// https://webassembly.github.io/spec/js-api/#enumdef-valuetype
#[derive(Clone, Copy)]
pub(crate) enum ValueType {
    I32,
    I64,
    F32,
    F64,
    V128,
    Externref,
    Anyfunc,
}

impl ValueType {
    // https://webassembly.github.io/spec/js-api/#tovaluetype
    pub(crate) fn to_value_type(self) -> ValType {
        let reference = |heap| ValType::Ref(RefType { nullable: true, heap });
        match self {
            Self::I32 => ValType::I32,
            Self::I64 => ValType::I64,
            Self::F32 => ValType::F32,
            Self::F64 => ValType::F64,
            Self::V128 => ValType::V128,
            Self::Anyfunc => reference(HeapType::Func),
            Self::Externref => reference(HeapType::Extern),
        }
    }
}

/// https://webassembly.github.io/spec/js-api/#dictdef-globaldescriptor
pub(crate) struct GlobalDescriptor {
    pub(crate) value: ValueType,
    pub(crate) mutable: bool,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Global {
    // [[Global]]
    address: EntityRef<GlobalInstance>,
}

impl Wrappable for Global {}

impl Global {
    // https://webassembly.github.io/spec/js-api/#dom-global-global
    pub(crate) fn construct(
        cx: &mut CallContext,
        descriptor: GlobalDescriptor,
        v: Option<Value>,
    ) -> WebIdlResult<Ref<Self>> {
        let vm = cx.vm();

        // 1. Let mutable be descriptor["mutable"].
        let mutable = descriptor.mutable;

        // 2. Let valuetype be ToValueType(descriptor["value"]).
        let value_type = descriptor.value.to_value_type();

        //     1. Throw a TypeError.
        if value_type == ValType::V128 {
            return Err(Exception::type_error("V128 is not supported as a global value type"));
        }

        // 4. If v is missing,
        //     1. Let value be DefaultValue(valuetype).
        //     2. Assert: value is not error.
        // 5. Otherwise,
        //     1. Let value be ? ToWebAssemblyValue(v, valuetype).
        let value = match v {
            None => default_webassembly_value(vm, value_type).expect("ToValueType gives types with a default value"),
            Some(v) => to_webassembly_value(vm, v, value_type)?,
        };

        // 6. If mutable is true, let globaltype be var valuetype; otherwise, let globaltype be const valuetype.
        // 7. Let store be the current agent’s associated store.
        // 8. Let (store, globaladdr) be global_alloc(store, globaltype, value).
        // 9. Set the current agent’s associated store to store.
        let global = Agent::the(vm).store().allocate_global(
            GlobalType {
                value: value_type,
                mutable,
            },
            value,
        );

        // 10. Initialize this from globaladdr.
        Ok(Self::initialize(vm, global))
    }

    // https://webassembly.github.io/spec/js-api/#initialize-a-global-object
    fn initialize(vm: &Vm, global: GlobalAddress) -> Ref<Self> {
        assert!(host_object::<()>(global.cell()).is_none());

        let object = allocate(vm, |_| Self {
            address: EntityRef(global),
        });

        set_host_object(global.cell(), object);
        object
    }

    /// The Global object of the global, https://webassembly.github.io/spec/js-api/#create-a-global-object
    pub(crate) fn from_address(vm: &Vm, global: GlobalAddress) -> Ref<Self> {
        if let Some(object) = host_object(global.cell()) {
            return object;
        }

        Self::initialize(vm, global)
    }

    pub(crate) fn from_value(value: Value) -> Option<Ref<Self>> {
        implementation_of_value(value)
    }

    pub(crate) fn address(&self) -> GlobalAddress {
        self.address.0
    }

    // https://webassembly.github.io/spec/js-api/#getglobalvalue
    fn get_global_value(&self, cx: &CallContext) -> WebIdlResult<Value> {
        // 1. Let store be the current agent’s associated store.
        // 2. Let globaladdr be global.[[Global]].
        // 3. Let globaltype be global_type(store, globaladdr).
        let global = Agent::the(cx.vm()).store().global(self.address());

        if global.ty.value == ValType::V128 {
            return Err(Exception::type_error("V128 is not supported as a global value type"));
        }

        // 5. Let value be global_read(store, globaladdr).
        Ok(to_js_value(cx.vm(), global.get(), global.ty.value)?)
    }

    // https://webassembly.github.io/spec/js-api/#dom-global-value
    pub(crate) fn value(&self, cx: &mut CallContext) -> WebIdlResult<Value> {
        self.get_global_value(cx)
    }

    // https://webassembly.github.io/spec/js-api/#dom-global-value
    pub(crate) fn set_value(&self, cx: &mut CallContext, value: Value) -> WebIdlResult<()> {
        // 1. Let store be the current agent’s associated store.
        // 2. Let globaladdr be this.[[Global]].
        // 3. Let mut valuetype be global_type(store, globaladdr).
        let ty = Agent::the(cx.vm()).store().global(self.address()).ty;

        if ty.value == ValType::V128 {
            return Err(Exception::type_error("Cannot set the value of a V128 global"));
        }

        // 5. If mut is const, throw a TypeError.
        if !ty.mutable {
            return Err(Exception::type_error("Cannot set the value of a const global"));
        }

        // 6. Let value be ? ToWebAssemblyValue(the given value, valuetype).
        let value = to_webassembly_value(cx.vm(), value, ty.value)?;

        // 7. Let store be global_write(store, globaladdr, value).
        // 8. If store is error, throw a RangeError exception.
        // 9. Set the current agent’s associated store to store.
        // NB: Writing cannot fail, as the value has the global's type.
        Agent::the(cx.vm()).store().global_mut(self.address()).set(value);
        Ok(())
    }

    // https://webassembly.github.io/spec/js-api/#dom-global-valueof
    pub(crate) fn value_of(&self, cx: &mut CallContext) -> WebIdlResult<Value> {
        self.get_global_value(cx)
    }
}
