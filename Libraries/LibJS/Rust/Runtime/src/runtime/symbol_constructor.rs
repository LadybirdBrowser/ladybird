/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::{NativeFunction, define_native_function_class};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;

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
        // 20.4.2.9 Symbol.prototype, https://tc39.es/ecma262/#sec-symbol.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().symbol_prototype(vm)),
            PropertyAttributes::new(0),
        );

        // NB: Symbol.for and Symbol.keyFor come with the Symbol builtins.

        let names = &vm.names;
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
            &vm.names.length,
            Value::from_i32(0),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 20.4.1.1 Symbol ( [ description ] ), https://tc39.es/ecma262/#sec-symbol-description
    fn call(_: &NativeFunction, _: &Vm) -> ThrowCompletionOr<Value> {
        unimplemented_runtime_function("SymbolConstructor::call, the [[Call]] of %Symbol%", 0)
    }

    // 20.4.1.1 Symbol ( [ description ] ), https://tc39.es/ecma262/#sec-symbol-description
    fn construct(_: &NativeFunction, _: &Vm, _: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        unimplemented_runtime_function("SymbolConstructor::construct, the [[Construct]] of %Symbol%", 0)
    }
}
