/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Runtime/Temporal/Temporal.cpp: the Temporal namespace object.

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::temporal::now::Now;

// 1 The Temporal Object, https://tc39.es/proposal-temporal/#sec-temporal-objects
#[repr(C)]
#[derive(Trace)]
pub struct Temporal {
    base: Object,
}

define_object_class!(Temporal, extends: [Object], methods: {
    initialize: Temporal::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl Temporal {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<Temporal> {
        realm.create_object(
            vm,
            Temporal {
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

        // 1.1.1 Temporal [ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal-%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Temporal")),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_direct_property(vm, &names.Now, Value::from_object(Now::create(vm, realm)), attr);
        object.define_intrinsic_accessor(vm, &names.Duration, attr, |vm, realm| {
            Value::from_object(realm.intrinsics().temporal_duration_constructor(vm))
        });
        object.define_intrinsic_accessor(vm, &names.Instant, attr, |vm, realm| {
            Value::from_object(realm.intrinsics().temporal_instant_constructor(vm))
        });
        object.define_intrinsic_accessor(vm, &names.PlainDate, attr, |vm, realm| {
            Value::from_object(realm.intrinsics().temporal_plain_date_constructor(vm))
        });
        object.define_intrinsic_accessor(vm, &names.PlainDateTime, attr, |vm, realm| {
            Value::from_object(realm.intrinsics().temporal_plain_date_time_constructor(vm))
        });
        object.define_intrinsic_accessor(vm, &names.PlainMonthDay, attr, |vm, realm| {
            Value::from_object(realm.intrinsics().temporal_plain_month_day_constructor(vm))
        });
        object.define_intrinsic_accessor(vm, &names.PlainTime, attr, |vm, realm| {
            Value::from_object(realm.intrinsics().temporal_plain_time_constructor(vm))
        });
        object.define_intrinsic_accessor(vm, &names.PlainYearMonth, attr, |vm, realm| {
            Value::from_object(realm.intrinsics().temporal_plain_year_month_constructor(vm))
        });
        object.define_intrinsic_accessor(vm, &names.ZonedDateTime, attr, |vm, realm| {
            Value::from_object(realm.intrinsics().temporal_zoned_date_time_constructor(vm))
        });
    }
}
