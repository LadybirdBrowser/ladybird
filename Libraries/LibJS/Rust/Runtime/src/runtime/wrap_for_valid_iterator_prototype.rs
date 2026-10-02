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
use crate::runtime::abstract_operations::{call, call_function_object};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::iterator::{Iterator, create_iterator_result_object};
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;

/// %WrapForValidIteratorPrototype%.
#[repr(C)]
#[derive(Trace)]
pub struct WrapForValidIteratorPrototype {
    base: Object,
}

define_object_class!(WrapForValidIteratorPrototype, extends: [Object], methods: {
    initialize: WrapForValidIteratorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

const DISPLAY_NAME: &str = "Iterator";

impl WrapForValidIteratorPrototype {
    // 27.1.3.2.1.1 The %WrapForValidIteratorPrototype% Object, https://tc39.es/ecma262/#sec-%wrapforvaliditeratorprototype%-object
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<WrapForValidIteratorPrototype> {
        realm.create_object(
            vm,
            WrapForValidIteratorPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().iterator_prototype(vm),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &vm.names.next,
            raw_native!(WrapForValidIteratorPrototype::next),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &vm.names.return_,
            raw_native!(WrapForValidIteratorPrototype::return_),
            0,
            attr,
            None,
        );
    }

    // 27.1.3.2.1.1.1 %WrapForValidIteratorPrototype%.next ( ), https://tc39.es/ecma262/#sec-%wrapforvaliditeratorprototype%.next
    fn next(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be this value.
        // 2. Perform ? RequireInternalSlot(O, [[Iterated]]).
        let object = typed_this_object::<Iterator>(vm, DISPLAY_NAME)?;

        // 3. Let iteratorRecord be O.[[Iterated]].
        let iterator_record = object.iterated();

        // 4. Return ? Call(iteratorRecord.[[NextMethod]], iteratorRecord.[[Iterator]]).
        call(
            vm,
            iterator_record.next_method(),
            Value::from_object(iterator_record.iterator()),
            &[],
        )
    }

    // 27.1.3.2.1.1.2 %WrapForValidIteratorPrototype%.return ( ), https://tc39.es/ecma262/#sec-%wrapforvaliditeratorprototype%.return
    fn return_(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be this value.
        // 2. Perform ? RequireInternalSlot(O, [[Iterated]]).
        let object = typed_this_object::<Iterator>(vm, DISPLAY_NAME)?;

        // 3. Let iterator be O.[[Iterated]].[[Iterator]].
        // 4. Assert: iterator is an Object.
        let iterator = Value::from_object(object.iterated().iterator());

        // 5. Let returnMethod be ? GetMethod(iterator, "return").
        let return_method = iterator.get_method(vm, &vm.names.return_)?;

        // 6. If returnMethod is undefined, then
        let Some(return_method) = return_method else {
            // a. Return CreateIterResultObject(undefined, true).
            let realm = vm
                .current_realm()
                .expect("a built-in function runs in an execution context with a realm");
            return Ok(Value::from_object(create_iterator_result_object(
                vm,
                realm,
                Value::UNDEFINED,
                true,
            )));
        };

        // 7. Return ? Call(returnMethod, iterator).
        call_function_object(vm, return_method, iterator, &[])
    }
}
