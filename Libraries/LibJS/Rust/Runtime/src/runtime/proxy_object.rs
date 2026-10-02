/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, RefCell};
use core::ops::Deref;
use std::collections::HashSet;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    call, call_function_object, create_list_from_array_like, is_compatible_property_descriptor,
};
use crate::runtime::array::Array;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::{FUNCTION_OBJECT_METHODS, FunctionObject};
use crate::runtime::function_prototype::STACK_ARGUMENT_CAPACITY;
use crate::runtime::object::{
    CacheableGetPropertyMetadata, CacheableSetPropertyMetadata, MayInterfereWithIndexedPropertyAccess, ObjectMethods,
    PropertyLookupPhase, StackFrameInfo,
};
use crate::runtime::property_descriptor::{PropertyDescriptor, from_property_descriptor, to_property_descriptor};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::value::same_value;

// NOTE: We can't rely on native stack overflows to catch infinite recursion in Proxy traps,
//       since the compiler may decide to optimize tail/sibling calls into loops.
//       So instead we keep track of the recursion depth and throw a TypeError if it exceeds a certain limit.

thread_local! {
    static RECURSION_DEPTH: Cell<u32> = const { Cell::new(0) };
}

const MAX_PROXY_RECURSION_DEPTH: u32 = 10000;

struct RecursionDepthUpdater;

impl RecursionDepthUpdater {
    fn new() -> Self {
        RECURSION_DEPTH.with(|depth| depth.set(depth.get() + 1));
        Self
    }
}

impl Drop for RecursionDepthUpdater {
    fn drop(&mut self) {
        RECURSION_DEPTH.with(|depth| depth.set(depth.get() - 1));
    }
}

/// LIMIT_PROXY_RECURSION_DEPTH(): counts the running trap for as long as the returned updater lives.
fn limit_proxy_recursion_depth(vm: &Vm) -> ThrowCompletionOr<RecursionDepthUpdater> {
    let recursion_depth_updater = RecursionDepthUpdater::new();
    if RECURSION_DEPTH.with(Cell::get) >= MAX_PROXY_RECURSION_DEPTH {
        return vm.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
    }
    Ok(recursion_depth_updater)
}

/// A field of a descriptor that [[GetOwnProperty]] returned, which C++ dereferences as an Optional that has a value.
fn fully_populated<T>(field: Option<T>) -> T {
    field.expect("an own property descriptor is fully populated")
}

#[repr(C)]
#[derive(Trace)]
pub struct ProxyObject {
    base: FunctionObject,
    target: Gc<Object>,  // [[ProxyTarget]]
    handler: Gc<Object>, // [[ProxyHandler]]
    is_revoked: Cell<bool>,
}

/// The internal methods of a Proxy exotic object. Every proxy has [[Call]] and [[Construct]], as the C++ ProxyObject
/// overrides both: IsCallable and IsConstructor only reach them for a proxy whose target has them, since the proxy
/// keeps the IsFunction flag only if its target is callable and has_constructor() asks the target.
pub static PROXY_OBJECT_METHODS: ObjectMethods = ObjectMethods {
    internal_get_prototype_of: ProxyObject::internal_get_prototype_of,
    internal_set_prototype_of: ProxyObject::internal_set_prototype_of,
    internal_is_extensible: ProxyObject::internal_is_extensible,
    internal_prevent_extensions: ProxyObject::internal_prevent_extensions,
    internal_get_own_property: ProxyObject::internal_get_own_property,
    internal_define_own_property: ProxyObject::internal_define_own_property,
    internal_has_property: ProxyObject::internal_has_property,
    internal_get: ProxyObject::internal_get,
    is_cacheable_for_property_absence: |_| false,
    internal_set: ProxyObject::internal_set,
    internal_delete: ProxyObject::internal_delete,
    internal_own_property_keys: ProxyObject::internal_own_property_keys,
    internal_call: Some(ProxyObject::internal_call),
    internal_construct: Some(ProxyObject::internal_construct),
    has_constructor: |object| as_proxy_object(object).has_constructor(),
    name_for_call_stack: |_| ProxyObject::name_for_call_stack(),
    eligible_for_own_property_enumeration_fast_path: |_| false,
    get_stack_frame_info: ProxyObject::get_stack_frame_info,
    ..FUNCTION_OBJECT_METHODS
};

define_cell!(ProxyObject, Object, extends: [FunctionObject, Object], methods: PROXY_OBJECT_METHODS);

impl Deref for ProxyObject {
    type Target = FunctionObject;

    fn deref(&self) -> &FunctionObject {
        &self.base
    }
}

/// The proxy an internal method of a proxy was called on.
fn as_proxy_object(object: &Object) -> &ProxyObject {
    assert!(object.is_proxy_object());
    // SAFETY: The object is a ProxyObject, which starts with its Object.
    unsafe { &*core::ptr::from_ref(object).cast::<ProxyObject>() }
}

impl Object {
    pub fn is_proxy_object(&self) -> bool {
        self.is::<ProxyObject>()
    }
}

/// The C++ GC::ConservativeHashTable<PropertyKey> of the keys an ownKeys trap reported. Symbol keys are told apart by
/// their address, which stays theirs since the list the trap result is collected into keeps them alive.
#[derive(Default)]
struct UniquePropertyKeys {
    string_and_number_keys: HashSet<PropertyKey>,
    symbol_addresses: HashSet<usize>,
}

impl UniquePropertyKeys {
    fn set(&mut self, property_key: PropertyKey) {
        if property_key.is_symbol() {
            self.symbol_addresses.insert(property_key.as_symbol().as_ptr().addr());
        } else {
            self.string_and_number_keys.insert(property_key);
        }
    }

    fn size(&self) -> usize {
        self.string_and_number_keys.len() + self.symbol_addresses.len()
    }
}

/// uncheckedResultKeys of [[OwnPropertyKeys]]: the elements of trapResult that have not been removed yet.
struct UncheckedResultKeys<'list, 'vm> {
    trap_result: &'list MarkedVec<'vm, Value>,
    is_unchecked: Vec<bool>,
}

impl<'list, 'vm> UncheckedResultKeys<'list, 'vm> {
    fn new(trap_result: &'list MarkedVec<'vm, Value>) -> Self {
        Self {
            trap_result,
            is_unchecked: vec![true; trap_result.len()],
        }
    }

    /// The C++ contains_slow() followed by remove_first_matching(), both of which compare with SameValue: removes the
    /// first unchecked element that is `key`, and says whether there was one.
    fn remove_first_matching(&mut self, key: Value) -> bool {
        for (index, is_unchecked) in self.is_unchecked.iter_mut().enumerate() {
            if *is_unchecked && same_value(self.trap_result.get(index).expect("the index is in bounds"), key) {
                *is_unchecked = false;
                return true;
            }
        }
        false
    }

    fn first(&self) -> Option<Value> {
        let index = self.is_unchecked.iter().position(|is_unchecked| *is_unchecked)?;
        self.trap_result.get(index)
    }
}

impl ProxyObject {
    pub fn create(vm: &Vm, realm: Gc<Realm>, target: Gc<Object>, handler: Gc<Object>) -> Gc<ProxyObject> {
        realm.create_object(vm, Self::new(vm, target, handler, realm.object_prototype()))
    }

    fn new(vm: &Vm, target: Gc<Object>, handler: Gc<Object>, prototype: Gc<Object>) -> ProxyObject {
        let proxy_object = ProxyObject {
            base: FunctionObject::new_with_prototype(
                vm,
                Self::CLASS,
                prototype,
                MayInterfereWithIndexedPropertyAccess::Yes,
            ),
            target,
            handler,
            is_revoked: Cell::new(false),
        };

        // A Proxy is callable iff its target is callable.
        if !target.is_function() {
            proxy_object.clear_is_function();
        }

        if let Some(array) = target.downcast::<Array>() {
            array.set_is_proxy_target(true);
        }

        proxy_object
    }

    pub fn target(&self) -> Gc<Object> {
        self.target
    }

    pub fn handler(&self) -> Gc<Object> {
        self.handler
    }

    pub fn is_revoked(&self) -> bool {
        self.is_revoked.get()
    }

    pub fn revoke(&self) {
        self.is_revoked.set(true);
    }

    // 10.5 Proxy Object Internal Methods and Internal Slots, https://tc39.es/ecma262/#sec-proxy-object-internal-methods-and-internal-slots

    // 10.5.1 [[GetPrototypeOf]] ( ), https://tc39.es/ecma262/#sec-proxy-object-internal-methods-and-internal-slots-getprototypeof
    fn internal_get_prototype_of(object: &Object, vm: &Vm) -> ThrowCompletionOr<Option<Gc<Object>>> {
        let _recursion_depth_updater = limit_proxy_recursion_depth(vm)?;
        let proxy = as_proxy_object(object);

        // 1. Perform ? ValidateNonRevokedProxy(O).
        proxy.validate_non_revoked_proxy(vm)?;

        // 2. Let target be O.[[ProxyTarget]].
        // 3. Let handler be O.[[ProxyHandler]].
        // 4. Assert: handler is an Object.
        let target = proxy.target;
        let handler = proxy.handler;

        // 5. Let trap be ? GetMethod(handler, "getPrototypeOf").
        let trap = Value::from_object(handler).get_method(vm, &vm.names.getPrototypeOf)?;

        // 6. If trap is undefined, then
        let Some(trap) = trap else {
            // a. Return ? target.[[GetPrototypeOf]]().
            return target.internal_get_prototype_of(vm);
        };

        // 7. Let handlerProto be ? Call(trap, handler, « target »).
        let handler_proto = call_function_object(vm, trap, Value::from_object(handler), &[Value::from_object(target)])?;

        // 8. If Type(handlerProto) is neither Object nor Null, throw a TypeError exception.
        if !handler_proto.is_object() && !handler_proto.is_null() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxyGetPrototypeOfReturn, &[]);
        }
        let handler_proto_object = (!handler_proto.is_null()).then(|| handler_proto.as_object());

        // 9. Let extensibleTarget be ? IsExtensible(target).
        let extensible_target = target.is_extensible(vm)?;

        // 10. If extensibleTarget is true, return handlerProto.
        if extensible_target {
            return Ok(handler_proto_object);
        }

        // 11. Let targetProto be ? target.[[GetPrototypeOf]]().
        let target_proto = target.internal_get_prototype_of(vm)?;

        // 12. If SameValue(handlerProto, targetProto) is false, throw a TypeError exception.
        if !same_value(handler_proto, target_proto.map_or(Value::NULL, Value::from_object)) {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxyGetPrototypeOfNonExtensible, &[]);
        }

        // 13. Return handlerProto.
        Ok(handler_proto_object)
    }

    // 10.5.2 [[SetPrototypeOf]] ( V ), https://tc39.es/ecma262/#sec-proxy-object-internal-methods-and-internal-slots-setprototypeof-v
    fn internal_set_prototype_of(object: &Object, vm: &Vm, prototype: Option<Gc<Object>>) -> ThrowCompletionOr<bool> {
        let _recursion_depth_updater = limit_proxy_recursion_depth(vm)?;
        let proxy = as_proxy_object(object);

        // 1. Perform ? ValidateNonRevokedProxy(O).
        proxy.validate_non_revoked_proxy(vm)?;

        // 2. Let target be O.[[ProxyTarget]].
        // 3. Let handler be O.[[ProxyHandler]].
        // 4. Assert: handler is an Object.
        let target = proxy.target;
        let handler = proxy.handler;

        // 5. Let trap be ? GetMethod(handler, "setPrototypeOf").
        let trap = Value::from_object(handler).get_method(vm, &vm.names.setPrototypeOf)?;

        // 6. If trap is undefined, then
        let Some(trap) = trap else {
            // a. Return ? target.[[SetPrototypeOf]](V).
            return target.internal_set_prototype_of(vm, prototype);
        };

        let prototype_value = prototype.map_or(Value::NULL, Value::from_object);

        // 7. Let booleanTrapResult be ToBoolean(? Call(trap, handler, « target, V »)).
        let trap_result = call_function_object(
            vm,
            trap,
            Value::from_object(handler),
            &[Value::from_object(target), prototype_value],
        )?
        .to_boolean();

        // 8. If booleanTrapResult is false, return false.
        if !trap_result {
            return Ok(false);
        }

        // 9. Let extensibleTarget be ? IsExtensible(target).
        let extensible_target = target.is_extensible(vm)?;

        // 10. If extensibleTarget is true, return true.
        if extensible_target {
            return Ok(true);
        }

        // 11. Let targetProto be ? target.[[GetPrototypeOf]]().
        let target_proto = target.internal_get_prototype_of(vm)?;

        // 12. If SameValue(V, targetProto) is false, throw a TypeError exception.
        if !same_value(prototype_value, target_proto.map_or(Value::NULL, Value::from_object)) {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxySetPrototypeOfNonExtensible, &[]);
        }

        // 13. Return true.
        Ok(true)
    }

    // 10.5.3 [[IsExtensible]] ( ), https://tc39.es/ecma262/#sec-proxy-object-internal-methods-and-internal-slots-isextensible
    fn internal_is_extensible(object: &Object, vm: &Vm) -> ThrowCompletionOr<bool> {
        let _recursion_depth_updater = limit_proxy_recursion_depth(vm)?;
        let proxy = as_proxy_object(object);

        // 1. Perform ? ValidateNonRevokedProxy(O).
        proxy.validate_non_revoked_proxy(vm)?;

        // 2. Let target be O.[[ProxyTarget]].
        // 3. Let handler be O.[[ProxyHandler]].
        // 4. Assert: handler is an Object.
        let target = proxy.target;
        let handler = proxy.handler;

        // 5. Let trap be ? GetMethod(handler, "isExtensible").
        let trap = Value::from_object(handler).get_method(vm, &vm.names.isExtensible)?;

        // 6. If trap is undefined, then
        let Some(trap) = trap else {
            // a. Return ? IsExtensible(target).
            return target.is_extensible(vm);
        };

        // 7. Let booleanTrapResult be ToBoolean(? Call(trap, handler, « target »)).
        let trap_result =
            call_function_object(vm, trap, Value::from_object(handler), &[Value::from_object(target)])?.to_boolean();

        // 8. Let targetResult be ? IsExtensible(target).
        let target_result = target.is_extensible(vm)?;

        // 9. If SameValue(booleanTrapResult, targetResult) is false, throw a TypeError exception.
        if trap_result != target_result {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxyIsExtensibleReturn, &[]);
        }

        // 10. Return booleanTrapResult.
        Ok(trap_result)
    }

    // 10.5.4 [[PreventExtensions]] ( ), https://tc39.es/ecma262/#sec-proxy-object-internal-methods-and-internal-slots-preventextensions
    fn internal_prevent_extensions(object: &Object, vm: &Vm) -> ThrowCompletionOr<bool> {
        let _recursion_depth_updater = limit_proxy_recursion_depth(vm)?;
        let proxy = as_proxy_object(object);

        // 1. Perform ? ValidateNonRevokedProxy(O).
        proxy.validate_non_revoked_proxy(vm)?;

        // 2. Let target be O.[[ProxyTarget]].
        // 3. Let handler be O.[[ProxyHandler]].
        // 4. Assert: handler is an Object.
        let target = proxy.target;
        let handler = proxy.handler;

        // 5. Let trap be ? GetMethod(handler, "preventExtensions").
        let trap = Value::from_object(handler).get_method(vm, &vm.names.preventExtensions)?;

        // 6. If trap is undefined, then
        let Some(trap) = trap else {
            // a. Return ? target.[[PreventExtensions]]().
            return target.internal_prevent_extensions(vm);
        };

        // 7. Let booleanTrapResult be ToBoolean(? Call(trap, handler, « target »)).
        let trap_result =
            call_function_object(vm, trap, Value::from_object(handler), &[Value::from_object(target)])?.to_boolean();

        // 8. If booleanTrapResult is true, then
        if trap_result {
            // a. Let extensibleTarget be ? IsExtensible(target).
            let extensible_target = target.is_extensible(vm)?;

            // b. If extensibleTarget is true, throw a TypeError exception.
            if extensible_target {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxyPreventExtensionsReturn, &[]);
            }
        }

        // 9. Return booleanTrapResult.
        Ok(trap_result)
    }

    // 10.5.5 [[GetOwnProperty]] ( P ), https://tc39.es/ecma262/#sec-proxy-object-internal-methods-and-internal-slots-getownproperty-p
    fn internal_get_own_property(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
    ) -> ThrowCompletionOr<Option<PropertyDescriptor>> {
        let _recursion_depth_updater = limit_proxy_recursion_depth(vm)?;
        let proxy = as_proxy_object(object);

        // 1. Perform ? ValidateNonRevokedProxy(O).
        proxy.validate_non_revoked_proxy(vm)?;

        // 2. Let target be O.[[ProxyTarget]].
        // 3. Let handler be O.[[ProxyHandler]].
        // 4. Assert: handler is an Object.
        let target = proxy.target;
        let handler = proxy.handler;

        // 5. Let trap be ? GetMethod(handler, "getOwnPropertyDescriptor").
        let trap = Value::from_object(handler).get_method(vm, &vm.names.getOwnPropertyDescriptor)?;

        // 6. If trap is undefined, then
        let Some(trap) = trap else {
            // a. Return ? target.[[GetOwnProperty]](P).
            return target.internal_get_own_property(vm, property_key);
        };

        // 7. Let trapResultObj be ? Call(trap, handler, « target, P »).
        let trap_result = call_function_object(
            vm,
            trap,
            Value::from_object(handler),
            &[Value::from_object(target), property_key.to_value(vm)],
        )?;

        // 8. If Type(trapResultObj) is neither Object nor Undefined, throw a TypeError exception.
        if !trap_result.is_object() && !trap_result.is_undefined() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxyGetOwnDescriptorReturn, &[]);
        }

        // 9. Let targetDesc be ? target.[[GetOwnProperty]](P).
        let target_descriptor = target.internal_get_own_property(vm, property_key)?;

        // 10. If trapResultObj is undefined, then
        if trap_result.is_undefined() {
            // a. If targetDesc is undefined, return undefined.
            let Some(target_descriptor) = target_descriptor else {
                return Ok(None);
            };

            // b. If targetDesc.[[Configurable]] is false, throw a TypeError exception.
            if !fully_populated(target_descriptor.configurable) {
                return vm.throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::ProxyGetOwnDescriptorNonConfigurable,
                    &[],
                );
            }

            // c. Let extensibleTarget be ? IsExtensible(target).
            let extensible_target = target.is_extensible(vm)?;

            // d. If extensibleTarget is false, throw a TypeError exception.
            if !extensible_target {
                return vm.throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::ProxyGetOwnDescriptorUndefinedReturn,
                    &[],
                );
            }

            // e. Return undefined.
            return Ok(None);
        }

        // 11. Let extensibleTarget be ? IsExtensible(target).
        let extensible_target = target.is_extensible(vm)?;

        // 12. Let resultDesc be ? ToPropertyDescriptor(trapResultObj).
        let mut result_desc = to_property_descriptor(vm, trap_result)?;

        // 13. Perform CompletePropertyDescriptor(resultDesc).
        result_desc.complete();

        // 14. Let valid be IsCompatiblePropertyDescriptor(extensibleTarget, resultDesc, targetDesc).
        let valid = is_compatible_property_descriptor(vm, extensible_target, &mut result_desc, &target_descriptor);

        // 15. If valid is false, throw a TypeError exception.
        if !valid {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::ProxyGetOwnDescriptorInvalidDescriptor,
                &[],
            );
        }

        // 16. If resultDesc.[[Configurable]] is false, then
        if !fully_populated(result_desc.configurable) {
            // a. If targetDesc is undefined or targetDesc.[[Configurable]] is true, then
            let Some(target_descriptor) =
                target_descriptor.filter(|descriptor| !fully_populated(descriptor.configurable))
            else {
                // i. Throw a TypeError exception.
                return vm.throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::ProxyGetOwnDescriptorInvalidNonConfig,
                    &[],
                );
            };

            // b. If resultDesc has a [[Writable]] field and resultDesc.[[Writable]] is false, then
            if result_desc.writable == Some(false) {
                // i. If targetDesc.[[Writable]] is true, throw a TypeError exception.
                if fully_populated(target_descriptor.writable) {
                    return vm.throw_completion(
                        ErrorKind::TypeError,
                        ErrorType::ProxyGetOwnDescriptorNonConfigurableNonWritable,
                        &[],
                    );
                }
            }
        }

        // 17. Return resultDesc.
        Ok(Some(result_desc))
    }

    // 10.5.6 [[DefineOwnProperty]] ( P, Desc ), https://tc39.es/ecma262/#sec-proxy-object-internal-methods-and-internal-slots-defineownproperty-p-desc
    fn internal_define_own_property(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
        property_descriptor: &mut PropertyDescriptor,
        _precomputed_get_own_property: Option<&Option<PropertyDescriptor>>,
    ) -> ThrowCompletionOr<bool> {
        let _recursion_depth_updater = limit_proxy_recursion_depth(vm)?;
        let proxy = as_proxy_object(object);

        // 1. Perform ? ValidateNonRevokedProxy(O).
        proxy.validate_non_revoked_proxy(vm)?;

        // 2. Let target be O.[[ProxyTarget]].
        // 3. Let handler be O.[[ProxyHandler]].
        // 4. Assert: handler is an Object.
        let target = proxy.target;
        let handler = proxy.handler;

        // 5. Let trap be ? GetMethod(handler, "defineProperty").
        let trap = Value::from_object(handler).get_method(vm, &vm.names.defineProperty)?;

        // 6. If trap is undefined, then
        let Some(trap) = trap else {
            // a. Return ? target.[[DefineOwnProperty]](P, Desc).
            return target.internal_define_own_property(vm, property_key, property_descriptor, None);
        };

        // 7. Let descObj be FromPropertyDescriptor(Desc).
        let descriptor_object = from_property_descriptor(vm, &Some(*property_descriptor));

        // 8. Let booleanTrapResult be ToBoolean(? Call(trap, handler, « target, P, descObj »)).
        let trap_result = call_function_object(
            vm,
            trap,
            Value::from_object(handler),
            &[Value::from_object(target), property_key.to_value(vm), descriptor_object],
        )?
        .to_boolean();

        // 9. If booleanTrapResult is false, return false.
        if !trap_result {
            return Ok(false);
        }

        // 10. Let targetDesc be ? target.[[GetOwnProperty]](P).
        let target_descriptor = target.internal_get_own_property(vm, property_key)?;

        // 11. Let extensibleTarget be ? IsExtensible(target).
        let extensible_target = target.is_extensible(vm)?;

        // 12. Else, let settingConfigFalse be false.
        let mut setting_config_false = false;

        // 13. If Desc has a [[Configurable]] field and if Desc.[[Configurable]] is false, then
        if property_descriptor.configurable == Some(false) {
            // a. Let settingConfigFalse be true.
            setting_config_false = true;
        }

        match target_descriptor {
            // 14. If targetDesc is undefined, then
            None => {
                // a. If extensibleTarget is false, throw a TypeError exception.
                if !extensible_target {
                    return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxyDefinePropNonExtensible, &[]);
                }

                // b. If settingConfigFalse is true, throw a TypeError exception.
                if setting_config_false {
                    return vm.throw_completion(
                        ErrorKind::TypeError,
                        ErrorType::ProxyDefinePropNonConfigurableNonExisting,
                        &[],
                    );
                }
            }
            // 15. Else,
            Some(existing_target_descriptor) => {
                // a. If IsCompatiblePropertyDescriptor(extensibleTarget, Desc, targetDesc) is false, throw a TypeError exception.
                if !is_compatible_property_descriptor(vm, extensible_target, property_descriptor, &target_descriptor) {
                    return vm.throw_completion(
                        ErrorKind::TypeError,
                        ErrorType::ProxyDefinePropIncompatibleDescriptor,
                        &[],
                    );
                }

                // b. If settingConfigFalse is true and targetDesc.[[Configurable]] is true, throw a TypeError exception.
                if setting_config_false && fully_populated(existing_target_descriptor.configurable) {
                    return vm.throw_completion(
                        ErrorKind::TypeError,
                        ErrorType::ProxyDefinePropExistingConfigurable,
                        &[],
                    );
                }

                // c. If IsDataDescriptor(targetDesc) is true, targetDesc.[[Configurable]] is false, and targetDesc.[[Writable]] is true, then
                if existing_target_descriptor.is_data_descriptor()
                    && !fully_populated(existing_target_descriptor.configurable)
                    && fully_populated(existing_target_descriptor.writable)
                {
                    // i. If Desc has a [[Writable]] field and Desc.[[Writable]] is false, throw a TypeError exception.
                    if property_descriptor.writable == Some(false) {
                        return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxyDefinePropNonWritable, &[]);
                    }
                }
            }
        }

        // 16. Return true.
        Ok(true)
    }

    // 10.5.7 [[HasProperty]] ( P ), https://tc39.es/ecma262/#sec-proxy-object-internal-methods-and-internal-slots-hasproperty-p
    fn internal_has_property(object: &Object, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<bool> {
        let _recursion_depth_updater = limit_proxy_recursion_depth(vm)?;
        let proxy = as_proxy_object(object);

        // 1. Perform ? ValidateNonRevokedProxy(O).
        proxy.validate_non_revoked_proxy(vm)?;

        // 2. Let target be O.[[ProxyTarget]].
        // 3. Let handler be O.[[ProxyHandler]].
        // 4. Assert: handler is an Object.
        let target = proxy.target;
        let handler = proxy.handler;

        // NOTE: We need to protect ourselves from a Proxy with the handler's prototype set to the
        // Proxy itself, which would by default bounce between these functions indefinitely and lead to
        // a stack overflow when the Proxy's (p) or Proxy handler's (h) Object::get() is called and the
        // handler doesn't have a `has` trap:
        //
        // 1. p -> ProxyObject::internal_has_property()  <- you are here
        // 2. target -> Object::internal_has_property()
        // 3. target.[[Prototype]] (which is internal_has_property) -> Object::internal_has_property()
        //
        // In JS code: `const proxy = new Proxy({}, {}); proxy.__proto__ = Object.create(proxy); "foo" in proxy;`
        if vm.did_reach_stack_space_limit() {
            return vm.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
        }

        // 5. Let trap be ? GetMethod(handler, "has").
        let trap = Value::from_object(handler).get_method(vm, &vm.names.has)?;

        // 6. If trap is undefined, then
        let Some(trap) = trap else {
            // a. Return ? target.[[HasProperty]](P).
            return target.internal_has_property(vm, property_key);
        };

        // 7. Let booleanTrapResult be ToBoolean(? Call(trap, handler, « target, P »)).
        let trap_result = call_function_object(
            vm,
            trap,
            Value::from_object(handler),
            &[Value::from_object(target), property_key.to_value(vm)],
        )?
        .to_boolean();

        // 8. If booleanTrapResult is false, then
        if !trap_result {
            // a. Let targetDesc be ? target.[[GetOwnProperty]](P).
            let target_descriptor = target.internal_get_own_property(vm, property_key)?;

            // b. If targetDesc is not undefined, then
            if let Some(target_descriptor) = target_descriptor {
                // i. If targetDesc.[[Configurable]] is false, throw a TypeError exception.
                if !fully_populated(target_descriptor.configurable) {
                    return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxyHasExistingNonConfigurable, &[]);
                }

                // ii. Let extensibleTarget be ? IsExtensible(target).
                let extensible_target = target.is_extensible(vm)?;

                // iii. If extensibleTarget is false, throw a TypeError exception.
                if !extensible_target {
                    return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxyHasExistingNonExtensible, &[]);
                }
            }
        }

        // 9. Return booleanTrapResult.
        Ok(trap_result)
    }

    // 10.5.8 [[Get]] ( P, Receiver ), https://tc39.es/ecma262/#sec-proxy-object-internal-methods-and-internal-slots-get-p-receiver
    fn internal_get(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
        receiver: Value,
        _cacheable_metadata: Option<&mut CacheableGetPropertyMetadata>,
        _phase: PropertyLookupPhase,
    ) -> ThrowCompletionOr<Value> {
        let _recursion_depth_updater = limit_proxy_recursion_depth(vm)?;
        let proxy = as_proxy_object(object);

        // NOTE: We don't return any cacheable metadata for proxy lookups.

        assert!(!receiver.is_empty());

        // 1. Perform ? ValidateNonRevokedProxy(O).
        proxy.validate_non_revoked_proxy(vm)?;

        // 2. Let target be O.[[ProxyTarget]].
        // 3. Let handler be O.[[ProxyHandler]].
        // 4. Assert: handler is an Object.
        let target = proxy.target;
        let handler = proxy.handler;

        // NOTE: We need to protect ourselves from a Proxy with its (or handler's) prototype set to the
        // Proxy itself, which would by default bounce between these functions indefinitely and lead to
        // a stack overflow when the Proxy's (p) or Proxy handler's (h) Object::get() is called and the
        // handler doesn't have a `get` trap:
        //
        // 1. p -> ProxyObject::internal_get()  <- you are here
        // 2. h -> Value::get_method()
        // 3. h -> Value::get()
        // 4. h -> Object::internal_get()
        // 5. h -> Object::internal_get_prototype_of() (result is p)
        // 6. goto 1
        //
        // In JS code: `h = {}; p = new Proxy({}, h); h.__proto__ = p; p.foo // or h.foo`
        if vm.did_reach_stack_space_limit() {
            return vm.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
        }

        // 5. Let trap be ? GetMethod(handler, "get").
        let trap = Value::from_object(handler).get_method(vm, &vm.names.get)?;

        // 6. If trap is undefined, then
        let Some(trap) = trap else {
            // a. Return ? target.[[Get]](P, Receiver).
            return target.internal_get(vm, property_key, receiver, None, PropertyLookupPhase::OwnProperty);
        };

        // 7. Let trapResult be ? Call(trap, handler, « target, P, Receiver »).
        let trap_result = call_function_object(
            vm,
            trap,
            Value::from_object(handler),
            &[Value::from_object(target), property_key.to_value(vm), receiver],
        )?;

        // 8. Let targetDesc be ? target.[[GetOwnProperty]](P).
        let target_descriptor = target.internal_get_own_property(vm, property_key)?;

        // 9. If targetDesc is not undefined and targetDesc.[[Configurable]] is false, then
        if let Some(target_descriptor) = target_descriptor
            && !fully_populated(target_descriptor.configurable)
        {
            // a. If IsDataDescriptor(targetDesc) is true and targetDesc.[[Writable]] is false, then
            if target_descriptor.is_data_descriptor() && !fully_populated(target_descriptor.writable) {
                // i. If SameValue(trapResult, targetDesc.[[Value]]) is false, throw a TypeError exception.
                if !same_value(trap_result, fully_populated(target_descriptor.value)) {
                    return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxyGetImmutableDataProperty, &[]);
                }
            }
            // b. If IsAccessorDescriptor(targetDesc) is true and targetDesc.[[Get]] is undefined, then
            if target_descriptor.is_accessor_descriptor() && fully_populated(target_descriptor.get).is_none() {
                // i. If trapResult is not undefined, throw a TypeError exception.
                if !trap_result.is_undefined() {
                    return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxyGetNonConfigurableAccessor, &[]);
                }
            }
        }

        // 10. Return trapResult.
        Ok(trap_result)
    }

    // 10.5.9 [[Set]] ( P, V, Receiver ), https://tc39.es/ecma262/#sec-proxy-object-internal-methods-and-internal-slots-set-p-v-receiver
    fn internal_set(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
        value: Value,
        receiver: Value,
        _cacheable_metadata: Option<&mut CacheableSetPropertyMetadata>,
        _phase: PropertyLookupPhase,
    ) -> ThrowCompletionOr<bool> {
        let _recursion_depth_updater = limit_proxy_recursion_depth(vm)?;
        let proxy = as_proxy_object(object);

        assert!(!value.is_empty());
        assert!(!receiver.is_empty());

        // 1. Perform ? ValidateNonRevokedProxy(O).
        proxy.validate_non_revoked_proxy(vm)?;

        // 2. Let target be O.[[ProxyTarget]].
        // 3. Let handler be O.[[ProxyHandler]].
        // 4. Assert: handler is an Object.
        let target = proxy.target;
        let handler = proxy.handler;

        // NOTE: We need to protect ourselves from a Proxy with its prototype set to the
        // Proxy itself, which would by default bounce between these functions indefinitely and lead to
        // a stack overflow when the Proxy's (p) or Proxy handler's (h) Object::get() is called and the
        // handler doesn't have a `has` trap:
        //
        // 1. p -> ProxyObject::internal_set()  <- you are here
        // 2. target -> Object::internal_set()
        // 3. target -> Object::ordinary_set_with_own_descriptor()
        // 4. target.[[Prototype]] -> Object::internal_set()
        // 5. target.[[Prototype]] -> Object::ordinary_set_with_own_descriptor()
        // 6. target.[[Prototype]].[[Prototype]] (which is ProxyObject) -> Object::internal_set()
        //
        // In JS code: `const proxy = new Proxy({}, {}); proxy.__proto__ = Object.create(proxy); proxy["foo"] = "bar";`
        if vm.did_reach_stack_space_limit() {
            return vm.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
        }

        // 5. Let trap be ? GetMethod(handler, "set").
        let trap = Value::from_object(handler).get_method(vm, &vm.names.set)?;

        // 6. If trap is undefined, then
        let Some(trap) = trap else {
            // a. Return ? target.[[Set]](P, V, Receiver).
            return target.internal_set(
                vm,
                property_key,
                value,
                receiver,
                None,
                PropertyLookupPhase::OwnProperty,
            );
        };

        // 7. Let booleanTrapResult be ToBoolean(? Call(trap, handler, « target, P, V, Receiver »)).
        let trap_result = call_function_object(
            vm,
            trap,
            Value::from_object(handler),
            &[Value::from_object(target), property_key.to_value(vm), value, receiver],
        )?
        .to_boolean();

        // 8. If booleanTrapResult is false, return false.
        if !trap_result {
            return Ok(false);
        }

        // 9. Let targetDesc be ? target.[[GetOwnProperty]](P).
        let target_descriptor = target.internal_get_own_property(vm, property_key)?;

        // 10. If targetDesc is not undefined and targetDesc.[[Configurable]] is false, then
        if let Some(target_descriptor) = target_descriptor
            && !fully_populated(target_descriptor.configurable)
        {
            // a. If IsDataDescriptor(targetDesc) is true and targetDesc.[[Writable]] is false, then
            if target_descriptor.is_data_descriptor() && !fully_populated(target_descriptor.writable) {
                // i. If SameValue(V, targetDesc.[[Value]]) is false, throw a TypeError exception.
                if !same_value(value, fully_populated(target_descriptor.value)) {
                    return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxySetImmutableDataProperty, &[]);
                }
            }
            // b. If IsAccessorDescriptor(targetDesc) is true, then
            if target_descriptor.is_accessor_descriptor() {
                // i. If targetDesc.[[Set]] is undefined, throw a TypeError exception.
                if fully_populated(target_descriptor.set).is_none() {
                    return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxySetNonConfigurableAccessor, &[]);
                }
            }
        }

        // 11. Return true.
        Ok(true)
    }

    // 10.5.10 [[Delete]] ( P ), https://tc39.es/ecma262/#sec-proxy-object-internal-methods-and-internal-slots-delete-p
    fn internal_delete(object: &Object, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<bool> {
        let _recursion_depth_updater = limit_proxy_recursion_depth(vm)?;
        let proxy = as_proxy_object(object);

        // 1. Perform ? ValidateNonRevokedProxy(O).
        proxy.validate_non_revoked_proxy(vm)?;

        // 2. Let target be O.[[ProxyTarget]].
        // 3. Let handler be O.[[ProxyHandler]].
        // 4. Assert: handler is an Object.
        let target = proxy.target;
        let handler = proxy.handler;

        // 5. Let trap be ? GetMethod(handler, "deleteProperty").
        let trap = Value::from_object(handler).get_method(vm, &vm.names.deleteProperty)?;

        // 6. If trap is undefined, then
        let Some(trap) = trap else {
            // a. Return ? target.[[Delete]](P).
            return target.internal_delete(vm, property_key);
        };

        // 7. Let booleanTrapResult be ToBoolean(? Call(trap, handler, « target, P »)).
        let trap_result = call_function_object(
            vm,
            trap,
            Value::from_object(handler),
            &[Value::from_object(target), property_key.to_value(vm)],
        )?
        .to_boolean();

        // 8. If booleanTrapResult is false, return false.
        if !trap_result {
            return Ok(false);
        }

        // 9. Let targetDesc be ? target.[[GetOwnProperty]](P).
        let target_descriptor = target.internal_get_own_property(vm, property_key)?;

        // 10. If targetDesc is undefined, return true.
        let Some(target_descriptor) = target_descriptor else {
            return Ok(true);
        };

        // 11. If targetDesc.[[Configurable]] is false, throw a TypeError exception.
        if !fully_populated(target_descriptor.configurable) {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxyDeleteNonConfigurable, &[]);
        }

        // 12. Let extensibleTarget be ? IsExtensible(target).
        let extensible_target = target.is_extensible(vm)?;

        // 13. If extensibleTarget is false, throw a TypeError exception.
        if !extensible_target {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxyDeleteNonExtensible, &[]);
        }

        // 14. Return true.
        Ok(true)
    }

    // 10.5.11 [[OwnPropertyKeys]] ( ), https://tc39.es/ecma262/#sec-proxy-object-internal-methods-and-internal-slots-ownpropertykeys
    fn internal_own_property_keys<'vm>(object: &Object, vm: &'vm Vm) -> ThrowCompletionOr<MarkedVec<'vm, Value>> {
        let _recursion_depth_updater = limit_proxy_recursion_depth(vm)?;
        let proxy = as_proxy_object(object);

        // 1. Perform ? ValidateNonRevokedProxy(O).
        proxy.validate_non_revoked_proxy(vm)?;

        // 2. Let target be O.[[ProxyTarget]].
        // 3. Let handler be O.[[ProxyHandler]].
        // 4. Assert: handler is an Object.
        let target = proxy.target;
        let handler = proxy.handler;

        // 5. Let trap be ? GetMethod(handler, "ownKeys").
        let trap = Value::from_object(handler).get_method(vm, &vm.names.ownKeys)?;

        // 6. If trap is undefined, then
        let Some(trap) = trap else {
            // a. Return ? target.[[OwnPropertyKeys]]().
            return target.internal_own_property_keys(vm);
        };

        // 7. Let trapResultArray be ? Call(trap, handler, « target »).
        let trap_result_array =
            call_function_object(vm, trap, Value::from_object(handler), &[Value::from_object(target)])?;

        // 8. Let trapResult be ? CreateListFromArrayLike(trapResultArray, « String, Symbol »).
        let unique_keys = RefCell::new(UniquePropertyKeys::default());
        let check_value = |value: Value| -> ThrowCompletionOr<()> {
            if !value.is_string() && !value.is_symbol() {
                return vm.throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::ProxyOwnPropertyKeysNotStringOrSymbol,
                    &[],
                );
            }
            let property_key = value.to_property_key(vm).must();
            unique_keys.borrow_mut().set(property_key);
            Ok(())
        };
        let trap_result = create_list_from_array_like(vm, trap_result_array, Some(&check_value))?;

        // 9. If trapResult contains any duplicate entries, throw a TypeError exception.
        if unique_keys.borrow().size() != trap_result.len() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxyOwnPropertyKeysDuplicates, &[]);
        }

        // 10. Let extensibleTarget be ? IsExtensible(target).
        let extensible_target = target.is_extensible(vm)?;

        // 11. Let targetKeys be ? target.[[OwnPropertyKeys]]().
        let target_keys = target.internal_own_property_keys(vm)?;

        // 12. Assert: targetKeys is a List of property keys.
        // 13. Assert: targetKeys contains no duplicate entries.

        // 14. Let targetConfigurableKeys be a new empty List.
        let target_configurable_keys = MarkedVec::new(vm);

        // 15. Let targetNonconfigurableKeys be a new empty List.
        let target_nonconfigurable_keys = MarkedVec::new(vm);

        // 16. For each element key of targetKeys, do
        for index in 0..target_keys.len() {
            let key = target_keys.get(index).expect("the index is in bounds");
            let property_key = PropertyKey::from_value(vm, key).must();

            // a. Let desc be ? target.[[GetOwnProperty]](key).
            let descriptor = target.internal_get_own_property(vm, &property_key)?;

            // b. If desc is not undefined and desc.[[Configurable]] is false, then
            if descriptor.is_some_and(|descriptor| !fully_populated(descriptor.configurable)) {
                // i. Append key as an element of targetNonconfigurableKeys.
                target_nonconfigurable_keys.push(key);
            }
            // c. Else,
            else {
                // i. Append key as an element of targetConfigurableKeys.
                target_configurable_keys.push(key);
            }
        }

        // 17. If extensibleTarget is true and targetNonconfigurableKeys is empty, then
        if extensible_target && target_nonconfigurable_keys.is_empty() {
            // a. Return trapResult.
            return Ok(trap_result);
        }

        // 18. Let uncheckedResultKeys be a List whose elements are the elements of trapResult.
        let mut unchecked_result_keys = UncheckedResultKeys::new(&trap_result);

        // 19. For each element key of targetNonconfigurableKeys, do
        for index in 0..target_nonconfigurable_keys.len() {
            let key = target_nonconfigurable_keys.get(index).expect("the index is in bounds");

            // a. If key is not an element of uncheckedResultKeys, throw a TypeError exception.
            // b. Remove key from uncheckedResultKeys.
            if !unchecked_result_keys.remove_first_matching(key) {
                return vm.throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::ProxyOwnPropertyKeysSkippedNonconfigurableProperty,
                    &[&key],
                );
            }
        }

        // 20. If extensibleTarget is true, return trapResult.
        if extensible_target {
            return Ok(trap_result);
        }

        // 21. For each element key of targetConfigurableKeys, do
        for index in 0..target_configurable_keys.len() {
            let key = target_configurable_keys.get(index).expect("the index is in bounds");

            // a. If key is not an element of uncheckedResultKeys, throw a TypeError exception.
            // b. Remove key from uncheckedResultKeys.
            if !unchecked_result_keys.remove_first_matching(key) {
                return vm.throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::ProxyOwnPropertyKeysNonExtensibleSkippedProperty,
                    &[&key],
                );
            }
        }

        // 22. If uncheckedResultKeys is not empty, throw a TypeError exception.
        if let Some(first_unchecked_result_key) = unchecked_result_keys.first() {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::ProxyOwnPropertyKeysNonExtensibleNewProperty,
                &[&first_unchecked_result_key],
            );
        }

        // 23. Return trapResult.
        Ok(trap_result)
    }

    /// callee_context.arguments_span().trim(callee_context.passed_argument_count): the passed arguments only, without
    /// the argument slots that a target with more formal parameters than arguments were passed asked for.
    fn arguments_of<'vm>(vm: &'vm Vm, callee_context: &ExecutionContext) -> MarkedVec<'vm, Value> {
        let passed_arguments = callee_context
            .arguments()
            .iter()
            .take(callee_context.passed_argument_count.get() as usize);
        let arguments = MarkedVec::with_capacity(vm, passed_arguments.len());
        for argument in passed_arguments {
            arguments.push(argument.get());
        }
        arguments
    }

    fn with_arguments<R>(vm: &Vm, callee_context: &ExecutionContext, callback: impl FnOnce(&[Value]) -> R) -> R {
        let passed_argument_count =
            (callee_context.passed_argument_count.get() as usize).min(callee_context.arguments().len());
        if passed_argument_count <= STACK_ARGUMENT_CAPACITY {
            let mut arguments = [Value::UNDEFINED; STACK_ARGUMENT_CAPACITY];
            for (argument, passed_argument) in arguments.iter_mut().zip(callee_context.arguments()) {
                *argument = passed_argument.get();
            }
            return callback(&arguments[..passed_argument_count]);
        }
        Self::arguments_of(vm, callee_context).with_values(callback)
    }

    // 10.5.12 [[Call]] ( thisArgument, argumentsList ), https://tc39.es/ecma262/#sec-proxy-object-internal-methods-and-internal-slots-call-thisargument-argumentslist
    fn internal_call(
        object: &Object,
        vm: &Vm,
        callee_context: &ExecutionContext,
        this_argument: Value,
    ) -> ThrowCompletionOr<Value> {
        let _recursion_depth_updater = limit_proxy_recursion_depth(vm)?;
        let proxy = as_proxy_object(object);

        let realm = vm.current_realm().expect("there is a current realm");

        // 1. Perform ? ValidateNonRevokedProxy(O).
        proxy.validate_non_revoked_proxy(vm)?;

        // 2. Let target be O.[[ProxyTarget]].
        // 3. Let handler be O.[[ProxyHandler]].
        // 4. Assert: handler is an Object.
        let target = proxy.target;
        let handler = proxy.handler;

        // NOTE: A Proxy exotic object only has a [[Call]] internal method if the initial value of its [[ProxyTarget]] internal slot is an object that has a [[Call]] internal method.
        assert!(proxy.is_function());

        // 5. Let trap be ? GetMethod(handler, "apply").
        let trap = Value::from_object(handler).get_method(vm, &vm.names.apply)?;

        // 6. If trap is undefined, then
        let Some(trap) = trap else {
            // a. Return ? Call(target, thisArgument, argumentsList).
            return Self::with_arguments(vm, callee_context, |arguments| {
                call(vm, Value::from_object(target), this_argument, arguments)
            });
        };

        // 7. Let argArray be CreateArrayFromList(argumentsList).
        let arguments_array =
            Self::with_arguments(vm, callee_context, |arguments| Array::create_from(vm, realm, arguments));

        // 8. Return ? Call(trap, handler, « target, thisArgument, argArray »).
        call(
            vm,
            Value::from_object(trap),
            Value::from_object(handler),
            &[
                Value::from_object(target),
                this_argument,
                Value::from_object(arguments_array),
            ],
        )
    }

    fn has_constructor(&self) -> bool {
        // Note: A Proxy exotic object only has a [[Construct]] internal method if the initial value of
        //       its [[ProxyTarget]] internal slot is an object that has a [[Construct]] internal method.
        if !self.is_function() {
            return false;
        }

        self.target.has_constructor()
    }

    // 10.5.13 [[Construct]] ( argumentsList, newTarget ), https://tc39.es/ecma262/#sec-proxy-object-internal-methods-and-internal-slots-construct-argumentslist-newtarget
    fn internal_construct(
        object: &Object,
        vm: &Vm,
        callee_context: &ExecutionContext,
        new_target: Gc<FunctionObject>,
    ) -> ThrowCompletionOr<Gc<Object>> {
        let _recursion_depth_updater = limit_proxy_recursion_depth(vm)?;
        let proxy = as_proxy_object(object);

        let realm = vm.current_realm().expect("there is a current realm");

        // 1. Perform ? ValidateNonRevokedProxy(O).
        proxy.validate_non_revoked_proxy(vm)?;

        // 2. Let target be O.[[ProxyTarget]].
        // 3. Let handler be O.[[ProxyHandler]].
        // 4. Assert: handler is an Object.
        let target = proxy.target;
        let handler = proxy.handler;

        // NOTE: A Proxy exotic object only has a [[Construct]] internal method if the initial value of its [[ProxyTarget]] internal slot is an object that has a [[Construct]] internal method.
        assert!(proxy.is_function());

        // 6. Let trap be ? GetMethod(handler, "construct").
        let trap = Value::from_object(handler).get_method(vm, &vm.names.construct)?;

        // 7. If trap is undefined, then
        let Some(trap) = trap else {
            // a. Return ? Construct(target, argumentsList, newTarget).
            assert!(target.is_function());
            let internal_construct = target
                .internal_construct_method()
                .expect("the target of a constructor proxy is a constructor");
            return internal_construct(&target, vm, callee_context, new_target);
        };

        // 8. Let argArray be CreateArrayFromList(argumentsList).
        let arguments_array =
            Self::with_arguments(vm, callee_context, |arguments| Array::create_from(vm, realm, arguments));

        // 9. Let newObj be ? Call(trap, handler, « target, argArray, newTarget »).
        let new_object = call(
            vm,
            Value::from_object(trap),
            Value::from_object(handler),
            &[
                Value::from_object(target),
                Value::from_object(arguments_array),
                Value::from_object(new_target),
            ],
        )?;

        // 10. If Type(newObj) is not Object, throw a TypeError exception.
        if !new_object.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxyConstructBadReturnType, &[]);
        }

        // 11. Return newObj.
        Ok(new_object.as_object())
    }

    // 10.5.14 ValidateNonRevokedProxy ( proxy )
    pub fn validate_non_revoked_proxy(&self, vm: &Vm) -> ThrowCompletionOr<()> {
        // FIXME: The spec expects us to model a revoked proxy by having ProxyTarget and ProxyHandler be nullable.

        // 1. If proxy.[[ProxyTarget]] is null, throw a TypeError exception.
        // 2. Assert: proxy.[[ProxyHandler]] is not null.
        if self.is_revoked() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ProxyRevoked, &[]);
        }

        // 3. Return unused.
        Ok(())
    }

    fn get_stack_frame_info(object: &Object, vm: &Vm, stack_frame_info: &mut StackFrameInfo) {
        let target = as_proxy_object(object).target;
        assert!(target.is_function());
        target.get_stack_frame_info(vm, stack_frame_info);
    }

    pub fn name_for_call_stack() -> Utf16String {
        Utf16String::from_utf8("(Proxy)")
    }
}
