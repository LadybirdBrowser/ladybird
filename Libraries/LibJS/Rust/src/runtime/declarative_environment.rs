/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! JS::DeclarativeEnvironment, whose bindings live where the interpreter reads them: the values in binding_values,
//! indexed like the environment coordinates the bytecode carries, and the flags either in the environment's shape,
//! shared by every environment that one piece of code creates, or in the rare data for bindings the shape does not
//! cover.

use core::cell::Cell;
use core::ops::Deref;
use std::collections::HashMap;

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::gc::class::{Class, Finalize, GcCell, define_cell};
use crate::gc::class_id::ClassId;
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::buffer::InterpreterBuffer;
use crate::layout::cell::Gc;
pub use crate::layout::environment::{DeclarativeEnvironment, DeclarativeEnvironmentRareData};
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{DisposeCapability, add_disposable_resource};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::environment::{ENVIRONMENT_METHODS, Environment, EnvironmentMethods, InitializeBindingHint};
use crate::runtime::environment_shape::{EnvironmentShape, EnvironmentShapeCache};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::module_environment::ModuleEnvironment;

#[derive(Clone)]
pub struct Binding {
    pub name: Utf16FlyString,
    pub value: Value,
    pub strict: bool,
    pub mutable: bool,
    pub can_be_deleted: bool,
    pub initialized: bool,
}

/// A binding of the environment that was searched, by index, or a copy of a binding of another environment, which
/// module environments find for their indirect bindings.
pub enum BindingAndIndex {
    Index(usize),
    Temporary(Binding),
}

impl BindingAndIndex {
    /// The binding, where `environment` is the environment that found it.
    pub fn binding(&self, environment: &DeclarativeEnvironment) -> Binding {
        match self {
            Self::Index(index) => environment.binding_at(*index),
            Self::Temporary(binding) => binding.clone(),
        }
    }

    pub fn index(&self) -> Option<usize> {
        match self {
            Self::Index(index) => Some(*index),
            Self::Temporary(_) => None,
        }
    }
}

/// Mirrors AK::Bitmap, as far as the deleted bindings of an environment use it.
#[derive(Default)]
struct Bitmap {
    data: Option<Box<[u8]>>,
    size: usize,
}

impl Bitmap {
    fn create(size: usize, default_value: bool) -> Self {
        assert!(size != 0);
        let fill = if default_value { 0xff } else { 0 };
        Self {
            data: Some(vec![fill; size.div_ceil(8)].into_boxed_slice()),
            size,
        }
    }

    fn is_null(&self) -> bool {
        self.data.is_none()
    }

    fn size(&self) -> usize {
        self.size
    }

    fn get(&self, index: usize) -> bool {
        assert!(index < self.size);
        let data = self.data.as_ref().expect("a bitmap with bits has data");
        data[index / 8] & (1 << (index % 8)) != 0
    }

    fn set(&mut self, index: usize, value: bool) {
        assert!(index < self.size);
        let byte = &mut self.data.as_mut().expect("a bitmap with bits has data")[index / 8];
        if value {
            *byte |= 1 << (index % 8);
        } else {
            *byte &= !(1 << (index % 8));
        }
    }

    fn grow(&mut self, size: usize, default_value: bool) {
        assert!(size > self.size);
        let previous_size = self.size;
        let mut data = vec![0; size.div_ceil(8)].into_boxed_slice();
        if let Some(previous_data) = &self.data {
            data[..previous_data.len()].copy_from_slice(previous_data);
        }
        self.data = Some(data);
        self.size = size;
        for index in previous_size..size {
            self.set(index, default_value);
        }
    }
}

/// The part of DeclarativeEnvironment::RareData that the interpreter does not read.
#[derive(Default, Trace)]
pub struct DeclarativeEnvironmentRareDataStorage {
    binding_names: GcRefCell<Vec<Utf16FlyString>>,
    #[gc(untraced)]
    deleted_bindings: GcRefCell<Bitmap>,
    #[gc(untraced)]
    bindings_assoc: GcRefCell<HashMap<Utf16FlyString, usize, foldhash::fast::RandomState>>,
    dispose_capability: GcRefCell<DisposeCapability>,
    environment_shape_cache: Cell<Option<EnvironmentShapeCache>>,
    expected_binding_count: Cell<usize>,
    is_catch_environment: Cell<bool>,
}

impl DeclarativeEnvironmentRareData {
    fn is_empty(&self) -> bool {
        let storage = &self.storage;
        storage.binding_names.borrow().is_empty()
            && self.binding_flags.is_empty()
            && storage.deleted_bindings.borrow().is_null()
            && storage.bindings_assoc.borrow().is_empty()
            && storage.dispose_capability.borrow().disposable_resource_stack.is_none()
            && storage.environment_shape_cache.get().is_none()
            && storage.expected_binding_count.get() == 0
            && !storage.is_catch_environment.get()
    }
}

// SAFETY: The flags hold no cells; the storage traces the rest.
unsafe impl Trace for DeclarativeEnvironmentRareData {
    fn trace(&self, visitor: &mut Visitor) {
        self.storage.trace(visitor);
    }
}

define_cell!(DeclarativeEnvironment, Other, extends: [Environment], finalize: finalize);

// SAFETY: Visits the outer environment, the rare data's dispose capability and shape cache, the shape and every
// binding value.
unsafe impl Trace for DeclarativeEnvironment {
    fn trace(&self, visitor: &mut Visitor) {
        self.base.trace(visitor);
        if let Some(rare_data) = self.rare_data() {
            rare_data.trace(visitor);
        }
        self.shape.trace(visitor);
        self.binding_values.trace(visitor);
    }
}

impl Finalize for DeclarativeEnvironment {
    fn finalize(&self) {
        self.binding_values.clear();
        self.free_rare_data();
    }
}

impl Deref for DeclarativeEnvironment {
    type Target = Environment;

    fn deref(&self) -> &Environment {
        &self.base
    }
}

fn declarative(environment: &Environment) -> &DeclarativeEnvironment {
    environment
        .as_declarative_environment()
        .expect("only declarative environments have the declarative environment methods")
}

/// The methods JS::DeclarativeEnvironment overrides.
pub const DECLARATIVE_ENVIRONMENT_METHODS: EnvironmentMethods = EnvironmentMethods {
    has_binding: |environment, _, name, out_index| declarative(environment).has_binding(name, out_index),
    create_mutable_binding: |environment, vm, name, can_be_deleted| {
        declarative(environment).create_mutable_binding(vm, name, can_be_deleted)
    },
    create_immutable_binding: |environment, vm, name, strict| {
        declarative(environment).create_immutable_binding(vm, name, strict)
    },
    initialize_binding: |environment, vm, name, value, hint| {
        declarative(environment).initialize_binding(vm, name, value, hint)
    },
    set_mutable_binding: |environment, vm, name, value, strict| {
        declarative(environment).set_mutable_binding(vm, name, value, strict)
    },
    get_binding_value: |environment, vm, name, strict| declarative(environment).get_binding_value(vm, name, strict),
    delete_binding: |environment, vm, name| declarative(environment).delete_binding(vm, name),
    is_catch_environment: |environment| declarative(environment).is_catch_environment(),
    ..ENVIRONMENT_METHODS
};

impl DeclarativeEnvironment {
    const BINDING_FLAG_STRICT: u8 = EnvironmentShape::BINDING_FLAG_STRICT;
    const BINDING_FLAG_MUTABLE: u8 = EnvironmentShape::BINDING_FLAG_MUTABLE;
    const BINDING_FLAG_CAN_BE_DELETED: u8 = EnvironmentShape::BINDING_FLAG_CAN_BE_DELETED;

    /// A new environment of `class`, which is DeclarativeEnvironment or a class that extends it, with no bindings.
    pub fn new(class: &'static Class, outer_environment: Option<Gc<Environment>>) -> Self {
        Self {
            base: Environment::new(class, outer_environment, true),
            shape: Cell::new(None),
            binding_values: InterpreterBuffer::new(),
            rare_data: Cell::new(core::ptr::null_mut()),
            serial_number: Cell::new(0),
        }
    }

    pub fn create(vm: &Vm, outer_environment: Option<Gc<Environment>>) -> Gc<DeclarativeEnvironment> {
        vm.heap().allocate(Self::new(Self::CLASS, outer_environment))
    }

    fn rare_data(&self) -> Option<&DeclarativeEnvironmentRareData> {
        // SAFETY: The rare data belongs to this environment. Only drop_rare_data_if_empty and finalize free it, and
        // nothing keeps a reference to it across either.
        unsafe { self.rare_data.get().as_ref() }
    }

    fn ensure_rare_data(&self) -> &DeclarativeEnvironmentRareData {
        if self.rare_data.get().is_null() {
            let rare_data = Box::new(DeclarativeEnvironmentRareData {
                binding_flags: InterpreterBuffer::new(),
                storage: DeclarativeEnvironmentRareDataStorage::default(),
            });
            self.rare_data.set(Box::into_raw(rare_data));
        }
        self.rare_data().expect("the rare data was just allocated")
    }

    fn drop_rare_data_if_empty(&self) {
        if self.rare_data().is_some_and(DeclarativeEnvironmentRareData::is_empty) {
            self.free_rare_data();
        }
    }

    fn free_rare_data(&self) {
        let rare_data = self.rare_data.replace(core::ptr::null_mut());
        if rare_data.is_null() {
            return;
        }
        // SAFETY: ensure_rare_data allocated the rare data as a Box, and nothing refers to it anymore.
        let rare_data = unsafe { Box::from_raw(rare_data) };
        rare_data.binding_flags.clear();
    }

    fn increment_environment_serial_number(&self) {
        if self.shape.get().is_some() {
            return;
        }
        if self
            .rare_data()
            .is_some_and(|rare_data| rare_data.storage.environment_shape_cache.get().is_some())
        {
            return;
        }
        self.serial_number.set(self.serial_number.get() + 1);
    }

    pub fn is_catch_environment(&self) -> bool {
        self.rare_data()
            .is_some_and(|rare_data| rare_data.storage.is_catch_environment.get())
    }

    pub fn set_is_catch_environment(&self, value: bool) {
        if !value && self.rare_data().is_none() {
            return;
        }
        self.ensure_rare_data().storage.is_catch_environment.set(value);
        self.drop_rare_data_if_empty();
    }

    pub fn environment_serial_number(&self) -> u64 {
        self.serial_number.get()
    }

    pub fn dispose_capability(&self) -> &GcRefCell<DisposeCapability> {
        &self.ensure_rare_data().storage.dispose_capability
    }

    pub fn dispose_capability_if_exists(&self) -> Option<&GcRefCell<DisposeCapability>> {
        let dispose_capability = &self.rare_data()?.storage.dispose_capability;
        let exists = dispose_capability.borrow().disposable_resource_stack.is_some();
        exists.then_some(dispose_capability)
    }

    fn shape_binding_count(&self) -> usize {
        self.shape.get().map_or(0, |shape| shape.size())
    }

    fn binding_count(&self) -> usize {
        self.binding_values.size()
    }

    fn local_binding_index(&self, index: usize) -> usize {
        index - self.shape_binding_count()
    }

    fn append_binding(&self, binding: Binding) {
        let index = self.binding_values.size();

        let mut flags = 0;
        if binding.strict {
            flags |= Self::BINDING_FLAG_STRICT;
        }
        if binding.mutable {
            flags |= Self::BINDING_FLAG_MUTABLE;
        }
        if binding.can_be_deleted {
            flags |= Self::BINDING_FLAG_CAN_BE_DELETED;
        }

        if let Some(shape) = self.shape.get()
            && index < shape.size()
        {
            assert!(shape.binding_name(index) == &binding.name);
            assert!(shape.binding_flags(index) == flags);
        } else {
            let rare_data = self.ensure_rare_data();
            rare_data
                .storage
                .bindings_assoc
                .borrow_mut()
                .insert(binding.name.clone(), index);
            rare_data.storage.binding_names.borrow_mut().push(binding.name);
            rare_data.binding_flags.append(flags);
        }

        self.binding_values.append(if binding.initialized {
            binding.value
        } else {
            Value::EMPTY
        });
    }

    fn clear_binding(&self, name: &Utf16FlyString, index: usize) {
        if index < self.shape_binding_count() {
            self.ensure_deleted_bindings_capacity(index + 1);
            let rare_data = self
                .rare_data()
                .expect("ensure_deleted_bindings_capacity allocates the rare data");
            rare_data.storage.deleted_bindings.borrow_mut().set(index, true);
            self.binding_values.set(index, Value::EMPTY);
            return;
        }

        let rare_data = self
            .rare_data()
            .expect("bindings outside the shape are in the rare data");
        rare_data.storage.bindings_assoc.borrow_mut().remove(name);
        let local_index = self.local_binding_index(index);
        rare_data.storage.binding_names.borrow_mut()[local_index] = Utf16FlyString::default();
        self.binding_values.set(index, Value::EMPTY);
        rare_data.binding_flags.set(local_index, 0);
    }

    fn ensure_deleted_bindings_capacity(&self, needed_capacity: usize) {
        let rare_data = self.ensure_rare_data();
        let mut deleted_bindings = rare_data.storage.deleted_bindings.borrow_mut();
        if needed_capacity <= deleted_bindings.size() {
            return;
        }
        deleted_bindings.grow(needed_capacity.div_ceil(8) * 8, false);
    }

    fn binding_at(&self, index: usize) -> Binding {
        Binding {
            name: self.binding_name(index),
            value: self.binding_values.get(index),
            strict: self.binding_is_strict(index),
            mutable: self.binding_is_mutable(index),
            can_be_deleted: self.binding_can_be_deleted(index),
            initialized: self.binding_is_initialized(index),
        }
    }

    fn binding_name(&self, index: usize) -> Utf16FlyString {
        if let Some(shape) = self.shape.get()
            && index < shape.size()
        {
            return shape.binding_name(index).clone();
        }
        let rare_data = self
            .rare_data()
            .expect("bindings outside the shape are in the rare data");
        rare_data.storage.binding_names.borrow()[self.local_binding_index(index)].clone()
    }

    fn binding_flags(&self, index: usize) -> u8 {
        if let Some(shape) = self.shape.get()
            && index < shape.size()
        {
            return shape.binding_flags(index);
        }
        let rare_data = self
            .rare_data()
            .expect("bindings outside the shape are in the rare data");
        rare_data.binding_flags.get(self.local_binding_index(index))
    }

    fn binding_is_strict(&self, index: usize) -> bool {
        (self.binding_flags(index) & Self::BINDING_FLAG_STRICT) != 0
    }

    fn binding_is_mutable(&self, index: usize) -> bool {
        (self.binding_flags(index) & Self::BINDING_FLAG_MUTABLE) != 0
    }

    fn binding_can_be_deleted(&self, index: usize) -> bool {
        (self.binding_flags(index) & Self::BINDING_FLAG_CAN_BE_DELETED) != 0
    }

    fn binding_is_initialized(&self, index: usize) -> bool {
        !self.binding_values.get(index).is_empty()
    }

    fn binding_is_deleted(&self, index: usize) -> bool {
        self.rare_data().is_some_and(|rare_data| {
            let deleted_bindings = rare_data.storage.deleted_bindings.borrow();
            !deleted_bindings.is_null() && deleted_bindings.get(index)
        })
    }

    pub fn set_environment_shape_cache(&self, cache: EnvironmentShapeCache, expected_binding_count: usize) {
        if expected_binding_count == 0 {
            return;
        }

        if let Some(shape) = cache.shape() {
            assert!(shape.size() == expected_binding_count);
            self.set_environment_shape(shape);
            return;
        }

        let rare_data = self.ensure_rare_data();
        rare_data.storage.environment_shape_cache.set(Some(cache));
        rare_data.storage.expected_binding_count.set(expected_binding_count);
    }

    fn set_environment_shape(&self, shape: Gc<EnvironmentShape>) {
        assert!(self.shape.get().is_none());
        assert!(self.binding_values.size() <= shape.size());

        self.shape.set(Some(shape));
        if let Some(rare_data) = self.rare_data() {
            rare_data.storage.binding_names.replace(Vec::new());
            rare_data.binding_flags.clear();
            rare_data.storage.bindings_assoc.replace(HashMap::default());
            rare_data.storage.environment_shape_cache.set(None);
            rare_data.storage.expected_binding_count.set(0);
            self.drop_rare_data_if_empty();
        }
    }

    fn maybe_finalize_environment_shape(&self, vm: &Vm) {
        let Some(rare_data) = self.rare_data() else {
            return;
        };
        let Some(cache) = rare_data.storage.environment_shape_cache.get() else {
            return;
        };
        let expected_binding_count = rare_data.storage.expected_binding_count.get();
        if self.shape.get().is_some() || expected_binding_count == 0 || self.binding_count() != expected_binding_count {
            return;
        }

        if let Some(shape) = cache.shape() {
            assert!(shape.size() == self.binding_count());
            {
                let binding_names = rare_data.storage.binding_names.borrow();
                for (index, binding_name) in binding_names.iter().enumerate().take(self.binding_count()) {
                    assert!(shape.binding_name(index) == binding_name);
                    assert!(shape.binding_flags(index) == rare_data.binding_flags.get(index));
                }
            }
            self.set_environment_shape(shape);
            return;
        }

        // The names and flags are copied out, since creating the shape allocates.
        let binding_names = rare_data.storage.binding_names.borrow().clone();
        let binding_flags = rare_data.binding_flags.to_vec();
        let shape = EnvironmentShape::create(vm, &binding_names, &binding_flags);
        cache.set_shape(shape);
        self.set_environment_shape(shape);
    }

    /// Finds a binding the way the class of the environment does; ModuleEnvironment overrides this.
    pub(crate) fn find_binding_and_index(&self, name: &Utf16FlyString) -> Option<BindingAndIndex> {
        if self.class().id == ClassId::ModuleEnvironment {
            let module_environment = self
                .downcast_ref::<ModuleEnvironment>()
                .expect("the environment is a module environment");
            return module_environment.find_binding_and_index(name);
        }
        self.declarative_find_binding_and_index(name)
    }

    /// DeclarativeEnvironment::find_binding_and_index itself, for the classes that override it.
    pub(crate) fn declarative_find_binding_and_index(&self, name: &Utf16FlyString) -> Option<BindingAndIndex> {
        if let Some(shape) = self.shape.get()
            && let Some(index) = shape.find_binding(name)
            && index < self.binding_count()
            && !self.binding_is_deleted(index)
        {
            return Some(BindingAndIndex::Index(index));
        }

        if let Some(rare_data) = self.rare_data()
            && let Some(&index) = rare_data.storage.bindings_assoc.borrow().get(name)
        {
            return Some(BindingAndIndex::Index(index));
        }

        None
    }

    // 9.1.1.1.1 HasBinding ( N ), https://tc39.es/ecma262/#sec-declarative-environment-records-hasbinding-n
    pub fn has_binding(&self, name: &Utf16FlyString, out_index: Option<&mut Option<usize>>) -> ThrowCompletionOr<bool> {
        let Some(binding_and_index) = self.find_binding_and_index(name) else {
            return Ok(false);
        };
        if !self.is_permanently_screwed_by_eval()
            && let Some(out_index) = out_index
            && let Some(index) = binding_and_index.index()
        {
            *out_index = Some(index);
        }
        Ok(true)
    }

    // 9.1.1.1.2 CreateMutableBinding ( N, D ), https://tc39.es/ecma262/#sec-declarative-environment-records-createmutablebinding-n-d
    pub fn create_mutable_binding(
        &self,
        vm: &Vm,
        name: &Utf16FlyString,
        can_be_deleted: bool,
    ) -> ThrowCompletionOr<()> {
        // 1. Assert: envRec does not already have a binding for N.
        // NOTE: We skip this to avoid O(n) traversal of m_binding_names.

        // 2. Create a mutable binding in envRec for N and record that it is uninitialized. If D is true, record that the newly created binding may be deleted by a subsequent DeleteBinding call.
        self.append_binding(Binding {
            name: name.clone(),
            value: Value::UNDEFINED,
            strict: false,
            mutable: true,
            can_be_deleted,
            initialized: false,
        });
        self.maybe_finalize_environment_shape(vm);

        self.increment_environment_serial_number();

        // 3. Return unused.
        Ok(())
    }

    // 9.1.1.1.3 CreateImmutableBinding ( N, S ), https://tc39.es/ecma262/#sec-declarative-environment-records-createimmutablebinding-n-s
    pub fn create_immutable_binding(&self, vm: &Vm, name: &Utf16FlyString, strict: bool) -> ThrowCompletionOr<()> {
        // 1. Assert: envRec does not already have a binding for N.
        // NOTE: We skip this to avoid O(n) traversal of m_binding_names.

        // 2. Create an immutable binding in envRec for N and record that it is uninitialized. If S is true, record that the newly created binding is a strict binding.
        self.append_binding(Binding {
            name: name.clone(),
            value: Value::UNDEFINED,
            strict,
            mutable: false,
            can_be_deleted: false,
            initialized: false,
        });
        self.maybe_finalize_environment_shape(vm);

        self.increment_environment_serial_number();

        // 3. Return unused.
        Ok(())
    }

    // 9.1.1.1.4 InitializeBinding ( N, V ), https://tc39.es/ecma262/#sec-declarative-environment-records-initializebinding-n-v
    // 4.1.1.1.1 InitializeBinding ( N, V, hint ), https://tc39.es/proposal-explicit-resource-management/#sec-declarative-environment-records
    pub fn initialize_binding(
        &self,
        vm: &Vm,
        name: &Utf16FlyString,
        value: Value,
        hint: InitializeBindingHint,
    ) -> ThrowCompletionOr<()> {
        let index = self
            .find_binding_and_index(name)
            .and_then(|binding_and_index| binding_and_index.index())
            .expect("InitializeBinding is only performed on a binding of the environment itself");
        self.initialize_binding_direct(vm, index, value, hint)
    }

    pub fn initialize_binding_direct(
        &self,
        vm: &Vm,
        index: usize,
        value: Value,
        hint: InitializeBindingHint,
    ) -> ThrowCompletionOr<()> {
        // 1. Assert: envRec must have an uninitialized binding for N.
        assert!(!self.binding_is_initialized(index));
        assert!(!value.is_empty());

        // 2. If hint is not normal, perform ? AddDisposableResource(envRec.[[DisposeCapability]], V, hint).
        if hint != InitializeBindingHint::Normal {
            add_disposable_resource(vm, self.dispose_capability(), value, hint, None)?;
        }

        // 3. Set the bound value for N in envRec to V.
        self.binding_values.set(index, value);

        // 5. Return unused.
        Ok(())
    }

    // 9.1.1.1.5 SetMutableBinding ( N, V, S ), https://tc39.es/ecma262/#sec-declarative-environment-records-setmutablebinding-n-v-s
    pub fn set_mutable_binding(
        &self,
        vm: &Vm,
        name: &Utf16FlyString,
        value: Value,
        strict: bool,
    ) -> ThrowCompletionOr<()> {
        // 1. If envRec does not have a binding for N, then
        let Some(binding_and_index) = self.find_binding_and_index(name) else {
            // a. If S is true, throw a ReferenceError exception.
            if strict {
                return vm.throw_completion(ErrorKind::ReferenceError, ErrorType::UnknownIdentifier, &[name]);
            }

            // b. Perform ! envRec.CreateMutableBinding(N, true).
            self.create_mutable_binding(vm, name, true)
                .expect("CreateMutableBinding cannot fail");

            // c. Perform ! envRec.InitializeBinding(N, V, normal).
            self.initialize_binding(vm, name, value, InitializeBindingHint::Normal)
                .expect("a normal InitializeBinding cannot fail");

            // d. Return unused.
            return Ok(());
        };

        // 2-5. (extracted into a non-standard function below)
        if let Some(index) = binding_and_index.index() {
            self.set_mutable_binding_direct(vm, index, value, strict)?;
        } else {
            let mut binding = binding_and_index.binding(self);
            self.set_mutable_binding_of_binding(vm, &mut binding, value, strict)?;
        }

        // 6. Return unused.
        Ok(())
    }

    pub fn set_mutable_binding_direct(
        &self,
        vm: &Vm,
        index: usize,
        value: Value,
        strict: bool,
    ) -> ThrowCompletionOr<()> {
        let strict = strict || self.binding_is_strict(index);

        if !self.binding_is_initialized(index) {
            return vm.throw_completion(
                ErrorKind::ReferenceError,
                ErrorType::BindingNotInitialized,
                &[&self.binding_name(index)],
            );
        }

        if self.binding_is_mutable(index) {
            self.binding_values.set(index, value);
        } else if strict {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::InvalidAssignToConst, &[]);
        }

        Ok(())
    }

    /// set_mutable_binding_direct for a binding found outside the environment's own storage.
    fn set_mutable_binding_of_binding(
        &self,
        vm: &Vm,
        binding: &mut Binding,
        value: Value,
        strict: bool,
    ) -> ThrowCompletionOr<()> {
        let strict = strict || binding.strict;

        if !binding.initialized {
            return vm.throw_completion(
                ErrorKind::ReferenceError,
                ErrorType::BindingNotInitialized,
                &[&binding.name],
            );
        }

        if binding.mutable {
            binding.value = value;
        } else if strict {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::InvalidAssignToConst, &[]);
        }

        Ok(())
    }

    // 9.1.1.1.6 GetBindingValue ( N, S ), https://tc39.es/ecma262/#sec-declarative-environment-records-getbindingvalue-n-s
    pub fn get_binding_value(&self, vm: &Vm, name: &Utf16FlyString, _strict: bool) -> ThrowCompletionOr<Value> {
        // 1. Assert: envRec has a binding for N.
        let binding_and_index = self
            .find_binding_and_index(name)
            .expect("GetBindingValue is only performed on a binding the environment has");

        // 2-3. (extracted into a non-standard function below)
        if let Some(index) = binding_and_index.index() {
            return self.get_binding_value_direct(vm, index);
        }

        let binding = binding_and_index.binding(self);
        self.get_binding_value_of_binding(vm, &binding)
    }

    pub fn get_binding_value_direct(&self, vm: &Vm, index: usize) -> ThrowCompletionOr<Value> {
        if !self.binding_is_initialized(index) {
            return vm.throw_completion(
                ErrorKind::ReferenceError,
                ErrorType::BindingNotInitialized,
                &[&self.binding_name(index)],
            );
        }

        Ok(self.binding_values.get(index))
    }

    pub fn get_initialized_binding_value_direct(&self, index: usize) -> Value {
        self.binding_values.get(index)
    }

    /// get_binding_value_direct for a binding found outside the environment's own storage.
    fn get_binding_value_of_binding(&self, vm: &Vm, binding: &Binding) -> ThrowCompletionOr<Value> {
        // 2. If the binding for N in envRec is an uninitialized binding, throw a ReferenceError exception.
        if !binding.initialized {
            return vm.throw_completion(
                ErrorKind::ReferenceError,
                ErrorType::BindingNotInitialized,
                &[&binding.name],
            );
        }

        // 3. Return the value currently bound to N in envRec.
        Ok(binding.value)
    }

    // 9.1.1.1.7 DeleteBinding ( N ), https://tc39.es/ecma262/#sec-declarative-environment-records-deletebinding-n
    pub fn delete_binding(&self, _vm: &Vm, name: &Utf16FlyString) -> ThrowCompletionOr<bool> {
        // 1. Assert: envRec has a binding for the name that is the value of N.
        let binding_and_index = self
            .find_binding_and_index(name)
            .expect("DeleteBinding is only performed on a binding the environment has");

        // 2. If the binding for N in envRec cannot be deleted, return false.
        let Some(index) = binding_and_index.index() else {
            if !binding_and_index.binding(self).can_be_deleted {
                return Ok(false);
            }
            unreachable!("only bindings of the environment itself can be deleted");
        };

        if !self.binding_can_be_deleted(index) {
            return Ok(false);
        }

        // 3. Remove the binding for N from envRec.
        // NOTE: We keep the entry in the parallel vectors to avoid disturbing indices.
        self.clear_binding(name, index);

        self.increment_environment_serial_number();

        // 4. Return true.
        Ok(true)
    }

    /// This is not a method defined in the spec! Do not use this in any LibJS (or other spec related) code.
    pub fn bindings(&self) -> Vec<Utf16FlyString> {
        let mut names = Vec::with_capacity(self.binding_count());

        let shape_bindings_to_visit = self.shape_binding_count().min(self.binding_count());
        if let Some(shape) = self.shape.get() {
            for index in 0..shape_bindings_to_visit {
                let name = shape.binding_name(index);
                if !self.binding_is_deleted(index) && !name.is_empty() {
                    names.push(name.clone());
                }
            }
        }

        if let Some(rare_data) = self.rare_data() {
            for name in rare_data.storage.binding_names.borrow().iter() {
                if !name.is_empty() {
                    names.push(name.clone());
                }
            }
        }

        names
    }

    pub fn binding_is_mutable_by_name(&self, name: &Utf16FlyString) -> bool {
        let binding = self
            .find_binding_and_index(name)
            .expect("the environment has a binding for the name");
        binding.binding(self).mutable
    }

    pub fn binding_index(&self, name: &Utf16FlyString) -> Option<usize> {
        self.find_binding_and_index(name)?.index()
    }

    /// Creates the binding the environment's shape has next, with Value::EMPTY, if the shape names it `name` with
    /// `flags` and has no other binding of that name, and returns whether it did. The binding is then the one
    /// CreateMutableBinding or CreateImmutableBinding would create, and HasBinding(name) would have returned false
    /// before. This is how the bindings of an environment that has its final shape from the start get created.
    pub fn create_next_binding_of_shape(&self, name: &Utf16FlyString, flags: u8) -> bool {
        let Some(shape) = self.shape.get() else {
            return false;
        };
        let index = self.binding_values.size();
        if !shape.has_unique_binding_names
            || index >= shape.size()
            || shape.binding_name(index) != name
            || shape.binding_flags(index) != flags
            || self.class().id == ClassId::ModuleEnvironment
        {
            return false;
        }
        self.binding_values.append(Value::EMPTY);
        true
    }

    pub fn ensure_capacity(&self, needed_capacity: usize) {
        self.binding_values.ensure_capacity(needed_capacity);
        if self.shape.get().is_some() || needed_capacity == 0 {
            return;
        }

        let rare_data = self.ensure_rare_data();
        {
            let mut binding_names = rare_data.storage.binding_names.borrow_mut();
            let additional_capacity = needed_capacity.saturating_sub(binding_names.len());
            binding_names.reserve_exact(additional_capacity);
        }
        rare_data.binding_flags.ensure_capacity(needed_capacity);
    }

    pub fn shrink_to_fit(&self) {
        self.binding_values.shrink_to_fit();

        let Some(rare_data) = self.rare_data() else {
            return;
        };

        rare_data.storage.binding_names.borrow_mut().shrink_to_fit();
        rare_data.binding_flags.shrink_to_fit();

        if self.binding_values.is_empty() {
            rare_data.storage.deleted_bindings.replace(Bitmap::default());
            self.drop_rare_data_if_empty();
            return;
        }

        if rare_data.storage.deleted_bindings.borrow().is_null() {
            self.drop_rare_data_if_empty();
            return;
        }

        let mut deleted_bindings = Bitmap::create(self.binding_values.size(), false);
        for index in 0..self.binding_values.size() {
            deleted_bindings.set(index, self.binding_is_deleted(index));
        }
        rare_data.storage.deleted_bindings.replace(deleted_bindings);
        self.drop_rare_data_if_empty();
    }
}
