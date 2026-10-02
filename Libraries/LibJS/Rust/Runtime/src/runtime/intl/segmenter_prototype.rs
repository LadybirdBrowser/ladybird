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
use crate::runtime::intl::segmenter::Segmenter;
use crate::runtime::intl::segments::Segments;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;

/// 19.3 Properties of the Intl.Segmenter Prototype Object, https://tc39.es/ecma402/#sec-properties-of-intl-segmenter-prototype-object
#[repr(C)]
#[derive(Trace)]
pub struct SegmenterPrototype {
    base: Object,
}

define_object_class!(SegmenterPrototype, extends: [Object], methods: {
    initialize: SegmenterPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_segmenter(vm: &Vm) -> ThrowCompletionOr<Gc<Segmenter>> {
    typed_this_object::<Segmenter>(vm, "Segmenter")
}

impl SegmenterPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<SegmenterPrototype> {
        realm.create_object(
            vm,
            SegmenterPrototype {
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

        // 19.3.4 Intl.Segmenter.prototype [ %Symbol.toStringTag% ], https://tc39.es/ecma402/#sec-intl.segmenter.prototype-%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Intl.Segmenter")),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.resolvedOptions,
            raw_native!(SegmenterPrototype::resolved_options),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.segment,
            raw_native!(SegmenterPrototype::segment),
            1,
            attr,
            None,
        );
    }

    // 19.3.2 Intl.Segmenter.prototype.resolvedOptions ( ), https://tc39.es/ecma402/#sec-intl.segmenter.prototype.resolvedoptions
    fn resolved_options(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");
        let names = &vm.names;

        // 1. Let segmenter be the this value.
        // 2. Perform ? RequireInternalSlot(segmenter, [[InitializedSegmenter]]).
        let segmenter = typed_this_segmenter(vm)?;

        // 3. Let options be OrdinaryObjectCreate(%Object.prototype%).
        let options = Object::create(vm, realm, Some(realm.object_prototype()));

        // 4. For each row of Table 34, except the header row, in table order, do
        //     a. Let p be the Property value of the current row.
        //     b. Let v be the value of segmenter's internal slot whose name is the Internal Slot value of the current row.
        //     c. Assert: v is not undefined.
        //     d. Perform ! CreateDataPropertyOrThrow(options, p, v).
        options
            .create_data_property_or_throw(
                vm,
                &names.locale,
                Value::from_string(PrimitiveString::create(vm, segmenter.locale())),
            )
            .must();
        options
            .create_data_property_or_throw(
                vm,
                &names.granularity,
                Value::from_string(PrimitiveString::create_from_utf8(
                    vm,
                    segmenter.segmenter_granularity_string(),
                )),
            )
            .must();

        // 5. Return options.
        Ok(Value::from_object(options))
    }

    // 19.3.3 Intl.Segmenter.prototype.segment ( string ), https://tc39.es/ecma402/#sec-intl.segmenter.prototype.segment
    fn segment(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");

        // 1. Let segmenter be the this value.
        // 2. Perform ? RequireInternalSlot(segmenter, [[InitializedSegmenter]]).
        let segmenter = typed_this_segmenter(vm)?;

        // 3. Let string be ? ToString(string).
        let string = vm.argument(0).to_primitive_string(vm)?;

        // 4. Return ! CreateSegmentsObject(segmenter, string).
        Ok(Value::from_object(Segments::create(
            vm,
            realm,
            segmenter.clone_segmenter(),
            string,
        )))
    }
}
