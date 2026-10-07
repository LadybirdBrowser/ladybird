/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::gc::class::{Class, Finalize, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::call_function_object;
use crate::runtime::aggregate_error::AggregateError;
use crate::runtime::array::Array;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::{
    NATIVE_FUNCTION_METHODS, NATIVE_FUNCTION_VIRTUAL_METHODS, NativeFunction, NativeFunctionMethods,
};
use crate::runtime::object::ObjectMethods;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::promise_capability::PromiseCapability;
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct RemainingElements {
    header: CellHeader,
    value: Cell<u64>,
}

define_cell!(RemainingElements, Other);

impl RemainingElements {
    pub fn create(vm: &Vm, initial_value: u64) -> Gc<RemainingElements> {
        vm.heap().allocate(RemainingElements {
            header: CellHeader::for_class(Self::CLASS),
            value: Cell::new(initial_value),
        })
    }

    pub fn value(&self) -> u64 {
        self.value.get()
    }

    /// --remaining_elements.value: decrements the count and returns what is left.
    pub fn decrement(&self) -> u64 {
        let value = self.value.get() - 1;
        self.value.set(value);
        value
    }

    pub fn increment(&self) {
        self.value.set(self.value.get() + 1);
    }
}

#[repr(C)]
#[derive(Trace)]
pub struct PromiseValueList {
    header: CellHeader,
    values: GcRefCell<Vec<Value>>,
}

define_cell!(PromiseValueList, Other, finalize: finalize);

impl Finalize for PromiseValueList {
    fn finalize(&self) {
        drop(self.values.replace(Vec::new()));
    }
}

impl PromiseValueList {
    pub fn create(vm: &Vm) -> Gc<PromiseValueList> {
        vm.heap().allocate(PromiseValueList {
            header: CellHeader::for_class(Self::CLASS),
            values: GcRefCell::new(Vec::new()),
        })
    }

    pub fn append(&self, value: Value) {
        self.values.borrow_mut().push(value);
    }

    pub fn get(&self, index: usize) -> Value {
        self.values.borrow()[index]
    }

    pub fn set(&self, index: usize, value: Value) {
        self.values.borrow_mut()[index] = value;
    }

    /// Array::create_from(realm, values.values()): CreateArrayFromList of the values.
    pub fn create_array(&self, vm: &Vm, realm: Gc<Realm>) -> Gc<Array> {
        let count = self.values.borrow().len();
        let values = MarkedVec::with_capacity(vm, count);
        for value in self.values.borrow().iter() {
            values.push(*value);
        }
        Array::create_from_list(vm, realm, &values)
    }
}

/// PromiseResolvingElementFunction::resolve_element(), which each kind of element function overrides.
type ResolveElement = fn(&PromiseResolvingElementFunction, &Vm) -> ThrowCompletionOr<Value>;

#[repr(C)]
#[derive(Trace)]
pub struct PromiseResolvingElementFunction {
    base: NativeFunction,
    index: Cell<usize>,
    values: Cell<Gc<PromiseValueList>>,
    capability: Cell<Gc<PromiseCapability>>,
    remaining_elements: Cell<Gc<RemainingElements>>,
    already_called: Cell<bool>,
    #[gc(untraced)]
    resolve_element: ResolveElement,
}

static PROMISE_RESOLVING_ELEMENT_FUNCTION_VIRTUAL_METHODS: NativeFunctionMethods = NativeFunctionMethods {
    call: PromiseResolvingElementFunction::call,
    ..NATIVE_FUNCTION_VIRTUAL_METHODS
};

static PROMISE_RESOLVING_ELEMENT_FUNCTION_METHODS: ObjectMethods = ObjectMethods {
    initialize: PromiseResolvingElementFunction::initialize,
    native_function: Some(&PROMISE_RESOLVING_ELEMENT_FUNCTION_VIRTUAL_METHODS),
    ..NATIVE_FUNCTION_METHODS
};

define_cell!(
    PromiseResolvingElementFunction,
    Object,
    extends: [NativeFunction, FunctionObject, Object],
    methods: PROMISE_RESOLVING_ELEMENT_FUNCTION_METHODS
);

impl Deref for PromiseResolvingElementFunction {
    type Target = NativeFunction;

    fn deref(&self) -> &NativeFunction {
        &self.base
    }
}

impl PromiseResolvingElementFunction {
    #[allow(clippy::too_many_arguments)]
    fn new(
        vm: &Vm,
        class: &'static Class,
        index: usize,
        values: Gc<PromiseValueList>,
        capability: Gc<PromiseCapability>,
        remaining_elements: Gc<RemainingElements>,
        prototype: Gc<Object>,
        resolve_element: ResolveElement,
    ) -> PromiseResolvingElementFunction {
        PromiseResolvingElementFunction {
            base: NativeFunction::new_with_prototype(vm, class, prototype),
            index: Cell::new(index),
            values: Cell::new(values),
            capability: Cell::new(capability),
            remaining_elements: Cell::new(remaining_elements),
            already_called: Cell::new(false),
            resolve_element,
        }
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        (NATIVE_FUNCTION_METHODS.initialize)(object, vm, realm);

        object.unsafe_set_shape(realm.native_function_shape());
        object.put_direct(realm.native_function_length_offset(), Value::from_i32(1));
        object.put_direct(
            realm.native_function_name_offset(),
            Value::from_string(vm.empty_string()),
        );
    }

    fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        assert!(function.is::<PromiseResolvingElementFunction>());
        // SAFETY: The function is a PromiseResolvingElementFunction, which starts with its NativeFunction.
        let function = unsafe { &*core::ptr::from_ref(function).cast::<PromiseResolvingElementFunction>() };

        if function.already_called.get() {
            return Ok(Value::UNDEFINED);
        }
        function.already_called.set(true);

        (function.resolve_element)(function, vm)
    }

    fn index(&self) -> usize {
        self.index.get()
    }

    fn values(&self) -> Gc<PromiseValueList> {
        self.values.get()
    }

    fn capability(&self) -> Gc<PromiseCapability> {
        self.capability.get()
    }

    fn remaining_elements(&self) -> Gc<RemainingElements> {
        self.remaining_elements.get()
    }
}

/// Defines a kind of element function, a class extending PromiseResolvingElementFunction whose resolve_element() is
/// `$resolve_element`.
macro_rules! define_promise_resolving_element_function {
    ($type:ident, $resolve_element:path) => {
        #[repr(C)]
        #[derive(Trace)]
        pub struct $type {
            base: PromiseResolvingElementFunction,
        }

        define_cell!(
            $type,
            Object,
            extends: [PromiseResolvingElementFunction, NativeFunction, FunctionObject, Object]
        );

        impl Deref for $type {
            type Target = PromiseResolvingElementFunction;

            fn deref(&self) -> &PromiseResolvingElementFunction {
                &self.base
            }
        }

        impl $type {
            pub fn create(
                vm: &Vm,
                realm: Gc<Realm>,
                index: usize,
                values: Gc<PromiseValueList>,
                capability: Gc<PromiseCapability>,
                remaining_elements: Gc<RemainingElements>,
            ) -> Gc<$type> {
                realm.create_object(
                    vm,
                    $type {
                        base: PromiseResolvingElementFunction::new(
                            vm,
                            Self::CLASS,
                            index,
                            values,
                            capability,
                            remaining_elements,
                            realm.function_prototype(),
                            $resolve_element,
                        ),
                    },
                )
            }
        }
    };
}

// 27.2.4.1.3 Promise.all Resolve Element Functions, https://tc39.es/ecma262/#sec-promise.all-resolve-element-functions
define_promise_resolving_element_function!(PromiseAllResolveElementFunction, promise_all_resolve_element);

fn promise_all_resolve_element(function: &PromiseResolvingElementFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
    let realm = vm.current_realm().expect("there is a current realm");

    // 8. Set values[index] to x.
    function.values().set(function.index(), vm.argument(0));

    // 9. Set remainingElementsCount.[[Value]] to remainingElementsCount.[[Value]] - 1.
    // 10. If remainingElementsCount.[[Value]] is 0, then
    if function.remaining_elements().decrement() == 0 {
        // a. Let valuesArray be CreateArrayFromList(values).
        let values_array = function.values().create_array(vm, realm);

        // b. Return ? Call(promiseCapability.[[Resolve]], undefined, « valuesArray »).
        return call_function_object(
            vm,
            function.capability().resolve(),
            Value::UNDEFINED,
            &[Value::from_object(values_array)],
        );
    }

    // 11. Return undefined.
    Ok(Value::UNDEFINED)
}

// 27.2.4.2.2 Promise.allSettled Resolve Element Functions, https://tc39.es/ecma262/#sec-promise.allsettled-resolve-element-functions
define_promise_resolving_element_function!(
    PromiseAllSettledResolveElementFunction,
    promise_all_settled_resolve_element
);

fn promise_all_settled_resolve_element(
    function: &PromiseResolvingElementFunction,
    vm: &Vm,
) -> ThrowCompletionOr<Value> {
    let realm = vm.current_realm().expect("there is a current realm");

    // The paired allSettled callbacks share this result slot as their already-called state.
    if !function.values().get(function.index()).is_undefined() {
        return Ok(Value::UNDEFINED);
    }

    // 9. Let obj be OrdinaryObjectCreate(%Object.prototype%).
    let object = Object::create(vm, realm, Some(realm.intrinsics().object_prototype(vm)));

    // 10. Perform ! CreateDataPropertyOrThrow(obj, "status", "fulfilled").
    object
        .create_data_property_or_throw(
            vm,
            &vm.names.status,
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("fulfilled"),
            )),
        )
        .must();

    // 11. Perform ! CreateDataPropertyOrThrow(obj, "value", x).
    object
        .create_data_property_or_throw(vm, &vm.names.value, vm.argument(0))
        .must();

    // 12. Set values[index] to obj.
    function.values().set(function.index(), Value::from_object(object));

    // 13. Set remainingElementsCount.[[Value]] to remainingElementsCount.[[Value]] - 1.
    // 14. If remainingElementsCount.[[Value]] is 0, then
    if function.remaining_elements().decrement() == 0 {
        // a. Let valuesArray be CreateArrayFromList(values).
        let values_array = function.values().create_array(vm, realm);

        // b. Return ? Call(promiseCapability.[[Resolve]], undefined, « valuesArray »).
        return call_function_object(
            vm,
            function.capability().resolve(),
            Value::UNDEFINED,
            &[Value::from_object(values_array)],
        );
    }

    // 15. Return undefined.
    Ok(Value::UNDEFINED)
}

// 27.2.4.2.3 Promise.allSettled Reject Element Functions, https://tc39.es/ecma262/#sec-promise.allsettled-reject-element-functions
define_promise_resolving_element_function!(
    PromiseAllSettledRejectElementFunction,
    promise_all_settled_reject_element
);

fn promise_all_settled_reject_element(function: &PromiseResolvingElementFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
    let realm = vm.current_realm().expect("there is a current realm");

    // The paired allSettled callbacks share this result slot as their already-called state.
    if !function.values().get(function.index()).is_undefined() {
        return Ok(Value::UNDEFINED);
    }

    // 9. Let obj be OrdinaryObjectCreate(%Object.prototype%).
    let object = Object::create(vm, realm, Some(realm.intrinsics().object_prototype(vm)));

    // 10. Perform ! CreateDataPropertyOrThrow(obj, "status", "rejected").
    object
        .create_data_property_or_throw(
            vm,
            &vm.names.status,
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("rejected"),
            )),
        )
        .must();

    // 11. Perform ! CreateDataPropertyOrThrow(obj, "reason", x).
    object
        .create_data_property_or_throw(vm, &vm.names.reason, vm.argument(0))
        .must();

    // 12. Set values[index] to obj.
    function.values().set(function.index(), Value::from_object(object));

    // 13. Set remainingElementsCount.[[Value]] to remainingElementsCount.[[Value]] - 1.
    // 14. If remainingElementsCount.[[Value]] is 0, then
    if function.remaining_elements().decrement() == 0 {
        // a. Let valuesArray be CreateArrayFromList(values).
        let values_array = function.values().create_array(vm, realm);

        // b. Return ? Call(promiseCapability.[[Resolve]], undefined, « valuesArray »).
        return call_function_object(
            vm,
            function.capability().resolve(),
            Value::UNDEFINED,
            &[Value::from_object(values_array)],
        );
    }

    // 15. Return undefined.
    Ok(Value::UNDEFINED)
}

// 27.2.4.3.2 Promise.any Reject Element Functions, https://tc39.es/ecma262/#sec-promise.any-reject-element-functions
define_promise_resolving_element_function!(PromiseAnyRejectElementFunction, promise_any_reject_element);

fn promise_any_reject_element(function: &PromiseResolvingElementFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
    let realm = vm.current_realm().expect("there is a current realm");

    // 8. Set errors[index] to x.
    function.values().set(function.index(), vm.argument(0));

    // 9. Set remainingElementsCount.[[Value]] to remainingElementsCount.[[Value]] - 1.
    // 10. If remainingElementsCount.[[Value]] is 0, then
    if function.remaining_elements().decrement() == 0 {
        // a. Let error be a newly created AggregateError object.
        let error = AggregateError::create(vm, realm);

        // b. Perform ! DefinePropertyOrThrow(error, "errors", PropertyDescriptor { [[Configurable]]: true, [[Enumerable]]: false, [[Writable]]: true, [[Value]]: CreateArrayFromList(errors) }).
        let errors_array = function.values().create_array(vm, realm);
        let mut descriptor = PropertyDescriptor {
            value: Some(Value::from_object(errors_array)),
            writable: Some(true),
            enumerable: Some(false),
            configurable: Some(true),
            ..Default::default()
        };
        error
            .define_property_or_throw(vm, &vm.names.errors, &mut descriptor)
            .must();

        // c. Return ? Call(promiseCapability.[[Reject]], undefined, « error »).
        return call_function_object(
            vm,
            function.capability().reject(),
            Value::UNDEFINED,
            &[Value::from_object(error)],
        );
    }

    Ok(Value::UNDEFINED)
}
