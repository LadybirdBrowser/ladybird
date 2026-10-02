/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intl::collator::Collator;
use crate::runtime::native_function::{
    NATIVE_FUNCTION_METHODS, NATIVE_FUNCTION_VIRTUAL_METHODS, NativeFunction, NativeFunctionMethods,
};
use crate::runtime::object::ObjectMethods;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::unicode::intl::CollatorOrder;
use crate::utf16::Utf16View;

#[repr(C)]
#[derive(Trace)]
pub struct CollatorCompareFunction {
    base: NativeFunction,
    collator: Cell<Gc<Collator>>, // [[Collator]]
}

static COLLATOR_COMPARE_FUNCTION_VIRTUAL_METHODS: NativeFunctionMethods = NativeFunctionMethods {
    call: CollatorCompareFunction::call,
    ..NATIVE_FUNCTION_VIRTUAL_METHODS
};

static COLLATOR_COMPARE_FUNCTION_METHODS: ObjectMethods = ObjectMethods {
    initialize: CollatorCompareFunction::initialize,
    native_function: Some(&COLLATOR_COMPARE_FUNCTION_VIRTUAL_METHODS),
    ..NATIVE_FUNCTION_METHODS
};

define_cell!(
    CollatorCompareFunction,
    Object,
    extends: [NativeFunction, FunctionObject, Object],
    methods: COLLATOR_COMPARE_FUNCTION_METHODS
);

impl Deref for CollatorCompareFunction {
    type Target = NativeFunction;

    fn deref(&self) -> &NativeFunction {
        &self.base
    }
}

impl CollatorCompareFunction {
    pub fn create(vm: &Vm, realm: Gc<Realm>, collator: Gc<Collator>) -> Gc<CollatorCompareFunction> {
        realm.create_object(
            vm,
            CollatorCompareFunction {
                base: NativeFunction::new_with_prototype(vm, Self::CLASS, realm.function_prototype()),
                collator: Cell::new(collator),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        (NATIVE_FUNCTION_METHODS.initialize)(object, vm, realm);
        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(2),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
        object.define_direct_property(
            vm,
            &vm.names.name,
            Value::from_string(vm.empty_string()),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 10.3.3.1 Collator Compare Functions, https://tc39.es/ecma402/#sec-collator-compare-functions
    fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        assert!(function.is::<CollatorCompareFunction>());
        // SAFETY: The function is a CollatorCompareFunction, which starts with its NativeFunction.
        let function = unsafe { &*core::ptr::from_ref(function).cast::<CollatorCompareFunction>() };

        // 1. Let collator be F.[[Collator]].
        // 2. Assert: Type(collator) is Object and collator has an [[InitializedCollator]] internal slot.
        let collator = function.collator.get();

        // 3. If x is not provided, let x be undefined.
        // 4. If y is not provided, let y be undefined.

        // 5. Let X be ? ToString(x).
        let x = vm.argument(0).to_utf16_string(vm)?;

        // 6. Let Y be ? ToString(y).
        let y = vm.argument(1).to_utf16_string(vm)?;

        // 7. Return CompareStrings(collator, X, Y).
        Ok(Value::from_i32(compare_strings(
            &collator,
            Utf16View::of_string(&x),
            Utf16View::of_string(&y),
        )))
    }
}

// 10.3.3.2 CompareStrings ( collator, x, y ), https://tc39.es/ecma402/#sec-collator-comparestrings
pub fn compare_strings(collator: &Collator, x: Utf16View<'_>, y: Utf16View<'_>) -> i32 {
    let result = collator.with_collator(|collator| collator.compare(x, y));

    // The result is intended to correspond with a sort order of String values according to the effective locale and
    // collation options of collator, and will be negative when x is ordered before y, positive when x is ordered after
    // y, and zero in all other cases (representing no relative ordering between x and y).
    match result {
        CollatorOrder::Before => -1,
        CollatorOrder::Equal => 0,
        CollatorOrder::After => 1,
    }
}
