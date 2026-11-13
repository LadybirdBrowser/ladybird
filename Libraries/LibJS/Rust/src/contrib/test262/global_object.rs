/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use std::time::Duration;

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

#[cfg(unix)]
use crate::contrib::test262::agents::{self, AgentRole};
use crate::contrib::test262::dollar_262_object::Dollar262Object;
use crate::gc::class::{GcCell, cast_ref, define_cell};
use crate::gc::heap_function::create_heap_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::call;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::global_object::GlobalObject;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{ORDINARY_OBJECT_METHODS, allocate_object, define_object_class};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::standard_output::outln;
use crate::utf16::Utf16View;

/// The global object test262 runs its tests with.
#[repr(C)]
#[derive(Trace)]
pub struct Test262GlobalObject {
    base: GlobalObject,
    dollar_262: Cell<Option<Gc<Dollar262Object>>>,
    async_test_complete: Cell<bool>,
}

define_object_class!(Test262GlobalObject, extends: [GlobalObject, Object], methods: {
    initialize: Test262GlobalObject::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl Test262GlobalObject {
    /// Allocates the global object of `realm`, whose initialize() InitializeHostDefinedRealm runs once the realm has
    /// its global environment.
    pub fn allocate(vm: &Vm, realm: Gc<Realm>) -> Gc<Test262GlobalObject> {
        allocate_object(vm, |_| Test262GlobalObject {
            base: GlobalObject::new(vm, Self::CLASS, realm),
            dollar_262: Cell::new(None),
            async_test_complete: Cell::new(false),
        })
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let base_initialize = GlobalObject::CLASS
            .object_methods
            .expect("GlobalObject is an object class")
            .initialize;
        base_initialize(object, vm, realm);

        let this: &Test262GlobalObject = cast_ref(object);
        let dollar_262 = Dollar262Object::create(vm, realm);
        this.dollar_262.set(Some(dollar_262));

        // https://github.com/tc39/test262/blob/master/INTERPRETING.md#host-defined-functions
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &PropertyKey::from(Utf16FlyString::from_utf8("print")),
            raw_native!(Test262GlobalObject::print),
            1,
            attr,
            None,
        );
        object.define_direct_property(
            vm,
            &PropertyKey::from(Utf16FlyString::from_utf8("$262")),
            Value::from_object(dollar_262),
            attr,
        );
        object.define_native_function(
            vm,
            realm,
            &PropertyKey::from_utf8("setTimeout"),
            raw_native!(Test262GlobalObject::set_timeout),
            2,
            attr,
            None,
        );
    }

    pub fn dollar_262(&self) -> Gc<Dollar262Object> {
        self.dollar_262.get().expect("the test262 global object has its $262")
    }

    pub fn async_test_complete(&self) -> bool {
        self.async_test_complete.get()
    }

    fn set_timeout(vm: &Vm) -> ThrowCompletionOr<Value> {
        let callback = vm.argument(0);
        if !callback.is_function() {
            return vm.throw_completion_with_message(
                ErrorKind::TypeError,
                "setTimeout requires a callable callback".to_string(),
            );
        }
        let delay = vm.argument(1).to_number(vm)?.as_f64().max(0.0);
        let delay = Duration::try_from_secs_f64(delay / 1000.0).unwrap_or_default();
        let job = create_heap_function(vm, callback, |vm, &callback| call(vm, callback, Value::UNDEFINED, &[]));
        vm.enqueue_timeout_job(job, delay);
        Ok(Value::UNDEFINED)
    }

    fn print(vm: &Vm) -> ThrowCompletionOr<Value> {
        let string = vm.argument(0).to_utf16_string(vm)?;
        // NB: What a test prints is its result, which agents have no say in.
        #[cfg(unix)]
        if agents::role() == AgentRole::Agent {
            return Ok(Value::UNDEFINED);
        }
        let bytes = Utf16View::of_string(&string).to_wtf8();
        if bytes.starts_with(b"Test262:AsyncTestComplete") || bytes.starts_with(b"Test262:AsyncTestFailure") {
            let realm = vm.current_realm().expect("print has a current realm");
            let global_object = realm.global_object();
            let global: &Test262GlobalObject = cast_ref(&*global_object);
            global.async_test_complete.set(true);
        }
        outln(&bytes);
        Ok(Value::UNDEFINED)
    }
}
