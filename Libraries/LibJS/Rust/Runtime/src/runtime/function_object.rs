/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use ak::Utf16String;
use libjs_abi::Builtin;

use crate::gc::class::{Class, define_cell};
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
pub use crate::layout::function_object::FunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::class_field_definition::ClassElementName;
use crate::runtime::completion::Must;
use crate::runtime::native_function::NativeFunction;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, ObjectMethods};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::realm::Realm;
use crate::utf16::Utf16View;

/// The internal methods of FunctionObject, which declares [[Call]] and leaves it to the classes extending it.
pub static FUNCTION_OBJECT_METHODS: ObjectMethods = ObjectMethods {
    ..ORDINARY_OBJECT_METHODS
};

define_cell!(FunctionObject, Object, extends: [Object], methods: FUNCTION_OBJECT_METHODS);

// SAFETY: The builtin is a number; everything else is the Object's.
unsafe impl Trace for FunctionObject {
    fn trace(&self, visitor: &mut Visitor) {
        self.base.trace(visitor);
    }
}

impl Deref for FunctionObject {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl FunctionObject {
    pub fn new_with_realm_and_prototype(
        vm: &Vm,
        class: &'static Class,
        realm: Gc<Realm>,
        prototype: Option<Gc<Object>>,
        may_interfere_with_indexed_property_access: MayInterfereWithIndexedPropertyAccess,
    ) -> FunctionObject {
        let object = Object::new_with_realm_and_prototype(
            vm,
            class,
            realm,
            prototype,
            may_interfere_with_indexed_property_access,
        );
        object.set_is_function();
        Self::from_object(object)
    }

    pub fn new_with_prototype(
        vm: &Vm,
        class: &'static Class,
        prototype: Gc<Object>,
        may_interfere_with_indexed_property_access: MayInterfereWithIndexedPropertyAccess,
    ) -> FunctionObject {
        let object = Object::new_with_prototype(vm, class, prototype, may_interfere_with_indexed_property_access);
        object.set_is_function();
        Self::from_object(object)
    }

    fn from_object(object: Object) -> FunctionObject {
        FunctionObject {
            base: object,
            builtin: Cell::new(0),
            has_builtin: Cell::new(false),
        }
    }

    pub fn builtin(&self) -> Option<Builtin> {
        if !self.has_builtin.get() {
            return None;
        }
        Some(Builtin::from_u8(self.builtin.get()).expect("a function's builtin is a Builtin"))
    }

    pub fn set_builtin(&self, builtin: Option<Builtin>) {
        self.builtin.set(builtin.map_or(0, |builtin| builtin as u8));
        self.has_builtin.set(builtin.is_some());
    }

    pub fn is_array_prototype_next_builtin(&self) -> bool {
        self.builtin() == Some(Builtin::ArrayIteratorPrototypeNext)
    }

    pub fn is_map_prototype_next_builtin(&self) -> bool {
        self.builtin() == Some(Builtin::MapIteratorPrototypeNext)
    }

    pub fn is_set_prototype_next_builtin(&self) -> bool {
        self.builtin() == Some(Builtin::SetIteratorPrototypeNext)
    }

    pub fn is_string_prototype_next_builtin(&self) -> bool {
        self.builtin() == Some(Builtin::StringIteratorPrototypeNext)
    }

    // [[Realm]]
    pub fn realm(&self) -> Option<Gc<Realm>> {
        self.function_realm()
    }

    // 10.2.9 SetFunctionName ( F, name [ , prefix ] ), https://tc39.es/ecma262/#sec-setfunctionname
    pub fn make_function_name(
        &self,
        vm: &Vm,
        name_arg: &ClassElementName,
        prefix: Option<&str>,
    ) -> Gc<PrimitiveString> {
        let mut name: Utf16String = match name_arg {
            // 2. If Type(name) is Symbol, then
            ClassElementName::PropertyKey(property_key) if property_key.is_symbol() => {
                // a. Let description be name's [[Description]] value.
                let symbol = property_key.as_symbol();
                match symbol.description() {
                    // b. If description is undefined, set name to the empty String.
                    None => Utf16String::default(),
                    // c. Else, set name to the string-concatenation of "[", description, and "]".
                    Some(description) => {
                        let mut code_units: Vec<u16> = "[".encode_utf16().collect();
                        Utf16View::of_string(description).append_to(&mut code_units);
                        code_units.extend("]".encode_utf16());
                        Utf16String::from_utf16(&code_units)
                    }
                }
            }
            // 3. Else if name is a Private Name, then
            ClassElementName::PrivateName(private_name) => {
                // a. Set name to name.[[Description]].
                Utf16String::from(&private_name.description)
            }
            // NOTE: This is necessary as we use a different parameter name.
            ClassElementName::PropertyKey(property_key) => property_key.to_utf16_string(),
        };

        // 4. If F has an [[InitialName]] internal slot, then
        let native_function = self.as_native_function();

        if let Some(native_function) = native_function {
            // a. Set F.[[InitialName]] to name.
            native_function.set_initial_name(Utf16View::of_string(&name).to_utf16_fly_string());
        }

        // 5. If prefix is present, then
        if let Some(prefix) = prefix {
            // a. Set name to the string-concatenation of prefix, the code unit 0x0020 (SPACE), and name.
            let mut code_units: Vec<u16> = prefix.encode_utf16().collect();
            code_units.push(u16::from(b' '));
            Utf16View::of_string(&name).append_to(&mut code_units);
            name = Utf16String::from_utf16(&code_units);

            // b. If F has an [[InitialName]] internal slot, then
            if let Some(native_function) = native_function {
                // i. Optionally, set F.[[InitialName]] to name.
                native_function.set_initial_name(Utf16View::of_string(&name).to_utf16_fly_string());
            }
        }

        PrimitiveString::create(vm, name)
    }

    // 10.2.9 SetFunctionName ( F, name [ , prefix ] ), https://tc39.es/ecma262/#sec-setfunctionname
    pub fn set_function_name(&self, vm: &Vm, name_arg: &ClassElementName, prefix: Option<&str>) {
        // 1. Assert: F is an extensible object that does not have a "name" own property.
        assert!(self.extensible());
        assert!(!self.storage_has(&vm.names.name));

        let name = self.make_function_name(vm, name_arg, prefix);

        // 6. Perform ! DefinePropertyOrThrow(F, "name", PropertyDescriptor { [[Value]]: name, [[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: true }).
        let mut descriptor = PropertyDescriptor {
            value: Some(Value::from_string(name)),
            writable: Some(false),
            enumerable: Some(false),
            configurable: Some(true),
            ..Default::default()
        };
        self.define_property_or_throw(vm, &vm.names.name, &mut descriptor)
            .must();

        // 7. Return unused.
    }

    // 10.2.10 SetFunctionLength ( F, length ), https://tc39.es/ecma262/#sec-setfunctionlength
    pub fn set_function_length(&self, vm: &Vm, length: f64) {
        // "length (a non-negative integer or +∞)"
        assert!(length.trunc() == length || length == f64::INFINITY);

        // 1. Assert: F is an extensible object that does not have a "length" own property.
        assert!(self.extensible());
        assert!(!self.storage_has(&vm.names.length));

        // 2. Perform ! DefinePropertyOrThrow(F, "length", PropertyDescriptor { [[Value]]: 𝔽(length), [[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: true }).
        let mut descriptor = PropertyDescriptor {
            value: Some(Value::from_f64(length)),
            writable: Some(false),
            enumerable: Some(false),
            configurable: Some(true),
            ..Default::default()
        };
        self.define_property_or_throw(vm, &vm.names.length, &mut descriptor)
            .must();

        // 3. Return unused.
    }

    /// The function as a NativeFunction, if it is one.
    pub fn as_native_function(&self) -> Option<&NativeFunction> {
        // SAFETY: A NativeFunction starts with its FunctionObject.
        self.is::<NativeFunction>()
            .then(|| unsafe { &*core::ptr::from_ref(self).cast::<NativeFunction>() })
    }
}
