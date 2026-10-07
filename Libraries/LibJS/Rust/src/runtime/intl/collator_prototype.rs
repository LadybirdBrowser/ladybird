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
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::intl::collator::Collator;
use crate::runtime::intl::collator_compare_function::CollatorCompareFunction;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;

/// 10.3 Properties of the Intl.Collator Prototype Object, https://tc39.es/ecma402/#sec-properties-of-the-intl-collator-prototype-object
#[repr(C)]
#[derive(Trace)]
pub struct CollatorPrototype {
    base: Object,
}

define_object_class!(CollatorPrototype, extends: [Object], methods: {
    initialize: CollatorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_collator(vm: &Vm) -> ThrowCompletionOr<Gc<Collator>> {
    typed_this_object::<Collator>(vm, "Collator")
}

impl CollatorPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<CollatorPrototype> {
        realm.create_object(
            vm,
            CollatorPrototype {
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

        // 10.3.4 Intl.Collator.prototype [ %Symbol.toStringTag% ], https://tc39.es/ecma402/#sec-intl.collator.prototype-%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Intl.Collator")),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.resolvedOptions,
            raw_native!(CollatorPrototype::resolved_options),
            0,
            attr,
            None,
        );
        object.define_native_accessor(
            vm,
            realm,
            &names.compare,
            raw_native!(CollatorPrototype::compare_getter),
            None,
            attr,
        );
    }

    // 10.3.2 Intl.Collator.prototype.resolvedOptions ( ), https://tc39.es/ecma402/#sec-intl.collator.prototype.resolvedoptions
    fn resolved_options(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");
        let names = &vm.names;

        // 1. Let collator be the this value.
        // 2. Perform ? RequireInternalSlot(collator, [[InitializedCollator]]).
        let collator = typed_this_collator(vm)?;

        // 3. Let options be OrdinaryObjectCreate(%Object.prototype%).
        let options = Object::create(vm, realm, Some(realm.object_prototype()));

        // 4. For each row of Table 3, except the header row, in table order, do
        //     a. Let p be the Property value of the current row.
        //     b. Let v be the value of collator's internal slot whose name is the Internal Slot value of the current row.
        //     c. If the current row has an Extension Key value, then
        //         i. Let extensionKey be the Extension Key value of the current row.
        //         ii. If %Collator%.[[RelevantExtensionKeys]] does not contain extensionKey, then
        //             1. Let v be undefined.
        //     d. If v is not undefined, then
        //         i. Perform ! CreateDataPropertyOrThrow(options, p, v).
        let string = |string: &str| Value::from_string(PrimitiveString::create_from_utf8(vm, string));
        options
            .create_data_property_or_throw(
                vm,
                &names.locale,
                Value::from_string(PrimitiveString::create(vm, collator.locale())),
            )
            .must();
        options
            .create_data_property_or_throw(vm, &names.usage, string(collator.usage_string()))
            .must();
        options
            .create_data_property_or_throw(vm, &names.sensitivity, string(collator.sensitivity_string()))
            .must();
        options
            .create_data_property_or_throw(
                vm,
                &names.ignorePunctuation,
                Value::from_bool(collator.ignore_punctuation()),
            )
            .must();
        options
            .create_data_property_or_throw(
                vm,
                &names.collation,
                Value::from_string(PrimitiveString::create(vm, collator.collation())),
            )
            .must();
        options
            .create_data_property_or_throw(vm, &names.numeric, Value::from_bool(collator.numeric()))
            .must();
        options
            .create_data_property_or_throw(vm, &names.caseFirst, string(collator.case_first_string()))
            .must();

        // 5. Return options.
        Ok(Value::from_object(options))
    }

    // 10.3.3 get Intl.Collator.prototype.compare, https://tc39.es/ecma402/#sec-intl.collator.prototype.compare
    fn compare_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");

        // 1. Let collator be the this value.
        // 2. Perform ? RequireInternalSlot(collator, [[InitializedCollator]]).
        let collator = typed_this_collator(vm)?;

        // 3. If collator.[[BoundCompare]] is undefined, then
        if collator.bound_compare().is_none() {
            // a. Let F be a new built-in function object as defined in 10.3.3.1.
            // b. Set F.[[Collator]] to collator.
            let function = CollatorCompareFunction::create(vm, realm, collator);

            // c. Set collator.[[BoundCompare]] to F.
            collator.set_bound_compare(Some(function));
        }

        // 4. Return collator.[[BoundCompare]].
        Ok(Value::from_object(
            collator
                .bound_compare()
                .expect("the bound compare function was just created"),
        ))
    }
}
