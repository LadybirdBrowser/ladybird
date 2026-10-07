/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ops::Deref;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::runtime::big_int::BigInt;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::realm::Realm;

/// A BigInt object, whose [[BigIntData]] is `bigint`.
#[repr(C)]
#[derive(Trace)]
pub struct BigIntObject {
    base: Object,
    bigint: Gc<BigInt>,
}

define_cell!(BigIntObject, Object, extends: [Object]);

impl Deref for BigIntObject {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl BigIntObject {
    pub fn create(vm: &Vm, realm: Gc<Realm>, bigint: Gc<BigInt>) -> Gc<BigIntObject> {
        realm.create_object(
            vm,
            BigIntObject {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().bigint_prototype(vm),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
                bigint,
            },
        )
    }

    pub fn bigint(&self) -> Gc<BigInt> {
        self.bigint
    }
}
