/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::can_be_held_weakly;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::finalization_registry::FinalizationRegistry;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;
use crate::runtime::value::same_value;

/// %FinalizationRegistry.prototype%.
#[repr(C)]
#[derive(Trace)]
pub struct FinalizationRegistryPrototype {
    base: Object,
}

define_object_class!(FinalizationRegistryPrototype, extends: [Object], methods: {
    initialize: FinalizationRegistryPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_finalization_registry(vm: &Vm) -> ThrowCompletionOr<Gc<FinalizationRegistry>> {
    typed_this_object::<FinalizationRegistry>(vm, "FinalizationRegistry")
}

impl FinalizationRegistryPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<FinalizationRegistryPrototype> {
        realm.create_object(
            vm,
            FinalizationRegistryPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);

        object.define_native_function(
            vm,
            realm,
            &names.register_,
            raw_native!(FinalizationRegistryPrototype::register_),
            2,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.unregister,
            raw_native!(FinalizationRegistryPrototype::unregister),
            1,
            attr,
            None,
        );

        // 26.2.3.4 FinalizationRegistry.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-finalization-registry.prototype-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                names.FinalizationRegistry.as_string(),
            )),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 26.2.3.2 FinalizationRegistry.prototype.register ( target, heldValue [ , unregisterToken ] ), https://tc39.es/ecma262/#sec-finalization-registry.prototype.register
    fn register_(vm: &Vm) -> ThrowCompletionOr<Value> {
        let target = vm.argument(0);
        let held_value = vm.argument(1);
        let unregister_token = vm.argument(2);

        // 1. Let finalizationRegistry be the this value.
        // 2. Perform ? RequireInternalSlot(finalizationRegistry, [[Cells]]).
        let finalization_registry = typed_this_finalization_registry(vm)?;

        // 3. If target is not an Object, throw a TypeError exception.
        if !can_be_held_weakly(target) {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::CannotBeHeldWeakly, &[&target]);
        }

        // 4. If SameValue(target, heldValue) is true, throw a TypeError exception.
        if same_value(target, held_value) {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::FinalizationRegistrySameTargetAndValue,
                &[],
            );
        }

        // 5. If unregisterToken is not an Object, then
        //     a. If unregisterToken is not undefined, throw a TypeError exception.
        //     b. Set unregisterToken to empty.
        if !can_be_held_weakly(unregister_token) && !unregister_token.is_undefined() {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::CannotBeHeldWeakly,
                &[&unregister_token],
            );
        }

        // 6. Let cell be the Record { [[WeakRefTarget]]: target, [[HeldValue]]: heldValue, [[UnregisterToken]]: unregisterToken }.
        // 7. Append cell to finalizationRegistry.[[Cells]].
        finalization_registry.add_finalization_record(
            target.as_cell(),
            held_value,
            (!unregister_token.is_undefined()).then(|| unregister_token.as_cell()),
        );

        // 8. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 26.2.3.3 FinalizationRegistry.prototype.unregister ( unregisterToken ), https://tc39.es/ecma262/#sec-finalization-registry.prototype.unregister
    fn unregister(vm: &Vm) -> ThrowCompletionOr<Value> {
        let unregister_token = vm.argument(0);

        // 1. Let finalizationRegistry be the this value.
        // 2. Perform ? RequireInternalSlot(finalizationRegistry, [[Cells]]).
        let finalization_registry = typed_this_finalization_registry(vm)?;

        // 3. If unregisterToken is not an Object, throw a TypeError exception.
        if !can_be_held_weakly(unregister_token) {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::CannotBeHeldWeakly,
                &[&unregister_token],
            );
        }

        // 4-6.
        Ok(Value::from_bool(
            finalization_registry.remove_by_token(unregister_token.as_cell()),
        ))
    }
}
