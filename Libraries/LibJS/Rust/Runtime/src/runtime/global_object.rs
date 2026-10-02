/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ops::Deref;

use libjs_runtime_macros::Trace;

use crate::gc::class::{Class, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::object::{IntrinsicAccessor, MayInterfereWithIndexedPropertyAccess};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

/// The global object of a realm, which SetDefaultGlobalBindings gives the properties of clause 19. Hosts that need
/// more extend it.
#[repr(C)]
#[derive(Trace)]
pub struct GlobalObject {
    base: Object,
}

define_cell!(GlobalObject, Object, extends: [Object]);

impl Deref for GlobalObject {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl GlobalObject {
    /// GlobalObject(Realm&), for `class`, which is GlobalObject or a class that extends it.
    pub fn new(vm: &Vm, class: &'static Class, realm: Gc<Realm>) -> GlobalObject {
        let base = Object::new_global_object(vm, class, realm, MayInterfereWithIndexedPropertyAccess::No);
        base.set_prototype(vm, Some(realm.object_prototype()));
        GlobalObject { base }
    }
}

impl Object {
    pub fn is_global_object(&self) -> bool {
        self.is::<GlobalObject>()
    }
}

// 9.3.3 SetDefaultGlobalBindings ( realmRec ), https://tc39.es/ecma262/#sec-setdefaultglobalbindings
pub fn set_default_global_bindings(vm: &Vm, realm: Gc<Realm>) {
    let names = &vm.names;

    // 1. Let global be realmRec.[[GlobalObject]].
    let global = realm.global_object();

    // 2. For each property of the Global Object specified in clause 19, do
    //     a. Let name be the String value of the property name.
    //     b. Let desc be the fully populated data Property Descriptor for the property, containing the specified attributes for the property.
    //        For properties listed in 19.2, 19.3, or 19.4 the value of the [[Value]] attribute is the corresponding intrinsic object from realmRec.
    //     c. Perform ? DefinePropertyOrThrow(global, name, desc).
    //     NOTE: This function is infallible as we set properties directly; property clashes in global object construction are not expected.

    let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
    let none = PropertyAttributes::new(0);
    let define_intrinsic_accessor = |name: &PropertyKey, accessor: IntrinsicAccessor| {
        global.define_intrinsic_accessor(vm, name, attr, accessor);
    };

    // 19.2 Function Properties of the Global Object, https://tc39.es/ecma262/#sec-function-properties-of-the-global-object
    // NB: eval, isFinite, isNaN, parseFloat, parseInt, decodeURI, decodeURIComponent, encodeURI and encodeURIComponent
    //     come with the global object's functions.

    // 19.1 Value Properties of the Global Object, https://tc39.es/ecma262/#sec-value-properties-of-the-global-object
    global.define_direct_property(
        vm,
        &names.globalThis,
        Value::from_object(realm.global_environment().global_this_value()),
        attr,
    );
    global.define_direct_property(vm, &names.Infinity, Value::from_f64(f64::INFINITY), none);
    global.define_direct_property(vm, &names.NaN, Value::from_f64(f64::NAN), none);
    global.define_direct_property(vm, &names.undefined, Value::UNDEFINED, none);

    // 19.3 Constructor Properties of the Global Object, https://tc39.es/ecma262/#sec-constructor-properties-of-the-global-object
    // NB: The constructors of the builtins the runtime does not have yet are left out, keeping the order of the others.
    define_intrinsic_accessor(&names.AggregateError, |vm, realm| {
        Value::from_object(realm.intrinsics().aggregate_error_constructor(vm))
    });
    define_intrinsic_accessor(&names.Array, |vm, realm| {
        Value::from_object(realm.intrinsics().array_constructor(vm))
    });
    define_intrinsic_accessor(&names.BigInt, |vm, realm| {
        Value::from_object(realm.intrinsics().bigint_constructor(vm))
    });
    define_intrinsic_accessor(&names.Boolean, |vm, realm| {
        Value::from_object(realm.intrinsics().boolean_constructor(vm))
    });
    define_intrinsic_accessor(&names.Error, |vm, realm| {
        Value::from_object(realm.intrinsics().error_constructor(vm))
    });
    define_intrinsic_accessor(&names.EvalError, |vm, realm| {
        Value::from_object(realm.intrinsics().eval_error_constructor(vm))
    });
    define_intrinsic_accessor(&names.Function, |vm, realm| {
        Value::from_object(realm.intrinsics().function_constructor(vm))
    });
    define_intrinsic_accessor(&names.Iterator, |vm, realm| {
        Value::from_object(realm.intrinsics().iterator_constructor(vm))
    });
    define_intrinsic_accessor(&names.Number, |vm, realm| {
        Value::from_object(realm.intrinsics().number_constructor(vm))
    });
    define_intrinsic_accessor(&names.Object, |vm, realm| {
        Value::from_object(realm.intrinsics().object_constructor(vm))
    });
    define_intrinsic_accessor(&names.Proxy, |_, realm| {
        Value::from_object(realm.intrinsics().proxy_constructor())
    });
    define_intrinsic_accessor(&names.RangeError, |vm, realm| {
        Value::from_object(realm.intrinsics().range_error_constructor(vm))
    });
    define_intrinsic_accessor(&names.ReferenceError, |vm, realm| {
        Value::from_object(realm.intrinsics().reference_error_constructor(vm))
    });
    define_intrinsic_accessor(&names.String, |vm, realm| {
        Value::from_object(realm.intrinsics().string_constructor(vm))
    });
    define_intrinsic_accessor(&names.Symbol, |vm, realm| {
        Value::from_object(realm.intrinsics().symbol_constructor(vm))
    });
    define_intrinsic_accessor(&names.SyntaxError, |vm, realm| {
        Value::from_object(realm.intrinsics().syntax_error_constructor(vm))
    });
    define_intrinsic_accessor(&names.TypeError, |vm, realm| {
        Value::from_object(realm.intrinsics().type_error_constructor(vm))
    });
    define_intrinsic_accessor(&names.URIError, |vm, realm| {
        Value::from_object(realm.intrinsics().uri_error_constructor(vm))
    });

    // 19.4 Other Properties of the Global Object, https://tc39.es/ecma262/#sec-other-properties-of-the-global-object
    // NB: Atomics, Intl, JSON, Math and Temporal come with their builtins.
    define_intrinsic_accessor(&names.Reflect, |vm, realm| {
        Value::from_object(realm.intrinsics().reflect_object(vm))
    });

    // B.2.1 Additional Properties of the Global Object, https://tc39.es/ecma262/#sec-additional-properties-of-the-global-object
    // NB: escape and unescape come with the global object's functions.

    // Non-standard
    global.define_direct_property(
        vm,
        &names.InternalError,
        Value::from_object(realm.intrinsics().internal_error_constructor(vm)),
        attr,
    );
    // NB: console comes with the console object.

    // 3. Return unused.
}
