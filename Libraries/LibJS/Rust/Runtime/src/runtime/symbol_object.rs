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
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::realm::Realm;
use crate::runtime::symbol::Symbol;

/// A Symbol object, whose [[SymbolData]] is `symbol`.
#[repr(C)]
#[derive(Trace)]
pub struct SymbolObject {
    base: Object,
    symbol: Gc<Symbol>,
}

define_cell!(SymbolObject, Object, extends: [Object]);

impl Deref for SymbolObject {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl SymbolObject {
    pub fn create(vm: &Vm, realm: Gc<Realm>, primitive_symbol: Gc<Symbol>) -> Gc<SymbolObject> {
        realm.create_object(
            vm,
            SymbolObject {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().symbol_prototype(vm),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
                symbol: primitive_symbol,
            },
        )
    }

    pub fn primitive_symbol(&self) -> Gc<Symbol> {
        self.symbol
    }
}
