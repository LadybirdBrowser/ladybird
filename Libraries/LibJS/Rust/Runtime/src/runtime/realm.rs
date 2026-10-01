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
use crate::layout::accessor::Accessor;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::environment::{DeclarativeEnvironment, GlobalEnvironment};
use crate::layout::function_object::FunctionObject;
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

/// Defines an accessor for each intrinsic the object model asks the realm for, and for each property offset of the
/// premade shapes among them. Until the realm has its intrinsics, each stops the process with the intrinsic's name.
macro_rules! define_intrinsic_accessors {
    (
        cells { $($name:ident: $type:ty => $description:literal,)* }
        offsets { $($offset_name:ident => $offset_description:literal,)* }
    ) => {
        impl Realm {
            $(
                pub fn $name(&self) -> Gc<$type> {
                    unimplemented_runtime_function(concat!("the realm intrinsic ", $description), 0)
                }
            )*
            $(
                pub fn $offset_name(&self) -> u32 {
                    unimplemented_runtime_function(concat!("the realm intrinsic ", $offset_description), 0)
                }
            )*
        }
    };
}

define_intrinsic_accessors! {
    cells {
        empty_object_shape: Shape => "empty object shape",
        new_object_shape: Shape => "new object shape",
        object_prototype: Object => "%Object.prototype%",
        array_prototype: Object => "%Array.prototype%",
        string_prototype: Object => "%String.prototype%",
        number_prototype: Object => "%Number.prototype%",
        boolean_prototype: Object => "%Boolean.prototype%",
        bigint_prototype: Object => "%BigInt.prototype%",
        symbol_prototype: Object => "%Symbol.prototype%",
        function_prototype: Object => "%Function.prototype%",
        generator_function_prototype: Object => "%GeneratorFunction.prototype%",
        async_function_prototype: Object => "%AsyncFunction.prototype%",
        async_generator_function_prototype: Object => "%AsyncGeneratorFunction.prototype%",
        generator_function_prototype_prototype: Object => "%GeneratorFunction.prototype.prototype%",
        async_generator_function_prototype_prototype: Object => "%AsyncGeneratorFunction.prototype.prototype%",
        normal_function_prototype_shape: Shape => "normal function prototype shape",
        normal_function_shape: Shape => "normal function shape",
        async_function_shape: Shape => "async function shape",
        generator_function_shape: Shape => "generator function shape",
        async_generator_function_shape: Shape => "async generator function shape",
        native_function_shape: Shape => "native function shape",
        unmapped_arguments_object_shape: Shape => "unmapped arguments object shape",
        mapped_arguments_object_shape: Shape => "mapped arguments object shape",
        array_prototype_values_function: FunctionObject => "%Array.prototype.values%",
        throw_type_error_accessor: Accessor => "%ThrowTypeError% accessor",
    }
    offsets {
        normal_function_prototype_constructor_offset => "normal function prototype constructor offset",
        normal_function_length_offset => "normal function length offset",
        normal_function_name_offset => "normal function name offset",
        generator_function_prototype_property_offset => "generator function prototype property offset",
        native_function_length_offset => "native function length offset",
        native_function_name_offset => "native function name offset",
        unmapped_arguments_object_length_offset => "unmapped arguments object length offset",
        unmapped_arguments_object_well_known_symbol_iterator_offset => "unmapped arguments object @@iterator offset",
        unmapped_arguments_object_callee_offset => "unmapped arguments object callee offset",
        mapped_arguments_object_length_offset => "mapped arguments object length offset",
        mapped_arguments_object_well_known_symbol_iterator_offset => "mapped arguments object @@iterator offset",
        mapped_arguments_object_callee_offset => "mapped arguments object callee offset",
    }
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

    pub fn set_global_environment(&self, environment: Gc<GlobalEnvironment>) {
        self.global_environment.set(Some(environment));
        self.global_declarative_environment
            .set(Some(environment.declarative_record()));
    }

    pub fn global_declarative_environment(&self) -> Gc<DeclarativeEnvironment> {
        self.global_declarative_environment
            .get()
            .expect("the realm has a global environment")
    }
}
