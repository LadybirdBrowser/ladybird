/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use libjs_runtime_macros::Trace;

use crate::bytecode::executable::StaticPropertyLookupCacheSite;
use crate::gc::class::{Finalize, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{call_function_object, get_options_object, ordinary_create_from_constructor};
use crate::runtime::array::Array;
use crate::runtime::completion::{Completion, Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::generator_object::IterationResult as HelperIterationResult;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::iterator::{
    IterationResult, Iterator, IteratorHint, IteratorRecord, PrimitiveHandling, get_iterator, get_iterator_direct,
    get_iterator_flattenable, iterator_close, iterator_close_all, iterator_step, iterator_step_value,
    try_or_close_iterators,
};
use crate::runtime::iterator_helper::{IteratorHelper, IteratorHelperClosure};
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::value::ordinary_has_instance;

#[repr(C)]
#[derive(Trace)]
pub struct IteratorConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    IteratorConstructor,
    initialize: IteratorConstructor::initialize,
    call: IteratorConstructor::call,
    construct: IteratorConstructor::construct
);

fn current_realm(vm: &Vm) -> Gc<Realm> {
    vm.current_realm()
        .expect("a built-in function runs in an execution context with a realm")
}

impl IteratorConstructor {
    // 27.1.3.1 The Iterator Constructor, https://tc39.es/ecma262/#sec-iterator-constructor
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<IteratorConstructor> {
        realm.create_object(
            vm,
            IteratorConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Iterator.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 27.1.3.2.3 Iterator.prototype Iterator.prototype, https://tc39.es/ecma262/#sec-iterator.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().iterator_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define = |property_key: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, property_key, function, length, attr, None);
        };
        define(&names.concat, raw_native!(IteratorConstructor::concat), 0);
        define(&names.from, raw_native!(IteratorConstructor::from), 1);
        define(&names.zip, raw_native!(IteratorConstructor::zip), 1);
        define(&names.zipKeyed, raw_native!(IteratorConstructor::zip_keyed), 1);

        object.define_direct_property(
            vm,
            &names.length,
            Value::from_i32(0),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 27.1.3.1.1 Iterator ( ), https://tc39.es/ecma262/#sec-iterator
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined or the active function object, throw a TypeError exception.
        vm.throw_completion(ErrorKind::TypeError, ErrorType::ConstructorWithoutNew, &[&"Iterator"])
    }

    // 27.1.3.1.1 Iterator ( ), https://tc39.es/ecma262/#sec-iterator
    fn construct(function: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = current_realm(vm);

        // 1. If NewTarget is undefined or the active function object, throw a TypeError exception.
        if new_target == function.as_function_object_gc() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ClassIsAbstract, &[&"Iterator"]);
        }

        // 2. Return ? OrdinaryCreateFromConstructor(NewTarget, "%Iterator.prototype%").
        ordinary_create_from_constructor(vm, realm, new_target, Intrinsics::iterator_prototype)
    }

    // 27.1.3.2.1 Iterator.concat ( ...items ), https://tc39.es/ecma262/#sec-iterator.concat
    fn concat(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        // 1. Let iterables be a new empty List.
        let iterables = ConcatIterator::create(vm);

        // 2. For each element item of items, do
        for i in 0..vm.argument_count() {
            let item = vm.argument(i);

            // a. If item is not an Object, throw a TypeError exception.
            if !item.is_object() {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&item]);
            }

            // b. Let method be ? GetMethod(item, %Symbol.iterator%).
            let method = item.get_method_with_cache(
                vm,
                &PropertyKey::from(vm.well_known_symbols().iterator),
                vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::IteratorConcatIteratorMethod),
            )?;

            // c. If method is undefined, throw a TypeError exception.
            let Some(method) = method else {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotIterable, &[&item]);
            };

            // d. Append the Record { [[OpenMethod]]: method, [[Iterable]]: item } to iterables.
            iterables.append_iterable(method, item.as_object());
        }

        // 3. Let closure be a new Abstract Closure with no parameters that captures iterables and performs the following steps when called:
        let closure = IteratorHelperClosure::Concat { iterables };

        // 4. Let gen be CreateIteratorFromClosure(closure, "Iterator Helper", %IteratorHelperPrototype%, « [[UnderlyingIterators]] »).
        // 5. Set gen.[[UnderlyingIterators]] to a new empty List.
        let gen_ = IteratorHelper::create(vm, realm, &MarkedVec::new(vm), closure);

        // 6. Return gen.
        Ok(Value::from_object(gen_))
    }

    // 27.1.3.2.2 Iterator.from ( O ), https://tc39.es/ecma262/#sec-iterator.from
    fn from(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        let object = vm.argument(0);

        // 1. Let iteratorRecord be ? GetIteratorFlattenable(O, iterate-string-primitives).
        let iterator_record = get_iterator_flattenable(vm, object, PrimitiveHandling::IterateStringPrimitives)?;

        // 2. Let hasInstance be ? OrdinaryHasInstance(%Iterator%, iteratorRecord.[[Iterator]]).
        let has_instance = ordinary_has_instance(
            vm,
            Value::from_object(iterator_record.iterator()),
            Value::from_object(realm.intrinsics().iterator_constructor(vm)),
        )?;

        // 3. If hasInstance is true, then
        if has_instance.is_boolean() && has_instance.as_bool() {
            // a. Return iteratorRecord.[[Iterator]].
            return Ok(Value::from_object(iterator_record.iterator()));
        }

        // 4. Let wrapper be OrdinaryObjectCreate(%WrapForValidIteratorPrototype%, « [[Iterated]] »).
        // 5. Set wrapper.[[Iterated]] to iteratorRecord.
        let wrapper = Iterator::create(
            vm,
            realm.intrinsics().wrap_for_valid_iterator_prototype(),
            iterator_record,
        );

        // 6. Return wrapper.
        Ok(Value::from_object(wrapper))
    }

    // 1 Iterator.zip ( iterables [ , options ] ), https://tc39.es/proposal-joint-iteration/#sec-iterator.zip
    fn zip(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        let iterables = vm.argument(0);
        let options_value = vm.argument(1);

        // 1. If iterables is not an Object, throw a TypeError exception.
        if !iterables.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&iterables]);
        }

        // 2. Set options to ? GetOptionsObject(options).
        let options = get_options_object(vm, options_value)?;

        // 3. Let mode be ? Get(options, "mode").
        // 4. If mode is undefined, set mode to "shortest".
        // 5. If mode is not one of "shortest", "longest", or "strict", throw a TypeError exception.
        let mode = get_zip_mode(vm, options)?;

        // 6. Let paddingOption be undefined.
        // 7. If mode is "longest", then
        //     a. Set paddingOption to ? Get(options, "padding").
        //     b. If paddingOption is not undefined and paddingOption is not an Object, throw a TypeError exception.
        let padding_option = get_padding_option(vm, options, mode)?;

        // 8. Let iters be a new empty List.
        // 9. Let padding be a new empty List.
        let zip_iterator = ZipIterator::create(vm, realm, mode);

        // 10. Let inputIter be ? GetIterator(iterables, SYNC).
        let input_iterator = get_iterator(vm, iterables, IteratorHint::Sync)?;

        // 11. Let next be NOT-STARTED.
        // 12. Repeat, while next is not DONE,
        loop {
            // a. Set next to Completion(IteratorStepValue(inputIter)).
            // b. IfAbruptCloseIterators(next, iters).
            let next = try_or_close_iterators!(
                vm,
                &zip_iterator.open_iterators(vm),
                iterator_step_value(vm, &input_iterator)
            );

            // c. If next is not DONE, then
            let Some(next) = next else {
                break;
            };

            // i. Let iter be Completion(GetIteratorFlattenable(next, REJECT-PRIMITIVES)).
            let iterator = get_iterator_flattenable(vm, next, PrimitiveHandling::RejectPrimitives);

            // ii. IfAbruptCloseIterators(iter, the list-concatenation of « inputIter » and iters).
            let iterator = match iterator {
                // NB: We don't use TRY_OR_CLOSE_ITERATORS above in order to avoid creating a separate vector for the
                //     IteratorCloseAll invocation. IteratorCloseAll would close the list in reverse order, which we
                //     match here.
                Err(throw) => {
                    let error = iterator_close_all(vm, &zip_iterator.open_iterators(vm), throw.into());
                    return iterator_close(vm, &input_iterator, error).into_throw_completion_or();
                }
                Ok(iterator) => iterator,
            };

            // iii. Append iter to iters.
            zip_iterator.append_iterator(iterator);
        }

        // 13. Let iterCount be the number of elements in iters.
        let iterator_count = zip_iterator.open_iterator_count();

        // 14. If mode is "longest", then
        if mode == ZipMode::Longest {
            match padding_option {
                // a. If paddingOption is undefined, then
                None => {
                    // i. Perform the following steps iterCount times:
                    for _ in 0..iterator_count {
                        // 1. Append undefined to padding.
                        zip_iterator.append_padding(Value::UNDEFINED);
                    }
                }
                // b. Else,
                Some(padding_option) => {
                    // i. Let paddingIter be Completion(GetIterator(paddingOption, SYNC)).
                    // ii. IfAbruptCloseIterators(paddingIter, iters).
                    let padding_iterator = try_or_close_iterators!(
                        vm,
                        &zip_iterator.open_iterators(vm),
                        get_iterator(vm, Value::from_object(padding_option), IteratorHint::Sync)
                    );

                    // iii. Let usingIterator be true.
                    let mut using_iterator = true;

                    // iv. Perform the following steps iterCount times:
                    for _ in 0..iterator_count {
                        // 1. If usingIterator is true, then
                        if using_iterator {
                            // a. Set next to Completion(IteratorStepValue(paddingIter)).
                            // b. IfAbruptCloseIterators(next, iters).
                            let next = try_or_close_iterators!(
                                vm,
                                &zip_iterator.open_iterators(vm),
                                iterator_step_value(vm, &padding_iterator)
                            );

                            match next {
                                // c. If next is DONE, then
                                // i. Set usingIterator to false.
                                None => using_iterator = false,
                                // d. Else,
                                // i. Append next to padding.
                                Some(next) => zip_iterator.append_padding(next),
                            }
                        }

                        // 2. If usingIterator is false, append undefined to padding.
                        if !using_iterator {
                            zip_iterator.append_padding(Value::UNDEFINED);
                        }
                    }

                    // v. If usingIterator is true, then
                    if using_iterator {
                        // 1. Let completion be Completion(IteratorClose(paddingIter, NormalCompletion(UNUSED))).
                        // 2. IfAbruptCloseIterators(completion, iters).
                        try_or_close_iterators!(
                            vm,
                            &zip_iterator.open_iterators(vm),
                            iterator_close(vm, &padding_iterator, Completion::normal(Value::UNDEFINED))
                                .into_throw_completion_or()
                        );
                    }
                }
            }
        }

        // 15. Let finishResults be a new Abstract Closure with parameters (results) that captures nothing and performs the
        //     following steps when called:
        zip_iterator.set_finish_results(ZipFinishResults::CreateArrayFromList);

        // 16. Return IteratorZip(iters, mode, padding, finishResults).
        Ok(Value::from_object(iterator_zip(vm, realm, zip_iterator)))
    }

    // 2 Iterator.zipKeyed ( iterables [ , options ] ), https://tc39.es/proposal-joint-iteration/#sec-iterator.zipkeyed
    fn zip_keyed(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        let iterables_value = vm.argument(0);
        let options_value = vm.argument(1);

        // 1. If iterables is not an Object, throw a TypeError exception.
        if !iterables_value.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&iterables_value]);
        }

        let iterables = iterables_value.as_object();

        // 2. Set options to ? GetOptionsObject(options).
        let options = get_options_object(vm, options_value)?;

        // 3. Let mode be ? Get(options, "mode").
        // 4. If mode is undefined, set mode to "shortest".
        // 5. If mode is not one of "shortest", "longest", or "strict", throw a TypeError exception.
        let mode = get_zip_mode(vm, options)?;

        // 6. Let paddingOption be undefined.
        // 7. If mode is "longest", then
        //     a. Set paddingOption to ? Get(options, "padding").
        //     b. If paddingOption is not undefined and paddingOption is not an Object, throw a TypeError exception.
        let padding_option = get_padding_option(vm, options, mode)?;

        // 8. Let iters be a new empty List.
        // 9. Let padding be a new empty List.
        // 11. Let keys be a new empty List.
        let zip_iterator = ZipIterator::create(vm, realm, mode);

        // 10. Let allKeys be ? iterables.[[OwnPropertyKeys]]().
        let all_keys = iterables.internal_own_property_keys(vm)?;

        // 12. For each element key of allKeys, do
        for index in 0..all_keys.len() {
            let key_value = all_keys.get(index).expect("the index is within the keys");
            let key = PropertyKey::from_value(vm, key_value).must();

            // a. Let desc be Completion(iterables.[[GetOwnProperty]](key)).
            // b. IfAbruptCloseIterators(desc, iters).
            let description = try_or_close_iterators!(
                vm,
                &zip_iterator.open_iterators(vm),
                iterables.internal_get_own_property(vm, &key)
            );

            // c. If desc is not undefined and desc.[[Enumerable]] is true, then
            if description.is_some_and(|description| description.enumerable == Some(true)) {
                // i. Let value be Completion(Get(iterables, key)).
                // ii. IfAbruptCloseIterators(value, iters).
                let value = try_or_close_iterators!(vm, &zip_iterator.open_iterators(vm), iterables.get(vm, &key));

                // iii. If value is not undefined, then
                if !value.is_undefined() {
                    // 1. Append key to keys.
                    zip_iterator.append_key(key);

                    // 2. Let iter be Completion(GetIteratorFlattenable(value, REJECT-PRIMITIVES)).
                    // 3. IfAbruptCloseIterators(iter, iters).
                    let iterator = try_or_close_iterators!(
                        vm,
                        &zip_iterator.open_iterators(vm),
                        get_iterator_flattenable(vm, value, PrimitiveHandling::RejectPrimitives)
                    );

                    // 4. Append iter to iters.
                    zip_iterator.append_iterator(iterator);
                }
            }
        }

        // 13. Let iterCount be the number of elements in iters.
        let iterator_count = zip_iterator.open_iterator_count();

        // 14. If mode is "longest", then
        if mode == ZipMode::Longest {
            match padding_option {
                // a. If paddingOption is undefined, then
                None => {
                    // i. Perform the following steps iterCount times:
                    for _ in 0..iterator_count {
                        // 1. Append undefined to padding.
                        zip_iterator.append_padding(Value::UNDEFINED);
                    }
                }
                // b. Else,
                Some(padding_option) => {
                    // i. For each element key of keys, do
                    for index in 0..zip_iterator.key_count() {
                        let key = zip_iterator.key(index);

                        // 1. Let value be Completion(Get(paddingOption, key)).
                        // 2. IfAbruptCloseIterators(value, iters).
                        let value =
                            try_or_close_iterators!(vm, &zip_iterator.open_iterators(vm), padding_option.get(vm, &key));

                        // 3. Append value to padding.
                        zip_iterator.append_padding(value);
                    }
                }
            }
        }

        // 15. Let finishResults be a new Abstract Closure with parameters (results) that captures keys and iterCount and
        //     performs the following steps when called:
        zip_iterator.set_finish_results(ZipFinishResults::CreateObjectFromKeys { iterator_count });

        // 16. Return IteratorZip(iters, mode, padding, finishResults).
        Ok(Value::from_object(iterator_zip(vm, realm, zip_iterator)))
    }
}

/// An iterable that Iterator.concat opens once the iteration reaches it.
#[derive(Clone, Copy, Trace)]
struct ConcatIterable {
    open_method: Gc<FunctionObject>, // [[OpenMethod]]
    iterable: Gc<Object>,            // [[Iterable]]
}

/// The state of the closure of Iterator.concat: the iterables it captures and the iterator it is reading.
#[repr(C)]
#[derive(Trace)]
pub struct ConcatIterator {
    header: CellHeader,
    iterables: GcRefCell<Vec<ConcatIterable>>,
    #[gc(untraced)]
    index: Cell<usize>,
    inner_iterator: Cell<Option<Gc<IteratorRecord>>>,
}

define_cell!(ConcatIterator, Other, finalize: finalize);

impl Finalize for ConcatIterator {
    fn finalize(&self) {
        drop(self.iterables.replace(Vec::new()));
    }
}

impl ConcatIterator {
    fn create(vm: &Vm) -> Gc<ConcatIterator> {
        vm.heap().allocate(ConcatIterator {
            header: CellHeader::for_class(Self::CLASS),
            iterables: GcRefCell::new(Vec::new()),
            index: Cell::new(0),
            inner_iterator: Cell::new(None),
        })
    }

    fn append_iterable(&self, open_method: Gc<FunctionObject>, iterable: Gc<Object>) {
        self.iterables
            .borrow_mut()
            .push(ConcatIterable { open_method, iterable });
    }

    pub fn next(&self, vm: &Vm, iterator: &IteratorHelper) -> ThrowCompletionOr<HelperIterationResult> {
        if self.inner_iterator.get().is_some() {
            return self.inner_next(vm, iterator);
        }
        self.outer_next(vm, iterator)
    }

    // NB: This implements step 3.a.v.3.b of Iterator.concat.
    pub fn on_abrupt_completion(&self, vm: &Vm, completion: Completion) -> ThrowCompletionOr<Value> {
        let inner_iterator = self
            .inner_iterator
            .get()
            .expect("a suspended concat helper has an inner iterator");

        // b. If completion is an abrupt completion, then
        //     i. Return ? IteratorClose(iteratorRecord, completion).
        iterator_close(vm, &inner_iterator, completion).into_throw_completion_or()
    }

    fn outer_next(&self, vm: &Vm, iterator: &IteratorHelper) -> ThrowCompletionOr<HelperIterationResult> {
        // a. For each Record iterable of iterables, do
        let index = self.index.get();
        let iterable = self.iterables.borrow().get(index).copied();
        if let Some(iterable) = iterable {
            self.index.set(index + 1);

            // i. Let iter be ? Call(iterable.[[OpenMethod]], iterable.[[Iterable]]).
            let iter = call_function_object(vm, iterable.open_method, Value::from_object(iterable.iterable), &[])?;

            // ii. If iter is not an Object, throw a TypeError exception.
            if !iter.is_object() {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&iter]);
            }

            // iii. Let iteratorRecord be ? GetIteratorDirect(iter).
            let iterator_record = get_iterator_direct(vm, iter.as_object())?;

            // iv. Let innerAlive be true.
            self.inner_iterator.set(Some(iterator_record));

            // v. Repeat, while innerAlive is true,
            return self.inner_next(vm, iterator);
        }

        // b. Return ReturnCompletion(undefined).
        Ok(HelperIterationResult::new(Value::UNDEFINED, true))
    }

    fn inner_next(&self, vm: &Vm, iterator: &IteratorHelper) -> ThrowCompletionOr<HelperIterationResult> {
        let inner_iterator = self
            .inner_iterator
            .get()
            .expect("the concat helper is reading an inner iterator");

        // 1. Let innerValue be ? IteratorStepValue(iteratorRecord).
        let inner_value = iterator_step_value(vm, &inner_iterator)?;

        // 2. If innerValue is DONE, then
        let Some(inner_value) = inner_value else {
            // a. Set innerAlive to false.
            self.inner_iterator.set(None);

            return self.outer_next(vm, iterator);
        };

        // 3. Else,
        // a. Let completion be Completion(Yield(innerValue)).
        // NB: Step b is implemented via on_abrupt_completion.
        Ok(HelperIterationResult::new(inner_value, false))
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ZipMode {
    Shortest,
    Longest,
    Strict,
}

/// The finishResults closures of Iterator.zip and Iterator.zipKeyed, with what they capture.
#[derive(Clone, Copy, Trace)]
enum ZipFinishResults {
    CreateArrayFromList,
    CreateObjectFromKeys { iterator_count: usize },
}

/// The state of IteratorZip: the iterators it zips, which of them are still open, and what pads them.
#[repr(C)]
#[derive(Trace)]
pub struct ZipIterator {
    header: CellHeader,
    realm: Gc<Realm>,
    #[gc(untraced)]
    mode: ZipMode,
    iterators: GcRefCell<Vec<Option<Gc<IteratorRecord>>>>,
    open_iterators: GcRefCell<Vec<Gc<IteratorRecord>>>,
    keys: GcRefCell<Vec<PropertyKey>>,
    padding: GcRefCell<Vec<Value>>,
    finish_results: Cell<Option<ZipFinishResults>>,
}

define_cell!(ZipIterator, Other, finalize: finalize);

impl Finalize for ZipIterator {
    fn finalize(&self) {
        drop(self.iterators.replace(Vec::new()));
        drop(self.open_iterators.replace(Vec::new()));
        drop(self.keys.replace(Vec::new()));
        drop(self.padding.replace(Vec::new()));
    }
}

impl ZipIterator {
    fn create(vm: &Vm, realm: Gc<Realm>, mode: ZipMode) -> Gc<ZipIterator> {
        vm.heap().allocate(ZipIterator {
            header: CellHeader::for_class(Self::CLASS),
            realm,
            mode,
            iterators: GcRefCell::new(Vec::new()),
            open_iterators: GcRefCell::new(Vec::new()),
            keys: GcRefCell::new(Vec::new()),
            padding: GcRefCell::new(Vec::new()),
            finish_results: Cell::new(None),
        })
    }

    pub fn next(&self, vm: &Vm) -> ThrowCompletionOr<HelperIterationResult> {
        let iterator_count = self.iterators.borrow().len();

        // a. If iterCount = 0, return ReturnCompletion(undefined).
        if iterator_count == 0 {
            return Ok(HelperIterationResult::new(Value::UNDEFINED, true));
        }

        // b. Repeat,

        // i. Let results be a new empty List.
        let results = MarkedVec::new(vm);

        // ii. Assert: openIters is not empty.
        assert!(self.open_iterator_count() != 0);

        // iii. For each integer i such that 0 ≤ i < iterCount, in ascending order, do
        for i in 0..iterator_count {
            let result;

            // 1. Let iter be iters[i].
            let iterator = self.iterators.borrow()[i];

            match iterator {
                // 2. If iter is null, then
                None => {
                    // a. Assert: mode is "longest".
                    assert!(self.mode == ZipMode::Longest);

                    // b. Let result be padding[i].
                    result = self.padding.borrow()[i];
                }
                // 3. Else,
                Some(iterator) => {
                    // a. Let result be Completion(IteratorStepValue(iter)).
                    let step_value_result = iterator_step_value(vm, &iterator);

                    // b. If result is an abrupt completion, then
                    // c. Set result to ! result.
                    let step_value = match step_value_result {
                        Err(throw) => {
                            // i. Remove iter from openIters.
                            self.remove_iterator_from_open_iterators(iterator);

                            // ii. Return ? IteratorCloseAll(openIters, result).
                            return self.close_all_open_iterators(vm, throw.into());
                        }
                        Ok(step_value) => step_value,
                    };

                    // d. If result is DONE, then
                    match step_value {
                        Some(step_value) => result = step_value,
                        None => {
                            // i. Remove iter from openIters.
                            self.remove_iterator_from_open_iterators(iterator);

                            match self.mode {
                                // ii. If mode is "shortest", then
                                ZipMode::Shortest => {
                                    // i. Return ? IteratorCloseAll(openIters, ReturnCompletion(undefined)).
                                    return self.close_all_open_iterators(vm, Completion::normal(Value::UNDEFINED));
                                }

                                // iii. Else if mode is "strict", then
                                ZipMode::Strict => {
                                    // i. If i ≠ 0, then
                                    if i != 0 {
                                        // i. Return ? IteratorCloseAll(openIters, ThrowCompletion(a newly created TypeError object)).
                                        let error = vm.throw_completion::<Value>(
                                            ErrorKind::TypeError,
                                            ErrorType::ZipIteratorNotEnoughResults,
                                            &[],
                                        );
                                        return self.close_all_open_iterators(vm, Completion::from(error));
                                    }

                                    // ii. For each integer k such that 1 ≤ k < iterCount, in ascending order, do
                                    for k in 1..iterator_count {
                                        // i. Assert: iters[k] is not null.
                                        let iterator_k = self.iterators.borrow()[k]
                                            .expect("a strict zip closes before any iterator is exhausted");

                                        // ii. Let open be Completion(IteratorStep(iters[k])).
                                        let step_result = iterator_step(vm, &iterator_k);

                                        // iii. If open is an abrupt completion, then
                                        // iv. Set open to ! open.
                                        let open = match step_result {
                                            Err(throw) => {
                                                // i. Remove iters[k] from openIters.
                                                self.remove_iterator_from_open_iterators(iterator_k);

                                                // ii. Return ? IteratorCloseAll(openIters, open).
                                                return self.close_all_open_iterators(vm, throw.into());
                                            }
                                            Ok(open) => open,
                                        };

                                        // v. If open is DONE, then
                                        if matches!(open, IterationResult::Done) {
                                            // i. Remove iters[k] from openIters.
                                            self.remove_iterator_from_open_iterators(iterator_k);
                                        }
                                        // vi. Else,
                                        else {
                                            // i. Return ? IteratorCloseAll(openIters, ThrowCompletion(a newly created TypeError object)).
                                            let error = vm.throw_completion::<Value>(
                                                ErrorKind::TypeError,
                                                ErrorType::ZipIteratorNotEnoughResults,
                                                &[],
                                            );
                                            return self.close_all_open_iterators(vm, Completion::from(error));
                                        }
                                    }

                                    // iii. Return ReturnCompletion(undefined).
                                    return Ok(HelperIterationResult::new(Value::UNDEFINED, true));
                                }

                                // iv. Else,
                                ZipMode::Longest => {
                                    // i. Assert: mode is "longest".
                                    // ii. If openIters is empty, return ReturnCompletion(undefined).
                                    if self.open_iterator_count() == 0 {
                                        return Ok(HelperIterationResult::new(Value::UNDEFINED, true));
                                    }

                                    // iii. Set iters[i] to null.
                                    self.iterators.borrow_mut()[i] = None;

                                    // iv. Set result to padding[i].
                                    result = self.padding.borrow()[i];
                                }
                            }
                        }
                    }
                }
            }

            // 4. Append result to results.
            results.push(result);
        }

        // iv. Set results to finishResults(results).
        let results_array = self.finish_results(vm, &results);

        // v. Let completion be Completion(Yield(results)).
        Ok(HelperIterationResult::new(results_array, false))
    }

    pub fn on_abrupt_completion(&self, vm: &Vm, completion: Completion) -> ThrowCompletionOr<Value> {
        // vi. If completion is an abrupt completion, then
        //     1. Return ? IteratorCloseAll(openIters, completion).
        iterator_close_all(vm, &self.open_iterators(vm), completion).into_throw_completion_or()
    }

    /// A copy of openIters, which stays alive while iterators are closed.
    pub fn open_iterators<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, Gc<IteratorRecord>> {
        let open_iterators = MarkedVec::new(vm);
        for iterator_record in self.open_iterators.borrow().iter() {
            open_iterators.push(*iterator_record);
        }
        open_iterators
    }

    fn open_iterator_count(&self) -> usize {
        self.open_iterators.borrow().len()
    }

    fn key_count(&self) -> usize {
        self.keys.borrow().len()
    }

    fn key(&self, index: usize) -> PropertyKey {
        self.keys.borrow()[index].clone()
    }

    fn set_finish_results(&self, finish_results: ZipFinishResults) {
        self.finish_results.set(Some(finish_results));
    }

    fn append_iterator(&self, iterator: Gc<IteratorRecord>) {
        self.iterators.borrow_mut().push(Some(iterator));
        self.open_iterators.borrow_mut().push(iterator);
    }

    fn append_key(&self, key: PropertyKey) {
        self.keys.borrow_mut().push(key);
    }

    fn append_padding(&self, padding: Value) {
        self.padding.borrow_mut().push(padding);
    }

    fn remove_iterator_from_open_iterators(&self, iterator: Gc<IteratorRecord>) {
        let mut open_iterators = self.open_iterators.borrow_mut();
        if let Some(index) = open_iterators.iter().position(|candidate| *candidate == iterator) {
            open_iterators.remove(index);
        }
    }

    fn close_all_open_iterators(&self, vm: &Vm, completion: Completion) -> ThrowCompletionOr<HelperIterationResult> {
        let close_result = iterator_close_all(vm, &self.open_iterators(vm), completion).into_throw_completion_or()?;
        Ok(HelperIterationResult::new(close_result, true))
    }

    fn finish_results(&self, vm: &Vm, results: &MarkedVec<'_, Value>) -> Value {
        let realm = self.realm;
        match self
            .finish_results
            .get()
            .expect("IteratorZip is given its finishResults")
        {
            ZipFinishResults::CreateArrayFromList => {
                // a. Return CreateArrayFromList(results).
                Value::from_object(Array::create_from_list(vm, realm, results))
            }
            ZipFinishResults::CreateObjectFromKeys { iterator_count } => {
                // a. Let obj be OrdinaryObjectCreate(null).
                let object = Object::create(vm, realm, None);

                // b. For each integer i such that 0 ≤ i < iterCount, in ascending order, do
                for i in 0..iterator_count {
                    // i. Perform ! CreateDataPropertyOrThrow(obj, keys[i], results[i]).
                    let result = results.get(i).expect("there is a result per iterator");
                    object.create_data_property_or_throw(vm, &self.key(i), result).must();
                }

                // c. Return obj.
                Value::from_object(object)
            }
        }
    }
}

// 3 IteratorZip ( iters, mode, padding, finishResults ), https://tc39.es/proposal-joint-iteration/#sec-IteratorZip
fn iterator_zip(vm: &Vm, realm: Gc<Realm>, zip_iterator: Gc<ZipIterator>) -> Gc<IteratorHelper> {
    // 1. Let iterCount be the number of elements in iters.
    // 2. Let openIters be a copy of iters.

    // 3. Let closure be a new Abstract Closure with no parameters that captures iters, iterCount, openIters, mode,
    //    padding, and finishResults, and performs the following steps when called:
    let closure = IteratorHelperClosure::Zip { zip_iterator };

    // 4. Let gen be CreateIteratorFromClosure(closure, "Iterator Helper", %IteratorHelperPrototype%, « [[UnderlyingIterators]] »).
    // 5. Set gen.[[UnderlyingIterators]] to openIters.
    // 6. Return gen.
    IteratorHelper::create(vm, realm, &zip_iterator.open_iterators(vm), closure)
}

fn get_zip_mode(vm: &Vm, options: Gc<Object>) -> ThrowCompletionOr<ZipMode> {
    // 3. Let mode be ? Get(options, "mode").
    let mode = options.get(vm, &vm.names.mode)?;

    // 4. If mode is undefined, set mode to "shortest".
    if mode.is_undefined() {
        return Ok(ZipMode::Shortest);
    }

    // 5. If mode is not one of "shortest", "longest", or "strict", throw a TypeError exception.
    if mode.is_string() {
        let mode_string = mode.as_string();
        let mode_string = mode_string.utf16_string_view();

        if mode_string == "shortest" {
            return Ok(ZipMode::Shortest);
        }
        if mode_string == "longest" {
            return Ok(ZipMode::Longest);
        }
        if mode_string == "strict" {
            return Ok(ZipMode::Strict);
        }
    }

    vm.throw_completion(
        ErrorKind::TypeError,
        ErrorType::OptionIsNotValidValue,
        &[&mode, &vm.names.mode],
    )
}

fn get_padding_option(vm: &Vm, options: Gc<Object>, mode: ZipMode) -> ThrowCompletionOr<Option<Gc<Object>>> {
    // 6. Let paddingOption be undefined.
    let mut padding_option = None;

    // 7. If mode is "longest", then
    if mode == ZipMode::Longest {
        // a. Set paddingOption to ? Get(options, "padding").
        let padding_value = options.get(vm, &vm.names.padding)?;

        // b. If paddingOption is not undefined and paddingOption is not an Object, throw a TypeError exception.
        if !padding_value.is_undefined() {
            if !padding_value.is_object() {
                return vm.throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::OptionIsNotValidValue,
                    &[&padding_value, &vm.names.padding],
                );
            }

            padding_option = Some(padding_value.as_object());
        }
    }

    Ok(padding_option)
}
