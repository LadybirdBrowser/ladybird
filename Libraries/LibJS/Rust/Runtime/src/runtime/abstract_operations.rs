/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The parts of Libraries/LibJS/Runtime/AbstractOperations.cpp the runtime has so far.

use core::cell::Cell;
use std::collections::{HashMap, HashSet};

use ak::{ScopeGuard, Utf16FlyString, Utf16String};

use crate::bytecode::executable::{Executable, StaticPropertyLookupCacheSite};
use crate::gc::class::{Extends, GcCell};
use crate::gc::class_id::ClassId;
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::root::MarkedVec;
use crate::hash_table::Utf16FlyStringHashTable;
use crate::interpreter::run::should_dump_bytecode;
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::{CompilationType, EvalMode, Vm};
use crate::layout::cell::Gc;
use crate::layout::function_object::EcmascriptFunctionObject;
use crate::layout::function_object::FunctionObject;
use crate::layout::value::Value;
use crate::runtime::accessor::Accessor;
use crate::runtime::arguments_object::ArgumentsObject;
use crate::runtime::bound_function::BoundFunction;
use crate::runtime::canonical_index::{CanonicalIndex, CanonicalIndexType};
use crate::runtime::completion::{Must, Throw, ThrowCompletionOr};
use crate::runtime::declarative_environment::DeclarativeEnvironment;
use crate::runtime::ecmascript_function_object::as_ecmascript_function_object;
use crate::runtime::environment::{Environment, InitializeBindingHint, ThisBindingStatus};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_environment::FunctionEnvironment;
use crate::runtime::global_environment::GlobalEnvironment;
use crate::runtime::indexed_properties::ValueAndAttributes;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::iterator::{IteratorHint, get_iterator, iterator_step_value, try_or_close_iterator};
use crate::runtime::native_function::NativeFunction;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, Object, StackFrameInfo};
use crate::runtime::object_environment::{IsWithEnvironment, ObjectEnvironment, name_for_message};
use crate::runtime::private_environment::PrivateEnvironment;
use crate::runtime::promise_capability::{new_promise_capability, try_or_reject};
use crate::runtime::property_attributes::{DEFAULT_ATTRIBUTES, PropertyAttributes};
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::shared_function_instance_data::{
    ClassFieldInitializerName, ConstructorKind, SharedFunctionInstanceData, ThisMode,
};
use crate::runtime::string_conversions::parse_number_f64;
use crate::runtime::string_prototype::string_index_of;
use crate::runtime::value::{number_to_utf16_string, same_value};
use crate::runtime::value_conversions::MAX_ARRAY_LIKE_INDEX;
use crate::script::LexicalBinding;
use crate::source_code::SourceCode;
use crate::utf16::{Utf16StringBuilder, Utf16View};
use libjs_runtime_macros::Trace;
use libjs_rust::compile::{CompiledEval, EvalContext, parse_eval};

/// The Object a function object starts with.
pub fn function_object_as_object(function: Gc<FunctionObject>) -> Gc<Object> {
    function.upcast()
}

pub fn max_js_string_length() -> usize {
    u32::MAX as usize - 1
}

pub fn checked_js_string_length_sum(
    vm: &Vm,
    addend_a: usize,
    addend_b: usize,
    error_type: ErrorType,
) -> ThrowCompletionOr<usize> {
    match addend_a.checked_add(addend_b) {
        Some(sum) if sum <= max_js_string_length() => Ok(sum),
        _ => vm.throw_completion(ErrorKind::RangeError, error_type, &[]),
    }
}

pub fn checked_js_string_length_product(
    vm: &Vm,
    factor_a: usize,
    factor_b: usize,
    error_type: ErrorType,
) -> ThrowCompletionOr<usize> {
    match factor_a.checked_mul(factor_b) {
        Some(product) if product <= max_js_string_length() => Ok(product),
        _ => vm.throw_completion(ErrorKind::RangeError, error_type, &[]),
    }
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
    function.get_stack_frame_info(vm, &mut stack_frame_info);

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

// 7.3.25 GetFunctionRealm ( obj ), https://tc39.es/ecma262/#sec-getfunctionrealm
pub fn get_function_realm(vm: &Vm, function: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Realm>> {
    // 1. If obj has a [[Realm]] internal slot, then
    if let Some(realm) = function.realm() {
        // a. Return obj.[[Realm]].
        return Ok(realm);
    }

    // 2. If obj is a bound function exotic object, then
    if let Some(bound_function) = function.upcast::<Object>().downcast::<BoundFunction>() {
        // a. Let boundTargetFunction be obj.[[BoundTargetFunction]].
        let bound_target_function = bound_function.bound_target_function();

        // b. Return ? GetFunctionRealm(boundTargetFunction).
        return get_function_realm(vm, bound_target_function);
    }

    // 3. If obj is a Proxy exotic object, then
    if function.class().id == ClassId::ProxyObject {
        // a. a. Perform ? ValidateNonRevokedProxy(obj).
        // b. Let proxyTarget be obj.[[ProxyTarget]].
        // c. Assert: proxyTarget is a function object.
        // d. Return ? GetFunctionRealm(proxyTarget).
        unimplemented_runtime_function("ProxyObject::target, for GetFunctionRealm", 0);
    }

    // 4. Return the current Realm Record.
    Ok(vm.current_realm().expect("there is a current realm"))
}

/// The groups GroupBy collects, in the order their keys first appeared, for the key coercion that tells their keys
/// apart: the C++ GroupsType and KeyType template parameters of group_by().
pub trait KeyedGroups<'vm> {
    type Key;

    fn new(vm: &'vm Vm) -> Self;

    /// Steps g and h of GroupBy: the key the coercion makes of what the callback returned.
    fn coerce_key(vm: &Vm, key: Value) -> ThrowCompletionOr<Self::Key>;

    // 7.3.35 AddValueToKeyedGroup ( groups, key, value ), https://tc39.es/ecma262/#sec-add-value-to-keyed-group
    fn add_value_to_keyed_group(&mut self, key: Self::Key, value: Value);
}

/// The groups of GroupBy with the property key coercion, the C++ OrderedHashMap<PropertyKey, GC::RootVector<Value>>.
pub struct PropertyKeyGroups<'vm> {
    vm: &'vm Vm,
    keys: MarkedVec<'vm, PropertyKey>,
    elements: Vec<MarkedVec<'vm, Value>>,
    /// Symbol keys are only kept in the marked list of keys and looked up one by one.
    index_of_string_or_number_key: HashMap<PropertyKey, usize>,
}

impl<'vm> PropertyKeyGroups<'vm> {
    pub fn len(&self) -> usize {
        self.keys.len()
    }

    pub fn is_empty(&self) -> bool {
        self.keys.is_empty()
    }

    pub fn key(&self, index: usize) -> PropertyKey {
        self.keys.get(index).expect("the group exists")
    }

    pub fn elements(&self, index: usize) -> &MarkedVec<'vm, Value> {
        &self.elements[index]
    }

    fn find(&self, key: &PropertyKey) -> Option<usize> {
        if key.is_symbol() {
            return (0..self.keys.len()).find(|&index| self.keys.get(index).as_ref() == Some(key));
        }
        self.index_of_string_or_number_key.get(key).copied()
    }
}

impl<'vm> KeyedGroups<'vm> for PropertyKeyGroups<'vm> {
    type Key = PropertyKey;

    fn new(vm: &'vm Vm) -> Self {
        Self {
            vm,
            keys: MarkedVec::new(vm),
            elements: Vec::new(),
            index_of_string_or_number_key: HashMap::new(),
        }
    }

    fn coerce_key(vm: &Vm, key: Value) -> ThrowCompletionOr<PropertyKey> {
        // g. If keyCoercion is property, then
        //     i. Set key to Completion(ToPropertyKey(key)).
        key.to_property_key(vm)
    }

    fn add_value_to_keyed_group(&mut self, key: PropertyKey, value: Value) {
        // 1. For each Record { [[Key]], [[Elements]] } g of groups, do
        //      a. If SameValue(g.[[Key]], key) is true, then
        //      NOTE: This is performed in KeyedGroupTraits::equals for groupToMap and Traits<JS::PropertyKey>::equals for group.
        if let Some(existing_group) = self.find(&key) {
            // i. Assert: exactly one element of groups meets this criteria.
            // NOTE: This is done on insertion into the hash map, as only `set` tells us if we overrode an entry.

            // ii. Append value as the last element of g.[[Elements]].
            self.elements[existing_group].push(value);

            // iii. Return unused.
            return;
        }

        // 2. Let group be the Record { [[Key]]: key, [[Elements]]: « value » }.
        let new_elements = MarkedVec::new(self.vm);
        new_elements.push(value);

        // 3. Append group as the last element of groups.
        if !key.is_symbol() {
            self.index_of_string_or_number_key.insert(key.clone(), self.keys.len());
        }
        self.keys.push(key);
        self.elements.push(new_elements);

        // 4. Return unused.
    }
}

// 7.3.36 GroupBy ( items, callbackfn, keyCoercion ), https://tc39.es/ecma262/#sec-groupby
pub fn group_by<'vm, Groups: KeyedGroups<'vm>>(
    vm: &'vm Vm,
    items: Value,
    callback_function: Value,
) -> ThrowCompletionOr<Groups> {
    // 1. Perform ? RequireObjectCoercible(items).
    require_object_coercible(vm, items)?;

    // 2. If IsCallable(callbackfn) is false, throw a TypeError exception.
    if !callback_function.is_function() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&callback_function]);
    }

    // 3. Let groups be a new empty List.
    let mut groups = Groups::new(vm);

    // 4. Let iteratorRecord be ? GetIterator(items, sync).
    let iterator_record = get_iterator(vm, items, IteratorHint::Sync)?;

    // 5. Let k be 0.
    let mut k: u64 = 0;

    // 6. Repeat,
    loop {
        // a. If k ≥ 2^53 - 1, then
        if k as f64 >= MAX_ARRAY_LIKE_INDEX {
            // i. Let error be ThrowCompletion(a newly created TypeError object).
            let error: ThrowCompletionOr<()> = vm.throw_completion(ErrorKind::TypeError, ErrorType::ArrayMaxSize, &[]);

            // ii. Return ? IteratorClose(iteratorRecord, error).
            try_or_close_iterator!(vm, &iterator_record, error);
        }

        // b. Let next be ? IteratorStepValue(iteratorRecord).
        let next = iterator_step_value(vm, &iterator_record)?;

        // c. If next is DONE, then
        let Some(value) = next else {
            // i. Return groups.
            return Ok(groups);
        };

        // d. Let value be next.

        // e. Let key be Completion(Call(callbackfn, undefined, « value, 𝔽(k) »)).
        // f. IfAbruptCloseIterator(key, iteratorRecord).
        let key = try_or_close_iterator!(
            vm,
            &iterator_record,
            call(
                vm,
                callback_function,
                Value::UNDEFINED,
                &[value, Value::from_f64(k as f64)]
            )
        );

        // g. If keyCoercion is property, then
        //     ii. IfAbruptCloseIterator(key, iteratorRecord).
        // h. Else,
        //     i. Assert: keyCoercion is zero.
        //     ii. Set key to CanonicalizeKeyedCollectionKey(key).
        let key = try_or_close_iterator!(vm, &iterator_record, Groups::coerce_key(vm, key));

        // i. Perform AddValueToKeyedGroup(groups, key, value).
        groups.add_value_to_keyed_group(key, value);

        // j. Set k to k + 1.
        k += 1;
    }
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

/// A realm's intrinsic object, the C++ pointer to an Intrinsics member function.
pub type IntrinsicDefaultPrototype = fn(&Intrinsics, &Vm) -> Gc<Object>;

// 10.1.13 OrdinaryCreateFromConstructor ( constructor, intrinsicDefaultProto [ , internalSlotsList ] ), https://tc39.es/ecma262/#sec-ordinarycreatefromconstructor
/// The form of OrdinaryCreateFromConstructor that creates an ordinary object.
pub fn ordinary_create_from_constructor(
    vm: &Vm,
    realm: Gc<Realm>,
    constructor: Gc<FunctionObject>,
    intrinsic_default_prototype: IntrinsicDefaultPrototype,
) -> ThrowCompletionOr<Gc<Object>> {
    ordinary_create_from_constructor_of(vm, realm, constructor, intrinsic_default_prototype, |prototype| {
        Object::new_with_prototype(vm, Object::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No)
    })
}

// 10.1.13 OrdinaryCreateFromConstructor ( constructor, intrinsicDefaultProto [ , internalSlotsList ] ), https://tc39.es/ecma262/#sec-ordinarycreatefromconstructor
/// The C++ ordinary_create_from_constructor<T>(): `create` makes the object with the internal slots of a T from the
/// prototype, as the C++ forwards its arguments and the prototype to realm.create<T>().
pub fn ordinary_create_from_constructor_of<T: GcCell + Extends<Object>>(
    vm: &Vm,
    realm: Gc<Realm>,
    constructor: Gc<FunctionObject>,
    intrinsic_default_prototype: IntrinsicDefaultPrototype,
    create: impl FnOnce(Gc<Object>) -> T,
) -> ThrowCompletionOr<Gc<T>> {
    let prototype = get_prototype_from_constructor(vm, constructor, intrinsic_default_prototype)?;
    Ok(realm.create_object(vm, create(prototype)))
}

// 10.1.14 GetPrototypeFromConstructor ( constructor, intrinsicDefaultProto ), https://tc39.es/ecma262/#sec-getprototypefromconstructor
pub fn get_prototype_from_constructor(
    vm: &Vm,
    constructor: Gc<FunctionObject>,
    intrinsic_default_prototype: IntrinsicDefaultPrototype,
) -> ThrowCompletionOr<Gc<Object>> {
    // 1. Assert: intrinsicDefaultProto is this specification's name of an intrinsic object. The corresponding object must be an intrinsic that is intended to be used as the [[Prototype]] value of an object.

    // 2. Let proto be ? Get(constructor, "prototype").
    let mut prototype = constructor.get_with_cache(
        vm,
        &vm.names.prototype,
        vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::GetPrototypeFromConstructorPrototype),
    )?;

    // 3. If Type(proto) is not Object, then
    if !prototype.is_object() {
        // a. Let realm be ? GetFunctionRealm(constructor).
        let realm = get_function_realm(vm, constructor)?;

        // b. Set proto to realm's intrinsic object named intrinsicDefaultProto.
        prototype = Value::from_object(intrinsic_default_prototype(&realm.intrinsics(), vm));
    }

    // 4. Return proto.
    Ok(prototype.as_object())
}

// 9.2.1.1 NewPrivateEnvironment ( outerPrivEnv ), https://tc39.es/ecma262/#sec-newprivateenvironment
pub fn new_private_environment(vm: &Vm, outer: Option<Gc<PrivateEnvironment>>) -> Gc<PrivateEnvironment> {
    // 1. Let names be a new empty List.
    // 2. Return the PrivateEnvironment Record { [[OuterPrivateEnvironment]]: outerPrivEnv, [[Names]]: names }.
    PrivateEnvironment::create(vm, outer)
}

// 9.1.2.2 NewDeclarativeEnvironment ( E ), https://tc39.es/ecma262/#sec-newdeclarativeenvironment
// 4.1.2.1 NewDeclarativeEnvironment ( E ), https://tc39.es/proposal-explicit-resource-management/#sec-declarative-environment-records-initializebinding-n-v
pub fn new_declarative_environment(vm: &Vm, environment: Gc<Environment>) -> Gc<DeclarativeEnvironment> {
    // 1. Let env be a new Declarative Environment Record containing no bindings.
    // 2. Set env.[[OuterEnv]] to E.
    // 3. Set env.[[DisposeCapability]] to NewDisposeCapability().
    // 4. Return env.
    DeclarativeEnvironment::create(vm, Some(environment))
}

// 9.1.2.3 NewObjectEnvironment ( O, W, E ), https://tc39.es/ecma262/#sec-newobjectenvironment
pub fn new_object_environment(
    vm: &Vm,
    object: Gc<Object>,
    is_with_environment: bool,
    environment: Option<Gc<Environment>>,
) -> Gc<ObjectEnvironment> {
    // 1. Let env be a new Object Environment Record.
    // 2. Set env.[[BindingObject]] to O.
    // 3. Set env.[[IsWithEnvironment]] to W.
    // 4. Set env.[[OuterEnv]] to E.
    // 5. Return env.
    ObjectEnvironment::create(
        vm,
        object,
        if is_with_environment {
            IsWithEnvironment::Yes
        } else {
            IsWithEnvironment::No
        },
        environment,
    )
}

fn native_javascript_backed_function_this_mode_is_lexical(_function: Gc<FunctionObject>) -> bool {
    unimplemented_runtime_function(
        "NativeJavaScriptBackedFunction::this_mode, for NewFunctionEnvironment",
        0,
    )
}

// 9.1.2.4 NewFunctionEnvironment ( F, newTarget ), https://tc39.es/ecma262/#sec-newfunctionenvironment
// 4.1.2.2 NewFunctionEnvironment ( F, newTarget ), https://tc39.es/proposal-explicit-resource-management/#sec-newfunctionenvironment
pub fn new_function_environment(
    vm: &Vm,
    function: Gc<EcmascriptFunctionObject>,
    new_target: Option<Gc<Object>>,
) -> Gc<FunctionEnvironment> {
    // 1. Let env be a new function Environment Record containing no bindings.
    let env = FunctionEnvironment::create(vm, function.environment());

    // 2. Set env.[[FunctionObject]] to F.
    env.set_function_object(function.upcast());

    if function.this_mode() == ThisMode::Lexical {
        // 3. If F.[[ThisMode]] is lexical, set env.[[ThisBindingStatus]] to lexical.
        env.set_this_binding_status(ThisBindingStatus::Lexical);
    } else {
        // 4. Else, set env.[[ThisBindingStatus]] to uninitialized.
        env.set_this_binding_status(ThisBindingStatus::Uninitialized);
    }

    // 5. Set env.[[NewTarget]] to newTarget.
    env.set_new_target(new_target.map_or(Value::UNDEFINED, Value::from_object));

    // 6. Set env.[[OuterEnv]] to F.[[Environment]].
    // 7. Set env.[[DisposeCapability]] to NewDisposeCapability().
    // NOTE: Done in step 1 via the FunctionEnvironment constructor.

    // 8. Return env.
    env
}

// 9.1.2.4 NewFunctionEnvironment ( F, newTarget ), https://tc39.es/ecma262/#sec-newfunctionenvironment
// 4.1.2.2 NewFunctionEnvironment ( F, newTarget ), https://tc39.es/proposal-explicit-resource-management/#sec-newfunctionenvironment
pub fn new_function_environment_for_native_javascript_backed_function(
    vm: &Vm,
    function: Gc<FunctionObject>,
    new_target: Option<Gc<Object>>,
) -> Gc<FunctionEnvironment> {
    // 1. Let env be a new function Environment Record containing no bindings.
    let env = FunctionEnvironment::create(vm, None);

    // 2. Set env.[[FunctionObject]] to F.
    env.set_function_object(function);

    if native_javascript_backed_function_this_mode_is_lexical(function) {
        // 3. If F.[[ThisMode]] is lexical, set env.[[ThisBindingStatus]] to lexical.
        env.set_this_binding_status(ThisBindingStatus::Lexical);
    } else {
        // 4. Else, set env.[[ThisBindingStatus]] to uninitialized.
        env.set_this_binding_status(ThisBindingStatus::Uninitialized);
    }

    // 5. Set env.[[NewTarget]] to newTarget.
    env.set_new_target(new_target.map_or(Value::UNDEFINED, Value::from_object));

    // 6. Set env.[[OuterEnv]] to F.[[Environment]].
    // 7. Set env.[[DisposeCapability]] to NewDisposeCapability().
    // NOTE: Done in step 1 via the FunctionEnvironment constructor.

    // 8. Return env.
    env
}

// 9.4.3 GetThisEnvironment ( ), https://tc39.es/ecma262/#sec-getthisenvironment
pub fn get_this_environment(vm: &Vm) -> Gc<Environment> {
    let context = vm
        .running_execution_context()
        .expect("GetThisEnvironment runs in an execution context");

    // 1. Let env be the running execution context's LexicalEnvironment.
    // SAFETY: The running execution context is live.
    let mut env = unsafe { context.as_ref() }.lexical_environment.get();

    // 2. Repeat,
    while let Some(environment) = env {
        // a. Let exists be env.HasThisBinding().
        // b. If exists is true, return env.
        if environment.has_this_binding() {
            return environment;
        }

        // c. Let outer be env.[[OuterEnv]].
        // d. Assert: outer is not null.
        // e. Set env to outer.
        env = environment.outer_environment();
    }
    unreachable!("the outermost environment has a this binding");
}

// 13.3.7.2 GetSuperConstructor ( ), https://tc39.es/ecma262/#sec-getsuperconstructor
pub fn get_super_constructor(vm: &Vm) -> Option<Gc<Object>> {
    // 1. Let envRec be GetThisEnvironment().
    let env = get_this_environment(vm);

    // 2. Assert: envRec is a function Environment Record.
    // 3. Let activeFunction be envRec.[[FunctionObject]].
    // 4. Assert: activeFunction is an ECMAScript function object.
    let active_function = env
        .downcast::<FunctionEnvironment>()
        .expect("GetSuperConstructor runs in a function environment")
        .function_object();

    // 5. Let superConstructor be ! activeFunction.[[GetPrototypeOf]]().
    // 6. Return superConstructor.
    active_function.internal_get_prototype_of(vm).must()
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CallerMode {
    Strict,
    NonStrict,
}

// 19.2.1.1 PerformEval ( x, strictCaller, direct ), https://tc39.es/ecma262/#sec-performeval
// 3 PerformEval ( x, strictCaller, direct ), https://tc39.es/proposal-dynamic-code-brand-checks/#sec-performeval
pub fn perform_eval(vm: &Vm, x: Value, strict_caller: CallerMode, direct: EvalMode) -> ThrowCompletionOr<Value> {
    // 1. Assert: If direct is false, then strictCaller is also false.
    assert!(direct == EvalMode::Direct || strict_caller == CallerMode::NonStrict);

    let code_string;

    // 2. If x is a String, then
    if x.is_string() {
        // a. Let xStr be x.
        code_string = x.as_string();
    }
    // 3. Else if x is an Object, then
    else if x.is_object() {
        // a. Let code be HostGetCodeForEval(x).
        let code = vm.host_get_code_for_eval()(vm, &x.as_object());

        // b. If code is a String, let xStr be code.
        if let Some(code) = code {
            code_string = code;
        }
        // c. Else, return x.
        else {
            return Ok(x);
        }
    }
    // 4. Else,
    else {
        // a. Return x.
        return Ok(x);
    }

    // 5. Let evalRealm be the current Realm Record.
    let eval_realm = vm.current_realm().expect("eval runs in a realm");

    // 6. NOTE: In the case of a direct eval, evalRealm is the realm of both the caller of eval and of the eval function itself.
    // 7. Perform ? HostEnsureCanCompileStrings(evalRealm, « », xStr, xStr, direct, « », x).
    let code = code_string.utf16_string();
    let compilation_type = if direct == EvalMode::Direct {
        CompilationType::DirectEval
    } else {
        CompilationType::IndirectEval
    };
    vm.host_ensure_can_compile_strings()(
        vm,
        eval_realm,
        &[],
        Utf16View::of_string(&code),
        Utf16View::of_string(&code),
        compilation_type,
        &[],
        x,
    )?;

    // 8. Let inFunction be false.
    let mut in_function = false;

    // 9. Let inMethod be false.
    let mut in_method = false;

    // 10. Let inDerivedConstructor be false.
    let mut in_derived_constructor = false;

    // 11. Let inClassFieldInitializer be false.
    let mut in_class_field_initializer = false;

    // 12. If direct is true, then
    if direct == EvalMode::Direct {
        // a. Let thisEnvRec be GetThisEnvironment().
        let this_environment_record = get_this_environment(vm);

        // b. If thisEnvRec is a function Environment Record, then
        if let Some(this_function_environment_record) = this_environment_record.downcast::<FunctionEnvironment>() {
            // i. Let F be thisEnvRec.[[FunctionObject]].
            let function = as_ecmascript_function_object(this_function_environment_record.function_object())
                .expect("a function environment's function is an ECMAScript function");

            // ii. Set inFunction to true.
            in_function = true;

            // iii. Set inMethod to thisEnvRec.HasSuperBinding().
            in_method = this_function_environment_record.has_super_binding();

            // iv. If F.[[ConstructorKind]] is derived, set inDerivedConstructor to true.
            if function.constructor_kind() == ConstructorKind::Derived {
                in_derived_constructor = true;
            }

            // v. Let classFieldInitializerName be F.[[ClassFieldInitializerName]].
            let class_field_initializer_name = function.class_field_initializer_name();

            // vi. If classFieldInitializerName is not empty, set inClassFieldInitializer to true.
            if !matches!(class_field_initializer_name, ClassFieldInitializerName::Empty) {
                in_class_field_initializer = true;
            }
        }
    }

    // 13. Perform the following substeps in an implementation-defined order, possibly interleaving parsing and error detection:
    //     a. Let script be ParseText(StringToCodePoints(x), Script).
    //     c. If script Contains ScriptBody is false, return undefined.
    //     d. Let body be the ScriptBody of script.
    //     NOTE: We do these next steps by passing initial state to the parser.
    //     e. If inFunction is false, and body Contains NewTarget, throw a SyntaxError exception.
    //     f. If inMethod is false, and body Contains SuperProperty, throw a SyntaxError exception.
    //     g. If inDerivedConstructor is false, and body Contains SuperCall, throw a SyntaxError exception.
    //     h. If inClassFieldInitializer is true, and ContainsArguments of body is true, throw a SyntaxError exception.
    let eval_result = match compile_eval(
        vm,
        code,
        strict_caller,
        in_function,
        in_method,
        in_derived_constructor,
        in_class_field_initializer,
    ) {
        Ok(eval_result) => eval_result,
        Err(error_message) => return vm.throw_completion_with_message(ErrorKind::SyntaxError, error_message),
    };
    let executable = eval_result.executable;
    let strict_eval = eval_result.is_strict_mode;
    let eval_declaration_data = eval_result.declaration_data;

    // 16. Let runningContext be the running execution context.
    // 17. NOTE: If direct is true, runningContext will be the execution context that performed the direct eval. If direct is false, runningContext will be the execution context for the invocation of the eval function.
    let running_context_pointer = vm
        .running_execution_context()
        .expect("eval runs in an execution context");
    // SAFETY: The running execution context stays live until eval returns to it.
    let running_context = unsafe { running_context_pointer.as_ref() };

    let lexical_environment: Gc<Environment>;
    let mut variable_environment: Gc<Environment>;
    let private_environment: Option<Gc<PrivateEnvironment>>;

    // 18. If direct is true, then
    if direct == EvalMode::Direct {
        // a. Let lexEnv be NewDeclarativeEnvironment(runningContext's LexicalEnvironment).
        lexical_environment = new_declarative_environment(
            vm,
            running_context
                .lexical_environment
                .get()
                .expect("the running execution context has a LexicalEnvironment"),
        )
        .upcast();

        // b. Let varEnv be runningContext's VariableEnvironment.
        variable_environment = running_context
            .variable_environment
            .get()
            .expect("the running execution context has a VariableEnvironment");

        // c. Let privateEnv be runningContext's PrivateEnvironment.
        private_environment = running_context.private_environment.get();
    }
    // 19. Else,
    else {
        // a. Let lexEnv be NewDeclarativeEnvironment(evalRealm.[[GlobalEnv]]).
        lexical_environment = new_declarative_environment(vm, eval_realm.global_environment().upcast()).upcast();

        // b. Let varEnv be evalRealm.[[GlobalEnv]].
        variable_environment = eval_realm.global_environment().upcast();

        // c. Let privateEnv be null.
        private_environment = None;
    }

    // 20. If strictEval is true, set varEnv to lexEnv.
    if strict_eval {
        variable_environment = lexical_environment;
    }

    if direct == EvalMode::Direct && !strict_eval {
        // NOTE: Non-strict direct eval() forces us to deoptimize variable accesses.
        //       Mark the variable environment chain as screwed since we will not be able
        //       to rely on cached environment coordinates from this point on.
        variable_environment.set_permanently_screwed_by_eval();
    }

    // 21. If runningContext is not already suspended, suspend runningContext.
    // NOTE: Done by the push on step 29.

    // NOTE: Spec steps are rearranged in order to compute number of registers+constants+locals before construction of the execution context.

    // 30. Let result be Completion(EvalDeclarationInstantiation(body, varEnv, lexEnv, privateEnv, strictEval)).
    eval_declaration_instantiation(
        vm,
        &eval_declaration_data,
        variable_environment,
        lexical_environment,
        private_environment,
        strict_eval,
    )?;

    if should_dump_bytecode() {
        executable.dump();
    }

    // 22. Let evalContext be a new ECMAScript code execution context.
    let stack = vm.interpreter_stack();
    let stack_mark = stack.top.get();
    let constant_count = u32::try_from(executable.constants().len()).expect("the constant count fits in u32");
    let Some(eval_context_pointer) = stack.allocate(executable.registers_and_locals_count(), constant_count, 0) else {
        return vm.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
    };
    // SAFETY: The context was just allocated, and stays allocated until the pop guard below frees it.
    let eval_context = unsafe { eval_context_pointer.as_ref() };

    // 23. Set evalContext's Function to null.
    // NOTE: This was done in the construction of eval_context.

    // 24. Set evalContext's Realm to evalRealm.
    eval_context.realm.set(Some(eval_realm));

    // 25. Set evalContext's ScriptOrModule to runningContext's ScriptOrModule.
    eval_context
        .script_or_module
        .set(running_context.script_or_module.get());

    // 26. Set evalContext's VariableEnvironment to varEnv.
    eval_context.variable_environment.set(Some(variable_environment));

    // 27. Set evalContext's LexicalEnvironment to lexEnv.
    eval_context.lexical_environment.set(Some(lexical_environment));

    // 28. Set evalContext's PrivateEnvironment to privateEnv.
    eval_context.private_environment.set(private_environment);

    // 29. Push evalContext onto the execution context stack; evalContext is now the running execution context.
    // NB: Like C++, a push that fails leaves the context allocated, until the frame that called eval frees the
    //     interpreter stack above its own mark.
    vm.push_execution_context_checking_stack_space(eval_context_pointer)?;

    // NOTE: We use a ScopeGuard to automatically pop the execution context when any of the `TRY`s below return a throw completion.
    let _pop_guard = ScopeGuard::new(|| {
        // 33. Suspend evalContext and remove it from the execution context stack.
        // 34. Resume the context that is now on the top of the execution context stack as the running execution context.
        vm.pop_execution_context();
        stack.deallocate(stack_mark);
    });

    let result = vm
        .run_executable(eval_context_pointer, executable, 0)
        .map_err(Throw::new)?;

    // 32. If result.[[Type]] is normal and result.[[Value]] is empty, then
    //     a. Set result to NormalCompletion(undefined).
    // NOTE: Step 33 and 34 is handled by `pop_guard` above.
    // 35. Return ? result.
    // NOTE: Step 35 is also performed with each use of `TRY` above.
    // NB: C++ only replaces a result that is missing, which it never is, so an empty result is returned as it is.
    Ok(result)
}

/// EvalDeclarationData::FunctionToInitialize, without its shared data, which the declaration data keeps rooted on its
/// own.
pub struct EvalFunctionToInitialize {
    pub name: Utf16FlyString,
}

/// What EvalDeclarationInstantiation needs from the body of an eval, which the frontend extracts as it compiles it.
pub struct EvalDeclarationData<'vm> {
    pub var_names: Vec<Utf16FlyString>,

    pub functions_to_initialize: Vec<EvalFunctionToInitialize>,
    /// The shared data of each function to initialize, in the same order.
    pub functions_to_initialize_shared_data: MarkedVec<'vm, Gc<SharedFunctionInstanceData>>,
    pub declared_function_names: HashSet<Utf16FlyString>,

    pub var_scoped_names: Vec<Utf16FlyString>,

    pub annex_b_candidate_names: Vec<Utf16FlyString>,

    pub lexical_bindings: Vec<LexicalBinding>,

    pub referenced_private_names: Vec<Utf16FlyString>,
}

/// RustIntegration::EvalResult.
struct EvalResult<'vm> {
    executable: Gc<Executable>,
    is_strict_mode: bool,
    declaration_data: EvalDeclarationData<'vm>,
}

fn fly_string_of(name: &libjs_rust::ast::Utf16String) -> Utf16FlyString {
    Utf16FlyString::from_utf16(&name.0)
}

fn fly_strings_of(names: &[libjs_rust::ast::Utf16String]) -> Vec<Utf16FlyString> {
    names.iter().map(fly_string_of).collect()
}

/// RustIntegration::compile_eval(): parses and compiles the code of an eval, or returns the message of its first
/// parse error.
#[allow(
    clippy::fn_params_excessive_bools,
    reason = "the flags are the ones PerformEval works out"
)]
fn compile_eval(
    vm: &Vm,
    code: Utf16String,
    strict_caller: CallerMode,
    in_function: bool,
    in_method: bool,
    in_derived_constructor: bool,
    in_class_field_initializer: bool,
) -> Result<EvalResult<'_>, String> {
    let source_code = SourceCode::create(Utf16String::default(), code);
    let mut code_units = Vec::with_capacity(source_code.length_in_code_units());
    Utf16View::of_string(source_code.code()).append_to(&mut code_units);
    let length = code_units.len();

    let context = EvalContext {
        starts_in_strict_mode: strict_caller == CallerMode::Strict,
        in_eval_function_context: in_function,
        allow_super_property_lookup: in_method,
        allow_super_constructor_call: in_derived_constructor,
        in_class_field_initializer,
    };
    let parsed = parse_eval(&code_units, context).map_err(|errors| {
        errors
            .first()
            .map(|error| format!("{} (line: {}, column: {})", error.message, error.line, error.column))
            .unwrap_or_default()
    })?;

    let CompiledEval {
        executable,
        declarations,
    } = libjs_rust::compile::compile_eval(parsed, length);
    let executable = Executable::create_with_source_code(vm, executable, Some(&source_code));
    executable.set_name(Utf16FlyString::from_utf8("eval"));

    let functions_to_initialize_shared_data = MarkedVec::with_capacity(vm, declarations.functions_to_initialize.len());
    let mut functions_to_initialize = Vec::with_capacity(declarations.functions_to_initialize.len());
    let mut declared_function_names = HashSet::new();
    for mut function in declarations.functions_to_initialize {
        functions_to_initialize_shared_data.push(SharedFunctionInstanceData::create_from_pending_shared_function_data(
            vm,
            &mut function.shared_function_data,
            declarations.is_strict,
            Some(&source_code),
        ));
        let name = fly_string_of(&function.name);
        declared_function_names.insert(name.clone());
        functions_to_initialize.push(EvalFunctionToInitialize { name });
    }

    let declaration_data = EvalDeclarationData {
        var_names: fly_strings_of(&declarations.var_names),
        functions_to_initialize,
        functions_to_initialize_shared_data,
        declared_function_names,
        var_scoped_names: fly_strings_of(&declarations.var_scoped_names),
        annex_b_candidate_names: fly_strings_of(&declarations.annex_b_candidate_names),
        lexical_bindings: declarations
            .lexical_bindings
            .iter()
            .map(|binding| LexicalBinding {
                name: fly_string_of(&binding.name),
                is_constant: binding.is_constant,
            })
            .collect(),
        referenced_private_names: fly_strings_of(&declarations.private_names),
    };

    Ok(EvalResult {
        executable,
        // If the caller is strict, the eval is always strict regardless of what Rust reported.
        is_strict_mode: declarations.is_strict || strict_caller == CallerMode::Strict,
        declaration_data,
    })
}

// 19.2.1.3 EvalDeclarationInstantiation ( body, varEnv, lexEnv, privateEnv, strict ), https://tc39.es/ecma262/#sec-evaldeclarationinstantiation
// 9.1.1.1 EvalDeclarationInstantiation ( body, varEnv, lexEnv, privateEnv, strict ), https://tc39.es/proposal-explicit-resource-management/#sec-evaldeclarationinstantiation
pub fn eval_declaration_instantiation(
    vm: &Vm,
    data: &EvalDeclarationData<'_>,
    variable_environment: Gc<Environment>,
    lexical_environment: Gc<Environment>,
    private_environment: Option<Gc<PrivateEnvironment>>,
    strict: bool,
) -> ThrowCompletionOr<()> {
    let realm = vm.current_realm().expect("eval runs in a realm");
    let global_var_environment = variable_environment.downcast::<GlobalEnvironment>();

    // 1. Let varNames be the VarDeclaredNames of body.
    // 2. Let varDeclarations be the VarScopedDeclarations of body.
    // 3. If strict is false, then
    if !strict {
        // a. If varEnv is a global Environment Record, then
        if let Some(global_var_environment) = global_var_environment {
            // i. For each element name of varNames, do
            for name in &data.var_names {
                // 1. If varEnv.HasLexicalDeclaration(name) is true, throw a SyntaxError exception.
                if global_var_environment.has_lexical_declaration(name) {
                    return vm.throw_completion(
                        ErrorKind::SyntaxError,
                        ErrorType::TopLevelVariableAlreadyDeclared,
                        &[&name_for_message(name)],
                    );
                }

                // 2. NOTE: eval will not create a global var declaration that would be shadowed by a global lexical declaration.
            }
        }

        // b. Let thisEnv be lexEnv.
        let mut this_environment = lexical_environment;
        // c. Assert: The following loop will terminate.

        // d. Repeat, while thisEnv is not the same as varEnv,
        while this_environment != variable_environment {
            // i. If thisEnv is not an object Environment Record, then
            if !this_environment.is_object_environment() {
                // 1. NOTE: The environment of with statements cannot contain any lexical declaration so it doesn't need to be checked for var/let hoisting conflicts.
                // 2. For each element name of varNames, do
                for name in &data.var_names {
                    // a. If ! thisEnv.HasBinding(name) is true, then
                    if this_environment.has_binding(vm, name, None).must() {
                        // B.3.4 Changes to EvalDeclarationInstantiation, https://tc39.es/ecma262/#sec-evaldeclarationinstantiation
                        // i. Normative Optional
                        //     If the host is a web browser or otherwise supports VariableStatements in Catch Blocks, then
                        //         i. If thisEnv is not the Environment Record for a Catch clause, throw a SyntaxError exception.
                        // ii. Else,
                        //     i. Throw a SyntaxError exception.
                        // AD-HOC: We are a web browser, so we only implement the web browser branch.
                        if !this_environment.is_catch_environment() {
                            return vm.throw_completion(
                                ErrorKind::SyntaxError,
                                ErrorType::EvalVarHoistingConflict,
                                &[&name_for_message(name)],
                            );
                        }
                    }
                    // b. NOTE: A direct eval will not hoist var declaration over a like-named lexical declaration.
                }
            }

            // ii. Set thisEnv to thisEnv.[[OuterEnv]].
            this_environment = this_environment
                .outer_environment()
                .expect("the variable environment encloses the lexical environment");
        }
    }

    // 4. Let privateIdentifiers be a new empty List.
    // 5. Let pointer be privateEnv.
    // 6. Repeat, while pointer is not null,
    //     a. For each Private Name binding of pointer.[[Names]], do
    //         i. If privateIdentifiers does not contain binding.[[Description]], append binding.[[Description]] to privateIdentifiers.
    //     b. Set pointer to pointer.[[OuterPrivateEnvironment]].
    // 7. If AllPrivateIdentifiersValid of body with argument privateIdentifiers is false, throw a SyntaxError exception.
    for name in &data.referenced_private_names {
        if !private_environment.is_some_and(|private_environment| private_environment.contains_private_identifier(name))
        {
            return vm.throw_completion(
                ErrorKind::SyntaxError,
                ErrorType::PrivateFieldNotDeclared,
                &[&name_for_message(name)],
            );
        }
    }

    // 8. Let functionsToInitialize be a new empty List.
    // 9. Let declaredFunctionNames be a new empty List.
    // 10. For each element d of varDeclarations, in reverse List order, do
    for function in &data.functions_to_initialize {
        // 1. If varEnv is a global Environment Record, then
        if let Some(global_var_environment) = global_var_environment {
            // a. Let fnDefinable be ? varEnv.CanDeclareGlobalFunction(fn).
            let function_definable = global_var_environment.can_declare_global_function(vm, &function.name)?;

            // b. If fnDefinable is false, throw a TypeError exception.
            if !function_definable {
                return vm.throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::CannotDeclareGlobalFunction,
                    &[&name_for_message(&function.name)],
                );
            }
        }
    }

    // 11. NOTE: Annex B.3.2.3 adds additional steps at this point.
    // B.3.2.3 Changes to EvalDeclarationInstantiation, https://tc39.es/ecma262/#sec-web-compat-evaldeclarationinstantiation
    // 11. If strict is false, then
    if !strict {
        // a. Let declaredFunctionOrVarNames be the list-concatenation of declaredFunctionNames and declaredVarNames.
        // The spec here uses 'declaredVarNames' but that has not been declared yet.
        let mut hoisted_functions = HashSet::new();

        // b. For each FunctionDeclaration f that is directly contained in the StatementList of a Block, CaseClause, or DefaultClause Contained within body, do
        for function_name in &data.annex_b_candidate_names {
            // i. Let F be StringValue of the BindingIdentifier of f.

            // ii. If replacing the FunctionDeclaration f with a VariableStatement that has F as a BindingIdentifier would not produce any Early Errors for body, then
            // Note: This is checked during parsing and for_each_function_hoistable_with_annexB_extension so it always passes here.

            // 1. Let bindingExists be false.
            // 2. Let thisEnv be lexEnv.
            let mut this_environment = lexical_environment;

            // 3. Assert: The following loop will terminate.

            // 4. Repeat, while thisEnv is not the same as varEnv,
            let mut binding_exists = false;
            while this_environment != variable_environment {
                // a. If thisEnv is not an object Environment Record, then
                //     i. If ! thisEnv.HasBinding(F) is true, then
                if !this_environment.is_object_environment()
                    && this_environment.has_binding(vm, function_name, None).must()
                {
                    // i. Let bindingExists be true.
                    binding_exists = true;
                    break;
                }

                // b. Set thisEnv to thisEnv.[[OuterEnv]].
                this_environment = this_environment
                    .outer_environment()
                    .expect("the variable environment encloses the lexical environment");
            }

            if binding_exists {
                continue;
            }

            // Note: At this point bindingExists is false.
            // 5. If bindingExists is false and varEnv is a global Environment Record, then
            if let Some(global_var_environment) = global_var_environment {
                // a. If varEnv.HasLexicalDeclaration(F) is false, then
                if !global_var_environment.has_lexical_declaration(function_name) {
                    // i. Let fnDefinable be ? varEnv.CanDeclareGlobalVar(F).
                    if !global_var_environment.can_declare_global_var(vm, function_name)? {
                        continue;
                    }
                }
                // b. Else,
                else {
                    // i. Let fnDefinable be false.
                    continue;
                }
            }
            // 6. Else,
            //     a. Let fnDefinable be true.

            // Note: At this point fnDefinable is true.
            // 7. If bindingExists is false and fnDefinable is true, then

            // a. If declaredFunctionOrVarNames does not contain F, then
            if !data.declared_function_names.contains(function_name) && !hoisted_functions.contains(function_name) {
                // i. If varEnv is a global Environment Record, then
                if let Some(global_var_environment) = global_var_environment {
                    // i. Perform ? varEnv.CreateGlobalVarBinding(F, true).
                    global_var_environment.create_global_var_binding(vm, function_name, true)?;
                }
                // ii. Else,
                else {
                    // i. Let bindingExists be ! varEnv.HasBinding(F).
                    // ii. If bindingExists is false, then
                    if !variable_environment.has_binding(vm, function_name, None).must() {
                        // i. Perform ! varEnv.CreateMutableBinding(F, true).
                        variable_environment
                            .create_mutable_binding(vm, function_name, true)
                            .must();
                        // ii. Perform ! varEnv.InitializeBinding(F, undefined, normal).
                        variable_environment
                            .initialize_binding(vm, function_name, Value::UNDEFINED, InitializeBindingHint::Normal)
                            .must();
                    }
                }
            }

            // iii. Append F to declaredFunctionOrVarNames.
            hoisted_functions.insert(function_name.clone());

            // b. When the FunctionDeclaration f is evaluated, perform the following steps in place of the FunctionDeclaration Evaluation algorithm provided in 15.2.6:
            //     i. Let genv be the running execution context's VariableEnvironment.
            //     ii. Let benv be the running execution context's LexicalEnvironment.
            //     iii. Let fobj be ! benv.GetBindingValue(F, false).
            //     iv. Perform ? genv.SetMutableBinding(F, fobj, false).
            //     v. Return unused.
        }
    }

    // 12. Let declaredVarNames be a new empty List.
    let mut declared_var_names = Utf16FlyStringHashTable::default();

    // 13. For each element d of varDeclarations, do
    for name in &data.var_scoped_names {
        // 1. If vn is not an element of declaredFunctionNames, then
        if !data.declared_function_names.contains(name) {
            // a. If varEnv is a global Environment Record, then
            if let Some(global_var_environment) = global_var_environment {
                // i. Let vnDefinable be ? varEnv.CanDeclareGlobalVar(vn).
                let variable_definable = global_var_environment.can_declare_global_var(vm, name)?;

                // ii. If vnDefinable is false, throw a TypeError exception.
                if !variable_definable {
                    return vm.throw_completion(
                        ErrorKind::TypeError,
                        ErrorType::CannotDeclareGlobalVariable,
                        &[&name_for_message(name)],
                    );
                }
            }

            // b. If vn is not an element of declaredVarNames, then
            // i. Append vn to declaredVarNames.
            declared_var_names.set(name.clone());
        }
    }

    // 14. NOTE: No abnormal terminations occur after this algorithm step unless varEnv is a global Environment Record and the global object is a Proxy exotic object.

    // 15. Let lexDeclarations be the LexicallyScopedDeclarations of body.
    // 16. For each element d of lexDeclarations, do
    for binding in &data.lexical_bindings {
        // i. If IsConstantDeclaration of d is true, then
        if binding.is_constant {
            // 1. Perform ? lexEnv.CreateImmutableBinding(dn, true).
            lexical_environment.create_immutable_binding(vm, &binding.name, true)?;
        }
        // ii. Else,
        else {
            // 1. Perform ? lexEnv.CreateMutableBinding(dn, false).
            lexical_environment.create_mutable_binding(vm, &binding.name, false)?;
        }
    }

    // 17. For each Parse Node f of functionsToInitialize, do
    for (function_index, function_to_initialize) in data.functions_to_initialize.iter().enumerate() {
        // a. Let fn be the sole element of the BoundNames of f.
        // b. Let fo be InstantiateFunctionObject of f with arguments lexEnv and privateEnv.
        let shared_data = data
            .functions_to_initialize_shared_data
            .get(function_index)
            .expect("each function to initialize has its shared data");
        let function = EcmascriptFunctionObject::create_from_function_data(
            vm,
            realm,
            shared_data,
            Some(lexical_environment),
            private_environment,
        );

        // c. If varEnv is a global Environment Record, then
        if let Some(global_var_environment) = global_var_environment {
            // i. Perform ? varEnv.CreateGlobalFunctionBinding(fn, fo, true).
            global_var_environment.create_global_function_binding(
                vm,
                &function_to_initialize.name,
                Value::from_object(function),
                true,
            )?;
        }
        // d. Else,
        else {
            // i. Let bindingExists be ! varEnv.HasBinding(fn).
            let binding_exists = variable_environment
                .has_binding(vm, &function_to_initialize.name, None)
                .must();

            // ii. If bindingExists is false, then
            if !binding_exists {
                // 1. NOTE: The following invocation cannot return an abrupt completion because of the validation preceding step 14.
                // 2. Perform ! varEnv.CreateMutableBinding(fn, true).
                variable_environment
                    .create_mutable_binding(vm, &function_to_initialize.name, true)
                    .must();

                // 3. Perform ! varEnv.InitializeBinding(fn, fo, normal).
                variable_environment
                    .initialize_binding(
                        vm,
                        &function_to_initialize.name,
                        Value::from_object(function),
                        InitializeBindingHint::Normal,
                    )
                    .must();
            }
            // iii. Else,
            else {
                // 1. Perform ! varEnv.SetMutableBinding(fn, fo, false).
                variable_environment
                    .set_mutable_binding(vm, &function_to_initialize.name, Value::from_object(function), false)
                    .must();
            }
        }
    }

    // 18. For each String vn of declaredVarNames, do
    for var_name in declared_var_names.iter() {
        // a. If varEnv is a global Environment Record, then
        if let Some(global_var_environment) = global_var_environment {
            // i. Perform ? varEnv.CreateGlobalVarBinding(vn, true).
            global_var_environment.create_global_var_binding(vm, var_name, true)?;
        }
        // b. Else,
        else {
            // i. Let bindingExists be ! varEnv.HasBinding(vn).
            let binding_exists = variable_environment.has_binding(vm, var_name, None).must();

            // ii. If bindingExists is false, then
            if !binding_exists {
                // 1. NOTE: The following invocation cannot return an abrupt completion because of the validation preceding step 14.
                // 2. Perform ! varEnv.CreateMutableBinding(vn, true).
                variable_environment.create_mutable_binding(vm, var_name, true).must();

                // 3. Perform ! varEnv.InitializeBinding(vn, undefined, normal).
                variable_environment
                    .initialize_binding(vm, var_name, Value::UNDEFINED, InitializeBindingHint::Normal)
                    .must();
            }
        }
    }

    // 19. Return unused.
    Ok(())
}

// 10.4.4.6 CreateUnmappedArgumentsObject ( argumentsList ), https://tc39.es/ecma262/#sec-createunmappedargumentsobject
pub fn create_unmapped_arguments_object(vm: &Vm, arguments: &[Cell<Value>]) -> Gc<Object> {
    let realm = vm.current_realm().expect("there is a current realm");

    // 1. Let len be the number of elements in argumentsList.
    let length = arguments.len();

    // 2. Let obj be OrdinaryObjectCreate(%Object.prototype%, « [[ParameterMap]] »).
    // 3. Set obj.[[ParameterMap]] to undefined.
    let object = Object::create_with_premade_shape(vm, realm.unmapped_arguments_object_shape());

    // 4. Perform ! DefinePropertyOrThrow(obj, "length", PropertyDescriptor { [[Value]]: 𝔽(len), [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: true }).
    object.put_direct(
        realm.unmapped_arguments_object_length_offset(),
        Value::from_f64(length as f64),
    );

    // 5. Let index be 0.
    // 6. Repeat, while index < len,
    for (index, argument) in arguments.iter().enumerate() {
        // a. Let val be argumentsList[index].
        let value = argument.get();

        // b. Perform ! CreateDataPropertyOrThrow(obj, ! ToString(𝔽(index)), val).
        object.indexed_put(
            u32::try_from(index).expect("the argument index fits in u32"),
            value,
            DEFAULT_ATTRIBUTES,
        );

        // c. Set index to index + 1.
    }

    // 7. Perform ! DefinePropertyOrThrow(obj, @@iterator, PropertyDescriptor { [[Value]]: %Array.prototype.values%, [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: true }).
    let array_prototype_values = realm.array_prototype_values_function();
    object.put_direct(
        realm.unmapped_arguments_object_well_known_symbol_iterator_offset(),
        Value::from_object(array_prototype_values),
    );

    // 8. Perform ! DefinePropertyOrThrow(obj, "callee", PropertyDescriptor { [[Get]]: %ThrowTypeError%, [[Set]]: %ThrowTypeError%, [[Enumerable]]: false, [[Configurable]]: false }).
    object.put_direct(
        realm.unmapped_arguments_object_callee_offset(),
        Value::from_accessor(realm.throw_type_error_accessor()),
    );

    // 9. Return obj.
    object
}

// 10.4.4.7 CreateMappedArgumentsObject ( func, formals, argumentsList, env ), https://tc39.es/ecma262/#sec-createmappedargumentsobject
pub fn create_mapped_arguments_object(
    vm: &Vm,
    function: Gc<FunctionObject>,
    parameter_names: &[Utf16FlyString],
    arguments: &[Cell<Value>],
    environment: Gc<Environment>,
) -> Gc<Object> {
    let realm = vm.current_realm().expect("there is a current realm");

    // 1. Assert: formals does not contain a rest parameter, any binding patterns, or any initializers. It may contain duplicate identifiers.

    // 2. Let len be the number of elements in argumentsList.
    let length = i32::try_from(arguments.len()).expect("the argument count fits in i32");

    // 3. Let obj be MakeBasicObject(« [[Prototype]], [[Extensible]], [[ParameterMap]] »).
    // 4. Set obj.[[GetOwnProperty]] as specified in 10.4.4.1.
    // 5. Set obj.[[DefineOwnProperty]] as specified in 10.4.4.2.
    // 6. Set obj.[[Get]] as specified in 10.4.4.3.
    // 7. Set obj.[[Set]] as specified in 10.4.4.4.
    // 8. Set obj.[[Delete]] as specified in 10.4.4.5.
    // 9. Set obj.[[Prototype]] to %Object.prototype%.
    let object = ArgumentsObject::create(vm, realm, environment, parameter_names.is_empty());

    // 14. Let index be 0.
    // 15. Repeat, while index < len,
    for (index, argument) in arguments.iter().enumerate() {
        // a. Let val be argumentsList[index].
        let value = argument.get();

        // b. Perform ! CreateDataPropertyOrThrow(obj, ! ToString(𝔽(index)), val).
        object.indexed_put(
            u32::try_from(index).expect("the argument index fits in u32"),
            value,
            DEFAULT_ATTRIBUTES,
        );

        // c. Set index to index + 1.
    }

    // 16. Perform ! DefinePropertyOrThrow(obj, "length", PropertyDescriptor { [[Value]]: 𝔽(len), [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: true }).
    object.put_direct(realm.mapped_arguments_object_length_offset(), Value::from_i32(length));

    // OPTIMIZATION: We take a different route here than what the spec suggests.
    //               The spec would have us allocate a new object for the parameter map,
    //               and then populate it with getters and setters for each mapped parameter.
    //               That would be 1 GC allocation for the parameter map and 2 more for each
    //               parameter's getter/setter pair.
    //               Instead, we allocate the ArgumentsObject and let it implement the parameter map
    //               and getter/setter behavior itself without extra GC allocations.

    // 17. Let mappedNames be a new empty List.
    let mut seen_names: HashSet<Utf16FlyString> = HashSet::new();
    let mut mapped_names: Vec<Utf16FlyString> = Vec::new();

    // 18. Set index to numberOfParameters - 1.
    // 19. Repeat, while index ≥ 0,
    let parameter_count = i32::try_from(parameter_names.len()).expect("the parameter count fits in i32");
    for index in (0..parameter_count).rev() {
        // a. Let name be parameterNames[index].
        let name = &parameter_names[index as usize];

        // b. If name is not an element of mappedNames, then
        if seen_names.contains(name) {
            continue;
        }

        // i. Add name as an element of the list mappedNames.
        seen_names.insert(name.clone());

        // ii. If index < len, then
        if index < length {
            // 1. Let g be MakeArgGetter(name, env).
            // 2. Let p be MakeArgSetter(name, env).
            // 3. Perform ! map.[[DefineOwnProperty]](! ToString(𝔽(index)), PropertyDescriptor { [[Set]]: p, [[Get]]: g, [[Enumerable]]: false, [[Configurable]]: true }).
            if index as usize >= mapped_names.len() {
                mapped_names.resize(index as usize + 1, Utf16FlyString::default());
            }

            mapped_names[index as usize] = name.clone();
        }
    }

    object.set_mapped_names(mapped_names);

    // 20. Perform ! DefinePropertyOrThrow(obj, @@iterator, PropertyDescriptor { [[Value]]: %Array.prototype.values%, [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: true }).
    let array_prototype_values = realm.array_prototype_values_function();
    object.put_direct(
        realm.mapped_arguments_object_well_known_symbol_iterator_offset(),
        Value::from_object(array_prototype_values),
    );

    // 21. Perform ! DefinePropertyOrThrow(obj, "callee", PropertyDescriptor { [[Value]]: func, [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: true }).
    object.put_direct(
        realm.mapped_arguments_object_callee_offset(),
        Value::from_object(function),
    );

    // 22. Return obj.
    object.upcast()
}

// 22.1.3.19.1 GetSubstitution ( matched, str, position, captures, namedCaptures, replacementTemplate ), https://tc39.es/ecma262/#sec-getsubstitution
pub fn get_substitution(
    vm: &Vm,
    matched: Utf16View<'_>,
    str: Utf16View<'_>,
    position: usize,
    captures: &[Value],
    named_captures: Value,
    replacement_template: Utf16View<'_>,
) -> ThrowCompletionOr<Utf16String> {
    // 1. Let stringLength be the length of str.
    let string_length = str.length_in_code_units();

    // 2. Assert: position ≤ stringLength.
    assert!(position <= string_length);

    // 3. Let result be the empty String.
    let mut result = Utf16StringBuilder::new();

    // 4. Let templateRemainder be replacementTemplate.
    let mut template_remainder = replacement_template;

    // 5. Repeat, while templateRemainder is not the empty String,
    while !template_remainder.is_empty() {
        // a. NOTE: The following steps isolate ref (a prefix of templateRemainder), determine refReplacement (its replacement), and then append that replacement to result.

        let ref_length;
        let capture_string;

        // b. If templateRemainder starts with "$$", then
        let ref_replacement = if template_remainder.starts_with(Utf16View::Ascii(b"$$")) {
            // i. Let ref be "$$".
            ref_length = 2;

            // ii. Let refReplacement be "$".
            Utf16View::Ascii(b"$")
        }
        // c. Else if templateRemainder starts with "$`", then
        else if template_remainder.starts_with(Utf16View::Ascii(b"$`")) {
            // i. Let ref be "$`".
            ref_length = 2;

            // ii. Let refReplacement be the substring of str from 0 to position.
            str.substring_view(0, position)
        }
        // d. Else if templateRemainder starts with "$&", then
        else if template_remainder.starts_with(Utf16View::Ascii(b"$&")) {
            // i. Let ref be "$&".
            ref_length = 2;

            // ii. Let refReplacement be matched.
            matched
        }
        // e. Else if templateRemainder starts with "$'" (0x0024 (DOLLAR SIGN) followed by 0x0027 (APOSTROPHE)), then
        else if template_remainder.starts_with(Utf16View::Ascii(b"$'")) {
            // i. Let ref be "$'".
            ref_length = 2;

            // ii. Let matchLength be the length of matched.
            let match_length = matched.length_in_code_units();

            // iii. Let tailPos be position + matchLength.
            let tail_pos = position + match_length;

            // iv. Let refReplacement be the substring of str from min(tailPos, stringLength).
            // v. NOTE: tailPos can exceed stringLength only if this abstract operation was invoked by a call to the intrinsic @@replace method of %RegExp.prototype% on an object whose "exec" property is not the intrinsic %RegExp.prototype.exec%.
            let tail_start = tail_pos.min(string_length);
            str.substring_view(tail_start, string_length - tail_start)
        }
        // f. Else if templateRemainder starts with "$" followed by 1 or more decimal digits, then
        else if template_remainder.starts_with(Utf16View::Ascii(b"$"))
            && template_remainder.length_in_code_units() > 1
            && is_ascii_digit_code_unit(template_remainder.code_unit_at(1))
        {
            // i. If templateRemainder starts with "$" followed by 2 or more decimal digits, let digitCount be 2. Otherwise, let digitCount be 1.
            let mut digit_count = 1;

            if template_remainder.length_in_code_units() > 2
                && is_ascii_digit_code_unit(template_remainder.code_unit_at(2))
            {
                digit_count = 2;
            }

            // ii. Let digits be the substring of templateRemainder from 1 to 1 + digitCount.
            let digits = template_remainder.substring_view(1, digit_count);

            // iii. Let index be ℝ(StringToNumber(digits)).
            let mut index = decimal_digits_value(digits);

            // iv. Assert: 0 ≤ index ≤ 99.
            assert!(index <= 99);

            // v. Let captureLen be the number of elements in captures.
            let capture_length = captures.len();

            // vi. If index > captureLen and digitCount = 2, then
            if index > capture_length && digit_count == 2 {
                // 1. NOTE: When a two-digit replacement pattern specifies an index exceeding the count of capturing groups, it is treated as a one-digit replacement pattern followed by a literal digit.

                // 2. Set digitCount to 1.
                digit_count = 1;

                // 3. Set digits to the substring of digits from 0 to 1.
                let digits = digits.substring_view(0, 1);

                // 4. Set index to ℝ(StringToNumber(digits)).
                index = decimal_digits_value(digits);
            }

            // vii. Let ref be the substring of templateRemainder from 0 to 1 + digitCount.
            ref_length = 1 + digit_count;

            // viii. If 1 ≤ index ≤ captureLen, then
            if 1 <= index && index <= capture_length {
                // 1. Let capture be captures[index - 1].
                let capture = captures[index - 1];

                // 2. If capture is undefined, then
                if capture.is_undefined() {
                    // a. Let refReplacement be the empty String.
                    Utf16View::EMPTY
                }
                // 3. Else,
                else {
                    // a. Let refReplacement be capture.
                    capture_string = capture.to_utf16_string(vm)?;
                    Utf16View::of_string(&capture_string)
                }
            }
            // ix. Else,
            else {
                // 1. Let refReplacement be ref.
                template_remainder.substring_view(0, ref_length)
            }
        }
        // g. Else if templateRemainder starts with "$<", then
        else if template_remainder.starts_with(Utf16View::Ascii(b"$<")) {
            // i. Let gtPos be StringIndexOf(templateRemainder, ">", 0).
            // NOTE: We can actually start at index 2 because we know the string starts with "$<".
            let greater_than_position = string_index_of(template_remainder, Utf16View::Ascii(b">"), 2);

            // ii. If gtPos = -1 or namedCaptures is undefined, then
            match greater_than_position.filter(|_| !named_captures.is_undefined()) {
                None => {
                    // 1. Let ref be "$<".
                    ref_length = 2;

                    // 2. Let refReplacement be ref.
                    Utf16View::Ascii(b"$<")
                }
                // iii. Else,
                Some(greater_than_position) => {
                    // 1. Let ref be the substring of templateRemainder from 0 to gtPos + 1.
                    ref_length = greater_than_position + 1;

                    // 2. Let groupName be the substring of templateRemainder from 2 to gtPos.
                    let group_name = template_remainder
                        .substring_view(2, greater_than_position - 2)
                        .to_utf16_string();

                    // 3. Assert: namedCaptures is an Object.
                    assert!(named_captures.is_object());

                    // 4. Let capture be ? Get(namedCaptures, groupName).
                    let capture = named_captures.as_object().get(vm, &PropertyKey::from(&group_name))?;

                    // 5. If capture is undefined, then
                    if capture.is_undefined() {
                        // a. Let refReplacement be the empty String.
                        Utf16View::EMPTY
                    }
                    // 6. Else,
                    else {
                        // a. Let refReplacement be ? ToString(capture).
                        capture_string = capture.to_utf16_string(vm)?;
                        Utf16View::of_string(&capture_string)
                    }
                }
            }
        }
        // h. Else,
        else {
            // i. Let ref be the substring of templateRemainder from 0 to 1.
            ref_length = 1;

            // ii. Let refReplacement be ref.
            template_remainder.substring_view(0, 1)
        };

        // i. Let refLength be the length of ref.

        // k. Set result to the string-concatenation of result and refReplacement.
        result.append(ref_replacement);

        // j. Set templateRemainder to the substring of templateRemainder from refLength.
        // NOTE: We do this step last because refReplacement may point to templateRemainder.
        template_remainder =
            template_remainder.substring_view(ref_length, template_remainder.length_in_code_units() - ref_length);
    }

    // 6. Return result.
    Ok(result.to_utf16_string())
}

fn is_ascii_digit_code_unit(code_unit: u16) -> bool {
    (u16::from(b'0')..=u16::from(b'9')).contains(&code_unit)
}

/// ℝ(StringToNumber(digits)) of the one or two decimal digits of a replacement pattern.
fn decimal_digits_value(digits: Utf16View<'_>) -> usize {
    digits.code_units().fold(0, |value, code_unit| {
        value * 10 + usize::from(code_unit - u16::from(b'0'))
    })
}

// 2.1.1 DisposeCapability Records, https://tc39.es/proposal-explicit-resource-management/#sec-disposecapability-records
#[derive(Default, Trace)]
pub struct DisposeCapability {
    pub disposable_resource_stack: Option<Vec<DisposableResource>>, // [[DisposableResourceStack]]
}

// 2.1.2 DisposableResource Records, https://tc39.es/proposal-explicit-resource-management/#sec-disposableresource-records
#[derive(Clone, Copy, Trace)]
pub struct DisposableResource {
    pub resource_value: Option<Gc<Object>>, // [[ResourceValue]]
    #[gc(untraced)]
    pub hint: InitializeBindingHint, // [[Hint]]
    pub dispose_method: Option<Gc<FunctionObject>>, // [[DisposeMethod]]
}

// 2.1.3 NewDisposeCapability ( ), https://tc39.es/proposal-explicit-resource-management/#sec-newdisposecapability
pub fn new_dispose_capability() -> DisposeCapability {
    // 1. Let stack be a new empty List.
    // 2. Return the DisposeCapability Record { [[DisposableResourceStack]]: stack }.
    DisposeCapability::default()
}

// 2.1.4 AddDisposableResource ( disposeCapability, V, hint [ , method ] ), https://tc39.es/proposal-explicit-resource-management/#sec-adddisposableresource-disposable-v-hint-disposemethod
pub fn add_disposable_resource(
    vm: &Vm,
    dispose_capability: &GcRefCell<DisposeCapability>,
    value: Value,
    hint: InitializeBindingHint,
    method: Option<Gc<FunctionObject>>,
) -> ThrowCompletionOr<()> {
    let resource = match method {
        // 1. If method is not present then,
        None => {
            // a. If V is either null or undefined and hint is sync-dispose, then
            if value.is_nullish() && hint == InitializeBindingHint::SyncDispose {
                // i. Return unused.
                return Ok(());
            }

            // b. NOTE: When V is either null or undefined and hint is async-dispose, we record that the resource was evaluated
            //    to ensure we will still perform an Await when resources are later disposed.

            // c. Let resource be ? CreateDisposableResource(V, hint).
            create_disposable_resource(vm, value, hint, None)?
        }
        // 2. Else,
        Some(method) => {
            // a. Assert: V is undefined.
            assert!(value.is_undefined());

            // b. Let resource be ? CreateDisposableResource(undefined, hint, method).
            create_disposable_resource(vm, Value::UNDEFINED, hint, Some(method))?
        }
    };

    // 3. Append resource to disposeCapability.[[DisposableResourceStack]].
    // NB: Creating the resource can run JavaScript, so the capability is only borrowed to append to it.
    dispose_capability
        .borrow_mut()
        .disposable_resource_stack
        .get_or_insert_with(Vec::new)
        .push(resource);

    // 4. Return unused.
    Ok(())
}

// 2.1.5 CreateDisposableResource ( V, hint [ , method ] ), https://tc39.es/proposal-explicit-resource-management/#sec-createdisposableresource
pub fn create_disposable_resource(
    vm: &Vm,
    value: Value,
    hint: InitializeBindingHint,
    method: Option<Gc<FunctionObject>>,
) -> ThrowCompletionOr<DisposableResource> {
    let mut method = method;

    // 1. If method is not present, then
    // a. If V is either null or undefined, then
    //    i. Set V to undefined.
    //    ii. Set method to undefined.
    // b. Else,
    if method.is_none() && !value.is_nullish() {
        // i. If V is not an Object, throw a TypeError exception.
        if !value.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&value]);
        }

        // ii. Set method to ? GetDisposeMethod(V, hint).
        method = get_dispose_method(vm, value, hint)?;

        // iii. If method is undefined, throw a TypeError exception.
        if method.is_none() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NoDisposeMethod, &[&value]);
        }
    }
    // 2. Else,
    //    a. If IsCallable(method) is false, throw a TypeError exception.
    //    NOTE: This is guaranteed to never occur due to its type.

    // 3. Return the DisposableResource Record { [[ResourceValue]]: V, [[Hint]]: hint, [[DisposeMethod]]: method }.
    Ok(DisposableResource {
        resource_value: value.is_object().then(|| value.as_object()),
        hint,
        dispose_method: method,
    })
}

// 2.1.6 GetDisposeMethod ( V, hint ), https://tc39.es/proposal-explicit-resource-management/#sec-getdisposemethod
pub fn get_dispose_method(
    vm: &Vm,
    value: Value,
    hint: InitializeBindingHint,
) -> ThrowCompletionOr<Option<Gc<FunctionObject>>> {
    // 1. If hint is async-dispose, then
    if hint == InitializeBindingHint::AsyncDispose {
        // a. Let method be ? GetMethod(V, @@asyncDispose).
        let method = value.get_method(vm, &PropertyKey::from(vm.well_known_symbols().async_dispose))?;

        // b. If method is undefined, then
        if method.is_none() {
            // i. Set method to ? GetMethod(V, @@dispose).
            let method = value.get_method(vm, &PropertyKey::from(vm.well_known_symbols().dispose))?;

            // ii. If method is not undefined, then
            if let Some(method) = method {
                let realm = vm.current_realm().expect("there is a current realm");

                // 1. Let closure be a new Abstract Closure with no parameters that captures method and performs the
                //    following steps when called:
                // 2. NOTE: This function is not observable to user code. It is used to ensure that a Promise returned
                //    from a synchronous @@dispose method will not be awaited and that any exception thrown will not be
                //    thrown synchronously.
                // 3. Return CreateBuiltinFunction(closure, 0, "", « »).
                let closure = NativeFunction::create_anonymous(
                    vm,
                    (realm, method),
                    |vm, &(realm, method): &(Gc<Realm>, Gc<FunctionObject>)| {
                        // a. Let O be the this value.
                        let object = vm.this_value();

                        // b. Let promiseCapability be ! NewPromiseCapability(%Promise%).
                        let promise_capability =
                            new_promise_capability(vm, Value::from_object(realm.intrinsics().promise_constructor(vm)))
                                .must();

                        // c. Let result be Completion(Call(method, O)).
                        // d. IfAbruptRejectPromise(result, promiseCapability).
                        try_or_reject!(vm, promise_capability, call_function_object(vm, method, object, &[]));

                        // e. Perform ? Call(promiseCapability.[[Resolve]], undefined, « undefined »).
                        call_function_object(vm, promise_capability.resolve(), Value::UNDEFINED, &[Value::UNDEFINED])?;

                        // f. Return promiseCapability.[[Promise]].
                        Ok(Value::from_object(promise_capability.promise()))
                    },
                    0,
                );
                return Ok(Some(closure.upcast()));
            }
            return Ok(None);
        }

        // 3. Return method.
        return Ok(method);
    }

    // 2. Else,
    //    a. Let method be ? GetMethod(V, @@dispose).
    // 3. Return method.
    value.get_method(vm, &PropertyKey::from(vm.well_known_symbols().dispose))
}

/// Mirrors JS::CanonicalIndexMode.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CanonicalIndexMode {
    DetectNumericRoundtrip,
    IgnoreNumericRoundtrip,
}

// 7.1.21 CanonicalNumericIndexString ( argument ), https://tc39.es/ecma262/#sec-canonicalnumericindexstring
pub fn canonical_numeric_index_string(property_key: &PropertyKey, mode: CanonicalIndexMode) -> CanonicalIndex {
    // NOTE: If the property name is a number type (An implementation-defined optimized
    // property key type), it can be treated as a string property that has already been
    // converted successfully into a canonical numeric index.

    assert!(property_key.is_string() || property_key.is_number());

    if property_key.is_number() {
        return CanonicalIndex::new(CanonicalIndexType::Index, property_key.as_number());
    }

    if mode != CanonicalIndexMode::DetectNumericRoundtrip {
        return CanonicalIndex::new(CanonicalIndexType::Undefined, 0);
    }

    let argument = Utf16View::of_fly_string(property_key.as_string());
    let is_code_unit = |index: usize, character: u8| argument.code_unit_at(index) == u16::from(character);

    // Handle trivial cases without a full round trip test
    // We do not need to check for argument == "0" at this point because we
    // already covered it with the is_number() == true path.
    if argument.is_empty() {
        return CanonicalIndex::new(CanonicalIndexType::Undefined, 0);
    }

    let mut current_index = 0;

    if is_code_unit(current_index, b'-') {
        current_index += 1;
        if current_index == argument.length_in_code_units() {
            return CanonicalIndex::new(CanonicalIndexType::Undefined, 0);
        }
    }

    if is_code_unit(current_index, b'0') {
        current_index += 1;
        if current_index == argument.length_in_code_units() {
            return CanonicalIndex::new(CanonicalIndexType::Numeric, 0);
        }
        if !is_code_unit(current_index, b'.') {
            return CanonicalIndex::new(CanonicalIndexType::Undefined, 0);
        }
        current_index += 1;
        if current_index == argument.length_in_code_units() {
            return CanonicalIndex::new(CanonicalIndexType::Undefined, 0);
        }
    }

    // Short circuit a few common cases
    if argument == "Infinity" || argument == "-Infinity" || argument == "NaN" {
        return CanonicalIndex::new(CanonicalIndexType::Numeric, 0);
    }

    // Short circuit any string that doesn't start with digits
    let first_non_zero = argument.code_unit_at(current_index);
    if first_non_zero < u16::from(b'0') || first_non_zero > u16::from(b'9') {
        return CanonicalIndex::new(CanonicalIndexType::Undefined, 0);
    }

    // 2. Let n be ! ToNumber(argument).
    let code_units: Vec<u16> = argument.code_units().collect();
    let Some(number) = parse_number_f64(&code_units) else {
        return CanonicalIndex::new(CanonicalIndexType::Undefined, 0);
    };

    // FIXME: We return 0 instead of n but it might not observable?
    // 3. If SameValue(! ToString(n), argument) is true, return n.
    if Utf16View::Utf16(&number_to_utf16_string(number)) == argument {
        return CanonicalIndex::new(CanonicalIndexType::Numeric, 0);
    }

    // 4. Return undefined.
    CanonicalIndex::new(CanonicalIndexType::Undefined, 0)
}
