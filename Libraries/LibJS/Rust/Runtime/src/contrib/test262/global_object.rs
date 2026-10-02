/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::contrib::test262::dollar_262_object::Dollar262Object;
use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::global_object::GlobalObject;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{ORDINARY_OBJECT_METHODS, allocate_object, define_object_class};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::standard_output::outln;
use crate::utf16::Utf16View;

/// JS::Test262::GlobalObject.
#[repr(C)]
#[derive(Trace)]
pub struct Test262GlobalObject {
    base: GlobalObject,
    dollar_262: Cell<Option<Gc<Dollar262Object>>>,
}

define_object_class!(Test262GlobalObject, extends: [GlobalObject, Object], methods: {
    initialize: Test262GlobalObject::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl Test262GlobalObject {
    /// Allocates the global object of `realm`, whose initialize() InitializeHostDefinedRealm runs once the realm has
    /// its global environment.
    pub fn allocate(vm: &Vm, realm: Gc<Realm>) -> Gc<Test262GlobalObject> {
        allocate_object(
            vm,
            Test262GlobalObject {
                base: GlobalObject::new(vm, Self::CLASS, realm),
                dollar_262: Cell::new(None),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let base_initialize = GlobalObject::CLASS
            .object_methods
            .expect("GlobalObject is an object class")
            .initialize;
        base_initialize(object, vm, realm);

        // SAFETY: Only Test262GlobalObject has these methods.
        let this = unsafe { &*core::ptr::from_ref(object).cast::<Test262GlobalObject>() };
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
    }

    pub fn dollar_262(&self) -> Gc<Dollar262Object> {
        self.dollar_262.get().expect("the test262 global object has its $262")
    }

    fn print(vm: &Vm) -> ThrowCompletionOr<Value> {
        let string = vm.argument(0).to_utf16_string(vm)?;
        outln(&Utf16View::of_string(&string).to_wtf8());
        Ok(Value::UNDEFINED)
    }
}
