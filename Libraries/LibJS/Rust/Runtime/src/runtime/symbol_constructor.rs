/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::runtime::symbol::{Kind, Symbol};

#[repr(C)]
#[derive(Trace)]
pub struct SymbolConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    SymbolConstructor,
    initialize: SymbolConstructor::initialize,
    call: SymbolConstructor::call,
    construct: SymbolConstructor::construct
);

impl SymbolConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<SymbolConstructor> {
        realm.create_object(
            vm,
            SymbolConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Symbol.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 20.4.2.9 Symbol.prototype, https://tc39.es/ecma262/#sec-symbol.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().symbol_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attributes = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.for_,
            raw_native!(SymbolConstructor::for_),
            1,
            attributes,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.keyFor,
            raw_native!(SymbolConstructor::key_for),
            1,
            attributes,
            None,
        );

        let symbols = vm.well_known_symbols();
        let well_known_symbols = [
            (&names.asyncDispose, symbols.async_dispose),
            (&names.asyncIterator, symbols.async_iterator),
            (&names.dispose, symbols.dispose),
            (&names.hasInstance, symbols.has_instance),
            (&names.isConcatSpreadable, symbols.is_concat_spreadable),
            (&names.iterator, symbols.iterator),
            (&names.match_, symbols.match_),
            (&names.matchAll, symbols.match_all),
            (&names.replace, symbols.replace),
            (&names.replaceAll, symbols.replace_all),
            (&names.search, symbols.search),
            (&names.species, symbols.species),
            (&names.split, symbols.split),
            (&names.toPrimitive, symbols.to_primitive),
            (&names.toStringTag, symbols.to_string_tag),
            (&names.unscopables, symbols.unscopables),
        ];
        for (name, symbol) in well_known_symbols {
            object.define_direct_property(vm, name, Value::from_symbol(symbol), PropertyAttributes::new(0));
        }

        object.define_direct_property(
            vm,
            &names.length,
            Value::from_i32(0),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 20.4.1.1 Symbol ( [ description ] ), https://tc39.es/ecma262/#sec-symbol-description
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        let description = vm.argument(0);

        // 2. If description is undefined, let descString be undefined.
        // 3. Else, let descString be ? ToString(description).
        let description_string = if description.is_undefined() {
            None
        } else {
            Some(description.to_utf16_string(vm)?)
        };

        // 4. Return a new Symbol whose [[Description]] is descString.
        Ok(Value::from_symbol(Symbol::create(vm, description_string, Kind::Unique)))
    }

    // 20.4.1.1 Symbol ( [ description ] ), https://tc39.es/ecma262/#sec-symbol-description
    fn construct(_: &NativeFunction, vm: &Vm, _: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        // 1. If NewTarget is not undefined, throw a TypeError exception.
        vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAConstructor, &[&"Symbol"])
    }

    // 20.4.2.2 Symbol.for ( key ), https://tc39.es/ecma262/#sec-symbol.for
    fn for_(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let stringKey be ? ToString(key).
        let string_key = vm.argument(0).to_utf16_string(vm)?;

        // 2. For each element e of the GlobalSymbolRegistry List, do
        let global_symbol_registry = vm.global_symbol_registry();
        let result = global_symbol_registry.get(&string_key);
        if let Some(symbol) = result {
            // a. If SameValue(e.[[Key]], stringKey) is true, return e.[[Symbol]].
            return Ok(Value::from_symbol(symbol));
        }

        // 3. Assert: GlobalSymbolRegistry does not currently contain an entry for stringKey.

        // 4. Let newSymbol be a new unique Symbol value whose [[Description]] value is stringKey.
        let new_symbol = Symbol::create(vm, Some(string_key.clone()), Kind::Global);

        // 5. Append the Record { [[Key]]: stringKey, [[Symbol]]: newSymbol } to the GlobalSymbolRegistry List.
        global_symbol_registry.set(&string_key, new_symbol);

        // 6. Return newSymbol.
        Ok(Value::from_symbol(new_symbol))
    }

    // 20.4.2.6 Symbol.keyFor ( sym ), https://tc39.es/ecma262/#sec-symbol.keyfor
    fn key_for(vm: &Vm) -> ThrowCompletionOr<Value> {
        let argument = vm.argument(0);

        // 1. If sym is not a Symbol, throw a TypeError exception.
        if !argument.is_symbol() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotASymbol, &[&argument]);
        }

        // 2. Return KeyForSymbol(sym).
        Ok(argument.as_symbol().key().map_or(Value::UNDEFINED, |key| {
            Value::from_string(PrimitiveString::create(vm, key))
        }))
    }
}
