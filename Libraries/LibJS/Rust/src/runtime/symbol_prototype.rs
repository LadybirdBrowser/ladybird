/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::symbol::Symbol;
use crate::runtime::symbol_object::SymbolObject;

/// %Symbol.prototype%.
#[repr(C)]
#[derive(Trace)]
pub struct SymbolPrototype {
    base: Object,
}

define_object_class!(SymbolPrototype, extends: [Object], methods: {
    initialize: SymbolPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl SymbolPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<SymbolPrototype> {
        realm.create_object(
            vm,
            SymbolPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;
        let attributes = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.toString,
            raw_native!(SymbolPrototype::to_string),
            0,
            attributes,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.valueOf,
            raw_native!(SymbolPrototype::value_of),
            0,
            attributes,
            None,
        );
        object.define_native_accessor(
            vm,
            realm,
            &names.description,
            raw_native!(SymbolPrototype::description_getter),
            None,
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
        object.define_native_function(
            vm,
            realm,
            &PropertyKey::from(vm.well_known_symbols().to_primitive),
            raw_native!(SymbolPrototype::symbol_to_primitive),
            1,
            PropertyAttributes::new(Attribute::CONFIGURABLE),
            None,
        );

        // 20.4.3.6 Symbol.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-symbol.prototype-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("Symbol"),
            )),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 20.4.3.2 get Symbol.prototype.description, https://tc39.es/ecma262/#sec-symbol.prototype.description
    fn description_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let s be the this value.
        // 2. Let sym be ? thisSymbolValue(s).
        let symbol = this_symbol_value(vm, vm.this_value())?;

        // 3. Return sym.[[Description]].
        let description = symbol.description().cloned();
        Ok(description.map_or(Value::UNDEFINED, |description| {
            Value::from_string(PrimitiveString::create(vm, description))
        }))
    }

    // 20.4.3.3 Symbol.prototype.toString ( ), https://tc39.es/ecma262/#sec-symbol.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let sym be ? thisSymbolValue(this value).
        let symbol = this_symbol_value(vm, vm.this_value())?;

        // 2. Return SymbolDescriptiveString(sym).
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            symbol.descriptive_string(),
        )))
    }

    // 20.4.3.4 Symbol.prototype.valueOf ( ), https://tc39.es/ecma262/#sec-symbol.prototype.valueof
    fn value_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return ? thisSymbolValue(this value).
        Ok(Value::from_symbol(this_symbol_value(vm, vm.this_value())?))
    }

    // 20.4.3.5 Symbol.prototype [ @@toPrimitive ] ( hint ), https://tc39.es/ecma262/#sec-symbol.prototype-@@toprimitive
    fn symbol_to_primitive(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return ? thisSymbolValue(this value).
        // NOTE: The argument is ignored.
        Ok(Value::from_symbol(this_symbol_value(vm, vm.this_value())?))
    }
}

// thisSymbolValue ( value ), https://tc39.es/ecma262/#thissymbolvalue
fn this_symbol_value(vm: &Vm, value: Value) -> ThrowCompletionOr<Gc<Symbol>> {
    // 1. If value is a Symbol, return value.
    if value.is_symbol() {
        return Ok(value.as_symbol());
    }

    // 2. If value is an Object and value has a [[SymbolData]] internal slot, then
    if value.is_object()
        && let Some(symbol) = value.as_object().downcast::<SymbolObject>()
    {
        // a. Let s be value.[[SymbolData]].
        // b. Assert: s is a Symbol.
        // c. Return s.
        return Ok(symbol.primitive_symbol());
    }

    // 3. Throw a TypeError exception.
    vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"Symbol"])
}
