/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
pub use crate::layout::environment::PrivateEnvironment;
use crate::layout::member::NullableMember;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::symbol::Symbol;

/// A Private Name, whose private elements are the properties an object stores under its symbol, which nothing outside
/// the engine ever sees.
#[derive(Clone)]
pub struct PrivateName {
    pub symbol: Gc<Symbol>,
    pub description: Utf16FlyString,
}

/// Utf16FlyString::operator==, which compares the raw words of the interned strings. Private names are never empty,
/// the one string the Rust equality also matches by contents.
fn fly_strings_are_equal(lhs: &Utf16FlyString, rhs: &Utf16FlyString) -> bool {
    lhs.raw_identity() == rhs.raw_identity()
}

impl PartialEq for PrivateName {
    fn eq(&self, other: &Self) -> bool {
        self.symbol == other.symbol
    }
}

impl Eq for PrivateName {}

impl core::fmt::Debug for PrivateName {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(
            formatter,
            "PrivateName({:p}, #{})",
            self.symbol.as_ptr(),
            crate::utf16::Utf16View::of_fly_string(&self.description).to_utf8()
        )
    }
}

impl PrivateName {
    /// The key of the name's private elements in the storage of objects.
    pub fn property_key(&self) -> PropertyKey {
        PropertyKey::from_symbol(self.symbol)
    }
}

// SAFETY: The symbol is the only cell a private name holds.
unsafe impl Trace for PrivateName {
    fn trace(&self, visitor: &mut Visitor) {
        self.symbol.trace(visitor);
    }
}

/// The parts of a private environment the interpreter does not read.
#[derive(Trace)]
pub struct PrivateEnvironmentStorage {
    /// [[Names]]
    private_names: GcRefCell<Vec<PrivateName>>,
}

const _: () = assert!(size_of::<PrivateEnvironment>() == 48);

define_cell!(PrivateEnvironment, Other);

// SAFETY: Visits the outer environment and the names.
unsafe impl Trace for PrivateEnvironment {
    fn trace(&self, visitor: &mut Visitor) {
        self.outer.trace(visitor);
        self.storage.trace(visitor);
    }
}

impl PrivateEnvironment {
    /// The constructor, which only NewPrivateEnvironment calls.
    pub(crate) fn create(vm: &Vm, parent: Option<Gc<PrivateEnvironment>>) -> Gc<PrivateEnvironment> {
        vm.heap().allocate(|_| PrivateEnvironment {
            header: CellHeader::for_class(Self::CLASS),
            outer: NullableMember::new(parent),
            storage: PrivateEnvironmentStorage {
                private_names: GcRefCell::new(Vec::new()),
            },
        })
    }

    fn find_private_name(&self, description: &Utf16FlyString) -> Option<PrivateName> {
        self.storage
            .private_names
            .borrow()
            .iter()
            .find(|private_name| fly_strings_are_equal(&private_name.description, description))
            .cloned()
    }

    pub fn resolve_private_identifier(&self, identifier: &Utf16FlyString) -> PrivateName {
        if let Some(private_name) = self.find_private_name(identifier) {
            return private_name;
        }

        // Note: This verify ensures that we must either have a private name with a matching description
        //       or have an outer environment. Combined this means that we assert that we always return a PrivateName.
        self.outer
            .get()
            .expect("an unresolved private identifier has an outer environment")
            .resolve_private_identifier(identifier)
    }

    pub fn contains_private_identifier(&self, identifier: &Utf16FlyString) -> bool {
        if self.find_private_name(identifier).is_some() {
            return true;
        }

        self.outer
            .get()
            .is_some_and(|outer| outer.contains_private_identifier(identifier))
    }

    pub fn add_private_name(&self, vm: &Vm, description: Utf16FlyString) {
        if self.find_private_name(&description).is_some() {
            return;
        }

        let symbol = Symbol::create_private(vm);
        self.storage
            .private_names
            .borrow_mut(vm)
            .push(PrivateName { symbol, description });
    }

    pub fn outer_environment(&self) -> Option<Gc<PrivateEnvironment>> {
        self.outer.get()
    }
}
