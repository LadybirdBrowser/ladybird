/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The parts of Libraries/LibJS/Runtime/AbstractOperations.cpp the object model needs.

use ak::{ScopeGuard, Utf16FlyString};

use crate::bytecode::executable::StaticPropertyLookupCacheSite;
use crate::gc::root::MarkedVec;
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::function_object::FunctionObject;
use crate::layout::value::Value;
use crate::runtime::accessor::Accessor;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::indexed_properties::ValueAndAttributes;
use crate::runtime::object::{Object, StackFrameInfo};
use crate::runtime::private_environment::PrivateEnvironment;
use crate::runtime::property_attributes::PropertyAttributes;
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::value::same_value;

/// The Object a function object starts with.
pub fn function_object_as_object(function: Gc<FunctionObject>) -> Gc<Object> {
    // SAFETY: FunctionObject is #[repr(C)] and starts with its Object.
    unsafe { Gc::from_non_null(function.as_non_null().cast()) }
}

// 7.2.1 RequireObjectCoercible ( argument ), https://tc39.es/ecma262/#sec-requireobjectcoercible
pub fn require_object_coercible(vm: &Vm, value: Value) -> ThrowCompletionOr<Value> {
    if value.is_nullish() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotObjectCoercible, &[&value]);
    }
    Ok(value)
}

// 7.2.3 IsCallable ( argument ), https://tc39.es/ecma262/#sec-iscallable
pub fn is_callable(argument: Value) -> bool {
    argument.is_function()
}

// 7.2.4 IsConstructor ( argument ), https://tc39.es/ecma262/#sec-isconstructor
pub fn is_constructor(argument: Value) -> bool {
    argument.is_constructor()
}

/// Allocates the callee's frame on the interpreter stack, as call_impl and construct_impl do, and runs `body` with
/// it. The frame is freed when `body` returns.
fn with_callee_context<T>(
    vm: &Vm,
    function: &Object,
    arguments_list: &[Value],
    body: impl FnOnce(&crate::layout::execution_context::ExecutionContext) -> ThrowCompletionOr<T>,
) -> ThrowCompletionOr<T> {
    let mut stack_frame_info = StackFrameInfo {
        argument_count: u32::try_from(arguments_list.len()).expect("the argument count fits in u32"),
        ..Default::default()
    };
    function.get_stack_frame_info(&mut stack_frame_info);

    let stack = vm.interpreter_stack();
    let stack_mark = stack.top.get();
    let Some(callee_context) = stack.allocate(
        stack_frame_info.registers_and_locals_count,
        stack_frame_info.constant_count,
        stack_frame_info.argument_count,
    ) else {
        return vm.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
    };
    let _deallocate_guard = ScopeGuard::new(|| stack.deallocate(stack_mark));

    // SAFETY: The frame was just allocated and stays allocated until the guard frees it.
    let callee_context = unsafe { callee_context.as_ref() };
    for (index, argument) in callee_context.arguments().iter().enumerate() {
        argument.set(arguments_list.get(index).copied().unwrap_or(Value::UNDEFINED));
    }
    callee_context.passed_argument_count.set(arguments_list.len() as u32);

    body(callee_context)
}

// 7.3.14 Call ( F, V [ , argumentsList ] ), https://tc39.es/ecma262/#sec-call
pub fn call(vm: &Vm, function: Value, this_value: Value, arguments_list: &[Value]) -> ThrowCompletionOr<Value> {
    // 1. If argumentsList is not present, set argumentsList to a new empty List.

    // 2. If IsCallable(F) is false, throw a TypeError exception.
    if !function.is_function() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&function]);
    }

    // 3. Return ? F.[[Call]](V, argumentsList).
    call_function_object(vm, function.as_function(), this_value, arguments_list)
}

// 7.3.14 Call ( F, V [ , argumentsList ] ), https://tc39.es/ecma262/#sec-call
pub fn call_function_object(
    vm: &Vm,
    function: Gc<FunctionObject>,
    this_value: Value,
    arguments_list: &[Value],
) -> ThrowCompletionOr<Value> {
    // 1. If argumentsList is not present, set argumentsList to a new empty List.

    // 2. If IsCallable(F) is false, throw a TypeError exception.
    // Note: Called with a FunctionObject ref

    // 3. Return ? F.[[Call]](V, argumentsList).
    let function = function_object_as_object(function);
    let Some(internal_call) = function.internal_call_method() else {
        unimplemented_runtime_function(&format!("[[Call]] of a {}", function.class().name), 0);
    };
    with_callee_context(vm, &function, arguments_list, |callee_context| {
        internal_call(&function, vm, callee_context, this_value)
    })
}

// 7.3.15 Construct ( F [ , argumentsList [ , newTarget ] ] ), https://tc39.es/ecma262/#sec-construct
pub fn construct(
    vm: &Vm,
    function: Gc<FunctionObject>,
    arguments_list: &[Value],
    new_target: Option<Gc<FunctionObject>>,
) -> ThrowCompletionOr<Gc<Object>> {
    // 1. If newTarget is not present, set newTarget to F.
    let new_target = new_target.unwrap_or(function);

    // 2. If argumentsList is not present, set argumentsList to a new empty List.

    // 3. Return ? F.[[Construct]](argumentsList, newTarget).
    let function = function_object_as_object(function);
    let Some(internal_construct) = function.internal_construct_method() else {
        unimplemented_runtime_function(&format!("[[Construct]] of a {}", function.class().name), 0);
    };
    with_callee_context(vm, &function, arguments_list, |callee_context| {
        internal_construct(&function, vm, callee_context, new_target)
    })
}

// 7.3.19 LengthOfArrayLike ( obj ), https://tc39.es/ecma262/#sec-lengthofarraylike
pub fn length_of_array_like(vm: &Vm, object: &Object) -> ThrowCompletionOr<u64> {
    // OPTIMIZATION: For Array objects with a magical "length" property, it should always reflect the size of indexed property storage.
    if object.has_magical_length_property() {
        return Ok(u64::from(object.indexed_array_like_size()));
    }

    // 1. Return ℝ(? ToLength(? Get(obj, "length"))).
    object
        .get_with_cache(
            vm,
            &vm.names.length,
            vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::LengthOfArrayLike),
        )?
        .to_length(vm)
}

// 7.3.20 CreateListFromArrayLike ( obj [ , elementTypes ] ), https://tc39.es/ecma262/#sec-createlistfromarraylike
pub fn create_list_from_array_like<'vm>(
    vm: &'vm Vm,
    value: Value,
    check_value: Option<&dyn Fn(Value) -> ThrowCompletionOr<()>>,
) -> ThrowCompletionOr<MarkedVec<'vm, Value>> {
    // 1. If elementTypes is not present, set elementTypes to « Undefined, Null, Boolean, String, Symbol, Number, BigInt, Object ».

    // 2. If Type(obj) is not Object, throw a TypeError exception.
    if !value.is_object() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&value]);
    }

    let array_like = value.as_object();

    // 3. Let len be ? LengthOfArrayLike(obj).
    let length = length_of_array_like(vm, &array_like)?;

    // 4. Let list be a new empty List.
    let list = MarkedVec::with_capacity(vm, length as usize);

    // 5. Let index be 0.
    // 6. Repeat, while index < len,
    for index in 0..length {
        // a. Let indexName be ! ToString(𝔽(index)).
        let index_name = PropertyKey::from_number(index);

        // b. Let next be ? Get(obj, indexName).
        let next = array_like.get(vm, &index_name)?;

        // c. If Type(next) is not an element of elementTypes, throw a TypeError exception.
        if let Some(check_value) = check_value {
            check_value(next)?;
        }

        // d. Append next as the last element of list.
        list.push(next);
    }

    // 7. Return list.
    Ok(list)
}

// 7.3.23 SpeciesConstructor ( O, defaultConstructor ), https://tc39.es/ecma262/#sec-speciesconstructor
pub fn species_constructor(
    vm: &Vm,
    object: &Object,
    default_constructor: Gc<FunctionObject>,
) -> ThrowCompletionOr<Gc<FunctionObject>> {
    // 1. Let C be ? Get(O, "constructor").
    let constructor = object.get_with_cache(
        vm,
        &vm.names.constructor,
        vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::SpeciesConstructorConstructor),
    )?;

    // 2. If C is undefined, return defaultConstructor.
    if constructor.is_undefined() {
        return Ok(default_constructor);
    }

    // 3. If Type(C) is not Object, throw a TypeError exception.
    if !constructor.is_object() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAConstructor, &[&constructor]);
    }

    // 4. Let S be ? Get(C, @@species).
    let species = constructor.as_object().get_with_cache(
        vm,
        &PropertyKey::from(vm.well_known_symbols().species),
        vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::SpeciesConstructorSpecies),
    )?;

    // 5. If S is either undefined or null, return defaultConstructor.
    if species.is_nullish() {
        return Ok(default_constructor);
    }

    // 6. If IsConstructor(S) is true, return S.
    if species.is_constructor() {
        return Ok(species.as_function());
    }

    // 7. Throw a TypeError exception.
    vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAConstructor, &[&species])
}

// 10.1.6.2 IsCompatiblePropertyDescriptor ( Extensible, Desc, Current ), https://tc39.es/ecma262/#sec-iscompatiblepropertydescriptor
pub fn is_compatible_property_descriptor(
    vm: &Vm,
    extensible: bool,
    descriptor: &mut PropertyDescriptor,
    current: &Option<PropertyDescriptor>,
) -> bool {
    // 1. Return ValidateAndApplyPropertyDescriptor(undefined, "", Extensible, Desc, Current).
    validate_and_apply_property_descriptor(
        vm,
        None,
        &PropertyKey::from(Utf16FlyString::default()),
        extensible,
        descriptor,
        current,
    )
}

// 10.1.6.3 ValidateAndApplyPropertyDescriptor ( O, P, extensible, Desc, current ), https://tc39.es/ecma262/#sec-validateandapplypropertydescriptor
pub fn validate_and_apply_property_descriptor(
    vm: &Vm,
    object: Option<&Object>,
    property_key: &PropertyKey,
    extensible: bool,
    descriptor: &mut PropertyDescriptor,
    current: &Option<PropertyDescriptor>,
) -> bool {
    // 1. Assert: IsPropertyKey(P) is true.

    // 2. If current is undefined, then
    let Some(current) = current else {
        // a. If extensible is false, return false.
        if !extensible {
            return false;
        }

        // b. If O is undefined, return true.
        let Some(object) = object else {
            return true;
        };

        // c. If IsAccessorDescriptor(Desc) is true, then
        if descriptor.is_accessor_descriptor() {
            // i. Create an own accessor property named P of object O whose [[Get]], [[Set]], [[Enumerable]], and [[Configurable]] attributes are set to the value of the corresponding field in Desc if Desc has that field, or to the attribute's default value otherwise.
            let accessor = Accessor::create(vm, descriptor.get.unwrap_or(None), descriptor.set.unwrap_or(None), None);
            let offset = object.storage_add(
                vm,
                property_key,
                ValueAndAttributes::new(Value::from_accessor(accessor), descriptor.attributes()),
            );
            descriptor.property_offset = offset;
        }
        // d. Else,
        else {
            // i. Create an own data property named P of object O whose [[Value]], [[Writable]], [[Enumerable]], and [[Configurable]] attributes are set to the value of the corresponding field in Desc if Desc has that field, or to the attribute's default value otherwise.
            let value = descriptor.value.unwrap_or(Value::UNDEFINED);
            let offset = object.storage_add(
                vm,
                property_key,
                ValueAndAttributes::new(value, descriptor.attributes()),
            );
            descriptor.property_offset = offset;
        }

        // e. Return true.
        return true;
    };

    // 3. Assert: current is a fully populated Property Descriptor.
    let fully_populated = |field: Option<bool>| field.expect("the current descriptor is fully populated");

    // 4. If Desc does not have any fields, return true.
    if descriptor.is_empty() {
        return true;
    }

    let current_configurable = fully_populated(current.configurable);

    // 5. If current.[[Configurable]] is false, then
    if !current_configurable {
        // a. If Desc has a [[Configurable]] field and Desc.[[Configurable]] is true, return false.
        if descriptor.configurable == Some(true) {
            return false;
        }

        // b. If Desc has an [[Enumerable]] field and SameValue(Desc.[[Enumerable]], current.[[Enumerable]]) is false, return false.
        if descriptor
            .enumerable
            .is_some_and(|enumerable| enumerable != fully_populated(current.enumerable))
        {
            return false;
        }

        // c. If IsGenericDescriptor(Desc) is false and SameValue(IsAccessorDescriptor(Desc), IsAccessorDescriptor(current)) is false, return false.
        if !descriptor.is_generic_descriptor()
            && (descriptor.is_accessor_descriptor() != current.is_accessor_descriptor())
        {
            return false;
        }

        // d. If IsAccessorDescriptor(current) is true, then
        if current.is_accessor_descriptor() {
            // i. If Desc has a [[Get]] field and SameValue(Desc.[[Get]], current.[[Get]]) is false, return false.
            if descriptor
                .get
                .is_some_and(|get| get != current.get.expect("the current descriptor is fully populated"))
            {
                return false;
            }

            // ii. If Desc has a [[Set]] field and SameValue(Desc.[[Set]], current.[[Set]]) is false, return false.
            if descriptor
                .set
                .is_some_and(|set| set != current.set.expect("the current descriptor is fully populated"))
            {
                return false;
            }
        }
        // e. Else if current.[[Writable]] is false, then
        else if !fully_populated(current.writable) {
            // i. If Desc has a [[Writable]] field and Desc.[[Writable]] is true, return false.
            if descriptor.writable == Some(true) {
                return false;
            }

            // ii. If Desc has a [[Value]] field and SameValue(Desc.[[Value]], current.[[Value]]) is false, return false.
            if descriptor.value.is_some_and(|value| {
                !same_value(value, current.value.expect("the current descriptor is fully populated"))
            }) {
                return false;
            }
        }
    }

    // 6. If O is not undefined, then
    if let Some(object) = object {
        // a. If IsDataDescriptor(current) is true and IsAccessorDescriptor(Desc) is true, then
        if current.is_data_descriptor() && descriptor.is_accessor_descriptor() {
            // i. If Desc has a [[Configurable]] field, let configurable be Desc.[[Configurable]], else let configurable be current.[[Configurable]].
            let configurable = descriptor.configurable.unwrap_or(current_configurable);

            // ii. If Desc has a [[Enumerable]] field, let enumerable be Desc.[[Enumerable]], else let enumerable be current.[[Enumerable]].
            let enumerable = descriptor.enumerable.unwrap_or(fully_populated(current.enumerable));

            // iii. Replace the property named P of object O with an accessor property having [[Configurable]] and [[Enumerable]] attributes set to configurable and enumerable, respectively, and each other attribute set to its corresponding value in Desc if present, otherwise to its default value.
            let accessor = Accessor::create(vm, descriptor.get.unwrap_or(None), descriptor.set.unwrap_or(None), None);
            let mut attributes = PropertyAttributes::default();
            attributes.set_enumerable(enumerable);
            attributes.set_configurable(configurable);
            let offset = object.storage_set(
                vm,
                property_key,
                ValueAndAttributes::new(Value::from_accessor(accessor), attributes),
            );
            descriptor.property_offset = offset;
        }
        // b. Else if IsAccessorDescriptor(current) is true and IsDataDescriptor(Desc) is true, then
        else if current.is_accessor_descriptor() && descriptor.is_data_descriptor() {
            // i. If Desc has a [[Configurable]] field, let configurable be Desc.[[Configurable]], else let configurable be current.[[Configurable]].
            let configurable = descriptor.configurable.unwrap_or(current_configurable);

            // ii. If Desc has a [[Enumerable]] field, let enumerable be Desc.[[Enumerable]], else let enumerable be current.[[Enumerable]].
            let enumerable = descriptor.enumerable.unwrap_or(fully_populated(current.enumerable));

            // iii. Replace the property named P of object O with a data property having [[Configurable]] and [[Enumerable]] attributes set to configurable and enumerable, respectively, and each other attribute set to its corresponding value in Desc if present, otherwise to its default value.
            let value = descriptor.value.unwrap_or(Value::UNDEFINED);
            let mut attributes = PropertyAttributes::default();
            attributes.set_writable(descriptor.writable.unwrap_or(false));
            attributes.set_enumerable(enumerable);
            attributes.set_configurable(configurable);
            let offset = object.storage_set(vm, property_key, ValueAndAttributes::new(value, attributes));
            descriptor.property_offset = offset;
        }
        // c. Else,
        else {
            // i. For each field of Desc, set the corresponding attribute of the property named P of object O to the value of the field.
            let value = if descriptor.is_accessor_descriptor()
                || (current.is_accessor_descriptor() && !descriptor.is_data_descriptor())
            {
                let getter = descriptor.get.unwrap_or(current.get.unwrap_or(None));
                let setter = descriptor.set.unwrap_or(current.set.unwrap_or(None));
                Value::from_accessor(Accessor::create(vm, getter, setter, None))
            } else {
                descriptor.value.unwrap_or(current.value.unwrap_or(Value::UNDEFINED))
            };
            let mut attributes = PropertyAttributes::default();
            attributes.set_writable(descriptor.writable.unwrap_or(current.writable.unwrap_or(false)));
            attributes.set_enumerable(descriptor.enumerable.unwrap_or(current.enumerable.unwrap_or(false)));
            attributes.set_configurable(descriptor.configurable.unwrap_or(current.configurable.unwrap_or(false)));
            let offset = object.storage_set(vm, property_key, ValueAndAttributes::new(value, attributes));
            descriptor.property_offset = offset;
        }
    }

    // 7. Return true.
    true
}

// 9.2.1.1 NewPrivateEnvironment ( outerPrivEnv ), https://tc39.es/ecma262/#sec-newprivateenvironment
pub fn new_private_environment(vm: &Vm, outer: Option<Gc<PrivateEnvironment>>) -> Gc<PrivateEnvironment> {
    // 1. Let names be a new empty List.
    // 2. Return the PrivateEnvironment Record { [[OuterPrivateEnvironment]]: outerPrivEnv, [[Names]]: names }.
    PrivateEnvironment::create(vm, outer)
}
