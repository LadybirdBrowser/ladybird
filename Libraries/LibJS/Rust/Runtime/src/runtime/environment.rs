/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! JS::Environment, the base of every Environment Record.
//!
//! The virtual methods of Environment are a table of function pointers per class of environment, EnvironmentMethods,
//! which Environment::methods() finds by matching on the environment's class. A class's table starts from the table of
//! the class it extends, as in `EnvironmentMethods { has_this_binding: ..., ..DECLARATIVE_ENVIRONMENT_METHODS }`, so it
//! overrides exactly the methods the class implements itself. GlobalEnvironment and ObjectEnvironment plug in by
//! defining their tables and adding their class to the match.
//!
//! The methods of Environment dispatch on the class. The inherent methods of each environment type are that type's own
//! implementation, such as DeclarativeEnvironment::get_binding_value(); call through Environment when the environment
//! may be of a class that overrides the method.

use ak::Utf16FlyString;

use crate::gc::class::{Class, Extends, GcCell, class_of, define_cell};
use crate::gc::class_id::ClassId;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::environment::DeclarativeEnvironment;
pub use crate::layout::environment::Environment;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::declarative_environment::DECLARATIVE_ENVIRONMENT_METHODS;
use crate::runtime::function_environment::FUNCTION_ENVIRONMENT_METHODS;
use crate::runtime::global_environment::GLOBAL_ENVIRONMENT_METHODS;
use crate::runtime::module_environment::MODULE_ENVIRONMENT_METHODS;
use crate::runtime::object_environment::OBJECT_ENVIRONMENT_METHODS;
use libjs_abi::value as nan_box;

/// The [[ThisBindingStatus]] of a function Environment Record, which Environment keeps to pack better.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ThisBindingStatus {
    Lexical,
    Initialized,
    Uninitialized,
}

impl ThisBindingStatus {
    pub fn from_raw(raw: u8) -> Self {
        match raw {
            0 => Self::Lexical,
            1 => Self::Initialized,
            2 => Self::Uninitialized,
            _ => unreachable!("{raw} is not a ThisBindingStatus"),
        }
    }
}

/// Mirrors JS::Environment::InitializeBindingHint.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InitializeBindingHint {
    Normal,
    SyncDispose,
    AsyncDispose,
}

/// HasBinding, which also reports the index of the binding it finds when asked to, for callers that cache it.
pub type HasBindingMethod =
    fn(&Environment, &Vm, &Utf16FlyString, Option<&mut Option<usize>>) -> ThrowCompletionOr<bool>;

/// The virtual methods of JS::Environment, as one class of environment implements them. The binding methods take
/// the VM because object environments call into their binding object.
pub struct EnvironmentMethods {
    pub has_this_binding: fn(&Environment) -> bool,
    pub get_this_binding: fn(&Environment, &Vm) -> ThrowCompletionOr<Value>,
    pub with_base_object: fn(&Environment) -> Option<Gc<Object>>,
    pub has_binding: HasBindingMethod,
    pub create_mutable_binding: fn(&Environment, &Vm, &Utf16FlyString, bool) -> ThrowCompletionOr<()>,
    pub create_immutable_binding: fn(&Environment, &Vm, &Utf16FlyString, bool) -> ThrowCompletionOr<()>,
    pub initialize_binding:
        fn(&Environment, &Vm, &Utf16FlyString, Value, InitializeBindingHint) -> ThrowCompletionOr<()>,
    pub set_mutable_binding: fn(&Environment, &Vm, &Utf16FlyString, Value, bool) -> ThrowCompletionOr<()>,
    pub get_binding_value: fn(&Environment, &Vm, &Utf16FlyString, bool) -> ThrowCompletionOr<Value>,
    pub delete_binding: fn(&Environment, &Vm, &Utf16FlyString) -> ThrowCompletionOr<bool>,
    pub is_global_environment: bool,
    pub is_function_environment: bool,
    pub is_object_environment: bool,
    pub is_catch_environment: fn(&Environment) -> bool,
}

fn pure_virtual(method: &str) -> ! {
    unreachable!("Environment::{method} is pure virtual")
}

/// The methods JS::Environment itself defines. The binding methods are pure virtual there.
pub const ENVIRONMENT_METHODS: EnvironmentMethods = EnvironmentMethods {
    has_this_binding: |_| false,
    get_this_binding: |_, _| Ok(Value::UNDEFINED),
    with_base_object: |_| None,
    has_binding: |_, _, _, _| pure_virtual("has_binding"),
    create_mutable_binding: |_, _, _, _| pure_virtual("create_mutable_binding"),
    create_immutable_binding: |_, _, _, _| pure_virtual("create_immutable_binding"),
    initialize_binding: |_, _, _, _, _| pure_virtual("initialize_binding"),
    set_mutable_binding: |_, _, _, _, _| pure_virtual("set_mutable_binding"),
    get_binding_value: |_, _, _, _| pure_virtual("get_binding_value"),
    delete_binding: |_, _, _| pure_virtual("delete_binding"),
    is_global_environment: false,
    is_function_environment: false,
    is_object_environment: false,
    is_catch_environment: |_| false,
};

define_cell!(Environment, Other);

// SAFETY: The outer environment is the only cell an Environment holds.
unsafe impl Trace for Environment {
    fn trace(&self, visitor: &mut Visitor) {
        self.outer.trace(visitor);
    }
}

impl Environment {
    /// The Environment part of a new environment of `class`.
    pub fn new(class: &'static Class, outer_environment: Option<Gc<Environment>>, is_declarative: bool) -> Self {
        Self {
            header: CellHeader::for_class(class),
            this_binding_status: (ThisBindingStatus::Uninitialized as u8).into(),
            permanently_screwed_by_eval: false.into(),
            declarative: is_declarative.into(),
            outer: outer_environment.into(),
        }
    }

    pub fn class(&self) -> &'static Class {
        self.header.class
    }

    fn methods(&self) -> &'static EnvironmentMethods {
        match self.class().id {
            ClassId::DeclarativeEnvironment => &DECLARATIVE_ENVIRONMENT_METHODS,
            ClassId::FunctionEnvironment => &FUNCTION_ENVIRONMENT_METHODS,
            ClassId::ModuleEnvironment => &MODULE_ENVIRONMENT_METHODS,
            ClassId::GlobalEnvironment => &GLOBAL_ENVIRONMENT_METHODS,
            ClassId::ObjectEnvironment => &OBJECT_ENVIRONMENT_METHODS,
            class_id => unreachable!("{class_id:?} is not a class of environment"),
        }
    }

    /// The environment as a `T`, if it was allocated as one.
    pub fn downcast_ref<T: GcCell + Extends<Environment>>(&self) -> Option<&T> {
        self.class()
            .is_subclass_of(T::CLASS)
            // SAFETY: The environment was allocated as a T or a subclass of it, which starts with a T.
            .then(|| unsafe { &*core::ptr::from_ref(self).cast::<T>() })
    }

    /// The environment as a DeclarativeEnvironment, if its declarative flag is set.
    pub fn as_declarative_environment(&self) -> Option<&DeclarativeEnvironment> {
        self.is_declarative_environment()
            // SAFETY: Only DeclarativeEnvironment and the classes extending it are declarative, and they start with
            // a DeclarativeEnvironment.
            .then(|| unsafe { &*core::ptr::from_ref(self).cast::<DeclarativeEnvironment>() })
    }

    pub fn has_this_binding(&self) -> bool {
        (self.methods().has_this_binding)(self)
    }

    pub fn get_this_binding(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        (self.methods().get_this_binding)(self, vm)
    }

    pub fn with_base_object(&self) -> Option<Gc<Object>> {
        (self.methods().with_base_object)(self)
    }

    pub fn has_binding(
        &self,
        vm: &Vm,
        name: &Utf16FlyString,
        out_index: Option<&mut Option<usize>>,
    ) -> ThrowCompletionOr<bool> {
        (self.methods().has_binding)(self, vm, name, out_index)
    }

    pub fn create_mutable_binding(
        &self,
        vm: &Vm,
        name: &Utf16FlyString,
        can_be_deleted: bool,
    ) -> ThrowCompletionOr<()> {
        (self.methods().create_mutable_binding)(self, vm, name, can_be_deleted)
    }

    pub fn create_immutable_binding(&self, vm: &Vm, name: &Utf16FlyString, strict: bool) -> ThrowCompletionOr<()> {
        (self.methods().create_immutable_binding)(self, vm, name, strict)
    }

    pub fn initialize_binding(
        &self,
        vm: &Vm,
        name: &Utf16FlyString,
        value: Value,
        hint: InitializeBindingHint,
    ) -> ThrowCompletionOr<()> {
        (self.methods().initialize_binding)(self, vm, name, value, hint)
    }

    pub fn set_mutable_binding(
        &self,
        vm: &Vm,
        name: &Utf16FlyString,
        value: Value,
        strict: bool,
    ) -> ThrowCompletionOr<()> {
        (self.methods().set_mutable_binding)(self, vm, name, value, strict)
    }

    pub fn get_binding_value(&self, vm: &Vm, name: &Utf16FlyString, strict: bool) -> ThrowCompletionOr<Value> {
        (self.methods().get_binding_value)(self, vm, name, strict)
    }

    pub fn delete_binding(&self, vm: &Vm, name: &Utf16FlyString) -> ThrowCompletionOr<bool> {
        (self.methods().delete_binding)(self, vm, name)
    }

    // [[OuterEnv]]
    pub fn outer_environment(&self) -> Option<Gc<Environment>> {
        self.outer.get()
    }

    pub fn is_declarative_environment(&self) -> bool {
        self.declarative.get()
    }

    pub fn is_global_environment(&self) -> bool {
        self.methods().is_global_environment
    }

    pub fn is_function_environment(&self) -> bool {
        self.methods().is_function_environment
    }

    pub fn is_object_environment(&self) -> bool {
        self.methods().is_object_environment
    }

    pub fn is_catch_environment(&self) -> bool {
        (self.methods().is_catch_environment)(self)
    }

    // This flag is set on environments within a function when direct eval() is performed in that function.
    // It propagates up to the function boundary (not beyond) and is used to disable variable access caching.
    // Code in parent functions is not affected because eval can only inject vars into its containing
    // function's variable environment, not into parent function scopes.
    pub fn is_permanently_screwed_by_eval(&self) -> bool {
        self.permanently_screwed_by_eval.get()
    }

    pub fn set_permanently_screwed_by_eval(&self) {
        if self.permanently_screwed_by_eval.get() {
            return;
        }
        self.permanently_screwed_by_eval.set(true);

        // Stop propagation at function or global boundaries.
        // Eval can only inject vars into its containing function's variable environment,
        // not into parent function scopes.
        if self.is_function_environment() || self.is_global_environment() {
            return;
        }

        if let Some(outer_environment) = self.outer_environment() {
            outer_environment.set_permanently_screwed_by_eval();
        }
    }
}

impl Value {
    /// An environment as the interpreter keeps it in a register: a cell value without a more specific tag.
    pub fn from_environment<T: GcCell + Extends<Environment>>(environment: Gc<T>) -> Self {
        Self::with_cell_tag(nan_box::IS_CELL_BIT, environment.upcast::<Environment>())
    }

    /// The environment the value holds, which must be one.
    pub fn as_environment(self) -> Gc<Environment> {
        assert!(self.is_cell());
        // SAFETY: The value holds a cell, and every cell starts with its class, which is checked below.
        let environment = unsafe { self.cell::<Environment>() };
        assert!(
            class_of(environment).is_subclass_of(Environment::CLASS),
            "the cell is not an environment"
        );
        environment
    }
}
