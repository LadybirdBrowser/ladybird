/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{call, call_function_object};
use crate::runtime::completion::{Completion, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::iterator::{get_iterator_from_method, iterator_close, iterator_step_value};
use crate::runtime::keyed_collections::canonicalize_keyed_collection_key;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{
    MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, PropertyKind, define_object_class,
};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;
use crate::runtime::set::{Set, get_set_record, set_data_has};
use crate::runtime::set_iterator::SetIterator;

/// %Set.prototype%.
#[repr(C)]
#[derive(Trace)]
pub struct SetPrototype {
    base: Object,
    values_function: Cell<Option<Gc<FunctionObject>>>,
}

define_object_class!(SetPrototype, extends: [Object], methods: {
    initialize: SetPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_set(vm: &Vm) -> ThrowCompletionOr<Gc<Set>> {
    typed_this_object::<Set>(vm, "Set")
}

fn current_realm(vm: &Vm) -> Gc<Realm> {
    vm.current_realm().expect("a builtin runs in a realm")
}

impl SetPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<SetPrototype> {
        realm.create_object(
            vm,
            SetPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
                values_function: Cell::new(None),
            },
        )
    }

    /// The original Set.prototype.values, which is also the original Set.prototype.keys and
    /// Set.prototype[%Symbol.iterator%].
    pub fn values_function(&self) -> Gc<FunctionObject> {
        self.values_function
            .get()
            .expect("%Set.prototype% defines values when it is initialized")
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |name: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, name, function, length, attr, None);
        };

        define_native_function(&names.add, raw_native!(SetPrototype::add), 1);
        define_native_function(&names.clear, raw_native!(SetPrototype::clear), 0);
        define_native_function(&names.delete_, raw_native!(SetPrototype::delete_), 1);
        define_native_function(&names.difference, raw_native!(SetPrototype::difference), 1);
        define_native_function(&names.entries, raw_native!(SetPrototype::entries), 0);
        define_native_function(&names.forEach, raw_native!(SetPrototype::for_each), 1);
        define_native_function(&names.has, raw_native!(SetPrototype::has), 1);
        define_native_function(&names.intersection, raw_native!(SetPrototype::intersection), 1);
        define_native_function(&names.isDisjointFrom, raw_native!(SetPrototype::is_disjoint_from), 1);
        define_native_function(&names.isSubsetOf, raw_native!(SetPrototype::is_subset_of), 1);
        define_native_function(&names.isSupersetOf, raw_native!(SetPrototype::is_superset_of), 1);
        object.define_native_accessor(
            vm,
            realm,
            &names.size,
            raw_native!(SetPrototype::size_getter),
            None,
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
        define_native_function(
            &names.symmetricDifference,
            raw_native!(SetPrototype::symmetric_difference),
            1,
        );
        define_native_function(&names.union_, raw_native!(SetPrototype::union_), 1);
        define_native_function(&names.values, raw_native!(SetPrototype::values), 0);
        let values_function = object.get_without_side_effects(vm, &names.values).as_function();
        object
            .as_gc()
            .downcast::<SetPrototype>()
            .expect("initializing %Set.prototype%")
            .values_function
            .set(Some(values_function));

        object.define_direct_property(vm, &names.keys, Value::from_object(values_function), attr);

        // 24.2.3.18 Set.prototype [ @@iterator ] ( ), https://tc39.es/ecma262/#sec-set.prototype-@@iterator
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().iterator),
            Value::from_object(values_function),
            attr,
        );

        // 24.2.3.19 Set.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-set.prototype-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(vm, names.Set.as_string())),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 24.2.3.1 Set.prototype.add ( value ), https://tc39.es/ecma262/#sec-set.prototype.add
    fn add(vm: &Vm) -> ThrowCompletionOr<Value> {
        let value = vm.argument(0);

        // 1. Let S be the this value.
        // 2. Perform ? RequireInternalSlot(S, [[SetData]]).
        let set = typed_this_set(vm)?;

        // 3. Set value to CanonicalizeKeyedCollectionKey(value).
        let value = canonicalize_keyed_collection_key(value);

        // 4. For each element e of S.[[SetData]], do
        //     a. If e is not empty and SameValue(e, value) is true, then
        //         i. Return S.
        // 5. Append value to S.[[SetData]].
        set.set_add(vm, value);

        // 6. Return S.
        Ok(Value::from_object(set))
    }

    // 24.2.3.2 Set.prototype.clear ( ), https://tc39.es/ecma262/#sec-set.prototype.clear
    fn clear(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let S be the this value.
        // 2. Perform ? RequireInternalSlot(S, [[SetData]]).
        let set = typed_this_set(vm)?;

        // 3. For each element e of S.[[SetData]], do
        //     a. Replace the element of S.[[SetData]] whose value is e with an element whose value is empty.
        set.set_clear(vm);

        // 4. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 24.2.3.4 Set.prototype.delete ( value ), https://tc39.es/ecma262/#sec-set.prototype.delete
    fn delete_(vm: &Vm) -> ThrowCompletionOr<Value> {
        let value = vm.argument(0);

        // 1. Let S be the this value.
        // 2. Perform ? RequireInternalSlot(S, [[SetData]]).
        let set = typed_this_set(vm)?;

        // 3. Set value to CanonicalizeKeyedCollectionKey(value).
        let value = canonicalize_keyed_collection_key(value);

        // 4. For each element e of S.[[SetData]], do
        //     a. If e is not empty and SameValue(e, value) is true, then
        //         i. Replace the element of S.[[SetData]] whose value is e with an element whose value is empty.
        //         ii. Return true.
        // 5. Return false.
        Ok(Value::from_bool(set.set_remove(vm, value)))
    }

    // 24.2.4.5 Set.prototype.difference ( other ), https://tc39.es/ecma262/#sec-set.prototype.difference
    fn difference(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
        let set = typed_this_set(vm)?;

        // 3. Let otherRec be ? GetSetRecord(other).
        let other_record = get_set_record(vm, vm.argument(0))?;

        // 4. Let resultSetData be a copy of O.[[SetData]].
        let result = set.copy(vm);

        // 5. If SetDataSize(O.[[SetData]]) ≤ otherRec.[[Size]], then
        if set.set_size() as f64 <= other_record.size {
            // a. Let thisSize be the number of elements in O.[[SetData]].
            // b. Let index be 0.
            // c. Repeat, while index < thisSize,
            let keys = MarkedVec::with_capacity(vm, result.set_size());
            result.for_each_value(|key| keys.push(key));

            for index in 0..keys.len() {
                let key = keys.get(index).expect("the key was just collected");

                // i. Let e be resultSetData[index].
                // ii. If e is not EMPTY, then
                //     1. Let inOther be ToBoolean(? Call(otherRec.[[Has]], otherRec.[[SetObject]], « e »)).
                let in_other = call_function_object(
                    vm,
                    other_record.has,
                    Value::from_object(other_record.set_object),
                    &[key],
                )?
                .to_boolean();

                //     2. If inOther is true, then
                if in_other {
                    // a. Set resultSetData[index] to EMPTY.
                    result.set_remove(vm, key);
                }

                // iii. Set index to index + 1.
            }
        }
        // 6. Else,
        else {
            // a. Let keysIter be ? GetIteratorFromMethod(otherRec.[[SetObject]], otherRec.[[Keys]]).
            let keys_iterator =
                get_iterator_from_method(vm, Value::from_object(other_record.set_object), other_record.keys)?;

            // b. Let next be NOT-STARTED.
            // c. Repeat, while next is not DONE,
            // i. Set next to ? IteratorStepValue(keysIter).
            // ii. If next is not DONE, then
            while let Some(next) = iterator_step_value(vm, &keys_iterator)? {
                // 1. Set next to CanonicalizeKeyedCollectionKey(next).
                let next = canonicalize_keyed_collection_key(next);

                // 2. Let valueIndex be SetDataIndex(resultSetData, next).
                // 3. If valueIndex is not NOT-FOUND, then
                if result.set_has(next) {
                    // a. Set resultSetData[valueIndex] to EMPTY.
                    result.set_remove(vm, next);
                }
            }
        }

        // 7. Let result be OrdinaryObjectCreate(%Set.prototype%, « [[SetData]] »).
        // 8. Set result.[[SetData]] to resultSetData.

        // 9. Return result.
        Ok(Value::from_object(result))
    }

    // 24.2.3.6 Set.prototype.entries ( ), https://tc39.es/ecma262/#sec-set.prototype.entries
    fn entries(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        // 1. Let S be the this value.
        let set = typed_this_set(vm)?;

        // 2. Return ? CreateSetIterator(S, key+value).
        Ok(Value::from_object(SetIterator::create(
            vm,
            realm,
            set,
            PropertyKind::KeyAndValue,
        )))
    }

    // 24.2.3.7 Set.prototype.forEach ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-set.prototype.foreach
    fn for_each(vm: &Vm) -> ThrowCompletionOr<Value> {
        let callback_fn = vm.argument(0);
        let this_arg = vm.argument(1);

        // 1. Let S be the this value.
        // 2. Perform ? RequireInternalSlot(S, [[SetData]]).
        let set = typed_this_set(vm)?;

        // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
        if !callback_fn.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&vm.argument(0)]);
        }

        // 4. Let entries be S.[[SetData]].
        // 5. Let numEntries be the number of elements in entries.
        // 6. Let index be 0.
        // 7. Repeat, while index < numEntries,
        let iterator = set.begin();
        while !iterator.is_end() {
            let value = iterator.current();

            // a. Let e be entries[index].
            // b. Set index to index + 1.
            // c. If e is not empty, then
            // NOTE: This is handled in Map::IteratorImpl.

            // i. Perform ? Call(callbackfn, thisArg, « e, e, S »).
            call(vm, callback_fn, this_arg, &[value, value, Value::from_object(set)])?;

            // ii. NOTE: The number of elements in entries may have increased during execution of callbackfn.
            // iii. Set numEntries to the number of elements in entries.
            // NOTE: This is handled in Map::IteratorImpl.
            iterator.advance();
        }

        // 8. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 24.2.3.8 Set.prototype.has ( value ), https://tc39.es/ecma262/#sec-set.prototype.has
    fn has(vm: &Vm) -> ThrowCompletionOr<Value> {
        let value = vm.argument(0);

        // 1. Let S be the this value.
        // 2. Perform ? RequireInternalSlot(S, [[SetData]]).
        let set = typed_this_set(vm)?;

        // 3. Set value to CanonicalizeKeyedCollectionKey(value).
        let value = canonicalize_keyed_collection_key(value);

        // 4. For each element e of S.[[SetData]], do
        //     a. If e is not empty and SameValue(e, value) is true, return true.
        // 5. Return false.
        Ok(Value::from_bool(set.set_has(value)))
    }

    // 24.2.4.9 Set.prototype.intersection ( other ), https://tc39.es/ecma262/#sec-set.prototype.intersection
    fn intersection(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
        let set = typed_this_set(vm)?;

        // 3. Let otherRec be ? GetSetRecord(other).
        let other_record = get_set_record(vm, vm.argument(0))?;

        // 4. Let resultSetData be a new empty List.
        let result = Set::create(vm, realm);

        // 5. If SetDataSize(O.[[SetData]]) ≤ otherRec.[[Size]], then
        if set.set_size() as f64 <= other_record.size {
            // a. Let thisSize be the number of elements in O.[[SetData]].
            // b. Let index be 0.
            // c. Repeat, while index < thisSize,
            let iterator = set.begin();
            while !iterator.is_end() {
                // i. Let e be O.[[SetData]][index].
                let e = iterator.current();

                // ii. Set index to index + 1.
                // iii. If e is not empty, then
                //     1. Let inOther be ToBoolean(? Call(otherRec.[[Has]], otherRec.[[SetObject]], « e »)).
                let in_other =
                    call_function_object(vm, other_record.has, Value::from_object(other_record.set_object), &[e])?
                        .to_boolean();

                //     2. If inOther is true, then
                if in_other {
                    // a. NOTE: It is possible for earlier calls to otherRec.[[Has]] to remove and re-add an element of O.[[SetData]], which can cause the same element to be visited twice during this iteration.
                    // b. If SetDataHas(resultSetData, e) is false, then
                    if !set_data_has(result, e) {
                        // i. Append e to resultSetData.
                        result.set_add(vm, e);
                    }
                }

                //     3. NOTE: The number of elements in O.[[SetData]] may have increased during execution of otherRec.[[Has]].
                //     4. Set thisSize to the number of elements in O.[[SetData]].
                iterator.advance();
            }
        }
        // 6. Else,
        else {
            // a. Let keysIter be ? GetIteratorFromMethod(otherRec.[[SetObject]], otherRec.[[Keys]]).
            let keys_iterator =
                get_iterator_from_method(vm, Value::from_object(other_record.set_object), other_record.keys)?;

            // b. Let next be NOT-STARTED.
            // c. Repeat, while next is not DONE,
            // i. Set next to ? IteratorStepValue(keysIter).
            // ii. If next is not DONE, then
            while let Some(next) = iterator_step_value(vm, &keys_iterator)? {
                // 1. Set next to CanonicalizeKeyedCollectionKey(next).
                let next = canonicalize_keyed_collection_key(next);

                // 2. Let inThis be SetDataHas(O.[[SetData]], next).
                let in_this = set_data_has(set, next);

                // 3. If inThis is true, then
                if in_this {
                    // a. NOTE: Because other is an arbitrary object, it is possible for its "keys" iterator to produce the same value more than once.

                    // b. If SetDataHas(resultSetData, next) is false, then
                    if !set_data_has(result, next) {
                        // i. Append next to resultSetData.
                        result.set_add(vm, next);
                    }
                }
            }
        }

        // 7. Let result be OrdinaryObjectCreate(%Set.prototype%, « [[SetData]] »).
        // 8. Set result.[[SetData]] to resultSetData.

        // 9. Return result.
        Ok(Value::from_object(result))
    }

    // 24.2.4.10 Set.prototype.isDisjointFrom ( other ), https://tc39.es/ecma262/#sec-set.prototype.isdisjointfrom
    fn is_disjoint_from(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
        let set = typed_this_set(vm)?;

        // 3. Let otherRec be ? GetSetRecord(other).
        let other_record = get_set_record(vm, vm.argument(0))?;

        // 4. If SetDataSize(O.[[SetData]]) ≤ otherRec.[[Size]], then
        if set.set_size() as f64 <= other_record.size {
            // a. Let thisSize be the number of elements in O.[[SetData]].
            // b. Let index be 0.
            // c. Repeat, while index < thisSize,
            let iterator = set.begin();
            while !iterator.is_end() {
                // i. Let e be O.[[SetData]][index].
                let e = iterator.current();

                // ii. Set index to index + 1.
                // iii. If e is not empty, then
                //     1. Let inOther be ToBoolean(? Call(otherRec.[[Has]], otherRec.[[SetObject]], « e »)).
                let in_other =
                    call_function_object(vm, other_record.has, Value::from_object(other_record.set_object), &[e])?
                        .to_boolean();

                //     2. If inOther is true, return false.
                if in_other {
                    return Ok(Value::FALSE);
                }

                //     3. NOTE: The number of elements in O.[[SetData]] may have increased during execution of otherRec.[[Has]].
                //     4. Set thisSize to the number of elements in O.[[SetData]].
                iterator.advance();
            }
        }
        // 5. Else,
        else {
            // a. Let keysIter be ? GetIteratorFromMethod(otherRec.[[SetObject]], otherRec.[[Keys]]).
            let keys_iterator =
                get_iterator_from_method(vm, Value::from_object(other_record.set_object), other_record.keys)?;

            // b. Let next be NOT-STARTED.
            // c. Repeat, while next is not DONE,
            // i. Set next to ? IteratorStepValue(keysIter).
            // ii. If next is not DONE, then
            while let Some(next) = iterator_step_value(vm, &keys_iterator)? {
                // 1. If SetDataHas(O.[[SetData]], next) is true, then
                if set_data_has(set, next) {
                    // a. Perform ? IteratorClose(keysIter, NormalCompletion(UNUSED)).
                    iterator_close(vm, &keys_iterator, Completion::normal(Value::UNDEFINED))
                        .into_throw_completion_or()?;

                    // b. Return false.
                    return Ok(Value::FALSE);
                }
            }
        }

        // 6. Return true.
        Ok(Value::TRUE)
    }

    // 24.2.4.11 Set.prototype.isSubsetOf ( other ), https://tc39.es/ecma262/#sec-set.prototype.issubsetof
    fn is_subset_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
        let set = typed_this_set(vm)?;

        // 3. Let otherRec be ? GetSetRecord(other).
        let other_record = get_set_record(vm, vm.argument(0))?;

        // 4. If SetDataSize(O.[[SetData]]) > otherRec.[[Size]], return false.
        if set.set_size() as f64 > other_record.size {
            return Ok(Value::FALSE);
        }

        // 5. Let thisSize be the number of elements in O.[[SetData]].
        // 6. Let index be 0.
        // 7. Repeat, while index < thisSize,
        let iterator = set.begin();
        while !iterator.is_end() {
            // a. Let e be O.[[SetData]][index].
            let e = iterator.current();

            // b. Set index to index + 1.
            // c. If e is not empty, then
            //     i. Let inOther be ToBoolean(? Call(otherRec.[[Has]], otherRec.[[SetObject]], « e »)).
            let in_other =
                call_function_object(vm, other_record.has, Value::from_object(other_record.set_object), &[e])?
                    .to_boolean();

            //     ii. If inOther is false, return false.
            if !in_other {
                return Ok(Value::FALSE);
            }

            //     iii. NOTE: The number of elements in O.[[SetData]] may have increased during execution of otherRec.[[Has]].
            //     iv. Set thisSize to the number of elements in O.[[SetData]].
            iterator.advance();
        }

        // 8. Return true.
        Ok(Value::TRUE)
    }

    // 24.2.4.12 Set.prototype.isSupersetOf ( other ), https://tc39.es/ecma262/#sec-set.prototype.issupersetof
    fn is_superset_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
        let set = typed_this_set(vm)?;

        // 3. Let otherRec be ? GetSetRecord(other).
        let other_record = get_set_record(vm, vm.argument(0))?;

        // 4. If SetDataSize(O.[[SetData]]) < otherRec.[[Size]], return false.
        if (set.set_size() as f64) < other_record.size {
            return Ok(Value::FALSE);
        }

        // 5. Let keysIter be ? GetIteratorFromMethod(otherRec.[[SetObject]], otherRec.[[Keys]]).
        let keys_iterator =
            get_iterator_from_method(vm, Value::from_object(other_record.set_object), other_record.keys)?;

        // 6. Let next be NOT-STARTED.
        // 7. Repeat, while next is not DONE,
        // a. Set next to ? IteratorStepValue(keysIter).
        // b. If next is not DONE, then
        while let Some(next) = iterator_step_value(vm, &keys_iterator)? {
            // i. If SetDataHas(O.[[SetData]], next) is false, then
            if !set_data_has(set, next) {
                // 1. Perform ? IteratorClose(keysIter, NormalCompletion(UNUSED)).
                iterator_close(vm, &keys_iterator, Completion::normal(Value::UNDEFINED)).into_throw_completion_or()?;

                // 2. Return false.
                return Ok(Value::FALSE);
            }
        }

        // 8. Return true.
        Ok(Value::TRUE)
    }

    // 24.2.3.14 get Set.prototype.size, https://tc39.es/ecma262/#sec-get-set.prototype.size
    fn size_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let S be the this value.
        // 2. Perform ? RequireInternalSlot(S, [[SetData]]).
        let set = typed_this_set(vm)?;

        // 3. Let count be 0.
        // 4. For each element e of S.[[SetData]], do
        //     a. If e is not empty, set count to count + 1.
        let count = set.set_size();

        // 5. Return 𝔽(count).
        Ok(Value::from_f64(count as f64))
    }

    // 24.2.4.15 Set.prototype.symmetricDifference ( other ), https://tc39.es/ecma262/#sec-set.prototype.symmetricdifference
    fn symmetric_difference(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
        let set = typed_this_set(vm)?;

        // 3. Let otherRec be ? GetSetRecord(other).
        let other_record = get_set_record(vm, vm.argument(0))?;

        // 4. Let keysIter be ? GetIteratorFromMethod(otherRec.[[SetObject]], otherRec.[[Keys]]).
        let keys_iterator =
            get_iterator_from_method(vm, Value::from_object(other_record.set_object), other_record.keys)?;

        // 5. Let resultSetData be a copy of O.[[SetData]].
        let result = set.copy(vm);

        // 6. Let next be NOT-STARTED.
        // 7. Repeat, while next is not DONE,
        // a. Set next to ? IteratorStepValue(keysIter).
        // b. If next is not DONE, then
        while let Some(next) = iterator_step_value(vm, &keys_iterator)? {
            // i. Set next to CanonicalizeKeyedCollectionKey(next).
            let next = canonicalize_keyed_collection_key(next);

            // ii. Let resultIndex be SetDataIndex(resultSetData, next).
            // iii. If resultIndex is not-found, let alreadyInResult be false. Otherwise let alreadyInResult be true.
            let already_in_result = result.set_has(next);

            // iv. If SetDataHas(O.[[SetData]], next) is true, then
            if set_data_has(set, next) {
                // 1. If alreadyInResult is true, set resultSetData[resultIndex] to empty.
                if already_in_result {
                    result.set_remove(vm, next);
                }
            }
            // v. Else,
            else {
                // 1. If alreadyInResult is false, append next to resultSetData.
                if !already_in_result {
                    result.set_add(vm, next);
                }
            }
        }

        // 8. Let result be OrdinaryObjectCreate(%Set.prototype%, « [[SetData]] »).
        // 9. Set result.[[SetData]] to resultSetData.

        // 10. Return result.
        Ok(Value::from_object(result))
    }

    // 24.2.4.16 Set.prototype.union ( other ), https://tc39.es/ecma262/#sec-set.prototype.union
    fn union_(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[SetData]]).
        let set = typed_this_set(vm)?;

        // 3. Let otherRec be ? GetSetRecord(other).
        let other_record = get_set_record(vm, vm.argument(0))?;

        // 4. Let keysIter be ? GetIteratorFromMethod(otherRec.[[SetObject]], otherRec.[[Keys]]).
        let keys_iterator =
            get_iterator_from_method(vm, Value::from_object(other_record.set_object), other_record.keys)?;

        // 5. Let resultSetData be a copy of O.[[SetData]].
        let result = set.copy(vm);

        // 6. Let next be NOT-STARTED.
        // 7. Repeat, while next is not DONE,
        // a. Set next to ? IteratorStepValue(keysIter).
        // b. If next is not DONE, then
        while let Some(next) = iterator_step_value(vm, &keys_iterator)? {
            // i. Set next to CanonicalizeKeyedCollectionKey(next).
            let next = canonicalize_keyed_collection_key(next);

            // ii. If SetDataHas(resultSetData, next) is false, then
            if !set_data_has(result, next) {
                // 1. Append next to resultSetData.
                result.set_add(vm, next);
            }
        }

        // 8. Let result be OrdinaryObjectCreate(%Set.prototype%, « [[SetData]] »).
        // 9. Set result.[[SetData]] to resultSetData.

        // 10. Return result.
        Ok(Value::from_object(result))
    }

    // 24.2.3.17 Set.prototype.values ( ), https://tc39.es/ecma262/#sec-set.prototype.values
    fn values(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        // 1. Let S be the this value.
        // NOTE: CreateSetIterator checks the presence of a [[SetData]] slot, so we can do this here.
        let set = typed_this_set(vm)?;

        // 2. Return ? CreateSetIterator(S, value).
        Ok(Value::from_object(SetIterator::create(
            vm,
            realm,
            set,
            PropertyKind::Value,
        )))
    }
}
