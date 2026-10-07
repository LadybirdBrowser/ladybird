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
use crate::runtime::completion::{Completion, CompletionType, ThrowCompletionOr};
use crate::runtime::generator_object::GeneratorState;
use crate::runtime::iterator::{create_iterator_result_object, iterator_close_all};
use crate::runtime::iterator_helper::{ITERATOR_HELPER_BRAND, IteratorHelper};
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;

/// %IteratorHelperPrototype%.
#[repr(C)]
#[derive(Trace)]
pub struct IteratorHelperPrototype {
    base: Object,
}

define_object_class!(IteratorHelperPrototype, extends: [Object], methods: {
    initialize: IteratorHelperPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

const DISPLAY_NAME: &str = "IteratorHelper";

impl IteratorHelperPrototype {
    // 27.1.2.1 The %IteratorHelperPrototype% Object, https://tc39.es/ecma262/#sec-%iteratorhelperprototype%-object
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<IteratorHelperPrototype> {
        realm.create_object(
            vm,
            IteratorHelperPrototype {
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
            raw_native!(IteratorHelperPrototype::next),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &vm.names.return_,
            raw_native!(IteratorHelperPrototype::return_),
            0,
            attr,
            None,
        );

        // 27.1.2.1.3 %IteratorHelperPrototype% [ %Symbol.toStringTag% ], https://tc39.es/ecma262/#sec-%iteratorhelperprototype%-%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("Iterator Helper"),
            )),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 27.1.2.1.1 %IteratorHelperPrototype%.next ( ), https://tc39.es/ecma262/#sec-%iteratorhelperprototype%.next
    fn next(vm: &Vm) -> ThrowCompletionOr<Value> {
        let iterator = typed_this_object::<IteratorHelper>(vm, DISPLAY_NAME)?;

        // 1. Return ? GeneratorResume(this value, undefined, "Iterator Helper").
        let iteration_result = iterator.resume(vm, Value::UNDEFINED, Some(ITERATOR_HELPER_BRAND))?;
        Ok(iterator_result_object(
            vm,
            iteration_result.value,
            iteration_result.done,
        ))
    }

    // 27.1.2.1.2 %IteratorHelperPrototype%.return ( ), https://tc39.es/ecma262/#sec-%iteratorhelperprototype%.return
    fn return_(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be this value.
        // 2. Perform ? RequireInternalSlot(O, [[UnderlyingIterators]]).
        let iterator = typed_this_object::<IteratorHelper>(vm, DISPLAY_NAME)?;

        // 3. Assert: O has a [[GeneratorState]] slot.
        // 4. If O.[[GeneratorState]] is suspended-start, then
        if iterator.generator_state() == GeneratorState::SuspendedStart {
            // a. Set O.[[GeneratorState]] to completed.
            iterator.set_generator_state(GeneratorState::Completed);

            // b. NOTE: Once a generator enters the completed state it never leaves it and its associated execution context is never resumed. Any execution state associated with O can be discarded at this point.

            // c. Perform ? IteratorCloseAll(O.[[UnderlyingIterators]], NormalCompletion(UNUSED)).
            iterator_close_all(
                vm,
                &iterator.underlying_iterators(vm),
                Completion::normal(Value::UNDEFINED),
            )
            .into_throw_completion_or()?;

            // d. Return CreateIterResultObject(undefined, true).
            return Ok(iterator_result_object(vm, Value::UNDEFINED, true));
        }

        // 5. Let C be Completion { [[Type]]: return, [[Value]]: undefined, [[Target]]: empty }.
        let completion = Completion::new(CompletionType::Return, Value::UNDEFINED);

        // 6. Return ? GeneratorResumeAbrupt(O, C, "Iterator Helper").
        let iteration_result = iterator.resume_abrupt(vm, completion, Some(ITERATOR_HELPER_BRAND))?;
        Ok(iterator_result_object(
            vm,
            iteration_result.value,
            iteration_result.done,
        ))
    }
}

fn iterator_result_object(vm: &Vm, value: Value, done: bool) -> Value {
    let realm = vm
        .current_realm()
        .expect("an iterator helper is resumed in an execution context with a realm");
    Value::from_object(create_iterator_result_object(vm, realm, value, done))
}
