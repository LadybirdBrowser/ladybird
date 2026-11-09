/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, OnceCell};
use core::ops::{ControlFlow, Deref};
use core::ptr::NonNull;
use std::rc::Rc;

use ak::{Utf16FlyString, Utf16String};
use libjs_runtime_macros::Trace;

use crate::bytecode::executable::Executable;
use crate::gc::class::{Extends, GcCell, cast_ref, define_cell};
use crate::gc::defer_gc::{DeferGc, Deferred};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::root::MarkedVec;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::run::should_dump_bytecode;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::environment::{Environment, PrivateEnvironment};
use crate::layout::execution_context::{ExecutionContext, ScriptOrModule};
pub use crate::layout::function_object::{EcmascriptFunctionObject, EcmascriptFunctionObjectWithRareData};
use crate::layout::function_object::{RawNativeFunction, ecmascript_function_flags};
use crate::layout::member::NullableMember;
use crate::layout::object::{IndexedStorageKind, Object};
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    create_unmapped_arguments_object, get_prototype_from_constructor, new_call_environment,
};
use crate::runtime::async_function_driver_wrapper::AsyncFunctionDriverWrapper;
use crate::runtime::async_generator::AsyncGenerator;
use crate::runtime::class_field_definition::{ClassElementName, ClassFieldDefinition};
use crate::runtime::completion::{Must, Throw, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_environment::FunctionEnvironment;
use crate::runtime::function_object::{FUNCTION_OBJECT_METHODS, FunctionObject};
use crate::runtime::function_prototype::FunctionPrototype;
use crate::runtime::generator_object::{GeneratingFunction, GeneratorObject};
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::object::{
    MayInterfereWithIndexedPropertyAccess, ObjectMethods, PrivateElement, StackFrameInfo, allocate_object,
};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::reflect_object::ReflectObject;
use crate::runtime::shape::Shape;
use crate::runtime::shared_function_instance_data::{
    ClassFieldInitializerName, ClosureCount, ConstructorKind, FunctionKind, SharedFunctionInstanceData, ThisMode,
};
use crate::source_code::SourceCode;
use crate::utf16::to_utf16_fly_string;

#[derive(Default, Trace)]
struct ClassData {
    fields: Vec<ClassFieldDefinition>,    // [[Fields]]
    private_methods: Vec<PrivateElement>, // [[PrivateMethods]]
    /// The [[ClassFieldInitializerName]] of an initializer of a field whose key is computed.
    class_field_initializer_name: ClassFieldInitializerName,
}

/// The parts of a class constructor, or of a field initializer, that the interpreter does not read.
#[derive(Default)]
pub struct EcmascriptFunctionObjectStorage {
    class_data: OnceCell<Box<GcRefCell<ClassData>>>,
}

// NB: Every ECMAScript function has these bytes, so keep what only some functions need in the rare data.
const _: () = assert!(size_of::<EcmascriptFunctionObject>() == 32);

// SAFETY: Visits the cells of the class data, if there is any.
unsafe impl Trace for EcmascriptFunctionObjectStorage {
    fn trace(&self, visitor: &mut Visitor) {
        if let Some(class_data) = self.class_data.get() {
            class_data.trace(visitor);
        }
    }
}

pub static ECMASCRIPT_FUNCTION_OBJECT_METHODS: ObjectMethods = ObjectMethods {
    internal_get_own_property: EcmascriptFunctionObject::internal_get_own_property,
    // NB: Functions answer for "length", "name" and "prototype" before creating them lazily, whatever their shape
    //     holds. They answer for other names like ordinary objects.
    is_cacheable_for_absence_of: |_, vm, property_key| {
        *property_key != vm.names.prototype && *property_key != vm.names.length && *property_key != vm.names.name
    },
    // NB: The non-enumerable lazy properties shadow enumerable ones of the prototype chain.
    eligible_for_own_property_enumeration_fast_path: |object| {
        !as_ecmascript_function(object).has_flag(
            ecmascript_function_flags::HAS_LAZY_LENGTH_AND_NAME
                | ecmascript_function_flags::MAY_NEED_LAZY_PROTOTYPE_INSTANTIATION,
        )
    },
    internal_own_property_keys: EcmascriptFunctionObject::internal_own_property_keys,
    internal_call: Some(EcmascriptFunctionObject::internal_call),
    internal_construct: Some(EcmascriptFunctionObject::internal_construct),
    has_constructor: |object| as_ecmascript_function(object).has_constructor(),
    is_strict_mode: |object| as_ecmascript_function(object).shared_data().strict(),
    get_stack_frame_info: EcmascriptFunctionObject::get_stack_frame_info,
    function_realm: |object| Some(object.shape().realm()),
    name_for_call_stack: |object| as_ecmascript_function(object).name_for_call_stack(),
    ..FUNCTION_OBJECT_METHODS
};

define_cell!(
    EcmascriptFunctionObject,
    Object,
    extends: [FunctionObject, Object],
    methods: ECMASCRIPT_FUNCTION_OBJECT_METHODS
);

// SAFETY: Visits every cell an ECMAScript function object holds, besides those of its FunctionObject.
unsafe impl Trace for EcmascriptFunctionObject {
    fn trace(&self, visitor: &mut Visitor) {
        self.base.trace(visitor);
        self.fields().environment.trace(visitor);
        visitor.visit_nullable_member(&self.fields().shared_data);
    }
}

define_cell!(
    EcmascriptFunctionObjectWithRareData,
    Object,
    extends: [EcmascriptFunctionObject, FunctionObject, Object],
    methods: ECMASCRIPT_FUNCTION_OBJECT_METHODS
);

// SAFETY: Visits every cell the rare data of an ECMAScript function object holds, besides those of the function.
unsafe impl Trace for EcmascriptFunctionObjectWithRareData {
    fn trace(&self, visitor: &mut Visitor) {
        self.base.trace(visitor);
        self.private_environment.trace(visitor);
        self.home_object.trace(visitor);
        self.storage.trace(visitor);
    }
}

impl Deref for EcmascriptFunctionObjectWithRareData {
    type Target = EcmascriptFunctionObject;

    fn deref(&self) -> &EcmascriptFunctionObject {
        &self.base
    }
}

impl Deref for EcmascriptFunctionObject {
    type Target = FunctionObject;

    fn deref(&self) -> &FunctionObject {
        &self.base
    }
}

/// The function an internal method of an ECMAScript function object was called on.
fn as_ecmascript_function(object: &Object) -> &EcmascriptFunctionObject {
    cast_ref(object)
}

/// The object as an ECMAScript function object, if it is one.
pub fn as_ecmascript_function_object<T: Extends<Object>>(cell: Gc<T>) -> Option<Gc<EcmascriptFunctionObject>> {
    cell.upcast::<Object>().downcast()
}

/// Value::as_if<ECMAScriptFunctionObject>().
pub fn value_as_ecmascript_function_object(value: Value) -> Option<Gc<EcmascriptFunctionObject>> {
    if !value.is_object() {
        return None;
    }
    as_ecmascript_function_object(value.as_object())
}

fn performs_prepare_for_tail_call(vm: &Vm, function: Gc<FunctionObject>) -> bool {
    let Some(function) = function.upcast::<Object>().downcast::<RawNativeFunction>() else {
        return false;
    };
    FunctionPrototype::is_call_function(vm, &function)
        || FunctionPrototype::is_apply_function(vm, &function)
        || ReflectObject::is_apply_function(vm, &function)
}

fn prototype_for_function_kind(realm: Gc<Realm>, kind: FunctionKind) -> Gc<Object> {
    match kind {
        FunctionKind::Normal => realm.function_prototype(),
        FunctionKind::Generator => realm.generator_function_prototype(),
        FunctionKind::Async => realm.async_function_prototype(),
        FunctionKind::AsyncGenerator => realm.async_generator_function_prototype(),
    }
}

impl EcmascriptFunctionObject {
    pub fn create_from_function_data_with_prototype(
        vm: &Vm,
        realm: Gc<Realm>,
        shared_data: Gc<SharedFunctionInstanceData>,
        parent_environment: Option<Gc<Environment>>,
        private_environment: Option<Gc<PrivateEnvironment>>,
        prototype: Gc<Object>,
    ) -> Gc<EcmascriptFunctionObject> {
        Self::create(
            vm,
            realm,
            shared_data,
            parent_environment,
            private_environment,
            None,
            prototype,
        )
    }

    /// A function that is a method of `home_object` (see make_method()).
    pub fn create_method(
        vm: &Vm,
        realm: Gc<Realm>,
        shared_data: Gc<SharedFunctionInstanceData>,
        parent_environment: Option<Gc<Environment>>,
        private_environment: Option<Gc<PrivateEnvironment>>,
        home_object: Gc<Object>,
        prototype: Gc<Object>,
    ) -> Gc<EcmascriptFunctionObject> {
        Self::create(
            vm,
            realm,
            shared_data,
            parent_environment,
            private_environment,
            Some(home_object),
            prototype,
        )
    }

    fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        shared_data: Gc<SharedFunctionInstanceData>,
        parent_environment: Option<Gc<Environment>>,
        private_environment: Option<Gc<PrivateEnvironment>>,
        home_object: Option<Gc<Object>>,
        prototype: Gc<Object>,
    ) -> Gc<EcmascriptFunctionObject> {
        // 15. Set F.[[ScriptOrModule]] to GetActiveScriptOrModule().
        // OPTIMIZATION: Every function with the same shared data keeps the one of the first: the code that creates a
        //               function belongs to the script or module of the function. Only code with no script or module
        //               of its own, like an event handler, could see different ones.
        if shared_data.closure_count() == ClosureCount::None {
            shared_data.script_or_module.set(vm.get_active_script_or_module());
        }
        shared_data.note_closure_created();
        let is_method = home_object.is_some();
        let function = if private_environment.is_some() || is_method || shared_data.is_class_constructor() {
            let function = allocate_object(vm, |defer_gc| EcmascriptFunctionObjectWithRareData {
                base: Self::new(
                    defer_gc,
                    vm,
                    realm,
                    shared_data,
                    parent_environment,
                    prototype,
                    is_method,
                ),
                private_environment: NullableMember::new(private_environment),
                home_object: NullableMember::new(home_object),
                storage: EcmascriptFunctionObjectStorage::default(),
            });
            function.set_flag(ecmascript_function_flags::HAS_RARE_DATA, true);
            function.upcast()
        } else {
            allocate_object(vm, |defer_gc| {
                Self::new(
                    defer_gc,
                    vm,
                    realm,
                    shared_data,
                    parent_environment,
                    prototype,
                    is_method,
                )
            })
        };
        function.initialize(vm, realm);
        if is_method {
            function.make_method();
        }
        function
    }

    /// The function's rare data, if it is a function that has any.
    fn with_rare_data(&self) -> Option<&EcmascriptFunctionObjectWithRareData> {
        if !self.has_flag(ecmascript_function_flags::HAS_RARE_DATA) {
            return None;
        }
        // SAFETY: Only an EcmascriptFunctionObjectWithRareData has this flag.
        Some(unsafe { &*core::ptr::from_ref(self).cast::<EcmascriptFunctionObjectWithRareData>() })
    }

    pub fn create_from_function_data(
        vm: &Vm,
        realm: Gc<Realm>,
        shared_data: Gc<SharedFunctionInstanceData>,
        parent_environment: Option<Gc<Environment>>,
        private_environment: Option<Gc<PrivateEnvironment>>,
    ) -> Gc<EcmascriptFunctionObject> {
        let prototype = prototype_for_function_kind(realm, shared_data.kind());
        Self::create_from_function_data_with_prototype(
            vm,
            realm,
            shared_data,
            parent_environment,
            private_environment,
            prototype,
        )
    }

    fn new(
        _: &DeferGc<'_>,
        vm: &Vm,
        realm: Gc<Realm>,
        shared_data: Gc<SharedFunctionInstanceData>,
        parent_environment: Option<Gc<Environment>>,
        prototype: Gc<Object>,
        is_method: bool,
    ) -> Self {
        // OPTIMIZATION: Start from a premade shape: normal and async functions create their properties lazily, and
        //               the other kinds already have theirs in spec order.
        let mut shape = Self::premade_shape(realm, shared_data, is_method);
        // NB: The function's [[Realm]] is the realm of its shape, so the shape comes from the function's realm even when
        //     the prototype belongs to another one.
        if shape.prototype() != Some(prototype) {
            shape = shape.create_prototype_transition(vm, Some(prototype));
        }
        let base = FunctionObject::new_with_shape(Self::CLASS, shape, MayInterfereWithIndexedPropertyAccess::No);
        base.set_is_ecmascript_function_object();
        base.class_bits.set(ecmascript_function_flags::IS_ECMASCRIPT_FUNCTION);

        let function = Self { base };
        function.fields().shared_data.set(Some(shared_data));
        function.fields().environment.set(parent_environment);
        function
    }

    /// The premade shape that functions with `shared_data` start from. Normal functions that are constructors have a
    /// "prototype" property, which most of them make lazily, so they start from a shape of their own: property lookup
    /// caches must not apply what they saw on an arrow function or a method, which has no "prototype" property, to
    /// them.
    fn premade_shape(realm: Gc<Realm>, shared_data: Gc<SharedFunctionInstanceData>, is_method: bool) -> Gc<Shape> {
        match shared_data.kind() {
            FunctionKind::Normal if Self::has_constructor_for(shared_data, is_method) => {
                realm.normal_constructor_function_shape()
            }
            FunctionKind::Normal => realm.normal_function_shape(),
            FunctionKind::Generator => realm.generator_function_shape(),
            FunctionKind::Async => realm.async_function_shape(),
            FunctionKind::AsyncGenerator => realm.async_generator_function_shape(),
        }
    }

    fn initialize(&self, vm: &Vm, realm: Gc<Realm>) {
        // Note: The ordering of these properties must be: length, name, prototype which is the order
        //       they are defined in the spec: https://tc39.es/ecma262/#sec-function-instances .
        //       This is observable through something like: https://tc39.es/ecma262/#sec-ordinaryownpropertykeys
        //       which must give the properties in chronological order which in this case is the order they
        //       are defined in the spec.

        match self.kind() {
            FunctionKind::Normal => {
                self.make_length_and_name_lazy();
                // NB: Class constructors get their "prototype" property from ClassDefinitionEvaluation.
                if !self.is_arrow_function() && !self.is_class_constructor() {
                    self.set_flag(ecmascript_function_flags::MAY_NEED_LAZY_PROTOTYPE_INSTANTIATION, true);
                }
            }
            FunctionKind::Generator => {
                self.initialize_length_and_name(vm, realm);
                // prototype is "g1.prototype" in figure-2 (https://tc39.es/ecma262/img/figure-2.png)
                self.initialize_direct(
                    realm.generator_function_prototype_property_offset(),
                    Value::from_object(Object::create_prototype(
                        vm,
                        realm,
                        Some(realm.generator_function_prototype_prototype()),
                    )),
                );
            }
            FunctionKind::Async => {
                // 27.7.4 AsyncFunction Instances, https://tc39.es/ecma262/#sec-async-function-instances
                // AsyncFunction instances do not have a prototype property as they are not constructible.
                self.make_length_and_name_lazy();
            }
            FunctionKind::AsyncGenerator => {
                self.initialize_length_and_name(vm, realm);
                self.initialize_direct(
                    realm.generator_function_prototype_property_offset(),
                    Value::from_object(Object::create_prototype(
                        vm,
                        realm,
                        Some(realm.async_generator_function_prototype_prototype()),
                    )),
                );
            }
        }
    }

    /// NOTE: The constructor gave us a premade shape with "length" and "name" (and "prototype" for generator kinds) at
    ///       these offsets, with the attributes the spec requires, so we only have to store the values.
    fn initialize_length_and_name(&self, vm: &Vm, realm: Gc<Realm>) {
        let name_string = PrimitiveString::create_from_fly_string(vm, &self.name());
        self.initialize_direct(
            realm.generator_function_length_offset(),
            Value::from_i32(self.function_length()),
        );
        self.initialize_direct(realm.generator_function_name_offset(), Value::from_string(name_string));
    }

    /// OPTIMIZATION: Few functions ever have their "length" or "name" looked at, so they make them when that happens.
    fn make_length_and_name_lazy(&self) {
        self.set_flag(ecmascript_function_flags::HAS_LAZY_LENGTH_AND_NAME, true);
    }

    /// Gives the function its "length" and "name" properties if it does not have them yet, as its first properties,
    /// since property keys come in the order they were added.
    #[inline(always)]
    pub(crate) fn reify_length_and_name(&self, vm: &Vm) {
        if self.has_flag(ecmascript_function_flags::HAS_LAZY_LENGTH_AND_NAME) {
            self.reify_lazy_length_and_name(vm);
        }
    }

    #[cold]
    fn reify_lazy_length_and_name(&self, vm: &Vm) {
        let name_string = PrimitiveString::create_from_fly_string(vm, &self.name());
        self.reify_lazy_length_and_name_with_name(vm, name_string);
    }

    fn reify_lazy_length_and_name_with_name(&self, vm: &Vm, name_string: Gc<PrimitiveString>) {
        self.set_flag(ecmascript_function_flags::HAS_LAZY_LENGTH_AND_NAME, false);

        let attributes = PropertyAttributes::new(Attribute::CONFIGURABLE);
        self.insert_properties(
            vm,
            0,
            &[(&vm.names.length, attributes), (&vm.names.name, attributes)],
            &[Value::from_i32(self.function_length()), Value::from_string(name_string)],
        );
    }

    /// OPTIMIZATION: Few normal functions are ever used as constructors, so they make their "prototype" property when
    ///               something looks for it.
    ///
    /// Gives the function its "prototype" property if it may not have it yet, right after "length" and "name", where
    /// the spec creates it.
    #[inline(always)]
    fn instantiate_lazy_prototype_if_needed(&self, vm: &Vm) {
        if self.has_flag(ecmascript_function_flags::MAY_NEED_LAZY_PROTOTYPE_INSTANTIATION) {
            self.instantiate_lazy_prototype(vm);
        }
    }

    fn instantiate_lazy_prototype(&self, vm: &Vm) {
        self.set_flag(ecmascript_function_flags::MAY_NEED_LAZY_PROTOTYPE_INSTANTIATION, false);

        let prototype = self.create_prototype_object(vm);
        let attributes = PropertyAttributes::new(Attribute::WRITABLE);
        // NB: Lazy "length" and "name" are put in front of all other properties when they are made.
        let index = if self.has_flag(ecmascript_function_flags::HAS_LAZY_LENGTH_AND_NAME) {
            0
        } else {
            let shape = self.shape();
            debug_assert!(
                shape
                    .lookup(&vm.names.length)
                    .is_some_and(|metadata| metadata.offset == 0)
                    && shape
                        .lookup(&vm.names.name)
                        .is_some_and(|metadata| metadata.offset == 1),
                "a function with a lazy prototype has its length and name first"
            );
            2
        };
        if self.shape().property_count() == index {
            self.define_direct_property(vm, &vm.names.prototype, Value::from_object(prototype), attributes);
            return;
        }
        self.insert_properties(
            vm,
            index,
            &[(&vm.names.prototype, attributes)],
            &[Value::from_object(prototype)],
        );
    }

    /// Whether the function has the property `property_key` without having made it yet, which looking it up makes.
    pub(crate) fn has_lazy_property(&self, vm: &Vm, property_key: &PropertyKey) -> bool {
        (self.has_flag(ecmascript_function_flags::HAS_LAZY_LENGTH_AND_NAME)
            && (*property_key == vm.names.length || *property_key == vm.names.name))
            || (self.has_flag(ecmascript_function_flags::MAY_NEED_LAZY_PROTOTYPE_INSTANTIATION)
                && *property_key == vm.names.prototype)
    }

    pub(crate) fn reify_lazy_property(&self, vm: &Vm, property_key: &PropertyKey) {
        if self.has_flag(ecmascript_function_flags::HAS_LAZY_LENGTH_AND_NAME)
            && (*property_key == vm.names.length || *property_key == vm.names.name)
        {
            self.reify_lazy_length_and_name(vm);
        }

        if self.has_flag(ecmascript_function_flags::MAY_NEED_LAZY_PROTOTYPE_INSTANTIATION)
            && *property_key == vm.names.prototype
        {
            self.instantiate_lazy_prototype(vm);
        }
    }

    /// Gives the function all of the properties it makes lazily. This must happen before its shape changes in a way that
    /// insert_properties() cannot rebuild.
    pub(crate) fn reify_lazy_properties(&self, vm: &Vm) {
        // OPTIMIZATION: When "length", "name" and "prototype" are all lazy, one rebuild of the shape makes them.
        if self.has_flag(ecmascript_function_flags::HAS_LAZY_LENGTH_AND_NAME)
            && self.has_flag(ecmascript_function_flags::MAY_NEED_LAZY_PROTOTYPE_INSTANTIATION)
        {
            self.set_flag(ecmascript_function_flags::HAS_LAZY_LENGTH_AND_NAME, false);
            self.set_flag(ecmascript_function_flags::MAY_NEED_LAZY_PROTOTYPE_INSTANTIATION, false);
            let name_string = PrimitiveString::create_from_fly_string(vm, &self.name());
            let prototype = self.create_prototype_object(vm);
            let length_and_name_attributes = PropertyAttributes::new(Attribute::CONFIGURABLE);
            self.insert_properties(
                vm,
                0,
                &[
                    (&vm.names.length, length_and_name_attributes),
                    (&vm.names.name, length_and_name_attributes),
                    (&vm.names.prototype, PropertyAttributes::new(Attribute::WRITABLE)),
                ],
                &[
                    Value::from_i32(self.function_length()),
                    Value::from_string(name_string),
                    Value::from_object(prototype),
                ],
            );
            return;
        }
        self.reify_length_and_name(vm);
        self.instantiate_lazy_prototype_if_needed(vm);
    }

    /// The object a lazy "prototype" property holds, made as MakeConstructor would make it.
    fn create_prototype_object(&self, vm: &Vm) -> Gc<Object> {
        debug_assert!(
            self.shape().lookup(&vm.names.prototype).is_none(),
            "a function with a lazy prototype does not have one yet"
        );
        let realm = self.realm().expect("an ECMAScript function has a realm");
        let prototype = Object::create_with_premade_shape(vm, realm.normal_function_prototype_shape());
        prototype.initialize_direct(
            realm.normal_function_prototype_constructor_offset(),
            Value::from_object(self.as_function_object_gc()),
        );
        prototype
    }

    /// Rebuilds the shape from the premade shape of the function's kind with the given properties in front of the one
    /// at `index` in insertion order, and gives them the given values.
    fn insert_properties(
        &self,
        vm: &Vm,
        index: u32,
        properties: &[(&PropertyKey, PropertyAttributes)],
        values: &[Value],
    ) {
        debug_assert_eq!(properties.len(), values.len());
        let old_shape = self.shape();
        // NB: Rebuilding the shape bypasses transitions away from the old one and moves existing property offsets.
        old_shape.mark_unstable(vm);
        let realm = old_shape.realm();
        let mut shape = match self.kind() {
            FunctionKind::Generator | FunctionKind::AsyncGenerator => {
                unreachable!("generator functions have all of their properties from the start")
            }
            FunctionKind::Normal | FunctionKind::Async => Self::premade_shape(
                realm,
                self.shared_data(),
                self.has_flag(ecmascript_function_flags::IS_METHOD),
            ),
        };
        if shape.prototype() != old_shape.prototype() {
            shape = shape.create_prototype_transition(vm, old_shape.prototype());
        }
        // NB: Adding the transitions can allocate, which must not happen while the shape is borrowed, so the properties
        //     are read out first.
        let property_count = old_shape.property_count() as usize;
        let property_keys = MarkedVec::with_capacity(vm, property_count);
        let mut property_attributes = Vec::with_capacity(property_count);
        old_shape.for_each_property_in_insertion_order(|property_key, metadata| {
            property_keys.push(property_key.clone());
            property_attributes.push(metadata.attributes);
            ControlFlow::Continue(())
        });
        for (position, attributes) in property_attributes.into_iter().enumerate() {
            if position == index as usize {
                for (inserted_key, inserted_attributes) in properties {
                    shape = shape.create_put_transition(vm, inserted_key, *inserted_attributes);
                }
            }
            let property_key = property_keys.get(position).expect("the position is in bounds");
            shape = shape.create_put_transition(vm, &property_key, attributes);
        }
        if index as usize == property_count {
            for (inserted_key, inserted_attributes) in properties {
                shape = shape.create_put_transition(vm, inserted_key, *inserted_attributes);
            }
        }

        self.insert_named_properties(shape, index, values);
    }

    pub fn as_ecmascript_function_gc(&self) -> Gc<EcmascriptFunctionObject> {
        Gc::from_ref(self)
    }

    /// The function's executable, compiled first if the function never ran.
    pub fn compiled_executable(&self, vm: &Vm) -> ThrowCompletionOr<Gc<Executable>> {
        let shared_data = self.shared_data();
        if let Some(executable) = shared_data.executable() {
            return Ok(executable);
        }
        let rust_executable = SharedFunctionInstanceData::compile_function(vm, shared_data, false)?;
        shared_data.set_executable(Some(rust_executable));
        rust_executable.set_name(self.name());
        if should_dump_bytecode() {
            rust_executable.dump();
        }
        shared_data.clear_compile_inputs();
        Ok(rust_executable)
    }

    fn get_stack_frame_info(object: &Object, vm: &Vm, stack_frame_info: &mut StackFrameInfo) -> ThrowCompletionOr<()> {
        let function = as_ecmascript_function(object);
        let executable = function.compiled_executable(vm)?;
        stack_frame_info.registers_and_locals_count = executable.registers_and_locals_count();
        stack_frame_info.constant_count =
            u32::try_from(executable.constants().len()).expect("the constant count fits in u32");
        stack_frame_info.argument_count = stack_frame_info.argument_count.max(function.formal_parameter_count());
        Ok(())
    }

    // 10.2.1 [[Call]] ( thisArgument, argumentsList ), https://tc39.es/ecma262/#sec-ecmascript-function-objects-call-thisargument-argumentslist
    fn internal_call(
        object: &Object,
        vm: &Vm,
        callee_context: &ExecutionContext,
        this_argument: Value,
    ) -> ThrowCompletionOr<Value> {
        let function = as_ecmascript_function(object);

        debug_assert!(function.bytecode_executable().is_some());

        // 1. Let callerContext be the running execution context.
        // NOTE: No-op, kept by the VM in its execution context stack.

        // 2. Let calleeContext be PrepareForOrdinaryCall(F, undefined).
        function.prepare_for_ordinary_call(vm, callee_context, None);

        // 3. Assert: calleeContext is now the running execution context.
        debug_assert!(vm.running_execution_context() == Some(NonNull::from(callee_context)));

        // 4. If F.[[IsClassConstructor]] is true, then
        if function.is_class_constructor() {
            // a. Let error be a newly created TypeError object.
            // b. NOTE: error is created in calleeContext with F's associated Realm Record.
            let throw_completion = vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::ClassConstructorWithoutNew,
                &[&function.name()],
            );

            // c. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
            vm.pop_execution_context();

            // d. Return ThrowCompletion(error).
            return throw_completion;
        }

        // 5. Perform OrdinaryCallBindThis(F, calleeContext, thisArgument).
        if function.uses_this() {
            function.ordinary_call_bind_this(vm, callee_context, this_argument);
        }

        // 6. Let result be Completion(OrdinaryCallEvaluateBody(F, argumentsList)).
        let result = function.ordinary_call_evaluate_body(vm, callee_context);

        // 7. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
        vm.pop_execution_context();

        // 8. If result.[[Type]] is return, return result.[[Value]].
        // 9. Assert: result is a throw completion.
        // 10. Return ? result.
        result
    }

    // 10.2.2 [[Construct]] ( argumentsList, newTarget ), https://tc39.es/ecma262/#sec-ecmascript-function-objects-construct-argumentslist-newtarget
    fn internal_construct(
        object: &Object,
        vm: &Vm,
        callee_context: &ExecutionContext,
        new_target: Gc<FunctionObject>,
    ) -> ThrowCompletionOr<Gc<Object>> {
        let function = as_ecmascript_function(object);

        debug_assert!(function.bytecode_executable().is_some());

        // 1. Let callerContext be the running execution context.
        // NOTE: No-op, kept by the VM in its execution context stack.

        // 2. Let kind be F.[[ConstructorKind]].
        let kind = function.constructor_kind();

        let mut this_argument: Option<Gc<Object>> = None;

        // 3. If kind is base, then
        if kind == ConstructorKind::Base {
            // a. Let thisArgument be ? OrdinaryCreateFromConstructor(newTarget, "%Object.prototype%").
            // NB: With room for the properties the constructor is known to add.
            let prototype = get_prototype_from_constructor(vm, new_target, Intrinsics::object_prototype)?;
            this_argument = Some(Object::create_for_construct(
                vm,
                vm.current_realm().expect("a construction runs in a realm"),
                prototype,
                function.shared_data(),
            ));
        }

        // 4. Let calleeContext be PrepareForOrdinaryCall(F, newTarget).
        function.prepare_for_ordinary_call(vm, callee_context, Some(new_target.upcast()));

        // 5. Assert: calleeContext is now the running execution context.
        debug_assert!(vm.running_execution_context() == Some(NonNull::from(callee_context)));

        // 6. If kind is base, then
        if kind == ConstructorKind::Base {
            let this_argument = this_argument.expect("a base constructor has a this argument");

            // a. Perform OrdinaryCallBindThis(F, calleeContext, thisArgument).
            if function.uses_this() {
                function.ordinary_call_bind_this(vm, callee_context, Value::from_object(this_argument));
            }

            // b. Let initializeResult be Completion(InitializeInstanceElements(thisArgument, F)).
            let initialize_result =
                this_argument.initialize_instance_elements(vm, function.as_ecmascript_function_gc());

            // c. If initializeResult is an abrupt completion, then
            if let Err(throw) = initialize_result {
                // i. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
                vm.pop_execution_context();

                // ii. Return ? initializeResult.
                return Err(throw);
            }
        }

        // 7. Let constructorEnv be the LexicalEnvironment of calleeContext.
        let constructor_env = callee_context.lexical_environment.get();

        // 8. Let result be Completion(OrdinaryCallEvaluateBody(F, argumentsList)).
        let result = function.ordinary_call_evaluate_body(vm, callee_context);

        // 9. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
        vm.pop_execution_context();

        // 10. If result is a throw completion, then
        //     a. Return ? result.
        let result = result?;

        // 11. Assert: result is a return completion.
        // NOTE: We already checked !is_error() above.

        // 12. If Type(result.[[Value]]) is Object, return result.[[Value]].
        if result.is_object() {
            return Ok(result.as_object());
        }

        // 13. If kind is base, return thisArgument.
        if kind == ConstructorKind::Base {
            return Ok(this_argument.expect("a base constructor has a this argument"));
        }

        // 14. If result.[[Value]] is not undefined, throw a TypeError exception.
        if !result.is_undefined() {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::DerivedConstructorReturningInvalidValue,
                &[],
            );
        }

        // 15. Let thisBinding be ? constructorEnv.GetThisBinding().
        let this_binding = constructor_env
            .expect("a constructor runs in an environment")
            .get_this_binding(vm)?;

        // 16. Assert: Type(thisBinding) is Object.
        debug_assert!(this_binding.is_object());

        // 17. Return thisBinding.
        Ok(this_binding.as_object())
    }

    // 10.2.7 MakeMethod ( F, homeObject ), https://tc39.es/ecma262/#sec-makemethod
    fn make_method(&self) {
        // 1. Set F.[[HomeObject]] to homeObject.
        // NB: create() did.
        self.set_flag(ecmascript_function_flags::IS_METHOD, true);
        self.set_flag(ecmascript_function_flags::MAY_NEED_LAZY_PROTOTYPE_INSTANTIATION, false);

        // 2. Return unused.
    }

    // 10.2.1.1 PrepareForOrdinaryCall ( F, newTarget ), https://tc39.es/ecma262/#sec-prepareforordinarycall
    pub fn prepare_for_ordinary_call(
        &self,
        vm: &Vm,
        callee_context: &ExecutionContext,
        new_target: Option<Gc<Object>>,
    ) {
        // 1. Let callerContext be the running execution context.
        // 2. Let calleeContext be a new ECMAScript code execution context.

        // 3. Set the Function of calleeContext to F.
        callee_context.function.set(Some(self.as_function_object_gc()));

        // 4. Let calleeRealm be F.[[Realm]].
        // 5. Set the Realm of calleeContext to calleeRealm.
        callee_context.realm.set(self.realm());

        // 6. Set the ScriptOrModule of calleeContext to F.[[ScriptOrModule]].
        callee_context.script_or_module.set(self.script_or_module());

        if self.function_environment_needed() {
            // 7. Let localEnv be NewFunctionEnvironment(F, newTarget).
            // NB: Or what the function's code needs of one.
            let local_environment = new_call_environment(vm, self.as_ecmascript_function_gc(), new_target);
            let shared_data = self.shared_data();
            let function_environment_bindings_count = shared_data.function_environment_bindings_count();
            local_environment.set_environment_shape_cache(
                shared_data.function_environment_shape_cache(),
                function_environment_bindings_count,
            );
            local_environment.ensure_capacity(function_environment_bindings_count);

            // 8. Set the LexicalEnvironment of calleeContext to localEnv.
            callee_context.lexical_environment.set(Some(local_environment.upcast()));

            // 9. Set the VariableEnvironment of calleeContext to localEnv.
            callee_context
                .variable_environment
                .set(Some(local_environment.upcast()));
        } else {
            callee_context.lexical_environment.set(self.environment());
            callee_context.variable_environment.set(self.environment());
        }

        // 10. Set the PrivateEnvironment of calleeContext to F.[[PrivateEnvironment]].
        callee_context.private_environment.set(self.private_environment());

        // 11. If callerContext is not already suspended, suspend callerContext.
        // 12. Push calleeContext onto the execution context stack; calleeContext is now the running execution context.

        // NOTE: We don't check for stack overflow here. The bytecode interpreter will do it anyway
        //       when entering the function we're about to call.
        vm.push_execution_context(NonNull::from(callee_context));

        // 13. NOTE: Any exception objects produced after this point are associated with calleeRealm.
        // 14. Return calleeContext.
        // NOTE: The caller allocated calleeContext, so there is nothing to return.
    }

    // 10.2.1.2 OrdinaryCallBindThis ( F, calleeContext, thisArgument ), https://tc39.es/ecma262/#sec-ordinarycallbindthis
    pub fn ordinary_call_bind_this(&self, vm: &Vm, callee_context: &ExecutionContext, this_argument: Value) {
        if let Some(this_value) =
            self.resolve_and_bind_this(vm, callee_context.lexical_environment.get(), this_argument)
        {
            callee_context.this_value.set(this_value);
        }
    }

    /// OrdinaryCallBindThis() for a call whose callee context has `local_env` as its LexicalEnvironment: binds and
    /// returns the this value, or returns nothing if the function's this is lexical.
    pub fn resolve_and_bind_this(
        &self,
        vm: &Vm,
        local_env: Option<Gc<Environment>>,
        this_argument: Value,
    ) -> Option<Value> {
        // 1. Let thisMode be F.[[ThisMode]].
        // If thisMode is lexical, return unused.
        if self.this_mode() == ThisMode::Lexical {
            return None;
        }

        // 3. Let calleeRealm be F.[[Realm]].
        let callee_realm = self.realm().expect("an ECMAScript function has a realm");

        // 4. Let localEnv be the LexicalEnvironment of calleeContext.
        // NB: The caller passes it.

        // 5. If thisMode is strict, let thisValue be thisArgument.
        let this_value = if self.this_mode() == ThisMode::Strict {
            this_argument
        }
        // 6. Else,
        else {
            // a. If thisArgument is undefined or null, then
            if this_argument.is_nullish() {
                // i. Let globalEnv be calleeRealm.[[GlobalEnv]].
                // ii. Assert: globalEnv is a global Environment Record.
                // iii. Let thisValue be globalEnv.[[GlobalThisValue]].
                Value::from_object(callee_realm.global_environment().global_this_value())
            }
            // b. Else,
            else {
                // i. Let thisValue be ! ToObject(thisArgument).
                let this_value = Value::from_object(this_argument.to_object(vm).must());

                // ii. NOTE: ToObject produces wrapper objects using calleeRealm.
                debug_assert!(vm.current_realm() == Some(callee_realm));

                this_value
            }
        };

        // 7. Assert: localEnv is a function Environment Record.
        // 8. Assert: The next step never returns an abrupt completion because localEnv.[[ThisBindingStatus]] is not initialized.
        // 9. Perform ! localEnv.BindThisValue(thisValue).
        if self.shared_data().function_environment_has_this_binding() {
            local_env
                .and_then(|environment| environment.downcast::<FunctionEnvironment>())
                .expect("the lexical environment of the call is a function environment")
                .bind_this_value(vm, this_value)
                .must();
        }

        // 10. Return unused.
        Some(this_value)
    }

    /// The LexicalEnvironment and VariableEnvironment of a call of the function in an inline frame: a new function
    /// environment (steps 7 to 9 of PrepareForOrdinaryCall) if the function needs one, and its [[Environment]]
    /// otherwise.
    pub fn inline_call_environment(&self, vm: &Vm, new_target: Option<Gc<Object>>) -> Option<Gc<Environment>> {
        if !self.function_environment_needed() {
            return self.environment();
        }
        let local_environment = new_call_environment(vm, self.as_ecmascript_function_gc(), new_target);
        let shared_data = self.shared_data();
        let function_environment_bindings_count = shared_data.function_environment_bindings_count();
        local_environment.set_environment_shape_cache(
            shared_data.function_environment_shape_cache(),
            function_environment_bindings_count,
        );
        local_environment.ensure_capacity(function_environment_bindings_count);
        Some(local_environment.upcast())
    }

    /// Whether calls of the function need a function environment or the resolution of their this value through the
    /// environment, which inline frames of JIT code do not set up.
    pub fn needs_environment_or_this_value_resolution(&self) -> bool {
        self.function_environment_needed() || self.this_value_needs_environment_resolution()
    }

    pub fn contains_direct_call_to_eval(&self) -> bool {
        self.shared_data().contains_direct_call_to_eval()
    }

    // 10.2.1.4 OrdinaryCallEvaluateBody ( F, argumentsList ), https://tc39.es/ecma262/#sec-ordinarycallevaluatebody
    // 15.8.4 Runtime Semantics: EvaluateAsyncFunctionBody, https://tc39.es/ecma262/#sec-runtime-semantics-evaluatefunctionbody
    fn ordinary_call_evaluate_body(&self, vm: &Vm, context: &ExecutionContext) -> ThrowCompletionOr<Value> {
        let executable = self
            .bytecode_executable()
            .expect("a function is compiled before it is called");
        let result = vm.run_executable(NonNull::from(context), executable, 0);

        // NOTE: Running the bytecode should eventually return a completion.
        // Until it does, we assume "return" and include the undefined fallback from the call site.
        if self.kind() == FunctionKind::Normal {
            return result.map_err(Throw::new);
        }

        if self.kind() == FunctionKind::Async {
            return Ok(Value::from_object(AsyncFunctionDriverWrapper::start(
                vm,
                context.realm.get().expect("a function runs in a realm"),
                GeneratingFunction::Ecmascript(self.as_ecmascript_function_gc()),
                context,
                result,
            )));
        }
        result.map_err(Throw::new)?;

        let realm = context.realm.get().expect("a function runs in a realm");
        if self.kind() == FunctionKind::AsyncGenerator {
            return Ok(Value::from_object(AsyncGenerator::create(
                vm,
                realm,
                GeneratingFunction::Ecmascript(self.as_ecmascript_function_gc()),
                context.copy(),
            )));
        }

        debug_assert!(self.kind() == FunctionKind::Generator);
        Ok(Value::from_object(GeneratorObject::create(
            vm,
            realm,
            GeneratingFunction::Ecmascript(self.as_ecmascript_function_gc()),
            context.copy(),
        )))
    }

    pub fn set_name(&self, vm: &Vm, name: &Utf16FlyString) {
        self.set_flag(ecmascript_function_flags::HAS_CUSTOM_NAME, true);
        let name_string = PrimitiveString::create_from_fly_string(vm, name);
        if self.has_flag(ecmascript_function_flags::HAS_LAZY_LENGTH_AND_NAME) {
            self.reify_lazy_length_and_name_with_name(vm, name_string);
            return;
        }
        let mut descriptor = PropertyDescriptor {
            value: Some(Value::from_string(name_string)),
            writable: Some(false),
            enumerable: Some(false),
            configurable: Some(true),
            ..Default::default()
        };
        self.define_property_or_throw(vm, &vm.names.name, &mut descriptor)
            .must();
    }

    pub fn set_inferred_name(&self, vm: &Vm, name: &ClassElementName, prefix: Option<&str>) {
        let function_name = self.make_function_name(vm, name, prefix);
        self.set_name(vm, &to_utf16_fly_string(&function_name.utf16_string()));
    }

    pub fn name_for_call_stack(&self) -> Utf16String {
        Utf16String::from(self.name())
    }

    /// Whether a copy of the function's bytes is a function of its own: it has no name of its own, no class data,
    /// nothing outside the heap as an object, and no property storage outside its cell.
    pub fn can_be_copied(&self) -> bool {
        let object = &self.base.base;
        !self.has_flag(ecmascript_function_flags::HAS_CUSTOM_NAME)
            && !self.has_flag(ecmascript_function_flags::HAS_RARE_DATA)
            && object.owns_nothing_outside_the_heap()
            && object.named_storage_is_inline()
            && object.indexed_storage_kind() == IndexedStorageKind::None
    }

    pub fn name(&self) -> Utf16FlyString {
        if self.has_flag(ecmascript_function_flags::HAS_CUSTOM_NAME)
            && let Some(metadata) = self.shape().lookup(&Vm::of_cell(self).names.name)
        {
            let name = self.get_direct(metadata.offset);
            if name.is_string() {
                return to_utf16_fly_string(&name.as_string().utf16_string());
            }
        }
        self.shared_data().name()
    }

    fn has_flag(&self, flag: u8) -> bool {
        self.class_bits.get() & flag != 0
    }

    fn set_flag(&self, flag: u8, value: bool) {
        let flags = self.class_bits.get();
        self.class_bits.set(if value { flags | flag } else { flags & !flag });
    }

    fn class_data(&self) -> Option<&GcRefCell<ClassData>> {
        self.with_rare_data()?.storage.class_data.get().map(Box::as_ref)
    }

    fn class_data_to_add_to(&self) -> &GcRefCell<ClassData> {
        self.with_rare_data()
            .expect("a class constructor or a field initializer has rare data")
            .storage
            .class_data
            .get_or_init(Box::default)
    }

    pub fn has_class_data(&self) -> bool {
        self.class_data().is_some_and(|class_data| {
            let class_data = class_data.borrow();
            !class_data.fields.is_empty() || !class_data.private_methods.is_empty()
        })
    }

    pub fn fields_count(&self) -> usize {
        self.class_data()
            .map_or(0, |class_data| class_data.borrow().fields.len())
    }

    /// A copy of the field record at `index` of [[Fields]].
    pub fn field(&self, index: usize) -> ClassFieldDefinition {
        self.class_data().expect("the function has class data").borrow().fields[index].clone()
    }

    pub fn add_field(&self, vm: &Vm, field: ClassFieldDefinition) {
        self.class_data_to_add_to().borrow_mut(vm).fields.push(field);
    }

    pub fn private_methods_count(&self) -> usize {
        self.class_data()
            .map_or(0, |class_data| class_data.borrow().private_methods.len())
    }

    /// A copy of the element at `index` of [[PrivateMethods]].
    pub fn private_method(&self, index: usize) -> PrivateElement {
        self.class_data()
            .expect("the function has class data")
            .borrow()
            .private_methods[index]
            .clone()
    }

    pub fn add_private_method(&self, vm: &Vm, method: PrivateElement) {
        self.class_data_to_add_to().borrow_mut(vm).private_methods.push(method);
    }

    pub fn shared_data(&self) -> Gc<SharedFunctionInstanceData> {
        // SAFETY: An ECMAScript function object gets its shared data when it is created, and keeps it.
        unsafe { self.fields().shared_data.get_unchecked() }
    }

    pub fn is_module_wrapper(&self) -> bool {
        self.shared_data().is_module_wrapper()
    }

    pub fn set_is_module_wrapper(&self, is_module_wrapper: bool) {
        self.shared_data().set_is_module_wrapper(is_module_wrapper);
    }

    pub fn formal_parameter_count(&self) -> u32 {
        self.shared_data().formal_parameter_count()
    }

    /// Shares the cached parameter map with an arguments object.
    pub fn mapped_argument_names(&self) -> Rc<[Utf16FlyString]> {
        self.shared_data().mapped_argument_names()
    }

    pub fn parameter_binding_names(&self) -> Rc<[Utf16FlyString]> {
        self.shared_data().parameter_binding_names()
    }

    pub fn bytecode_executable(&self) -> Option<Gc<Executable>> {
        self.shared_data().executable()
    }

    pub fn can_inline_call(&self) -> bool {
        self.shared_data().can_inline_call()
    }

    pub fn inline_call_executable(&self) -> Gc<Executable> {
        assert!(self.can_inline_call());
        self.shared_data()
            .executable()
            .expect("a function that can be called inline has an executable")
    }

    pub fn environment(&self) -> Option<Gc<Environment>> {
        self.fields().environment.get()
    }

    pub fn private_environment(&self) -> Option<Gc<PrivateEnvironment>> {
        self.with_rare_data()?.private_environment.get()
    }

    pub fn script_or_module(&self) -> ScriptOrModule {
        self.shared_data().script_or_module.get()
    }

    pub fn constructor_kind(&self) -> ConstructorKind {
        self.shared_data().constructor_kind()
    }

    pub fn set_constructor_kind(&self, constructor_kind: ConstructorKind) {
        self.shared_data().set_constructor_kind(constructor_kind);
    }

    pub fn this_mode(&self) -> ThisMode {
        self.shared_data().this_mode()
    }

    pub fn is_arrow_function(&self) -> bool {
        self.shared_data().is_arrow_function()
    }

    pub fn is_class_constructor(&self) -> bool {
        self.shared_data().is_class_constructor()
    }

    pub fn uses_this(&self) -> bool {
        self.shared_data().uses_this()
    }

    pub fn this_value_needs_environment_resolution(&self) -> bool {
        self.shared_data().this_value_needs_environment_resolution()
    }

    pub fn function_length(&self) -> i32 {
        self.shared_data().function_length()
    }

    pub fn home_object(&self) -> Option<Gc<Object>> {
        self.with_rare_data()?.home_object.get()
    }

    /// Gives a class constructor its home object, which it was created without.
    pub fn set_home_object_of_class_constructor(&self, home_object: Gc<Object>) {
        self.with_rare_data()
            .expect("a class constructor has rare data")
            .home_object
            .set(Some(home_object));
    }

    pub fn source_text(&self) -> Utf16String {
        self.shared_data().source_text()
    }

    pub fn set_source_text(&self, source_text: Utf16String) {
        self.shared_data().set_source_text(source_text);
    }

    pub fn set_source_text_range(
        &self,
        source_code: &Rc<SourceCode>,
        source_text_offset: usize,
        source_text_length: usize,
    ) {
        self.shared_data()
            .set_source_text_range(source_code, source_text_offset, source_text_length);
    }

    // This is for IsSimpleParameterList (static semantics)
    pub fn has_simple_parameter_list(&self) -> bool {
        self.shared_data().has_simple_parameter_list()
    }

    // Equivalent to absence of [[Construct]]
    pub fn has_constructor(&self) -> bool {
        Self::has_constructor_for(self.shared_data(), self.has_flag(ecmascript_function_flags::IS_METHOD))
    }

    /// Whether a function with `shared_data` has a [[Construct]] internal method.
    fn has_constructor_for(shared_data: Gc<SharedFunctionInstanceData>, is_method: bool) -> bool {
        shared_data.kind() == FunctionKind::Normal && !shared_data.is_arrow_function() && !is_method
    }

    pub fn kind(&self) -> FunctionKind {
        self.shared_data().kind()
    }

    // This is used by LibWeb to disassociate event handler attribute callback functions from the nearest script on the call stack.
    // https://html.spec.whatwg.org/multipage/webappapis.html#getting-the-current-value-of-the-event-handler Step 3.11
    // NB: This applies to every function with the same shared data, which such a callback function has to itself.
    pub fn set_script_or_module(&self, script_or_module: ScriptOrModule) {
        self.shared_data().script_or_module.set(script_or_module);
    }

    pub fn class_field_initializer_name(&self) -> ClassFieldInitializerName {
        // NB: The shared data has the name of a field whose key is not computed.
        if let Some(class_data) = self.class_data() {
            let name = class_data.borrow().class_field_initializer_name.clone();
            if !matches!(name, ClassFieldInitializerName::Empty) {
                return name;
            }
        }
        self.shared_data().class_field_initializer_name()
    }

    pub fn set_class_field_initializer_name(&self, vm: &Vm, name: ClassFieldInitializerName) {
        self.class_data_to_add_to().borrow_mut(vm).class_field_initializer_name = name;
    }

    pub fn allocates_function_environment(&self) -> bool {
        self.shared_data().function_environment_needed()
    }

    pub fn function_environment_needed(&self) -> bool {
        self.shared_data().function_environment_needed()
    }

    pub fn supports_legacy_caller_or_arguments(&self) -> bool {
        // NB: These are the functions that the "caller" and "arguments" accessors of %Function.prototype% accept. The
        //     others are the functions that https://tc39.es/ecma262/#sec-forbidden-extensions keeps from exposing them.
        self.kind() == FunctionKind::Normal
            && !self.is_arrow_function()
            && !self.is_class_constructor()
            && !self.has_flag(ecmascript_function_flags::IS_METHOD)
            && !self.is_strict_mode()
    }

    pub fn legacy_caller(&self, vm: &Vm) -> Value {
        let this_function = self.as_function_object_gc();
        let mut caller: Option<Gc<EcmascriptFunctionObject>> = None;
        let mut found_this_function = false;

        vm.for_each_stack_frame_top_to_bottom(|frame| {
            if !found_this_function {
                if frame.function == Some(this_function) {
                    found_this_function = true;
                }
                return ControlFlow::Continue(());
            }

            let Some(function) = frame.function else {
                return ControlFlow::Continue(());
            };

            // NB: The spec has these built-ins perform PrepareForTailCall(), which discards their execution context
            //     before they call this function. We don't implement it, so skip the frames we keep for them.
            if performs_prepare_for_tail_call(vm, function) {
                return ControlFlow::Continue(());
            }

            caller = as_ecmascript_function_object(function);
            ControlFlow::Break(())
        });

        // NB: https://tc39.es/ecma262/#sec-forbidden-extensions keeps own "caller" properties from exposing strict
        //     functions. The accessor on %Function.prototype% holds to the same rule, and also hides generator and
        //     async functions.
        match caller {
            Some(caller) if caller.kind() == FunctionKind::Normal && !caller.is_strict_mode() => {
                Value::from_object(caller)
            }
            _ => Value::NULL,
        }
    }

    pub fn legacy_arguments(&self, vm: &Vm) -> Value {
        let this_function = self.as_function_object_gc();
        // NB: The arguments of a frame of an inlined call may be objects JIT code never created, which only the
        //     arguments object holds once it exists.
        let defer_gc = DeferGc::new(vm);
        let mut passed_arguments: Option<Deferred<'_, Vec<Cell<Value>>>> = None;

        vm.for_each_stack_frame_top_to_bottom(|frame| {
            if frame.function != Some(this_function) {
                return ControlFlow::Continue(());
            }

            // NB: The frame of a call that JIT code inlined has its arguments in the JIT frame.
            passed_arguments = Some(
                defer_gc.cover(match frame.inlined_call {
                    Some(inlined_call) => crate::jit::translate::call_site_frame_arguments(
                        vm,
                        frame.execution_context,
                        inlined_call.site,
                        inlined_call.frame_pointer,
                        inlined_call.frame_state,
                        &defer_gc,
                    )
                    .into_inner()
                    .into_iter()
                    .map(Cell::new)
                    .collect(),
                    None => {
                        let context = frame.execution_context;
                        // NB: The frame of an inlined call that JIT code published or materialized for a call it made has
                        //     markers for the objects among its arguments that the code never allocated.
                        crate::jit::translate::resolve_deferred_arguments(vm, context);
                        context.arguments()[..context.passed_argument_count.get() as usize]
                            .iter()
                            .map(|argument| Cell::new(argument.get()))
                            .collect()
                    }
                }),
            );
            ControlFlow::Break(())
        });

        let Some(passed_arguments) = passed_arguments else {
            return Value::NULL;
        };

        let arguments_object = create_unmapped_arguments_object(vm, &passed_arguments);
        if self.has_simple_parameter_list() {
            arguments_object.define_direct_property(
                vm,
                &vm.names.callee,
                Value::from_object(self.as_function_object_gc()),
                PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE),
            );
        }
        Value::from_object(arguments_object)
    }

    fn internal_get_own_property(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
    ) -> ThrowCompletionOr<Option<PropertyDescriptor>> {
        as_ecmascript_function(object).reify_lazy_property(vm, property_key);
        object.ordinary_get_own_property(vm, property_key)
    }

    fn internal_own_property_keys<'vm>(object: &Object, vm: &'vm Vm) -> ThrowCompletionOr<MarkedVec<'vm, Value>> {
        let function = as_ecmascript_function(object);

        function.reify_lazy_properties(vm);

        object.ordinary_own_property_keys(vm)
    }
}
