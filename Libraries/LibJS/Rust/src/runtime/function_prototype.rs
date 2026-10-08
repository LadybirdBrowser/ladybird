/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::{Utf16FlyString, Utf16String};
use libjs_abi::Builtin;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{call_function_object, length_of_array_like};
use crate::runtime::bound_function::{
    BOUND_FUNCTION_LENGTH_OFFSET, BOUND_FUNCTION_NAME_OFFSET, BoundFunction, LengthAndNameSlots,
};
use crate::runtime::class_field_definition::ClassElementName;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::ecmascript_function_object::as_ecmascript_function_object;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::{FUNCTION_OBJECT_METHODS, FunctionObject};
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{IndexedStorageKind, MayInterfereWithIndexedPropertyAccess, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::value::ordinary_has_instance;
use crate::utf16::{Utf16StringBuilder, Utf16View, concatenate};

/// %Function.prototype%, a function object that accepts any arguments and returns undefined.
#[repr(C)]
#[derive(Trace)]
pub struct FunctionPrototype {
    base: FunctionObject,
}

define_object_class!(FunctionPrototype, extends: [FunctionObject, Object], methods: {
    initialize: FunctionPrototype::initialize,
    internal_call: Some(FunctionPrototype::internal_call),
    name_for_call_stack: |_| Utf16String::from_utf8("(Function.prototype)"),
    ..FUNCTION_OBJECT_METHODS
});

impl FunctionPrototype {
    pub fn new(vm: &Vm, realm: Gc<Realm>) -> FunctionPrototype {
        FunctionPrototype {
            base: FunctionObject::new_with_prototype(
                vm,
                Self::CLASS,
                realm.object_prototype(),
                MayInterfereWithIndexedPropertyAccess::No,
            ),
        }
    }

    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<FunctionPrototype> {
        realm.create_object(vm, Self::new(vm, realm))
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.apply,
            raw_native!(FunctionPrototype::apply),
            2,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.bind,
            raw_native!(FunctionPrototype::bind),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.call,
            raw_native!(FunctionPrototype::call),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.toString,
            raw_native!(FunctionPrototype::to_string),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &PropertyKey::from(vm.well_known_symbols().has_instance),
            raw_native!(FunctionPrototype::symbol_has_instance),
            1,
            PropertyAttributes::new(0),
            Some(Builtin::OrdinaryHasInstance),
        );
        object.define_direct_property(
            vm,
            &names.length,
            Value::from_i32(0),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
        object.define_direct_property(
            vm,
            &names.name,
            Value::from_string(PrimitiveString::create(vm, Utf16String::default())),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    #[allow(clippy::unnecessary_wraps, reason = "[[Call]] can throw for other functions")]
    fn internal_call(_: &Object, _: &Vm, _: &ExecutionContext, _: Value) -> ThrowCompletionOr<Value> {
        // The Function prototype object:
        // - accepts any arguments and returns undefined when invoked.
        Ok(Value::UNDEFINED)
    }

    // 20.2.3.1 Function.prototype.apply ( thisArg, argArray ), https://tc39.es/ecma262/#sec-function.prototype.apply
    fn apply(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let func be the this value.
        let function_value = vm.this_value();

        // 2. If IsCallable(func) is false, throw a TypeError exception.
        if !function_value.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&function_value]);
        }

        let function = function_value.as_function();

        let this_arg = vm.argument(0);
        let arg_array = vm.argument(1);

        // 3. If argArray is undefined or null, then
        if arg_array.is_nullish() {
            // FIXME: a. Perform PrepareForTailCall().

            // b. Return ? Call(func, thisArg).
            return call_function_object(vm, function, this_arg, &[]);
        }

        // NOTE: Do the check performed by CreateListFromArrayLike here, so we could avoid branching in optimized code path.
        if !arg_array.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&arg_array]);
        }

        let arg_array_object = arg_array.as_object();

        // 4. Let argList be ? CreateListFromArrayLike(argArray).
        let length = length_of_array_like(vm, &arg_array_object)?;

        // OPTIMIZATION: If argArray has a simple indexed storage without holes and doesn't interfere with indexed property access,
        //               we can skip building argList and directly use the storage elements.
        if !arg_array_object.may_interfere_with_indexed_property_access()
            && arg_array_object.indexed_storage_kind() == IndexedStorageKind::Packed
        {
            // NB: A call may change the storage, so this copies the elements first: few enough onto the stack, which
            //     the collector scans, and others into a rooted list.
            if u64::from(arg_array_object.indexed_packed_elements_span_size()) >= length {
                let length = length as usize;
                if length <= STACK_ARGUMENT_CAPACITY {
                    let mut arguments = [Value::UNDEFINED; STACK_ARGUMENT_CAPACITY];
                    let arguments = &mut arguments[..length];
                    arg_array_object.copy_indexed_packed_elements(arguments);
                    return call_function_object(vm, function, this_arg, arguments);
                }
                let elements = arg_array_object.indexed_packed_elements(vm);
                return elements
                    .with_values(|elements| call_function_object(vm, function, this_arg, &elements[..length]));
            }
        }

        // OPTIMIZATION: Few enough arguments are gathered on the stack, which the collector scans, rather than in a rooted
        //               list.
        if length <= STACK_ARGUMENT_CAPACITY as u64 {
            let mut arguments = [Value::UNDEFINED; STACK_ARGUMENT_CAPACITY];
            let arguments = &mut arguments[..length as usize];
            for (index, argument) in arguments.iter_mut().enumerate() {
                *argument = arg_array_object.get(vm, &PropertyKey::from_number(index as u64))?;
            }
            return call_function_object(vm, function, this_arg, arguments);
        }

        let arguments = MarkedVec::with_capacity(vm, length as usize);
        for index in 0..length {
            arguments.push(arg_array_object.get(vm, &PropertyKey::from_number(index))?);
        }

        // FIXME: 5. Perform PrepareForTailCall().

        // 6. Return ? Call(func, thisArg, argList).
        arguments.with_values(|arguments| call_function_object(vm, function, this_arg, arguments))
    }

    // 20.2.3.2 Function.prototype.bind ( thisArg, ...args ), https://tc39.es/ecma262/#sec-function.prototype.bind
    fn bind(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a native function runs in a realm");

        let this_argument = vm.argument(0);

        // 1. Let Target be the this value.
        let target_value = vm.this_value();

        // 2. If IsCallable(Target) is false, throw a TypeError exception.
        if !target_value.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&target_value]);
        }

        let target = target_value.as_function();

        let arguments = arguments_after_the_first(vm);

        // 3. Let F be ? BoundFunctionCreate(Target, thisArg, args).
        let (function, length_and_name_slots) =
            BoundFunction::create(vm, realm, target, this_argument, &arguments.to_vec())?;

        // 4. Let L be 0.
        let mut length = 0.0;

        // 5. Let targetHasLength be ? HasOwnProperty(Target, "length").
        // OPTIMIZATION: When Target has an own "length" data property, which functions usually do, HasOwnProperty is
        //               true and Get returns its value.
        let own_length = own_data_property_value(vm, &target, &vm.names.length);
        let target_has_length = own_length.is_some() || target.has_own_property(vm, &vm.names.length)?;

        // 6. If targetHasLength is true, then
        if target_has_length {
            // a. Let targetLen be ? Get(Target, "length").
            let target_length = match own_length {
                Some(length) => length,
                None => target.get(vm, &vm.names.length)?,
            };

            // b. If targetLen is a Number, then
            if target_length.is_number() {
                // i. If targetLen is +∞𝔽, then
                if target_length.is_positive_infinity() {
                    // 1. Set L to +∞.
                    length = target_length.as_f64();
                }
                // ii. Else if targetLen is -∞𝔽, then
                else if target_length.is_negative_infinity() {
                    // 1. Set L to 0.
                    length = 0.0;
                }
                // iii. Else,
                else {
                    // 1. Let targetLenAsInt be ! ToIntegerOrInfinity(targetLen).
                    let target_length_as_int = target_length.to_integer_or_infinity(vm).must();

                    // 2. Assert: targetLenAsInt is finite.
                    assert!(!target_length_as_int.is_infinite());

                    // 3. Let argCount be the number of elements in args.
                    let arg_count = vm.argument_count().saturating_sub(1);

                    // 4. Set L to max(targetLenAsInt - argCount, 0).
                    length = (target_length_as_int - arg_count as f64).max(0.0);
                }
            }
        }

        // 7. Perform SetFunctionLength(F, L).
        match length_and_name_slots {
            LengthAndNameSlots::Premade => function.put_direct(BOUND_FUNCTION_LENGTH_OFFSET, Value::from_f64(length)),
            LengthAndNameSlots::Absent => function.set_function_length(vm, length),
        }

        // 8. Let targetName be ? Get(Target, "name").
        let target_name = match own_data_property_value(vm, &target, &vm.names.name) {
            Some(name) => name,
            None => target.get(vm, &vm.names.name)?,
        };

        // 9. If targetName is not a String, set targetName to the empty String.
        // 10. Perform SetFunctionName(F, targetName, "bound").
        match length_and_name_slots {
            LengthAndNameSlots::Premade => {
                // OPTIMIZATION: F is not a native function, and targetName is a String by now, so SetFunctionName sets its
                //               "name" to the string-concatenation of "bound", a space and targetName.
                let target_name = target_name.is_string().then(|| target_name.as_string());
                let target_name_length = target_name.map_or(0, |name| name.length_in_utf16_code_units());
                let mut builder = Utf16StringBuilder::with_capacity("bound ".len() + target_name_length);
                builder.append_ascii("bound ");
                if let Some(target_name) = target_name {
                    builder.append(target_name.utf16_string_view());
                }
                function.put_direct(
                    BOUND_FUNCTION_NAME_OFFSET,
                    Value::from_string(PrimitiveString::create(vm, builder.to_utf16_string())),
                );
            }
            LengthAndNameSlots::Absent => {
                let target_name = if target_name.is_string() {
                    target_name.as_string().utf16_string()
                } else {
                    Utf16String::default()
                };
                function.set_function_name(
                    vm,
                    &ClassElementName::PropertyKey(PropertyKey::from(&target_name)),
                    Some("bound"),
                );
            }
        }

        // 11. Return F.
        Ok(Value::from_object(function))
    }

    // 20.2.3.3 Function.prototype.call ( thisArg, ...args ), https://tc39.es/ecma262/#sec-function.prototype.call
    fn call(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let func be the this value.
        let function_value = vm.this_value();

        // 2. If IsCallable(func) is false, throw a TypeError exception.
        if !function_value.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&function_value]);
        }

        let function = function_value.as_function();

        // FIXME: 3. Perform PrepareForTailCall().

        let this_arg = vm.argument(0);

        // 4. Return ? Call(func, thisArg, args).
        // NB: This call's arguments are in the interpreter stack the call runs on, so they are copied first: few
        //     enough onto the stack, which the collector scans, and others into a rooted list.
        let argument_count = vm.argument_count().saturating_sub(1);
        if argument_count <= STACK_ARGUMENT_CAPACITY {
            let mut args = [Value::UNDEFINED; STACK_ARGUMENT_CAPACITY];
            let args = &mut args[..argument_count];
            for (index, argument) in args.iter_mut().enumerate() {
                *argument = vm.argument(index + 1);
            }
            return call_function_object(vm, function, this_arg, args);
        }
        let args = arguments_after_the_first(vm);
        args.with_values(|args| call_function_object(vm, function, this_arg, args))
    }

    // 20.2.3.5 Function.prototype.toString ( ), https://tc39.es/ecma262/#sec-function.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let func be the this value.
        let function_value = vm.this_value();

        // OPTIMIZATION: If func is not a function, bail out early. The order of this step is not observable.
        if !function_value.is_function() {
            // 5. Throw a TypeError exception.
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"Function"]);
        }

        let function = function_value.as_function();

        // 2. If Type(func) is Object and func has a [[SourceText]] internal slot and func.[[SourceText]] is a sequence of Unicode code points and HostHasSourceTextAvailable(func) is true, then
        if let Some(ecma_script_function_object) = as_ecmascript_function_object(function) {
            // a. Return CodePointsToString(func.[[SourceText]]).
            return Ok(Value::from_string(PrimitiveString::create(
                vm,
                ecma_script_function_object.source_text(),
            )));
        }

        // 3. If func is a built-in function object, return an implementation-defined String source code representation of func. The representation must have the syntax of a NativeFunction. Additionally, if func has an [[InitialName]] internal slot and func.[[InitialName]] is a String, the portion of the returned String that would be matched by NativeFunctionAccessor[opt] PropertyName must be the value of func.[[InitialName]].
        if let Some(native_function) = function.as_native_function() {
            // NOTE: once we remove name(), the fallback here can simply be an empty string.
            let name = native_function.initial_name().unwrap_or_else(|| native_function.name());
            return Ok(Value::from_string(PrimitiveString::create(
                vm,
                concatenate(&[
                    Utf16View::Ascii(b"function "),
                    Utf16View::of_fly_string(&name),
                    Utf16View::Ascii(b"() { [native code] }"),
                ]),
            )));
        }

        // 4. If Type(func) is Object and IsCallable(func) is true, return an implementation-defined String source code representation of func. The representation must have the syntax of a NativeFunction.
        // NOTE: ProxyObject, BoundFunction, WrappedFunction
        Ok(Value::from_string(PrimitiveString::create_from_fly_string(
            vm,
            &Utf16FlyString::from_utf8("function () { [native code] }"),
        )))
    }

    // 20.2.3.6 Function.prototype [ @@hasInstance ] ( V ), https://tc39.es/ecma262/#sec-function.prototype-@@hasinstance
    fn symbol_has_instance(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let F be the this value.
        // 2. Return ? OrdinaryHasInstance(F, V).
        ordinary_has_instance(vm, vm.argument(0), vm.this_value())
    }
}

/// How many arguments apply(), call() and proxies copy onto the stack rather than into a rooted list.
pub(crate) const STACK_ARGUMENT_CAPACITY: usize = 16;

/// The value of an own data property of `object` named `property_key`, if [[GetOwnProperty]] of the object is the
/// ordinary one for that key and finds such a property. Then HasOwnProperty is true and Get returns that value, and
/// reading it runs no code.
fn own_data_property_value(vm: &Vm, object: &Object, property_key: &PropertyKey) -> Option<Value> {
    if !object.has_ordinary_get_own_property_for(vm, property_key) {
        return None;
    }
    let property = object.storage_get(vm, property_key)?;
    if property.value.is_accessor() {
        return None;
    }
    Some(property.value)
}

/// The arguments of the running native function after its first, which the spec calls ...args.
fn arguments_after_the_first(vm: &Vm) -> MarkedVec<'_, Value> {
    let argument_count = vm.argument_count();
    let arguments = MarkedVec::with_capacity(vm, argument_count.saturating_sub(1));
    for index in 1..argument_count {
        arguments.push(vm.argument(index));
    }
    arguments
}
