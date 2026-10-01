/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::environment::{DeclarativeEnvironment, GlobalEnvironment};
use crate::layout::object::Object;
pub use crate::layout::realm::Realm;
use crate::runtime::shape::Shape;

/// The parts of a realm the interpreter does not read. [[HostDefined]] comes with the hosts that define it.
#[derive(Default)]
pub struct RealmStorage {}

define_cell!(Realm, Other);

// SAFETY: Visits every cell a realm holds.
unsafe impl Trace for Realm {
    fn trace(&self, visitor: &mut Visitor) {
        self.intrinsics.trace(visitor);
        self.global_object.trace(visitor);
        self.global_environment.trace(visitor);
        self.global_declarative_environment.trace(visitor);
    }
}

/// Defines an accessor for each intrinsic the object model asks the realm for. Until the realm has its intrinsics,
/// each stops the process with the intrinsic's name.
macro_rules! define_intrinsic_accessors {
    ($($name:ident: $type:ty => $description:literal,)*) => {
        impl Realm {
            $(
                pub fn $name(&self) -> Gc<$type> {
                    unimplemented_runtime_function(concat!("the realm intrinsic ", $description), 0)
                }
            )*
        }
    };
}

define_intrinsic_accessors! {
    empty_object_shape: Shape => "empty object shape",
    new_object_shape: Shape => "new object shape",
    object_prototype: Object => "%Object.prototype%",
    array_prototype: Object => "%Array.prototype%",
    string_prototype: Object => "%String.prototype%",
    number_prototype: Object => "%Number.prototype%",
    boolean_prototype: Object => "%Boolean.prototype%",
    bigint_prototype: Object => "%BigInt.prototype%",
    symbol_prototype: Object => "%Symbol.prototype%",
}

impl Realm {
    /// A realm without intrinsics, a global object or a global environment, which
    /// InitializeHostDefinedRealm goes on to create.
    pub fn create(vm: &Vm) -> Gc<Realm> {
        vm.heap().allocate(Realm {
            header: CellHeader::for_class(Self::CLASS),
            global_object: Cell::new(None),
            global_declarative_environment: Cell::new(None),
            global_environment: Cell::new(None),
            intrinsics: Cell::new(None),
            storage: RealmStorage::default(),
        })
    }

    pub fn global_object(&self) -> Gc<Object> {
        self.global_object.get().expect("the realm has a global object")
    }

    pub fn set_global_object(&self, global: Gc<Object>) {
        self.global_object.set(Some(global));
    }

    pub fn global_environment(&self) -> Gc<GlobalEnvironment> {
        self.global_environment
            .get()
            .expect("the realm has a global environment")
    }

    pub fn global_declarative_environment(&self) -> Gc<DeclarativeEnvironment> {
        self.global_declarative_environment
            .get()
            .expect("the realm has a global environment")
    }
}
