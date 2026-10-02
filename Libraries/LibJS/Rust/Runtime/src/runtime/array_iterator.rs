/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::length_of_array_like;
use crate::runtime::array::Array;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::iterator::BuiltinIteratorNext;
use crate::runtime::object::{
    MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, PropertyKind, define_object_class,
};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

/// An Array Iterator instance, the iterator CreateArrayIterator creates.
#[repr(C)]
#[derive(Trace)]
pub struct ArrayIterator {
    base: Object,
    array: Cell<Value>, // [[IteratedArrayLike]]
    #[gc(untraced)]
    iteration_kind: PropertyKind, // [[ArrayLikeIterationKind]]
    #[gc(untraced)]
    index: Cell<u64>, // [[ArrayLikeNextIndex]]
}

define_object_class!(ArrayIterator, extends: [Object], methods: {
    as_builtin_iterator_if_next_is_not_redefined: ArrayIterator::as_builtin_iterator_if_next_is_not_redefined,
    ..ORDINARY_OBJECT_METHODS
});

impl ArrayIterator {
    // 23.1.5.1 CreateArrayIterator ( array, kind ), https://tc39.es/ecma262/#sec-createarrayiterator
    pub fn create(vm: &Vm, realm: Gc<Realm>, array: Value, iteration_kind: PropertyKind) -> Gc<ArrayIterator> {
        // 1. Let iterator be OrdinaryObjectCreate(%ArrayIteratorPrototype%, « [[IteratedArrayLike]], [[ArrayLikeNextIndex]], [[ArrayLikeIterationKind]] »).
        // 2. Set iterator.[[IteratedArrayLike]] to array.
        // 3. Set iterator.[[ArrayLikeNextIndex]] to 0.
        // 4. Set iterator.[[ArrayLikeIterationKind]] to kind.
        // 5. Return iterator.
        realm.create_object(
            vm,
            ArrayIterator {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().array_iterator_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
                array: Cell::new(array),
                iteration_kind,
                index: Cell::new(0),
            },
        )
    }

    fn as_builtin_iterator_if_next_is_not_redefined(_: &Object, next_method: Value) -> Option<BuiltinIteratorNext> {
        // NB: Only functions are native functions, so this checks is_function() where C++ checks is_object().
        if next_method.is_function() {
            let next_function = next_method.as_function();
            if next_function.as_native_function().is_some() && next_function.is_array_prototype_next_builtin() {
                return Some(ArrayIterator::builtin_next);
            }
        }
        None
    }

    fn builtin_next(object: &Object, vm: &Vm, done: &mut bool, value: &mut Value) -> ThrowCompletionOr<()> {
        object
            .as_gc()
            .downcast::<ArrayIterator>()
            .expect("the builtin step of an ArrayIterator steps an ArrayIterator")
            .next(vm, done, value)
    }

    pub fn next(&self, vm: &Vm, done: &mut bool, value: &mut Value) -> ThrowCompletionOr<()> {
        // 1. Let O be the this value.
        // 2. If O is not an Object, throw a TypeError exception.
        // 3. If O does not have all of the internal slots of an Array Iterator Instance (23.1.5.3), throw a TypeError exception.

        // 4. Let array be O.[[IteratedArrayLike]].
        let target_array = self.array.get();

        // 5. If array is undefined, return CreateIteratorResultObject(undefined, true).
        if target_array.is_undefined() {
            *value = Value::UNDEFINED;
            *done = true;
            return Ok(());
        }

        assert!(target_array.is_object());
        let array = target_array.as_object();

        // 6. Let index be O.[[ArrayLikeNextIndex]].
        let index = self.index.get();

        // 7. Let kind be O.[[ArrayLikeIterationKind]].
        let kind = self.iteration_kind;

        // 8. If array has a [[TypedArrayName]] internal slot, then
        let length = if array.is_typed_array() {
            // a. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(array, SEQ-CST).
            // b. If IsTypedArrayOutOfBounds(taRecord) is true, throw a TypeError exception.
            // c. Let len be TypedArrayLength(taRecord).
            unimplemented_runtime_function(
                "TypedArrayLength of a typed array record, for %ArrayIteratorPrototype%.next, which comes with typed \
                 arrays",
                0,
            )
        }
        // 9. Else,
        else {
            // a. Let len be ? LengthOfArrayLike(array).
            length_of_array_like(vm, &array)?
        };

        // 10. If index ≥ len, then
        if index >= length {
            // a. Set O.[[IteratedArrayLike]] to undefined.
            self.array.set(Value::UNDEFINED);

            // b. Return CreateIteratorResultObject(undefined, true).
            *value = Value::UNDEFINED;
            *done = true;
            return Ok(());
        }

        // 11. Set O.[[ArrayLikeNextIndex]] to index + 1.
        self.index.set(index + 1);

        // 12. Let indexNumber be 𝔽(index).
        // NB: The C++ runtime makes the index an i32, which wraps for indices past 2^31 - 1.
        let index_number = Value::from_i32(index as i32);

        // 13. If kind is KEY, then
        let result = if kind == PropertyKind::Key {
            // a. Let result be indexNumber.
            index_number
        }
        // 14. Else,
        else {
            // a. Let elementKey be ! ToString(indexNumber).
            // b. Let elementValue be ? Get(array, elementKey).
            let element_value = 'element_value: {
                // OPTIMIZATION: For objects that don't interfere with indexed property access, we try looking directly at storage.
                // NB: The C++ runtime passes the index to the storage as a u32, which truncates indices past 2^32 - 1.
                let storage_index = index as u32;
                if !array.may_interfere_with_indexed_property_access()
                    && array.indexed_has(storage_index)
                    && let Some(element) = array.indexed_get(storage_index)
                    && !element.value.is_accessor()
                {
                    break 'element_value element.value;
                }

                array.get(vm, &PropertyKey::from_number(index))?
            };

            // c. If kind is VALUE, then
            if kind == PropertyKind::Value {
                // i. Let result be elementValue.
                element_value
            }
            // d. Else,
            else {
                // i. Assert: kind is KEY+VALUE.
                assert!(kind == PropertyKind::KeyAndValue);

                // ii. Let result be CreateArrayFromList(« indexNumber, elementValue »).
                let realm = vm.current_realm().expect("an iterator steps in a realm");
                Value::from_object(Array::create_from(vm, realm, &[index_number, element_value]))
            }
        };

        // 15. Return CreateIteratorResultObject(result, false).
        *value = result;
        Ok(())
    }
}
