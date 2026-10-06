/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::module::Module;
use crate::runtime::object::{
    CacheableGetPropertyMetadata, CacheableSetPropertyMetadata, MayInterfereWithIndexedPropertyAccess,
    ORDINARY_OBJECT_METHODS, ObjectMethods, PropertyLookupPhase,
};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::PropertyAttributes;
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::value::same_value;
use crate::utf16::{Utf16View, to_utf16_fly_string};

// 10.4.6 Module Namespace Exotic Objects, https://tc39.es/ecma262/#sec-module-namespace-exotic-objects
#[repr(C)]
#[derive(Trace)]
pub struct ModuleNamespaceObject {
    base: Object,
    module: Gc<Module>,           // [[Module]]
    exports: Vec<Utf16FlyString>, // [[Exports]]
}

pub static MODULE_NAMESPACE_OBJECT_METHODS: ObjectMethods = ObjectMethods {
    initialize: ModuleNamespaceObject::initialize,
    internal_get_prototype_of: ModuleNamespaceObject::internal_get_prototype_of,
    internal_set_prototype_of: ModuleNamespaceObject::internal_set_prototype_of,
    internal_is_extensible: ModuleNamespaceObject::internal_is_extensible,
    internal_prevent_extensions: ModuleNamespaceObject::internal_prevent_extensions,
    internal_get_own_property: ModuleNamespaceObject::internal_get_own_property,
    internal_define_own_property: ModuleNamespaceObject::internal_define_own_property,
    internal_has_property: ModuleNamespaceObject::internal_has_property,
    internal_get: ModuleNamespaceObject::internal_get,
    is_cacheable_for_property_absence: |_| false,
    internal_set: ModuleNamespaceObject::internal_set,
    internal_delete: ModuleNamespaceObject::internal_delete,
    internal_own_property_keys: ModuleNamespaceObject::internal_own_property_keys,
    eligible_for_own_property_enumeration_fast_path: |_| false,
    ..ORDINARY_OBJECT_METHODS
};

define_cell!(ModuleNamespaceObject, Object, extends: [Object], methods: MODULE_NAMESPACE_OBJECT_METHODS);

impl core::ops::Deref for ModuleNamespaceObject {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

/// The module namespace object an internal method of a module namespace object was called on.
fn as_module_namespace_object(object: &Object) -> &ModuleNamespaceObject {
    assert!(object.is::<ModuleNamespaceObject>());
    // SAFETY: The object is a ModuleNamespaceObject, which starts with its Object.
    unsafe { &*core::ptr::from_ref(object).cast::<ModuleNamespaceObject>() }
}

/// The String a property key that is not a Symbol stands for, which [[Exports]] holds export names as.
fn export_name_of(property_key: &PropertyKey) -> Utf16FlyString {
    to_utf16_fly_string(&property_key.to_utf16_string())
}

impl ModuleNamespaceObject {
    pub fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        module: Gc<Module>,
        mut exports: Vec<Utf16FlyString>,
    ) -> Gc<ModuleNamespaceObject> {
        // Note: We just perform step 6 of 10.4.6.12 ModuleNamespaceCreate ( module, exports ), https://tc39.es/ecma262/#sec-modulenamespacecreate
        // 6. Let sortedExports be a List whose elements are the elements of exports ordered as if an Array of the same values had been sorted using %Array.prototype.sort% using undefined as comparefn.
        exports.sort_by(|lhs, rhs| {
            let (lhs, rhs) = (Utf16View::of_fly_string(lhs), Utf16View::of_fly_string(rhs));
            if lhs.is_code_unit_less_than(rhs) {
                core::cmp::Ordering::Less
            } else if rhs.is_code_unit_less_than(lhs) {
                core::cmp::Ordering::Greater
            } else {
                core::cmp::Ordering::Equal
            }
        });

        realm.create_object(
            vm,
            ModuleNamespaceObject {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::Yes,
                ),
                module,
                exports,
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        (ORDINARY_OBJECT_METHODS.initialize)(object, vm, realm);

        // 28.3.1 @@toStringTag, https://tc39.es/ecma262/#sec-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Module")),
            PropertyAttributes::new(0),
        );
    }

    fn is_export(&self, property_key: &PropertyKey) -> bool {
        self.exports.contains(&export_name_of(property_key))
    }

    // 10.4.6.1 [[GetPrototypeOf]] ( ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-getprototypeof
    #[allow(
        clippy::unnecessary_wraps,
        reason = "the internal method's type lets other objects throw"
    )]
    fn internal_get_prototype_of(_object: &Object, _vm: &Vm) -> ThrowCompletionOr<Option<Gc<Object>>> {
        // 1. Return null.
        Ok(None)
    }

    // 10.4.6.2 [[SetPrototypeOf]] ( V ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-setprototypeof-v
    #[allow(
        clippy::unnecessary_wraps,
        reason = "the internal method's type lets other objects throw"
    )]
    fn internal_set_prototype_of(object: &Object, vm: &Vm, prototype: Option<Gc<Object>>) -> ThrowCompletionOr<bool> {
        // 1. Return ! SetImmutablePrototype(O, V).
        Ok(object.set_immutable_prototype(vm, prototype).must())
    }

    // 10.4.6.3 [[IsExtensible]] ( ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-isextensible
    #[allow(
        clippy::unnecessary_wraps,
        reason = "the internal method's type lets other objects throw"
    )]
    fn internal_is_extensible(_object: &Object, _vm: &Vm) -> ThrowCompletionOr<bool> {
        // 1. Return false.
        Ok(false)
    }

    // 10.4.6.4 [[PreventExtensions]] ( ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-preventextensions
    #[allow(
        clippy::unnecessary_wraps,
        reason = "the internal method's type lets other objects throw"
    )]
    fn internal_prevent_extensions(_object: &Object, _vm: &Vm) -> ThrowCompletionOr<bool> {
        // 1. Return true.
        Ok(true)
    }

    // 10.4.6.5 [[GetOwnProperty]] ( P ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-getownproperty-p
    fn internal_get_own_property(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
    ) -> ThrowCompletionOr<Option<PropertyDescriptor>> {
        // 1. If Type(P) is Symbol, return OrdinaryGetOwnProperty(O, P).
        if property_key.is_symbol() {
            return object.ordinary_get_own_property(vm, property_key);
        }

        // 2. Let exports be O.[[Exports]].
        // 3. If P is not an element of exports, return undefined.
        if !as_module_namespace_object(object).is_export(property_key) {
            return Ok(None);
        }

        // 4. Let value be ? O.[[Get]](P, O).
        let value = object.internal_get(
            vm,
            property_key,
            Value::from_object(object.as_gc()),
            None,
            PropertyLookupPhase::OwnProperty,
        )?;

        // 5. Return PropertyDescriptor { [[Value]]: value, [[Writable]]: true, [[Enumerable]]: true, [[Configurable]]: false }.
        Ok(Some(PropertyDescriptor {
            value: Some(value),
            writable: Some(true),
            enumerable: Some(true),
            configurable: Some(false),
            ..PropertyDescriptor::default()
        }))
    }

    // 10.4.6.6 [[DefineOwnProperty]] ( P, Desc ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-defineownproperty-p-desc
    fn internal_define_own_property(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
        descriptor: &mut PropertyDescriptor,
        precomputed_get_own_property: Option<&Option<PropertyDescriptor>>,
    ) -> ThrowCompletionOr<bool> {
        // 1. If Type(P) is Symbol, return ! OrdinaryDefineOwnProperty(O, P, Desc).
        if property_key.is_symbol() {
            return Ok(object
                .ordinary_define_own_property(vm, property_key, descriptor, precomputed_get_own_property)
                .must());
        }

        // 2. Let current be ? O.[[GetOwnProperty]](P).
        let current = object.internal_get_own_property(vm, property_key)?;

        // 3. If current is undefined, return false.
        let Some(current) = current else {
            return Ok(false);
        };

        // 4. If Desc has a [[Configurable]] field and Desc.[[Configurable]] is true, return false.
        if descriptor.configurable == Some(true) {
            return Ok(false);
        }

        // 5. If Desc has an [[Enumerable]] field and Desc.[[Enumerable]] is false, return false.
        if descriptor.enumerable == Some(false) {
            return Ok(false);
        }

        // 6. If IsAccessorDescriptor(Desc) is true, return false.
        if descriptor.is_accessor_descriptor() {
            return Ok(false);
        }

        // 7. If Desc has a [[Writable]] field and Desc.[[Writable]] is false, return false.
        if descriptor.writable == Some(false) {
            return Ok(false);
        }

        // 8. If Desc has a [[Value]] field, return SameValue(Desc.[[Value]], current.[[Value]]).
        if let Some(value) = descriptor.value {
            return Ok(same_value(
                value,
                current.value.expect("the descriptor of an export has a value"),
            ));
        }

        // 9. Return true.
        Ok(true)
    }

    // 10.4.6.7 [[HasProperty]] ( P ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-hasproperty-p
    #[allow(
        clippy::unnecessary_wraps,
        reason = "the internal method's type lets other objects throw"
    )]
    fn internal_has_property(object: &Object, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<bool> {
        // 1. If Type(P) is Symbol, return ! OrdinaryHasProperty(O, P).
        if property_key.is_symbol() {
            return Ok(object.ordinary_has_property(vm, property_key).must());
        }

        // 2. Let exports be O.[[Exports]].
        // 3. If P is an element of exports, return true.
        // 4. Return false.
        Ok(as_module_namespace_object(object).is_export(property_key))
    }

    // 10.4.6.8 [[Get]] ( P, Receiver ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-get-p-receiver
    fn internal_get(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
        receiver: Value,
        cacheable_metadata: Option<&mut CacheableGetPropertyMetadata>,
        phase: PropertyLookupPhase,
    ) -> ThrowCompletionOr<Value> {
        // 1. If Type(P) is Symbol, then
        if property_key.is_symbol() {
            // a. Return ! OrdinaryGet(O, P, Receiver).
            return Ok(object
                .ordinary_get(vm, property_key, receiver, cacheable_metadata, phase)
                .must());
        }

        // 2. Let exports be O.[[Exports]].
        // 3. If P is not an element of exports, return undefined.
        let namespace_object = as_module_namespace_object(object);
        if !namespace_object.is_export(property_key) {
            return Ok(Value::UNDEFINED);
        }

        // 4. Let m be O.[[Module]].
        // 5. Let binding be m.ResolveExport(P).
        let binding = namespace_object
            .module
            .resolve_export(vm, &export_name_of(property_key));

        // 6. Assert: binding is a ResolvedBinding Record.
        assert!(binding.is_valid());

        // 7. Let targetModule be binding.[[Module]].
        // 8. Assert: targetModule is not undefined.
        let target_module = binding.module.expect("a resolved binding has a module");

        // 9. If binding.[[BindingName]] is namespace, then
        if binding.is_namespace() {
            // a. Return GetModuleNamespace(targetModule)..
            return Ok(Value::from_object(target_module.get_module_namespace(vm)));
        }

        // 10. Let targetEnv be targetModule.[[Environment]].
        // 11. If targetEnv is empty, throw a ReferenceError exception.
        let Some(target_environment) = target_module.environment() else {
            return vm.throw_completion(ErrorKind::ReferenceError, ErrorType::ModuleNoEnvironment, &[]);
        };

        // 12. Return ? targetEnv.GetBindingValue(binding.[[BindingName]], true).
        target_environment.get_binding_value(vm, &binding.export_name, true)
    }

    // 10.4.6.9 [[Set]] ( P, V, Receiver ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-set-p-v-receiver
    #[allow(
        clippy::unnecessary_wraps,
        reason = "the internal method's type lets other objects throw"
    )]
    fn internal_set(
        _object: &Object,
        _vm: &Vm,
        _property_key: &PropertyKey,
        _value: Value,
        _receiver: Value,
        _cacheable_metadata: Option<&mut CacheableSetPropertyMetadata>,
        _phase: PropertyLookupPhase,
    ) -> ThrowCompletionOr<bool> {
        // 1. Return false.
        Ok(false)
    }

    // 10.4.6.10 [[Delete]] ( P ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-delete-p
    #[allow(
        clippy::unnecessary_wraps,
        reason = "the internal method's type lets other objects throw"
    )]
    fn internal_delete(object: &Object, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<bool> {
        // 1. If Type(P) is Symbol, then
        if property_key.is_symbol() {
            // a. Return ! OrdinaryDelete(O, P).
            return Ok(object.ordinary_delete(vm, property_key).must());
        }

        // 2. Let exports be O.[[Exports]].
        // 3. If P is an element of exports, return false.
        // 4. Return true.
        Ok(!as_module_namespace_object(object).is_export(property_key))
    }

    // 10.4.6.11 [[OwnPropertyKeys]] ( ), https://tc39.es/ecma262/#sec-module-namespace-exotic-objects-ownpropertykeys
    #[allow(
        clippy::unnecessary_wraps,
        reason = "the internal method's type lets other objects throw"
    )]
    fn internal_own_property_keys<'vm>(object: &Object, vm: &'vm Vm) -> ThrowCompletionOr<MarkedVec<'vm, Value>> {
        let namespace_object = as_module_namespace_object(object);

        // 1. Let exports be O.[[Exports]].
        // NOTE: We only add the exports after we know the size of symbolKeys
        // 2. Let symbolKeys be OrdinaryOwnPropertyKeys(O).
        let symbol_keys = object.ordinary_own_property_keys(vm).must();

        // 3. Return the list-concatenation of exports and symbolKeys.
        let exports = MarkedVec::with_capacity(vm, namespace_object.exports.len() + symbol_keys.len());
        for export_index in 0..namespace_object.exports.len() {
            let export_name = namespace_object.exports[export_index].clone();
            exports.push(Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &export_name,
            )));
        }
        for index in 0..symbol_keys.len() {
            exports.push(symbol_keys.get(index).expect("the index is in bounds"));
        }

        Ok(exports)
    }
}
