/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use ak::Utf16FlyString;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
pub use crate::layout::environment::PrivateEnvironment;

#[derive(Clone, Default)]
pub struct PrivateName {
    pub unique_id: u64,
    pub description: Utf16FlyString,
}

/// Utf16FlyString::operator==, which compares the raw words of the interned strings. Private names are never empty,
/// the one string the Rust equality also matches by contents.
fn fly_strings_are_equal(lhs: &Utf16FlyString, rhs: &Utf16FlyString) -> bool {
    lhs.raw_identity() == rhs.raw_identity()
}

impl PartialEq for PrivateName {
    fn eq(&self, other: &Self) -> bool {
        self.unique_id == other.unique_id && fly_strings_are_equal(&self.description, &other.description)
    }
}

impl Eq for PrivateName {}

impl core::fmt::Debug for PrivateName {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(
            formatter,
            "PrivateName({}, #{})",
            self.unique_id,
            crate::utf16::Utf16View::of_fly_string(&self.description).to_utf8()
        )
    }
}

impl PrivateName {
    pub fn new(unique_id: u64, description: Utf16FlyString) -> Self {
        Self { unique_id, description }
    }
}

/// The parts of a private environment the interpreter does not read.
pub struct PrivateEnvironmentStorage {
    /// [[Names]]
    private_names: GcRefCell<Vec<PrivateName>>,
    unique_id: u64,
}

define_cell!(PrivateEnvironment, Other);

// SAFETY: The outer environment is the only cell a private environment reaches; names hold no cells.
unsafe impl Trace for PrivateEnvironment {
    fn trace(&self, visitor: &mut Visitor) {
        self.outer.trace(visitor);
    }
}

impl PrivateEnvironment {
    /// The constructor, which only NewPrivateEnvironment calls.
    pub(crate) fn create(vm: &Vm, parent: Option<Gc<PrivateEnvironment>>) -> Gc<PrivateEnvironment> {
        // FIXME: We might want to delay getting the next unique id until required.
        let unique_id = vm.next_private_environment_id().get();
        assert!(unique_id != u64::MAX);
        vm.next_private_environment_id().set(unique_id + 1);
        vm.heap().allocate(PrivateEnvironment {
            header: CellHeader::for_class(Self::CLASS),
            outer: Cell::new(parent),
            storage: PrivateEnvironmentStorage {
                private_names: GcRefCell::new(Vec::new()),
                unique_id,
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

    pub fn add_private_name(&self, description: Utf16FlyString) {
        if self.find_private_name(&description).is_some() {
            return;
        }

        self.storage
            .private_names
            .borrow_mut()
            .push(PrivateName::new(self.storage.unique_id, description));
    }

    pub fn outer_environment(&self) -> Option<Gc<PrivateEnvironment>> {
        self.outer.get()
    }
}
