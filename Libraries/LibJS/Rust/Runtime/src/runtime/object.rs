/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::ControlFlow;
use core::ptr::NonNull;
use std::alloc::{Layout, handle_alloc_error};
use std::collections::HashSet;

use ak::{Utf16FlyString, Utf16String};
use libjs_abi::{Builtin, PutKind};
use libjs_runtime_macros::Trace;

use crate::bytecode::executable::{PropertyLookupCache, StaticPropertyLookupCacheSite};
use crate::bytecode::property_access::{Strict, put_by_property_key};
use crate::gc::class::{Class, Extends, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::root::MarkedVec;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::execution_context::ExecutionContext;
use crate::layout::function_object::{EcmascriptFunctionObject, FunctionObject};
pub use crate::layout::object::{
    INDEXED_ELEMENTS_HEADER_SIZE, INLINE_NAMED_STORAGE_CAPACITY, IndexedStorageKind, Object, object_flag,
};
use crate::layout::value::Value;
use crate::layout_forward::RawNativeFunctionPointer;
use crate::runtime::abstract_operations::{
    call, call_function_object, function_object_as_object, validate_and_apply_property_descriptor,
};
use crate::runtime::accessor::Accessor;
use crate::runtime::array::Array;
use crate::runtime::class_field_definition::{ClassElementName, ClassFieldDefinition, ClassFieldInitializer};
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::indexed_properties::{GenericIndexedPropertyStorage, ValueAndAttributes};
use crate::runtime::iterator::BuiltinIteratorNext;
use crate::runtime::native_function::{NativeFunction, NativeFunctionMethods, RawNativeFunction};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::private_environment::PrivateName;
use crate::runtime::property_attributes::{DEFAULT_ATTRIBUTES, PropertyAttributes};
use crate::runtime::property_descriptor::{PropertyDescriptor, to_property_descriptor};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::shape::Shape;
use crate::runtime::symbol::Symbol;
use crate::runtime::value::{PreferredType, same_value};
use crate::utf16::to_utf16_fly_string;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PrivateElementKind {
    Field,
    Method,
    Accessor,
}

#[derive(Clone, Debug, Trace)]
pub struct PrivateElement {
    #[gc(untraced)]
    pub key: PrivateName,
    #[gc(untraced)]
    pub kind: PrivateElementKind,
    pub value: Value,
}

/// [[PrivateElements]], allocated when the first one is added and freed when the object is finalized.
#[repr(transparent)]
#[derive(Default)]
pub struct PrivateElements(Cell<Option<NonNull<GcRefCell<Vec<PrivateElement>>>>>);

impl PrivateElements {
    fn get(&self) -> Option<&GcRefCell<Vec<PrivateElement>>> {
        // SAFETY: The elements are owned by the object and live until it is destroyed.
        self.0.get().map(|elements| unsafe { elements.as_ref() })
    }

    fn ensure(&self) -> &GcRefCell<Vec<PrivateElement>> {
        if self.0.get().is_none() {
            let elements = Box::new(GcRefCell::new(Vec::new()));
            self.0.set(Some(NonNull::from(Box::leak(elements))));
        }
        self.get().expect("the private elements were just allocated")
    }

    fn free(&self) {
        if let Some(elements) = self.0.take() {
            // SAFETY: The elements were leaked from a Box by ensure() and nothing refers to them any more.
            drop(unsafe { Box::from_raw(elements.as_ptr()) });
        }
    }
}

// Non-standard: This is information optionally returned by object property access functions.
//               It can be used to implement inline caches for property lookup.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CacheableGetPropertyMetadataType {
    NotCacheable,
    GetOwnProperty,
    GetPropertyInPrototypeChain,
    GetMissingProperty,
}

#[derive(Clone, Copy)]
pub struct CacheableGetPropertyMetadata {
    pub r#type: CacheableGetPropertyMetadataType,
    pub property_offset: Option<u32>,
    pub prototype: Option<Gc<Object>>,
    pub property_absence_is_cacheable: bool,
}

impl Default for CacheableGetPropertyMetadata {
    fn default() -> Self {
        Self {
            r#type: CacheableGetPropertyMetadataType::NotCacheable,
            property_offset: None,
            prototype: None,
            property_absence_is_cacheable: true,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CacheableSetPropertyMetadataType {
    NotCacheable,
    AddOwnProperty,
    ChangeOwnProperty,
    ChangePropertyInPrototypeChain,
}

#[derive(Clone, Copy)]
pub struct CacheableSetPropertyMetadata {
    pub r#type: CacheableSetPropertyMetadataType,
    pub property_offset: Option<u32>,
    pub prototype: Option<Gc<Object>>,
    pub writes_data_property: bool,
}

impl Default for CacheableSetPropertyMetadata {
    fn default() -> Self {
        Self {
            r#type: CacheableSetPropertyMetadataType::NotCacheable,
            property_offset: None,
            prototype: None,
            writes_data_property: false,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PropertyKind {
    Key,
    Value,
    KeyAndValue,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IntegrityLevel {
    Sealed,
    Frozen,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShouldThrowExceptions {
    No,
    Yes,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MayInterfereWithIndexedPropertyAccess {
    No,
    Yes,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PropertyLookupPhase {
    OwnProperty,
    PrototypeChain,
}

/// What FunctionObject::get_stack_frame_info reports: the frame a call to the function needs.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct StackFrameInfo {
    pub registers_and_locals_count: u32,
    pub constant_count: u32,
    pub argument_count: u32,
}

pub type InternalDefineOwnProperty = fn(
    &Object,
    &Vm,
    &PropertyKey,
    &mut PropertyDescriptor,
    Option<&Option<PropertyDescriptor>>,
) -> ThrowCompletionOr<bool>;

pub type InternalGet = fn(
    &Object,
    &Vm,
    &PropertyKey,
    Value,
    Option<&mut CacheableGetPropertyMetadata>,
    PropertyLookupPhase,
) -> ThrowCompletionOr<Value>;

pub type InternalSet = fn(
    &Object,
    &Vm,
    &PropertyKey,
    Value,
    Value,
    Option<&mut CacheableSetPropertyMetadata>,
    PropertyLookupPhase,
) -> ThrowCompletionOr<bool>;

/// [[Call]], given the callee's frame with the arguments in place and the this value.
pub type InternalCall = fn(&Object, &Vm, &ExecutionContext, Value) -> ThrowCompletionOr<Value>;

/// [[Construct]], given the callee's frame with the arguments in place and the new target.
pub type InternalConstruct = fn(&Object, &Vm, &ExecutionContext, Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>>;

/// The internal methods of a class of objects: the Rust form of the virtual methods of the C++ Object, and of the
/// FunctionObject virtuals that calls go through. Exotic objects override some of them, as in
/// `ObjectMethods { internal_get: ..., ..ORDINARY_OBJECT_METHODS }`.
pub struct ObjectMethods {
    /// Cell::initialize(Realm&), which C++ calls once an object is allocated through Realm::create and which defines
    /// the properties of built-in objects. Subclasses call the method of the class they extend first.
    pub initialize: fn(&Object, &Vm, Gc<Realm>),
    pub internal_get_prototype_of: fn(&Object, &Vm) -> ThrowCompletionOr<Option<Gc<Object>>>,
    pub internal_set_prototype_of: fn(&Object, &Vm, Option<Gc<Object>>) -> ThrowCompletionOr<bool>,
    pub internal_is_extensible: fn(&Object, &Vm) -> ThrowCompletionOr<bool>,
    pub internal_prevent_extensions: fn(&Object, &Vm) -> ThrowCompletionOr<bool>,
    pub internal_get_own_property: fn(&Object, &Vm, &PropertyKey) -> ThrowCompletionOr<Option<PropertyDescriptor>>,
    pub internal_define_own_property: InternalDefineOwnProperty,
    pub internal_has_property: fn(&Object, &Vm, &PropertyKey) -> ThrowCompletionOr<bool>,
    pub internal_get: InternalGet,
    pub internal_set: InternalSet,
    pub internal_delete: fn(&Object, &Vm, &PropertyKey) -> ThrowCompletionOr<bool>,
    pub internal_own_property_keys: for<'vm> fn(&Object, &'vm Vm) -> ThrowCompletionOr<MarkedVec<'vm, Value>>,
    /// [[Call]], which only function objects have.
    pub internal_call: Option<InternalCall>,
    /// [[Construct]], which only constructors have.
    pub internal_construct: Option<InternalConstruct>,
    pub has_constructor: fn(&Object) -> bool,
    pub is_strict_mode: fn(&Object) -> bool,
    /// Takes the VM since an ECMAScript function compiles its body the first time it is asked.
    pub get_stack_frame_info: fn(&Object, &Vm, &mut StackFrameInfo),
    /// FunctionObject::realm(), the [[Realm]] of a function that has one.
    pub function_realm: fn(&Object) -> Option<Gc<Realm>>,
    /// FunctionObject::name_for_call_stack(), which every class of function object defines.
    pub name_for_call_stack: fn(&Object) -> Utf16String,
    /// The virtual methods of NativeFunction, which only native functions have.
    pub native_function: Option<&'static NativeFunctionMethods>,
    pub is_cacheable_for_property_absence: fn(&Object) -> bool,
    pub is_cacheable_for_inherited_property: fn(&Object) -> bool,
    pub eligible_for_own_property_enumeration_fast_path: fn(&Object) -> bool,
    /// The step of a built-in iterator whose next method is the original one, which IteratorStep takes without calling
    /// it; given the iterator and the next method of its iterator record.
    pub as_builtin_iterator_if_next_is_not_redefined: fn(&Object, Value) -> Option<BuiltinIteratorNext>,
}

pub static ORDINARY_OBJECT_METHODS: ObjectMethods = ObjectMethods {
    initialize: |_, _, _| {},
    internal_get_prototype_of: Object::ordinary_get_prototype_of,
    internal_set_prototype_of: Object::ordinary_set_prototype_of,
    internal_is_extensible: Object::ordinary_is_extensible,
    internal_prevent_extensions: Object::ordinary_prevent_extensions,
    internal_get_own_property: Object::ordinary_get_own_property,
    internal_define_own_property: Object::ordinary_define_own_property,
    internal_has_property: Object::ordinary_has_property,
    internal_get: Object::ordinary_get,
    internal_set: Object::ordinary_set,
    internal_delete: Object::ordinary_delete,
    internal_own_property_keys: Object::ordinary_own_property_keys,
    internal_call: None,
    internal_construct: None,
    has_constructor: |_| false,
    is_strict_mode: |_| false,
    get_stack_frame_info: |_, _, _| {},
    function_realm: |_| None,
    name_for_call_stack: |_| unreachable!("FunctionObject::name_for_call_stack is pure virtual"),
    native_function: None,
    is_cacheable_for_property_absence: |_| true,
    is_cacheable_for_inherited_property: |_| true,
    eligible_for_own_property_enumeration_fast_path: |_| true,
    as_builtin_iterator_if_next_is_not_redefined: |_, _| None,
};

define_cell!(Object, Object, methods: ORDINARY_OBJECT_METHODS);

// SAFETY: Visits the shape, the named properties the shape describes, the indexed properties and the private
// elements, which are all the cells an object reaches.
unsafe impl Trace for Object {
    fn trace(&self, visitor: &mut Visitor) {
        self.shape.trace(visitor);
        let property_count = self.shape().property_count() as usize;
        let named_properties = self.named_properties.get();
        if property_count > 0 && !named_properties.is_null() {
            debug_assert!(property_count <= self.named_storage_capacity() as usize);
            // SAFETY: The storage holds at least one slot per property of the shape, and nothing writes it during
            // a collection.
            visitor.visit_values(unsafe { core::slice::from_raw_parts(named_properties, property_count) });
        }

        match self.indexed_storage_kind() {
            IndexedStorageKind::None => {}
            IndexedStorageKind::Packed => self.visit_indexed_elements(self.indexed_packed_element_count(), visitor),
            IndexedStorageKind::Holey => self.visit_indexed_elements(
                self.indexed_array_like_size().min(self.indexed_elements_capacity()),
                visitor,
            ),
            IndexedStorageKind::Dictionary => self.indexed_dictionary().trace(visitor),
        }

        if let Some(private_elements) = self.private_elements.get() {
            private_elements.trace(visitor);
        }
    }
}

/// Object::~Object(), which LibGC runs as it sweeps the object, rather than in a finalizer every dead object would
/// have to visit while the world is stopped.
impl Drop for Object {
    fn drop(&mut self) {
        self.free_indexed_elements();
        let named_properties = self.named_properties.get();
        if !named_properties.is_null() && !self.named_storage_is_inline() {
            heap_value_storage::deallocate(named_properties);
            self.named_properties.set(self.inline_named_storage_pointer());
        }
        self.private_elements.free();
    }
}

/// Heap-allocated property storage layout:
///   [u32 capacity] [u32 padding] [Value 0] [Value 1] ...
/// These are the only functions that depend on this allocation layout.
mod heap_value_storage {
    use super::*;

    // Allocating through AK puts the storage in its heap partition for JS object storage, apart from every other
    // allocation, as C++ objects keep theirs. That partition belongs to the thread that first allocates from it, so
    // the standalone tests, which run each test on a thread of its own, use the global allocator instead.
    #[cfg(feature = "allocator")]
    mod raw {
        unsafe extern "C" {
            fn ladybird_js_object_storage_alloc(size: usize) -> *mut u8;
            fn ladybird_js_object_storage_realloc(pointer: *mut u8, new_size: usize) -> *mut u8;
            fn ladybird_dealloc(pointer: *mut u8, alignment: usize);
        }

        pub unsafe fn allocate(layout: super::Layout) -> *mut u8 {
            // SAFETY: Any size can be allocated, and AK aligns every allocation for a Value.
            unsafe { ladybird_js_object_storage_alloc(layout.size()) }
        }

        pub unsafe fn reallocate(pointer: *mut u8, _old_layout: super::Layout, new_size: usize) -> *mut u8 {
            // SAFETY: The caller passes storage that allocate() returned.
            unsafe { ladybird_js_object_storage_realloc(pointer, new_size) }
        }

        pub unsafe fn deallocate(pointer: *mut u8, layout: super::Layout) {
            // SAFETY: The caller passes storage that allocate() returned.
            unsafe { ladybird_dealloc(pointer, layout.align()) }
        }
    }

    #[cfg(not(feature = "allocator"))]
    mod raw {
        pub use std::alloc::{alloc as allocate, dealloc as deallocate, realloc as reallocate};
    }

    const HEADER_SIZE: usize = INDEXED_ELEMENTS_HEADER_SIZE;

    fn layout(capacity: u32) -> Layout {
        Layout::from_size_align(allocation_size(capacity), align_of::<Value>()).expect("the storage layout is valid")
    }

    pub fn allocation_size(capacity: u32) -> usize {
        HEADER_SIZE + capacity as usize * size_of::<Value>()
    }

    pub fn allocate(capacity: u32) -> *mut Value {
        let layout = layout(capacity);
        // SAFETY: The layout has room for at least the header.
        let raw = unsafe { raw::allocate(layout) };
        if raw.is_null() {
            handle_alloc_error(layout);
        }
        // SAFETY: The allocation starts with the header, and the elements follow it.
        unsafe {
            raw.cast::<u32>().write(capacity);
            raw.add(size_of::<u32>()).cast::<u32>().write(0);
            raw.add(HEADER_SIZE).cast::<Value>()
        }
    }

    pub fn reallocate(storage: *mut Value, new_capacity: u32) -> *mut Value {
        let old_layout = layout(capacity(storage));
        let new_layout = layout(new_capacity);
        // SAFETY: The storage came from allocate() with the capacity in its header.
        let raw = unsafe { raw::reallocate(allocation_start(storage), old_layout, new_layout.size()) };
        if raw.is_null() {
            handle_alloc_error(new_layout);
        }
        // SAFETY: The reallocated storage keeps its header in front of the elements.
        unsafe {
            raw.cast::<u32>().write(new_capacity);
            raw.add(HEADER_SIZE).cast::<Value>()
        }
    }

    pub fn deallocate(storage: *mut Value) {
        if storage.is_null() {
            return;
        }
        // SAFETY: The storage came from allocate() with the capacity in its header.
        unsafe { raw::deallocate(allocation_start(storage), layout(capacity(storage))) };
    }

    pub fn capacity(storage: *const Value) -> u32 {
        // SAFETY: The storage came from allocate(), which put the capacity in front of the first element.
        unsafe { storage.cast::<u8>().sub(HEADER_SIZE).cast::<u32>().read() }
    }

    fn allocation_start(storage: *mut Value) -> *mut u8 {
        // SAFETY: The header is part of the same allocation.
        unsafe { storage.cast::<u8>().sub(HEADER_SIZE) }
    }
}

/// Defines the class of an object type that extends `$parent` and overrides Cell::initialize() and the internal methods
/// in `methods`, as most built-in prototypes do. The type keeps its parent in a field named `base` and derefs to it.
macro_rules! define_object_class {
    ($type:ident, extends: [$parent:ident $(, $ancestor:ident)*], methods: { $($method:ident: $value:expr,)* ..$parent_methods:path }) => {
        const _: () = {
            static METHODS: $crate::runtime::object::ObjectMethods = $crate::runtime::object::ObjectMethods {
                $($method: $value,)*
                ..$parent_methods
            };
            define_cell!($type, Object, extends: [$parent $(, $ancestor)*], methods: METHODS);
        };

        impl core::ops::Deref for $type {
            type Target = $parent;

            fn deref(&self) -> &$parent {
                &self.base
            }
        }
    };
}

pub(crate) use define_object_class;

/// Object::IntrinsicAccessor: computes the value of a property defined with define_intrinsic_accessor().
pub type IntrinsicAccessor = fn(&Vm, Gc<Realm>) -> Value;

/// Takes the intrinsic accessor of a property that has not been read yet, which is used at most once.
fn find_intrinsic_accessor(vm: &Vm, object: &Object, property_key: &PropertyKey) -> Option<IntrinsicAccessor> {
    if !property_key.is_string() {
        return None;
    }

    let mut intrinsic_accessors = vm.intrinsic_accessors().borrow_mut();
    let intrinsics = intrinsic_accessors.get_mut(&(core::ptr::from_ref(object) as usize))?;
    intrinsics.remove(property_key.as_string())
}

fn remove_intrinsic_accessor(vm: &Vm, object: &Object, property_key: &PropertyKey) {
    if let Some(intrinsics) = vm
        .intrinsic_accessors()
        .borrow_mut()
        .get_mut(&(core::ptr::from_ref(object) as usize))
    {
        intrinsics.remove(property_key.as_string());
    }
}

const SPARSE_ARRAY_HOLE_THRESHOLD: u32 = 200;
const MAX_TRANSITIONS_BEFORE_CONVERTING_TO_DICTIONARY: u32 = 64;

/// Moves an object into the heap and finishes what its constructor could not do before it had an address: its named
/// properties start out in its inline storage. Every object, of any class, is allocated through this, as C++ objects
/// are through Realm::create.
pub fn allocate_object<T: GcCell + Extends<Object>>(vm: &Vm, object: T) -> Gc<T> {
    let cell = vm.heap().allocate(object);
    let object = cell.upcast::<Object>();
    debug_assert!(object.named_properties.get().is_null());
    object.named_properties.set(object.inline_named_storage_pointer());
    let property_count = object.shape().property_count();
    if property_count > 0 {
        object.ensure_named_storage_capacity(property_count);
    }
    cell
}

impl Object {
    // The constructors of the C++ Object, which build the data of an object of `class` for allocate_object().

    /// Object(Shape&)
    pub fn new_with_shape(
        class: &'static Class,
        shape: Gc<Shape>,
        may_interfere_with_indexed_property_access: MayInterfereWithIndexedPropertyAccess,
    ) -> Object {
        assert!(class.is_subclass_of(Self::CLASS));
        let object = Object {
            header: CellHeader::for_class(class),
            flags: Cell::new(object_flag::IS_EXTENSIBLE),
            indexed_storage_kind: Cell::new(IndexedStorageKind::None),
            indexed_array_like_size: Cell::new(0),
            shape: Cell::new(shape),
            named_properties: Cell::new(core::ptr::null_mut()),
            indexed_elements: Cell::new(core::ptr::null_mut()),
            private_elements: PrivateElements::default(),
            inline_named_storage: [const { Cell::new(Value::UNDEFINED) }; INLINE_NAMED_STORAGE_CAPACITY],
        };
        if may_interfere_with_indexed_property_access == MayInterfereWithIndexedPropertyAccess::Yes {
            object.set_may_interfere_with_indexed_property_access();
        }
        object
    }

    /// Object(GlobalObjectTag, Realm&)
    pub fn new_global_object(
        vm: &Vm,
        class: &'static Class,
        realm: Gc<Realm>,
        may_interfere_with_indexed_property_access: MayInterfereWithIndexedPropertyAccess,
    ) -> Object {
        // This is the global object
        let object = Self::new_with_shape(
            class,
            Shape::create(vm, realm),
            may_interfere_with_indexed_property_access,
        );
        object.set_global_object_flag();
        object
    }

    /// Object(ConstructWithoutPrototypeTag, Realm&)
    pub fn new_without_prototype(
        vm: &Vm,
        class: &'static Class,
        realm: Gc<Realm>,
        may_interfere_with_indexed_property_access: MayInterfereWithIndexedPropertyAccess,
    ) -> Object {
        Self::new_with_shape(
            class,
            Shape::create(vm, realm),
            may_interfere_with_indexed_property_access,
        )
    }

    /// Object(Realm&, GC::Ptr<Object> prototype)
    pub fn new_with_realm_and_prototype(
        vm: &Vm,
        class: &'static Class,
        realm: Gc<Realm>,
        prototype: Option<Gc<Object>>,
        may_interfere_with_indexed_property_access: MayInterfereWithIndexedPropertyAccess,
    ) -> Object {
        let mut shape = realm.empty_object_shape();
        if prototype.is_some() && shape.prototype() != prototype {
            shape = shape.create_prototype_transition(vm, prototype);
        }
        Self::new_with_shape(class, shape, may_interfere_with_indexed_property_access)
    }

    /// Object(ConstructWithPrototypeTag, Object& prototype)
    pub fn new_with_prototype(
        vm: &Vm,
        class: &'static Class,
        prototype: Gc<Object>,
        may_interfere_with_indexed_property_access: MayInterfereWithIndexedPropertyAccess,
    ) -> Object {
        let mut shape = prototype.shape().realm().empty_object_shape();
        if shape.prototype() != Some(prototype) {
            shape = shape.create_prototype_transition(vm, Some(prototype));
        }
        Self::new_with_shape(class, shape, may_interfere_with_indexed_property_access)
    }

    // 10.1.12 OrdinaryObjectCreate ( proto [ , additionalInternalSlotsList ] ), https://tc39.es/ecma262/#sec-ordinaryobjectcreate
    pub fn create(vm: &Vm, realm: Gc<Realm>, prototype: Option<Gc<Object>>) -> Gc<Object> {
        let Some(prototype) = prototype else {
            return allocate_object(
                vm,
                Self::new_with_shape(
                    Self::CLASS,
                    realm.empty_object_shape(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            );
        };
        if prototype == realm.object_prototype() {
            return allocate_object(
                vm,
                Self::new_with_shape(
                    Self::CLASS,
                    realm.new_object_shape(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            );
        }
        allocate_object(
            vm,
            Self::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
        )
    }

    pub fn create_prototype(vm: &Vm, realm: Gc<Realm>, prototype: Option<Gc<Object>>) -> Gc<Object> {
        let shape = Shape::create(vm, realm);
        if let Some(prototype) = prototype {
            shape.set_prototype_without_transition(vm, prototype);
        }
        allocate_object(
            vm,
            Self::new_with_shape(Self::CLASS, shape, MayInterfereWithIndexedPropertyAccess::No),
        )
    }

    pub fn create_with_premade_shape(vm: &Vm, shape: Gc<Shape>) -> Gc<Object> {
        allocate_object(
            vm,
            Self::new_with_shape(Self::CLASS, shape, MayInterfereWithIndexedPropertyAccess::No),
        )
    }

    pub(crate) fn as_gc(&self) -> Gc<Object> {
        // SAFETY: Objects only exist as cells once constructed, since allocate_object() moves every one into the heap.
        unsafe { Gc::from_ref(self) }
    }

    pub fn class(&self) -> &'static Class {
        self.header.class
    }

    fn methods(&self) -> &'static ObjectMethods {
        self.class()
            .object_methods
            .expect("an object class has internal methods")
    }

    pub fn unsafe_set_shape(&self, shape: Gc<Shape>) {
        self.shape.set(shape);
        self.ensure_named_storage_capacity(shape.property_count());
    }

    pub fn invalidate_property_lookup_caches(&self, vm: &Vm) {
        self.shape.set(self.shape().create_dictionary_transition(vm));
    }

    // 7.2 Testing and Comparison Operations, https://tc39.es/ecma262/#sec-testing-and-comparison-operations

    // 7.2.5 IsExtensible ( O ), https://tc39.es/ecma262/#sec-isextensible-o
    pub fn is_extensible(&self, vm: &Vm) -> ThrowCompletionOr<bool> {
        // 1. Return ? O.[[IsExtensible]]().
        self.internal_is_extensible(vm)
    }

    // 7.3 Operations on Objects, https://tc39.es/ecma262/#sec-operations-on-objects

    // 7.3.2 Get ( O, P ), https://tc39.es/ecma262/#sec-get-o-p
    pub fn get(&self, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<Value> {
        // 1. Return ? O.[[Get]](P, O).
        self.internal_get(
            vm,
            property_key,
            Value::from_object(self.as_gc()),
            None,
            PropertyLookupPhase::OwnProperty,
        )
    }

    // 7.3.2 Get ( O, P ), https://tc39.es/ecma262/#sec-get-o-p
    pub fn get_with_cache(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        cache: &PropertyLookupCache,
    ) -> ThrowCompletionOr<Value> {
        // 1. Return ? O.[[Get]](P, O).
        Value::from_object(self.as_gc()).get_with_cache(vm, property_key, cache)
    }

    // NOTE: 7.3.3 GetV ( V, P ) is implemented as Value::get().

    // 7.3.4 Set ( O, P, V, Throw ), https://tc39.es/ecma262/#sec-set-o-p-v-throw
    pub fn set(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        value: Value,
        throw_exceptions: ShouldThrowExceptions,
    ) -> ThrowCompletionOr<()> {
        assert!(!value.is_empty());

        // 1. Let success be ? O.[[Set]](P, V, O).
        let success = self.internal_set(
            vm,
            property_key,
            value,
            Value::from_object(self.as_gc()),
            None,
            PropertyLookupPhase::OwnProperty,
        )?;

        // 2. If success is false and Throw is true, throw a TypeError exception.
        if !success && throw_exceptions == ShouldThrowExceptions::Yes {
            // FIXME: Improve/contextualize error message
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ObjectSetReturnedFalse, &[]);
        }

        // 3. Return unused.
        Ok(())
    }

    pub fn set_with_cache(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        value: Value,
        cache: &PropertyLookupCache,
    ) -> ThrowCompletionOr<()> {
        let mut strict = Strict::No;
        if let Some(context) = vm.running_execution_context()
            // SAFETY: The running execution context is live.
            && let Some(function) = unsafe { context.as_ref() }.function.get()
            && function_object_as_object(function).is_strict_mode()
        {
            strict = Strict::Yes;
        }
        let this_value = Value::from_object(self.as_gc());
        put_by_property_key(
            vm,
            this_value,
            this_value,
            value,
            || None,
            property_key,
            PutKind::Normal,
            strict,
            Some(cache),
        )
    }

    // 7.3.5 CreateDataProperty ( O, P, V ), https://tc39.es/ecma262/#sec-createdataproperty
    pub fn create_data_property(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        value: Value,
        new_property_offset: Option<&mut Option<u32>>,
        precomputed_get_own_property: Option<&Option<PropertyDescriptor>>,
    ) -> ThrowCompletionOr<bool> {
        // 1. Let newDesc be the PropertyDescriptor { [[Value]]: V, [[Writable]]: true, [[Enumerable]]: true, [[Configurable]]: true }.
        let mut new_descriptor = PropertyDescriptor {
            value: Some(value),
            writable: Some(true),
            enumerable: Some(true),
            configurable: Some(true),
            ..Default::default()
        };

        // 2. Return ? O.[[DefineOwnProperty]](P, newDesc).
        let result =
            self.internal_define_own_property(vm, property_key, &mut new_descriptor, precomputed_get_own_property);
        if let Some(new_property_offset) = new_property_offset
            && let Some(property_offset) = new_descriptor.property_offset
        {
            *new_property_offset = Some(property_offset);
        }
        result
    }

    // 7.3.6 CreateMethodProperty ( O, P, V ), https://tc39.es/ecma262/#sec-createmethodproperty

    // 7.3.7 CreateDataPropertyOrThrow ( O, P, V ), https://tc39.es/ecma262/#sec-createdatapropertyorthrow
    pub fn create_data_property_or_throw(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        value: Value,
    ) -> ThrowCompletionOr<bool> {
        assert!(!value.is_empty());

        // 1. Let success be ? CreateDataProperty(O, P, V).
        let success = self.create_data_property(vm, property_key, value, None, None)?;

        // 2. If success is false, throw a TypeError exception.
        if !success {
            // FIXME: Improve/contextualize error message
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::ObjectDefineOwnPropertyReturnedFalse,
                &[],
            );
        }

        // 3. Return success.
        Ok(success)
    }

    // 7.3.8 CreateNonEnumerableDataPropertyOrThrow ( O, P, V ), https://tc39.es/ecma262/#sec-createnonenumerabledatapropertyorthrow
    pub fn create_non_enumerable_data_property_or_throw(&self, vm: &Vm, property_key: &PropertyKey, value: Value) {
        assert!(!value.is_empty());

        // 1. Assert: O is an ordinary, extensible object with no non-configurable properties.

        // 2. Let newDesc be the PropertyDescriptor { [[Value]]: V, [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: true }.
        let mut new_description = PropertyDescriptor {
            value: Some(value),
            writable: Some(true),
            enumerable: Some(false),
            configurable: Some(true),
            ..Default::default()
        };

        // 3. Perform ! DefinePropertyOrThrow(O, P, newDesc).
        self.define_property_or_throw(vm, property_key, &mut new_description)
            .must();

        // 4. Return unused.
    }

    // 7.3.9 DefinePropertyOrThrow ( O, P, desc ), https://tc39.es/ecma262/#sec-definepropertyorthrow
    pub fn define_property_or_throw(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        property_descriptor: &mut PropertyDescriptor,
    ) -> ThrowCompletionOr<()> {
        // 1. Let success be ? O.[[DefineOwnProperty]](P, desc).
        let success = self.internal_define_own_property(vm, property_key, property_descriptor, None)?;

        // 2. If success is false, throw a TypeError exception.
        if !success {
            // FIXME: Improve/contextualize error message
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::ObjectDefineOwnPropertyReturnedFalse,
                &[],
            );
        }

        // 3. Return unused.
        Ok(())
    }

    // 7.3.10 DeletePropertyOrThrow ( O, P ), https://tc39.es/ecma262/#sec-deletepropertyorthrow
    pub fn delete_property_or_throw(&self, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<()> {
        // 1. Let success be ? O.[[Delete]](P).
        let success = self.internal_delete(vm, property_key)?;

        // 2. If success is false, throw a TypeError exception.
        if !success {
            // FIXME: Improve/contextualize error message
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ObjectDeleteReturnedFalse, &[]);
        }

        // 3. Return unused.
        Ok(())
    }

    // 7.3.12 HasProperty ( O, P ), https://tc39.es/ecma262/#sec-hasproperty
    pub fn has_property(&self, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<bool> {
        // 1. Return ? O.[[HasProperty]](P).
        self.internal_has_property(vm, property_key)
    }

    // 7.3.13 HasOwnProperty ( O, P ), https://tc39.es/ecma262/#sec-hasownproperty
    pub fn has_own_property(&self, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<bool> {
        // 1. Let desc be ? O.[[GetOwnProperty]](P).
        let descriptor = self.internal_get_own_property(vm, property_key)?;

        // 2. If desc is undefined, return false.
        // 3. Return true.
        Ok(descriptor.is_some())
    }

    // 7.3.16 SetIntegrityLevel ( O, level ), https://tc39.es/ecma262/#sec-setintegritylevel
    pub fn set_integrity_level(&self, vm: &Vm, level: IntegrityLevel) -> ThrowCompletionOr<bool> {
        // 1. Let status be ? O.[[PreventExtensions]]().
        let status = self.internal_prevent_extensions(vm)?;

        // 2. If status is false, return false.
        if !status {
            return Ok(false);
        }

        // 3. Let keys be ? O.[[OwnPropertyKeys]]().
        let keys = self.internal_own_property_keys(vm)?;

        // 4. If level is sealed, then
        if level == IntegrityLevel::Sealed {
            // a. For each element k of keys, do
            for index in 0..keys.len() {
                let key = keys.get(index).expect("the index is in bounds");
                let property_key = PropertyKey::from_value(vm, key).must();

                // i. Perform ? DefinePropertyOrThrow(O, k, PropertyDescriptor { [[Configurable]]: false }).
                let mut descriptor = PropertyDescriptor {
                    configurable: Some(false),
                    ..Default::default()
                };
                self.define_property_or_throw(vm, &property_key, &mut descriptor)?;
            }
        }
        // 5. Else,
        else {
            // a. Assert: level is frozen.

            // b. For each element k of keys, do
            for index in 0..keys.len() {
                let key = keys.get(index).expect("the index is in bounds");
                let property_key = PropertyKey::from_value(vm, key).must();

                // i. Let currentDesc be ? O.[[GetOwnProperty]](k).
                let current_descriptor = self.internal_get_own_property(vm, &property_key)?;

                // ii. If currentDesc is not undefined, then
                let Some(current_descriptor) = current_descriptor else {
                    continue;
                };

                // 1. If IsAccessorDescriptor(currentDesc) is true, then
                let mut descriptor = if current_descriptor.is_accessor_descriptor() {
                    // a. Let desc be the PropertyDescriptor { [[Configurable]]: false }.
                    PropertyDescriptor {
                        configurable: Some(false),
                        ..Default::default()
                    }
                }
                // 2. Else,
                else {
                    // a. Let desc be the PropertyDescriptor { [[Configurable]]: false, [[Writable]]: false }.
                    PropertyDescriptor {
                        writable: Some(false),
                        configurable: Some(false),
                        ..Default::default()
                    }
                };

                // 3. Perform ? DefinePropertyOrThrow(O, k, desc).
                self.define_property_or_throw(vm, &property_key, &mut descriptor)?;
            }
        }

        // 6. Return true.
        Ok(true)
    }

    // 7.3.17 TestIntegrityLevel ( O, level ), https://tc39.es/ecma262/#sec-testintegritylevel
    pub fn test_integrity_level(&self, vm: &Vm, level: IntegrityLevel) -> ThrowCompletionOr<bool> {
        // 1. Let extensible be ? IsExtensible(O).
        let extensible = self.is_extensible(vm)?;

        // 2. If extensible is true, return false.
        // 3. NOTE: If the object is extensible, none of its properties are examined.
        if extensible {
            return Ok(false);
        }

        // 4. Let keys be ? O.[[OwnPropertyKeys]]().
        let keys = self.internal_own_property_keys(vm)?;

        // 5. For each element k of keys, do
        for index in 0..keys.len() {
            let key = keys.get(index).expect("the index is in bounds");
            let property_key = PropertyKey::from_value(vm, key).must();

            // a. Let currentDesc be ? O.[[GetOwnProperty]](k).
            let current_descriptor = self.internal_get_own_property(vm, &property_key)?;

            // b. If currentDesc is not undefined, then
            let Some(current_descriptor) = current_descriptor else {
                continue;
            };
            // i. If currentDesc.[[Configurable]] is true, return false.
            if current_descriptor
                .configurable
                .expect("a property descriptor of an own property is complete")
            {
                return Ok(false);
            }

            // ii. If level is frozen and IsDataDescriptor(currentDesc) is true, then
            if level == IntegrityLevel::Frozen && current_descriptor.is_data_descriptor() {
                // 1. If currentDesc.[[Writable]] is true, return false.
                if current_descriptor
                    .writable
                    .expect("a data descriptor of an own property is complete")
                {
                    return Ok(false);
                }
            }
        }

        // 6. Return true.
        Ok(true)
    }

    // 7.3.24 EnumerableOwnPropertyNames ( O, kind ), https://tc39.es/ecma262/#sec-enumerableownpropertynames
    pub fn enumerable_own_property_names<'vm>(
        &self,
        vm: &'vm Vm,
        kind: PropertyKind,
    ) -> ThrowCompletionOr<MarkedVec<'vm, Value>> {
        // NOTE: This has been flattened for readability, so some `else` branches in the
        //       spec text have been replaced with `continue`s in the loop below.

        // 1. Let ownKeys be ? O.[[OwnPropertyKeys]]().

        // 2. Let properties be a new empty List.
        let properties = MarkedVec::with_capacity(vm, self.own_properties_count());

        let pre_iteration_shape = self.shape();
        let pre_iteration_dictionary_generation = pre_iteration_shape.dictionary_generation();
        self.for_each_own_property_with_enumerability(vm, |property_key, enumerable| {
            // a. If Type(key) is String, then
            // i. Let desc be ? O.[[GetOwnProperty]](key).
            // ii. If desc is not undefined and desc.[[Enumerable]] is true, then
            // NOTE: If the object's shape has been mutated during iteration through own properties
            //       by executing a getter, we can no longer assume that subsequent properties
            //       are still present and enumerable.
            if self.shape() == pre_iteration_shape
                && self.shape().dictionary_generation() == pre_iteration_dictionary_generation
            {
                if !enumerable {
                    return Ok(());
                }
            } else {
                let descriptor = self.internal_get_own_property(vm, property_key)?;
                if descriptor.is_none_or(|descriptor| {
                    !descriptor
                        .enumerable
                        .expect("a property descriptor of an own property is complete")
                }) {
                    return Ok(());
                }
            }

            // 1. If kind is key, append key to properties.
            if kind == PropertyKind::Key {
                // 1. If kind is key, append key to properties.
                properties.push(property_key.to_value(vm));
                return Ok(());
            }

            // 2. Else,
            // a. Let value be ? Get(O, key).
            let value = self.get(vm, property_key)?;

            // b. If kind is value, append value to properties.
            if kind == PropertyKind::Value {
                properties.push(value);
                return Ok(());
            }

            // c. Else,
            // i. Assert: kind is key+value.
            assert!(kind == PropertyKind::KeyAndValue);

            // ii. Let entry be CreateArrayFromList(« key, value »).
            let realm = vm
                .current_realm()
                .expect("there is a current realm to create the entry in");
            let entry = Array::create_from(vm, realm, &[property_key.to_value(vm), value]);

            // iii. Append entry to properties.
            properties.push(Value::from_object(entry));

            Ok(())
        })?;

        // 4. Return properties.
        Ok(properties)
    }

    // 7.3.26 CopyDataProperties ( target, source, excludedItems ), https://tc39.es/ecma262/#sec-copydataproperties
    // 14.6 CopyDataProperties ( target, source, excludedItems, excludedKeys [ , excludedValues ] ), https://tc39.es/proposal-temporal/#sec-copydataproperties
    pub fn copy_data_properties(
        &self,
        vm: &Vm,
        source: Value,
        excluded_keys: &MarkedVec<'_, PropertyKey>,
        excluded_values: &MarkedVec<'_, Value>,
    ) -> ThrowCompletionOr<()> {
        let is_excluded_key = |property_key: &PropertyKey| {
            (0..excluded_keys.len()).any(|index| excluded_keys.get(index).as_ref() == Some(property_key))
        };

        // 1. If source is either undefined or null, return unused.
        if source.is_nullish() {
            return Ok(());
        }

        // 2. Let from be ! ToObject(source).
        let from = source.to_object(vm).must();

        // OPTIMIZATION: An empty ordinary object can reuse the shape of a compatible ordinary source
        //               and copy its property storage directly. This is equivalent to defining each
        //               property because all source properties already have the default attributes.
        let current_realm = || vm.current_realm().expect("there is a current realm");
        if from != self.as_gc()
            && excluded_keys.is_empty()
            && excluded_values.is_empty()
            && self.shape() == current_realm().new_object_shape()
            && self.indexed_storage_kind() == IndexedStorageKind::None
            && self.extensible()
            && self.eligible_for_own_property_enumeration_fast_path()
            && !self.has_intrinsic_accessors()
            && !self.may_interfere_with_indexed_property_access()
            && !self.requires_slow_add_own_property()
            && from.indexed_storage_kind() == IndexedStorageKind::None
            && from.eligible_for_own_property_enumeration_fast_path()
            && !from.has_intrinsic_accessors()
            && !from.may_interfere_with_indexed_property_access()
            && !from.requires_slow_add_own_property()
            && !from.shape().is_dictionary()
            && !from.shape().is_prototype_shape()
            && from.shape().realm() == current_realm()
            && from.shape().prototype() == self.shape().prototype()
        {
            let mut has_only_default_data_properties = true;
            from.shape().for_each_property_in_insertion_order(|_, metadata| {
                if metadata.attributes != DEFAULT_ATTRIBUTES || from.get_direct(metadata.offset).is_accessor() {
                    has_only_default_data_properties = false;
                    return ControlFlow::Break(());
                }
                ControlFlow::Continue(())
            });

            if has_only_default_data_properties {
                self.unsafe_set_shape(from.shape());
                from.shape().for_each_property_in_insertion_order(|_, metadata| {
                    self.put_direct(metadata.offset, from.get_direct(metadata.offset));
                    ControlFlow::Continue(())
                });
                return Ok(());
            }
        }

        // OPTIMIZATION: For ordinary objects we can iterate the shape directly and read values by storage
        //               offset, avoiding repeated property lookups through DescriptorArray::find.
        if from.eligible_for_own_property_enumeration_fast_path()
            && !from.has_intrinsic_accessors()
            && !from.may_interfere_with_indexed_property_access()
            && excluded_values.is_empty()
            && matches!(
                from.indexed_storage_kind(),
                IndexedStorageKind::None | IndexedStorageKind::Packed
            )
        {
            let mut has_accessors = false;
            from.shape().for_each_property_in_insertion_order(|_, metadata| {
                if metadata.attributes.is_enumerable() && from.get_direct(metadata.offset).is_accessor() {
                    has_accessors = true;
                    return ControlFlow::Break(());
                }
                ControlFlow::Continue(())
            });

            if !has_accessors {
                let available_elements = from.indexed_packed_element_count();
                for index in 0..available_elements {
                    let property_key = PropertyKey::from(index);
                    if !excluded_keys.is_empty() && is_excluded_key(&property_key) {
                        continue;
                    }
                    self.create_data_property_or_throw(vm, &property_key, from.indexed_element(index))
                        .must();
                }

                // Defining the properties can allocate, which must not happen while the shape is borrowed, so the
                // properties are read out first. Reading them has no side effects.
                let properties = MarkedVec::with_capacity(vm, from.shape().property_count() as usize);
                from.shape()
                    .for_each_property_in_insertion_order(|property_key, metadata| {
                        if !metadata.attributes.is_enumerable() {
                            return ControlFlow::Continue(());
                        }
                        if !excluded_keys.is_empty() && is_excluded_key(property_key) {
                            return ControlFlow::Continue(());
                        }
                        properties.push((property_key.clone(), from.get_direct(metadata.offset)));
                        ControlFlow::Continue(())
                    });
                for index in 0..properties.len() {
                    let (property_key, value) = properties.get(index).expect("the index is in bounds");
                    self.create_data_property_or_throw(vm, &property_key, value).must();
                }

                return Ok(());
            }
        }

        // 3. Let keys be ? from.[[OwnPropertyKeys]]().
        let keys = from.internal_own_property_keys(vm)?;

        // 4. For each element nextKey of keys, do
        for index in 0..keys.len() {
            let next_key_value = keys.get(index).expect("the index is in bounds");
            let next_key = PropertyKey::from_value(vm, next_key_value).must();

            // a. Let excluded be false.
            // b. For each element e of excludedKeys, do
            //    i. If SameValue(e, nextKey) is true, then
            //        1. Set excluded to true.
            if is_excluded_key(&next_key) {
                continue;
            }

            // c. If excluded is false, then

            // i. Let desc be ? from.[[GetOwnProperty]](nextKey).
            let descriptor = from.internal_get_own_property(vm, &next_key)?;

            // ii. If desc is not undefined and desc.[[Enumerable]] is true, then
            if let Some(descriptor) = descriptor
                && descriptor.attributes().is_enumerable()
            {
                // 1. Let propValue be ? Get(from, nextKey).
                let property_value = from.get(vm, &next_key)?;

                // 2. If excludedValues is present, then
                //     a. For each element e of excludedValues, do
                //         i. If SameValue(e, propValue) is true, then
                //             i. Set excluded to true.
                // 3. If excluded is false, Perform ! CreateDataPropertyOrThrow(target, nextKey, propValue).
                let is_excluded_value = (0..excluded_values.len()).any(|index| {
                    same_value(
                        excluded_values.get(index).expect("the index is in bounds"),
                        property_value,
                    )
                });
                if !is_excluded_value {
                    self.create_data_property_or_throw(vm, &next_key, property_value).must();
                }
            }
        }

        // 5. Return unused.
        Ok(())
    }

    // 7.3.27 PrivateElementFind ( O, P ), https://tc39.es/ecma262/#sec-privateelementfind
    /// The index of the element in [[PrivateElements]], which stays valid until another element is added.
    pub fn private_element_find(&self, name: &PrivateName) -> Option<usize> {
        let private_elements = self.private_elements.get()?;

        // 1. If O.[[PrivateElements]] contains a PrivateElement pe such that pe.[[Key]] is P, then
        //    a. Return pe.
        // 2. Return empty.
        private_elements
            .borrow()
            .iter()
            .position(|element| element.key == *name)
    }

    fn private_element_at(&self, index: usize) -> PrivateElement {
        self.private_elements
            .get()
            .expect("the object has private elements")
            .borrow()[index]
            .clone()
    }

    // 7.3.28 PrivateFieldAdd ( O, P, value ), https://tc39.es/ecma262/#sec-privatefieldadd
    pub fn private_field_add(&self, vm: &Vm, name: &PrivateName, value: Value) -> ThrowCompletionOr<()> {
        // 1. If the host is a web browser, then
        //    a. Perform ? HostEnsureCanAddPrivateElement(O).
        // NOTE: Since LibJS has no way of knowing whether it is in a browser we just always call the hook.
        (vm.host_ensure_can_add_private_element())(vm, self)?;

        // 2. Let entry be PrivateElementFind(O, P).
        // 3. If entry is not empty, throw a TypeError exception.
        if self.private_element_find(name).is_some() {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::PrivateFieldAlreadyDeclared,
                &[&display_fly_string(&name.description)],
            );
        }

        // 4. Append PrivateElement { [[Key]]: P, [[Kind]]: field, [[Value]]: value } to O.[[PrivateElements]].
        self.private_elements.ensure().borrow_mut().push(PrivateElement {
            key: name.clone(),
            kind: PrivateElementKind::Field,
            value,
        });

        // 5. Return unused.
        Ok(())
    }

    // 7.3.29 PrivateMethodOrAccessorAdd ( O, method ), https://tc39.es/ecma262/#sec-privatemethodoraccessoradd
    pub fn private_method_or_accessor_add(&self, vm: &Vm, element: PrivateElement) -> ThrowCompletionOr<()> {
        // 1. Assert: method.[[Kind]] is either method or accessor.
        assert!(element.kind == PrivateElementKind::Method || element.kind == PrivateElementKind::Accessor);

        // 2. If the host is a web browser, then
        //    a. Perform ? HostEnsureCanAddPrivateElement(O).
        // NOTE: Since LibJS has no way of knowing whether it is in a browser we just always call the hook.
        (vm.host_ensure_can_add_private_element())(vm, self)?;

        // 3. Let entry be PrivateElementFind(O, method.[[Key]]).
        // 4. If entry is not empty, throw a TypeError exception.
        if self.private_element_find(&element.key).is_some() {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::PrivateFieldAlreadyDeclared,
                &[&display_fly_string(&element.key.description)],
            );
        }

        // 5. Append method to O.[[PrivateElements]].
        self.private_elements.ensure().borrow_mut().push(element);

        // 6. Return unused.
        Ok(())
    }

    // 7.3.31 PrivateGet ( O, P ), https://tc39.es/ecma262/#sec-privateget
    pub fn private_get(&self, vm: &Vm, name: &PrivateName) -> ThrowCompletionOr<Value> {
        // 1. Let entry be PrivateElementFind(O, P).
        let entry = self.private_element_find(name);

        // 2. If entry is empty, throw a TypeError exception.
        let Some(entry) = entry else {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::PrivateFieldDoesNotExistOnObject,
                &[&display_fly_string(&name.description)],
            );
        };
        let entry = self.private_element_at(entry);

        let value = entry.value;

        // 3. If entry.[[Kind]] is either field or method, then
        if entry.kind != PrivateElementKind::Accessor {
            // a. Return entry.[[Value]].
            return Ok(value);
        }

        // Assert: entry.[[Kind]] is accessor.
        assert!(value.is_accessor());

        // 6. Let getter be entry.[[Get]].
        let getter = value.as_accessor().getter();

        // 5. If entry.[[Get]] is undefined, throw a TypeError exception.
        let Some(getter) = getter else {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::PrivateFieldGetAccessorWithoutGetter,
                &[&display_fly_string(&name.description)],
            );
        };

        // 7. Return ? Call(getter, O).
        call_function_object(vm, getter, Value::from_object(self.as_gc()), &[])
    }

    // 7.3.32 PrivateSet ( O, P, value ), https://tc39.es/ecma262/#sec-privateset
    pub fn private_set(&self, vm: &Vm, name: &PrivateName, value: Value) -> ThrowCompletionOr<()> {
        // 1. Let entry be PrivateElementFind(O, P).
        let entry_index = self.private_element_find(name);

        // 2. If entry is empty, throw a TypeError exception.
        let Some(entry_index) = entry_index else {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::PrivateFieldDoesNotExistOnObject,
                &[&display_fly_string(&name.description)],
            );
        };
        let entry = self.private_element_at(entry_index);

        // 3. If entry.[[Kind]] is field, then
        if entry.kind == PrivateElementKind::Field {
            // a. Set entry.[[Value]] to value.
            self.private_elements
                .get()
                .expect("the object has private elements")
                .borrow_mut()[entry_index]
                .value = value;
            return Ok(());
        }
        // 4. Else if entry.[[Kind]] is method, then
        else if entry.kind == PrivateElementKind::Method {
            // a. Throw a TypeError exception.
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::PrivateFieldSetMethod,
                &[&display_fly_string(&name.description)],
            );
        }

        // 5. Else,

        // a. Assert: entry.[[Kind]] is accessor.
        assert!(entry.kind == PrivateElementKind::Accessor);

        let accessor = entry.value;
        assert!(accessor.is_accessor());

        // c. Let setter be entry.[[Set]].
        let setter = accessor.as_accessor().setter();

        // b. If entry.[[Set]] is undefined, throw a TypeError exception.
        let Some(setter) = setter else {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::PrivateFieldSetAccessorWithoutSetter,
                &[&display_fly_string(&name.description)],
            );
        };

        // d. Perform ? Call(setter, O, « value »).
        call_function_object(vm, setter, Value::from_object(self.as_gc()), &[value])?;

        // 6. Return unused.
        Ok(())
    }

    // 7.3.33 DefineField ( receiver, fieldRecord ), https://tc39.es/ecma262/#sec-definefield
    pub fn define_field(&self, vm: &Vm, field: &ClassFieldDefinition) -> ThrowCompletionOr<()> {
        // 1. Let fieldName be fieldRecord.[[Name]].
        let field_name = &field.name;

        // 2. Let initializer be fieldRecord.[[Initializer]].
        let initializer = field.initializer;

        let mut init_value = Value::UNDEFINED;

        // 3. If initializer is not empty, then
        match initializer {
            // OPTIMIZATION: If the initializer is a value (from a literal), we can skip the call.
            ClassFieldInitializer::Value(initializer_value) => init_value = initializer_value,
            ClassFieldInitializer::Function(initializer_function) => {
                // a. Let initValue be ? Call(initializer, receiver).
                init_value =
                    call_function_object(vm, initializer_function.upcast(), Value::from_object(self.as_gc()), &[])?;
            }
            // 4. Else, let initValue be undefined.
            ClassFieldInitializer::Empty => {}
        }

        match field_name {
            // 5. If fieldName is a Private Name, then
            ClassElementName::PrivateName(private_name) => {
                // a. Perform ? PrivateFieldAdd(receiver, fieldName, initValue).
                self.private_field_add(vm, private_name, init_value)?;
            }
            // 6. Else,
            ClassElementName::PropertyKey(property_key) => {
                // a. Assert: IsPropertyKey(fieldName) is true.
                // b. Perform ? CreateDataPropertyOrThrow(receiver, fieldName, initValue).
                self.create_data_property_or_throw(vm, property_key, init_value)?;
            }
        }

        // 7. Return unused.
        Ok(())
    }

    // 7.3.34 InitializeInstanceElements ( O, constructor ), https://tc39.es/ecma262/#sec-initializeinstanceelements
    pub fn initialize_instance_elements(
        &self,
        vm: &Vm,
        constructor: Gc<EcmascriptFunctionObject>,
    ) -> ThrowCompletionOr<()> {
        // AD-HOC: Avoid lazy instantiation of ECMAScriptFunctionObject::ClassData.
        if !constructor.has_class_data() {
            return Ok(());
        }

        // 1. Let methods be the value of constructor.[[PrivateMethods]].
        // 2. For each PrivateElement method of methods, do
        for index in 0..constructor.private_methods_count() {
            // a. Perform ? PrivateMethodOrAccessorAdd(O, method).
            self.private_method_or_accessor_add(vm, constructor.private_method(index))?;
        }

        // 3. Let fields be the value of constructor.[[Fields]].
        // 4. For each element fieldRecord of fields, do
        for index in 0..constructor.fields_count() {
            // a. Perform ? DefineField(O, fieldRecord).
            self.define_field(vm, &constructor.field(index))?;
        }

        // 5. Return unused.
        Ok(())
    }

    // The internal methods, which dispatch through the object's class.

    pub fn internal_get_prototype_of(&self, vm: &Vm) -> ThrowCompletionOr<Option<Gc<Object>>> {
        (self.methods().internal_get_prototype_of)(self, vm)
    }

    pub fn internal_set_prototype_of(&self, vm: &Vm, prototype: Option<Gc<Object>>) -> ThrowCompletionOr<bool> {
        (self.methods().internal_set_prototype_of)(self, vm, prototype)
    }

    pub fn internal_is_extensible(&self, vm: &Vm) -> ThrowCompletionOr<bool> {
        (self.methods().internal_is_extensible)(self, vm)
    }

    pub fn internal_prevent_extensions(&self, vm: &Vm) -> ThrowCompletionOr<bool> {
        (self.methods().internal_prevent_extensions)(self, vm)
    }

    pub fn internal_get_own_property(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
    ) -> ThrowCompletionOr<Option<PropertyDescriptor>> {
        (self.methods().internal_get_own_property)(self, vm, property_key)
    }

    pub fn internal_define_own_property(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        property_descriptor: &mut PropertyDescriptor,
        precomputed_get_own_property: Option<&Option<PropertyDescriptor>>,
    ) -> ThrowCompletionOr<bool> {
        (self.methods().internal_define_own_property)(
            self,
            vm,
            property_key,
            property_descriptor,
            precomputed_get_own_property,
        )
    }

    pub fn internal_has_property(&self, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<bool> {
        (self.methods().internal_has_property)(self, vm, property_key)
    }

    pub fn internal_get(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        receiver: Value,
        cacheable_metadata: Option<&mut CacheableGetPropertyMetadata>,
        phase: PropertyLookupPhase,
    ) -> ThrowCompletionOr<Value> {
        (self.methods().internal_get)(self, vm, property_key, receiver, cacheable_metadata, phase)
    }

    pub fn internal_set(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        value: Value,
        receiver: Value,
        cacheable_metadata: Option<&mut CacheableSetPropertyMetadata>,
        phase: PropertyLookupPhase,
    ) -> ThrowCompletionOr<bool> {
        (self.methods().internal_set)(self, vm, property_key, value, receiver, cacheable_metadata, phase)
    }

    pub fn internal_delete(&self, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<bool> {
        (self.methods().internal_delete)(self, vm, property_key)
    }

    pub fn internal_own_property_keys<'vm>(&self, vm: &'vm Vm) -> ThrowCompletionOr<MarkedVec<'vm, Value>> {
        (self.methods().internal_own_property_keys)(self, vm)
    }

    pub fn is_cacheable_for_property_absence(&self) -> bool {
        (self.methods().is_cacheable_for_property_absence)(self)
    }

    pub fn is_cacheable_for_inherited_property(&self) -> bool {
        (self.methods().is_cacheable_for_inherited_property)(self)
    }

    pub fn eligible_for_own_property_enumeration_fast_path(&self) -> bool {
        (self.methods().eligible_for_own_property_enumeration_fast_path)(self)
    }

    pub fn initialize(&self, vm: &Vm, realm: Gc<Realm>) {
        (self.methods().initialize)(self, vm, realm);
    }

    pub fn as_builtin_iterator_if_next_is_not_redefined(&self, next_method: Value) -> Option<BuiltinIteratorNext> {
        (self.methods().as_builtin_iterator_if_next_is_not_redefined)(self, next_method)
    }

    pub fn has_constructor(&self) -> bool {
        (self.methods().has_constructor)(self)
    }

    pub fn is_strict_mode(&self) -> bool {
        (self.methods().is_strict_mode)(self)
    }

    pub fn get_stack_frame_info(&self, vm: &Vm, info: &mut StackFrameInfo) {
        (self.methods().get_stack_frame_info)(self, vm, info);
    }

    pub fn function_realm(&self) -> Option<Gc<Realm>> {
        (self.methods().function_realm)(self)
    }

    pub fn name_for_call_stack(&self) -> Utf16String {
        (self.methods().name_for_call_stack)(self)
    }

    pub(crate) fn native_function_methods(&self) -> Option<&'static NativeFunctionMethods> {
        self.methods().native_function
    }

    pub(crate) fn internal_call_method(&self) -> Option<InternalCall> {
        self.methods().internal_call
    }

    pub(crate) fn internal_construct_method(&self) -> Option<InternalConstruct> {
        self.methods().internal_construct
    }

    // 10.1 Ordinary Object Internal Methods and Internal Slots, https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots

    // 10.1.1 [[GetPrototypeOf]] ( ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-getprototypeof
    pub fn ordinary_get_prototype_of(&self, _vm: &Vm) -> ThrowCompletionOr<Option<Gc<Object>>> {
        // 1. Return O.[[Prototype]].
        Ok(self.prototype())
    }

    // 10.1.2 [[SetPrototypeOf]] ( V ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-setprototypeof-v
    pub fn ordinary_set_prototype_of(&self, vm: &Vm, new_prototype: Option<Gc<Object>>) -> ThrowCompletionOr<bool> {
        // 1. Let current be O.[[Prototype]].
        // 2. If SameValue(V, current) is true, return true.
        if self.prototype() == new_prototype {
            return Ok(true);
        }

        // 3. Let extensible be O.[[Extensible]].
        // 4. If extensible is false, return false.
        if !self.extensible() {
            return Ok(false);
        }

        // 5. Let p be V.
        let mut prototype = new_prototype;

        // 6. Let done be false.
        // 7. Repeat, while done is false,
        while let Some(current) = prototype {
            // a. If p is null, set done to true.

            // b. Else if SameValue(p, O) is true, return false.
            if current == self.as_gc() {
                return Ok(false);
            }
            // c. Else,

            // i. If p.[[GetPrototypeOf]] is not the ordinary object internal method defined in 10.1.1, set done to true.
            // NOTE: This is a best-effort implementation; we don't have a good way of detecting whether certain virtual
            // Object methods have been overridden by a given object, but as ProxyObject is the only one doing that for
            // [[SetPrototypeOf]], this check does the trick.
            if current.is_proxy_object() {
                break;
            }

            // ii. Else, set p to p.[[Prototype]].
            prototype = current.prototype();
        }

        // 8. Set O.[[Prototype]] to V.
        self.set_prototype(vm, new_prototype);

        // 9. Return true.
        Ok(true)
    }

    // 10.1.3 [[IsExtensible]] ( ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-isextensible
    pub fn ordinary_is_extensible(&self, _vm: &Vm) -> ThrowCompletionOr<bool> {
        // 1. Return O.[[Extensible]].
        Ok(self.extensible())
    }

    // 10.1.4 [[PreventExtensions]] ( ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-preventextensions
    pub fn ordinary_prevent_extensions(&self, _vm: &Vm) -> ThrowCompletionOr<bool> {
        // 1. Set O.[[Extensible]] to false.
        self.set_extensible(false);

        // 2. Return true.
        Ok(true)
    }

    // 10.1.5 [[GetOwnProperty]] ( P ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-getownproperty-p
    // 10.1.5.1 OrdinaryGetOwnProperty ( O, P ) https://tc39.es/ecma262/#sec-ordinarygetownproperty
    pub fn ordinary_get_own_property(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
    ) -> ThrowCompletionOr<Option<PropertyDescriptor>> {
        // 1. If O does not have an own property with key P, return undefined.
        let Some(storage_entry) = self.storage_get(vm, property_key) else {
            // AD-HOC: Report accesses to unimplemented IDL properties without making them observable to JavaScript.
            if self.is_unimplemented_property(property_key) {
                unimplemented_runtime_function("VM::on_unimplemented_property_access", 0);
            }
            return Ok(None);
        };

        // 2. Let D be a newly created Property Descriptor with no fields.
        let mut descriptor = PropertyDescriptor::default();

        // 3. Let X be O's own property whose key is P.
        let ValueAndAttributes {
            value,
            attributes,
            property_offset,
        } = storage_entry;

        // 4. If X is a data property, then
        if !value.is_accessor() {
            // a. Set D.[[Value]] to the value of X's [[Value]] attribute.
            descriptor.value = Some(value);

            // b. Set D.[[Writable]] to the value of X's [[Writable]] attribute.
            descriptor.writable = Some(attributes.is_writable());
        }
        // 5. Else,
        else {
            // a. Assert: X is an accessor property.

            // b. Set D.[[Get]] to the value of X's [[Get]] attribute.
            descriptor.get = Some(value.as_accessor().getter());

            // c. Set D.[[Set]] to the value of X's [[Set]] attribute.
            descriptor.set = Some(value.as_accessor().setter());
        }

        // 6. Set D.[[Enumerable]] to the value of X's [[Enumerable]] attribute.
        descriptor.enumerable = Some(attributes.is_enumerable());

        // 7. Set D.[[Configurable]] to the value of X's [[Configurable]] attribute.
        descriptor.configurable = Some(attributes.is_configurable());

        // Non-standard: Add the property offset to the descriptor. This is used to populate CacheablePropertyMetadata.
        descriptor.property_offset = property_offset;

        // 8. Return D.
        Ok(Some(descriptor))
    }

    // 10.1.6 [[DefineOwnProperty]] ( P, Desc ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-defineownproperty-p-desc
    // 10.1.6.1 OrdinaryDefineOwnProperty ( O, P, Desc ), https://tc39.es/ecma262/#sec-ordinarydefineownproperty
    pub fn ordinary_define_own_property(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        property_descriptor: &mut PropertyDescriptor,
        precomputed_get_own_property: Option<&Option<PropertyDescriptor>>,
    ) -> ThrowCompletionOr<bool> {
        // 1. Let current be ? O.[[GetOwnProperty]](P).
        let current = match precomputed_get_own_property {
            Some(precomputed_get_own_property) => *precomputed_get_own_property,
            None => self.internal_get_own_property(vm, property_key)?,
        };

        // 2. Let extensible be ? IsExtensible(O).
        let extensible = self.is_extensible(vm)?;

        // 3. Return ValidateAndApplyPropertyDescriptor(O, P, extensible, Desc, current).
        Ok(validate_and_apply_property_descriptor(
            vm,
            Some(self),
            property_key,
            extensible,
            property_descriptor,
            &current,
        ))
    }

    // 10.1.7 [[HasProperty]] ( P ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-hasproperty-p
    // 10.1.7.1 OrdinaryHasProperty ( O, P ), https://tc39.es/ecma262/#sec-ordinaryhasproperty
    pub fn ordinary_has_property(&self, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<bool> {
        // 1. Let hasOwn be ? O.[[GetOwnProperty]](P).
        let has_own = self.internal_get_own_property(vm, property_key)?;

        // 2. If hasOwn is not undefined, return true.
        if has_own.is_some() {
            return Ok(true);
        }

        // 3. Let parent be ? O.[[GetPrototypeOf]]().
        let parent = self.internal_get_prototype_of(vm)?;

        // 4. If parent is not null, then
        if let Some(parent) = parent {
            // a. Return ? parent.[[HasProperty]](P).
            return parent.internal_has_property(vm, property_key);
        }

        // 5. Return false.
        Ok(false)
    }

    // 10.1.8 [[Get]] ( P, Receiver ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-get-p-receiver
    // 10.1.8.1 OrdinaryGet ( O, P, Receiver ), https://tc39.es/ecma262/#sec-ordinaryget
    pub fn ordinary_get(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        receiver: Value,
        mut cacheable_metadata: Option<&mut CacheableGetPropertyMetadata>,
        phase: PropertyLookupPhase,
    ) -> ThrowCompletionOr<Value> {
        assert!(!receiver.is_empty());

        // 1. Let desc be ? O.[[GetOwnProperty]](P).
        let descriptor = self.internal_get_own_property(vm, property_key)?;

        // 2. If desc is undefined, then
        let Some(descriptor) = descriptor else {
            // a. Let parent be ? O.[[GetPrototypeOf]]().
            let parent = self.internal_get_prototype_of(vm)?;

            // b. If parent is null, return undefined.
            let Some(parent) = parent else {
                if let Some(cacheable_metadata) = cacheable_metadata
                    && cacheable_metadata.property_absence_is_cacheable
                {
                    cacheable_metadata.r#type = CacheableGetPropertyMetadataType::GetMissingProperty;
                }
                return Ok(Value::UNDEFINED);
            };

            // c. Return ? parent.[[Get]](P, Receiver).
            // AD-HOC: Avoid a native stack overflow when walking a pathologically-deep prototype chain.
            if vm.did_reach_stack_space_limit() {
                return vm.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
            }
            if let Some(cacheable_metadata) = cacheable_metadata.as_deref_mut()
                && !parent.is_cacheable_for_property_absence()
            {
                cacheable_metadata.property_absence_is_cacheable = false;
            }
            // NB: An object whose own lookup can start answering for a name at any time cannot vouch for a
            //     result found further up the prototype chain.
            if cacheable_metadata.is_some() && !self.is_cacheable_for_inherited_property() {
                cacheable_metadata = None;
            }
            return parent.internal_get(
                vm,
                property_key,
                receiver,
                cacheable_metadata,
                PropertyLookupPhase::PrototypeChain,
            );
        };

        let mut update_inline_cache = |property_offset: u32| {
            // Non-standard: If the caller has requested cacheable metadata and the property is an own property, fill it in.
            let Some(cacheable_metadata) = cacheable_metadata.as_deref_mut() else {
                return;
            };
            match phase {
                PropertyLookupPhase::OwnProperty => {
                    *cacheable_metadata = CacheableGetPropertyMetadata {
                        r#type: CacheableGetPropertyMetadataType::GetOwnProperty,
                        property_offset: Some(property_offset),
                        prototype: None,
                        ..Default::default()
                    };
                }
                PropertyLookupPhase::PrototypeChain => {
                    assert!(self.shape().is_prototype_shape());
                    assert!(
                        self.shape()
                            .prototype_chain_validity()
                            .expect("a prototype shape has a validity")
                            .is_valid()
                    );
                    *cacheable_metadata = CacheableGetPropertyMetadata {
                        r#type: CacheableGetPropertyMetadataType::GetPropertyInPrototypeChain,
                        property_offset: Some(property_offset),
                        prototype: Some(self.as_gc()),
                        ..Default::default()
                    };
                }
            }
        };

        // 3. If IsDataDescriptor(desc) is true, return desc.[[Value]].
        if descriptor.is_data_descriptor() {
            if let Some(property_offset) = descriptor.property_offset {
                update_inline_cache(property_offset);
            }
            return Ok(descriptor
                .value
                .expect("a data descriptor of an own property has a value"));
        }

        // 4. Assert: IsAccessorDescriptor(desc) is true.
        assert!(descriptor.is_accessor_descriptor());

        // 5. Let getter be desc.[[Get]].
        let getter = descriptor
            .get
            .expect("an accessor descriptor of an own property has a getter");

        // 6. If getter is undefined, return undefined.
        let Some(getter) = getter else {
            return Ok(Value::UNDEFINED);
        };

        let mut accessor = None;
        let mut receiver_uses_holder_cache = false;
        if let Some(property_offset) = descriptor.property_offset {
            let value = self.get_direct(property_offset);
            if value.is_accessor() {
                let holder_accessor = value.as_accessor();
                accessor = Some(holder_accessor);
                receiver_uses_holder_cache = receiver.is_object()
                    && (receiver.as_object() == self.as_gc() || receiver.as_object().prototype() == Some(self.as_gc()));
                if let Some(cached_value_key) = holder_accessor.cached_value_key()
                    && receiver_uses_holder_cache
                    && let Some(cached_value) = self.get_engine_private_property(cached_value_key)
                {
                    update_inline_cache(
                        cached_value
                            .property_offset
                            .expect("an engine private property is a named property"),
                    );
                    return Ok(cached_value.value);
                }
            }

            update_inline_cache(property_offset);
        }

        // 7. Return ? Call(getter, Receiver).
        let result = call_function_object(vm, getter, receiver, &[])?;

        if let Some(accessor) = accessor
            && receiver_uses_holder_cache
            && let Some(cached_value_key) = accessor.cached_value_key()
        {
            let property = self.storage_get(vm, property_key);
            if property.is_some_and(|property| property.value.is_accessor() && property.value.as_accessor() == accessor)
            {
                self.set_engine_private_property(vm, cached_value_key, result);
            }
        }

        Ok(result)
    }

    // 10.1.9 [[Set]] ( P, V, Receiver ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-set-p-v-receiver
    // 10.1.9.1 OrdinarySet ( O, P, V, Receiver ), https://tc39.es/ecma262/#sec-ordinaryset
    pub fn ordinary_set(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        value: Value,
        receiver: Value,
        cacheable_metadata: Option<&mut CacheableSetPropertyMetadata>,
        phase: PropertyLookupPhase,
    ) -> ThrowCompletionOr<bool> {
        assert!(!value.is_empty());
        assert!(!receiver.is_empty());

        // 2. Let ownDesc be ? O.[[GetOwnProperty]](P).
        let own_descriptor = self.internal_get_own_property(vm, property_key)?;

        // OPTIMIZATION: Walk the prototype chain without recursing through [[Set]], so a property new to O is added without
        //               asking O for P again.
        if own_descriptor.is_none() && receiver.is_object() && receiver.as_object() == self.as_gc() {
            return self.set_through_prototype_chain(vm, property_key, value, cacheable_metadata);
        }

        // 3. Return ? OrdinarySetWithOwnDescriptor(O, P, V, Receiver, ownDesc).
        self.ordinary_set_with_own_descriptor(
            vm,
            property_key,
            value,
            receiver,
            own_descriptor,
            cacheable_metadata,
            phase,
        )
    }

    fn set_through_prototype_chain(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        value: Value,
        cacheable_metadata: Option<&mut CacheableSetPropertyMetadata>,
    ) -> ThrowCompletionOr<bool> {
        let this_value = Value::from_object(self.as_gc());
        let mut prototype = self.internal_get_prototype_of(vm)?;
        while let Some(current) = prototype {
            if !current.is_cacheable_for_property_absence() {
                return current.internal_set(
                    vm,
                    property_key,
                    value,
                    this_value,
                    cacheable_metadata,
                    PropertyLookupPhase::PrototypeChain,
                );
            }

            let prototype_descriptor = current.internal_get_own_property(vm, property_key)?;
            if prototype_descriptor.is_some() {
                return current.ordinary_set_with_own_descriptor(
                    vm,
                    property_key,
                    value,
                    this_value,
                    prototype_descriptor,
                    cacheable_metadata,
                    PropertyLookupPhase::PrototypeChain,
                );
            }
            prototype = current.internal_get_prototype_of(vm)?;
        }

        create_data_property_for_set(vm, self, property_key, value, cacheable_metadata)
    }

    // 10.1.9.2 OrdinarySetWithOwnDescriptor ( O, P, V, Receiver, ownDesc ), https://tc39.es/ecma262/#sec-ordinarysetwithowndescriptor
    #[allow(clippy::too_many_arguments)]
    pub fn ordinary_set_with_own_descriptor(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        value: Value,
        receiver: Value,
        own_descriptor: Option<PropertyDescriptor>,
        cacheable_metadata: Option<&mut CacheableSetPropertyMetadata>,
        phase: PropertyLookupPhase,
    ) -> ThrowCompletionOr<bool> {
        assert!(!value.is_empty());
        assert!(!receiver.is_empty());

        let own_descriptor_was_undefined = own_descriptor.is_none();

        // 1. If ownDesc is undefined, then
        let own_descriptor = match own_descriptor {
            Some(own_descriptor) => own_descriptor,
            None => {
                // a. Let parent be ? O.[[GetPrototypeOf]]().
                let parent = self.internal_get_prototype_of(vm)?;

                // b. If parent is not null, then
                if let Some(parent) = parent {
                    // i. Return ? parent.[[Set]](P, V, Receiver).
                    return parent.internal_set(
                        vm,
                        property_key,
                        value,
                        receiver,
                        cacheable_metadata,
                        PropertyLookupPhase::PrototypeChain,
                    );
                }
                // c. Else,
                // i. Set ownDesc to the PropertyDescriptor { [[Value]]: undefined, [[Writable]]: true, [[Enumerable]]: true, [[Configurable]]: true }.
                PropertyDescriptor {
                    value: Some(Value::UNDEFINED),
                    writable: Some(true),
                    enumerable: Some(true),
                    configurable: Some(true),
                    ..Default::default()
                }
            }
        };

        let update_inline_cache_for_property_change =
            |cacheable_metadata: Option<&mut CacheableSetPropertyMetadata>, writes_data_property: bool| {
                // Non-standard: If the caller has requested cacheable metadata and the property is an own property, fill it in.
                let (Some(cacheable_metadata), Some(property_offset)) =
                    (cacheable_metadata, own_descriptor.property_offset)
                else {
                    return;
                };
                match phase {
                    PropertyLookupPhase::OwnProperty => {
                        *cacheable_metadata = CacheableSetPropertyMetadata {
                            r#type: CacheableSetPropertyMetadataType::ChangeOwnProperty,
                            property_offset: Some(property_offset),
                            prototype: None,
                            writes_data_property,
                        };
                    }
                    PropertyLookupPhase::PrototypeChain => {
                        assert!(self.shape().is_prototype_shape());
                        assert!(
                            self.shape()
                                .prototype_chain_validity()
                                .expect("a prototype shape has a validity")
                                .is_valid()
                        );
                        *cacheable_metadata = CacheableSetPropertyMetadata {
                            r#type: CacheableSetPropertyMetadataType::ChangePropertyInPrototypeChain,
                            property_offset: Some(property_offset),
                            prototype: Some(self.as_gc()),
                            writes_data_property,
                        };
                    }
                }
            };

        // 2. If IsDataDescriptor(ownDesc) is true, then
        if own_descriptor.is_data_descriptor() {
            // a. If ownDesc.[[Writable]] is false, return false.
            if !own_descriptor
                .writable
                .expect("a data descriptor of an own property is complete")
            {
                return Ok(false);
            }

            // b. If Receiver is not an Object, return false.
            if !receiver.is_object() {
                return Ok(false);
            }

            let receiver_object = receiver.as_object();

            // c. Let existingDescriptor be ? Receiver.[[GetOwnProperty]](P).
            // OPTIMIZATION: If we were called with an ownDescriptor, and receiver == this, don't do [[GetOwnProperty]] again.
            let existing_descriptor = if !own_descriptor_was_undefined && receiver_object == self.as_gc() {
                Some(own_descriptor)
            } else {
                receiver_object.internal_get_own_property(vm, property_key)?
            };

            // d. If existingDescriptor is not undefined, then
            if let Some(existing) = existing_descriptor {
                // i. If IsAccessorDescriptor(existingDescriptor) is true, return false.
                if existing.is_accessor_descriptor() {
                    return Ok(false);
                }

                // ii. If existingDescriptor.[[Writable]] is false, return false.
                if !existing
                    .writable
                    .expect("a data descriptor of an own property is complete")
                {
                    return Ok(false);
                }

                // iii. Let valueDesc be the PropertyDescriptor { [[Value]]: V }.
                let mut value_descriptor = PropertyDescriptor {
                    value: Some(value),
                    ..Default::default()
                };

                // NOTE: We don't cache non-setter properties in the prototype chain, as that's a weird
                //       use-case, and doesn't seem like something in need of optimization.
                if phase == PropertyLookupPhase::OwnProperty {
                    update_inline_cache_for_property_change(cacheable_metadata, true);
                }

                // iv. Return ? Receiver.[[DefineOwnProperty]](P, valueDesc).
                return receiver_object.internal_define_own_property(
                    vm,
                    property_key,
                    &mut value_descriptor,
                    Some(&existing_descriptor),
                );
            }
            // e. Else,
            else {
                // i. Assert: Receiver does not currently have a property P.
                assert!(!receiver_object.storage_has(property_key));

                // ii. Return ? CreateDataProperty(Receiver, P, V).
                return create_data_property_for_set(vm, &receiver_object, property_key, value, cacheable_metadata);
            }
        }

        // 3. Assert: IsAccessorDescriptor(ownDesc) is true.
        assert!(own_descriptor.is_accessor_descriptor());

        // 4. Let setter be ownDesc.[[Set]].
        let setter = own_descriptor
            .set
            .expect("an accessor descriptor of an own property has a setter");

        // 5. If setter is undefined, return false.
        let Some(setter) = setter else {
            return Ok(false);
        };

        update_inline_cache_for_property_change(cacheable_metadata, false);

        // 6. Perform ? Call(setter, Receiver, « V »).
        call_function_object(vm, setter, receiver, &[value])?;

        // 7. Return true.
        Ok(true)
    }

    // 10.1.10 [[Delete]] ( P ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-delete-p
    // 10.1.10.1 OrdinaryDelete ( O, P ), https://tc39.es/ecma262/#sec-ordinarydelete
    pub fn ordinary_delete(&self, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<bool> {
        // 1. Let desc be ? O.[[GetOwnProperty]](P).
        let descriptor = self.internal_get_own_property(vm, property_key)?;

        // 2. If desc is undefined, return true.
        let Some(descriptor) = descriptor else {
            return Ok(true);
        };

        // 3. If desc.[[Configurable]] is true, then
        if descriptor
            .configurable
            .expect("a property descriptor of an own property is complete")
        {
            // a. Remove the own property with name P from O.
            self.storage_delete(vm, property_key);

            // b. Return true.
            return Ok(true);
        }

        // 4. Return false.
        Ok(false)
    }

    // 10.1.11 [[OwnPropertyKeys]] ( ), https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots-ownpropertykeys
    pub fn ordinary_own_property_keys<'vm>(&self, vm: &'vm Vm) -> ThrowCompletionOr<MarkedVec<'vm, Value>> {
        // 1. Let keys be a new empty List.
        let keys = MarkedVec::new(vm);

        // 2. For each own property key P of O such that P is an array index, in ascending numeric index order, do
        {
            let indices = self.indexed_indices();
            for index in indices {
                // a. Add P as the last element of keys.
                keys.push(Value::from_string(PrimitiveString::create_from_unsigned_integer(
                    vm,
                    u64::from(index),
                )));
            }
        }

        // The keys are copied out first, since turning them into values allocates.
        let string_keys = MarkedVec::new(vm);
        let symbol_keys = MarkedVec::new(vm);
        self.shape().for_each_property_in_insertion_order(|property_key, _| {
            if property_key.is_string() {
                string_keys.push(property_key.clone());
            } else if property_key.is_symbol() && !property_key.is_private() {
                symbol_keys.push(property_key.clone());
            }
            ControlFlow::Continue(())
        });

        // 3. For each own property key P of O such that Type(P) is String and P is not an array index, in ascending chronological order of property creation, do
        for index in 0..string_keys.len() {
            let property_key: PropertyKey = string_keys.get(index).expect("the index is in bounds");
            // a. Add P as the last element of keys.
            keys.push(property_key.to_value(vm));
        }

        // 4. For each own property key P of O such that Type(P) is Symbol, in ascending chronological order of property creation, do
        for index in 0..symbol_keys.len() {
            let property_key: PropertyKey = symbol_keys.get(index).expect("the index is in bounds");
            // a. Add P as the last element of keys.
            keys.push(property_key.to_value(vm));
        }

        // 5. Return keys.
        Ok(keys)
    }

    // 10.4.7.2 SetImmutablePrototype ( O, V ), https://tc39.es/ecma262/#sec-set-immutable-prototype
    pub fn set_immutable_prototype(&self, vm: &Vm, prototype: Option<Gc<Object>>) -> ThrowCompletionOr<bool> {
        // 1. Let current be ? O.[[GetPrototypeOf]]().
        let current = self.internal_get_prototype_of(vm)?;

        // 2. If SameValue(V, current) is true, return true.
        // 3. Return false.
        Ok(prototype == current)
    }

    // Implementation-specific storage abstractions

    pub fn storage_get(&self, vm: &Vm, property_key: &PropertyKey) -> Option<ValueAndAttributes> {
        if property_key.is_number()
            && let Some(value_and_attributes) = self.indexed_get(property_key.as_number())
        {
            return Some(ValueAndAttributes {
                value: value_and_attributes.value,
                attributes: value_and_attributes.attributes,
                property_offset: None,
            });
        }

        let metadata = self.shape().lookup(property_key)?;

        if self.has_intrinsic_accessors()
            && let Some(accessor) = find_intrinsic_accessor(vm, self, property_key)
        {
            let value = accessor(vm, self.shape().realm());
            self.put_direct(metadata.offset, value);
        }

        Some(ValueAndAttributes {
            value: self.get_direct(metadata.offset),
            attributes: metadata.attributes,
            property_offset: Some(metadata.offset),
        })
    }

    /// storage_get() for a symbol key, which never has an intrinsic accessor.
    fn storage_get_symbol(&self, property_key: &PropertyKey) -> Option<ValueAndAttributes> {
        assert!(property_key.is_symbol());
        let metadata = self.shape().lookup(property_key)?;
        Some(ValueAndAttributes {
            value: self.get_direct(metadata.offset),
            attributes: metadata.attributes,
            property_offset: Some(metadata.offset),
        })
    }

    pub fn storage_has(&self, property_key: &PropertyKey) -> bool {
        if property_key.is_number() && self.indexed_has(property_key.as_number()) {
            return true;
        }
        self.shape().lookup(property_key).is_some()
    }

    pub fn storage_set(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        value_and_attributes: ValueAndAttributes,
    ) -> Option<u32> {
        let ValueAndAttributes { value, attributes, .. } = value_and_attributes;

        if property_key.is_number() && self.indexed_has(property_key.as_number()) {
            self.indexed_put(property_key.as_number(), value, attributes);
            return None;
        }

        let Some(metadata) = self.shape().lookup(property_key) else {
            return self.storage_add(vm, property_key, value_and_attributes);
        };

        if self.has_intrinsic_accessors() && property_key.is_string() {
            remove_intrinsic_accessor(vm, self, property_key);
        }

        if attributes != metadata.attributes {
            let shape = self.shape();
            if shape.is_dictionary() {
                shape.set_property_attributes_without_transition(vm, property_key, attributes);
            } else {
                self.shape
                    .set(shape.create_configure_transition(vm, property_key, attributes));
            }
        }

        self.put_direct(metadata.offset, value);
        Some(metadata.offset)
    }

    pub fn storage_add(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        value_and_attributes: ValueAndAttributes,
    ) -> Option<u32> {
        debug_assert!(!self.storage_has(property_key));
        let ValueAndAttributes { value, attributes, .. } = value_and_attributes;

        if property_key.is_number() {
            self.indexed_put(property_key.as_number(), value, attributes);
            return None;
        }

        if !self.shape().is_dictionary()
            && self.shape().property_count() >= MAX_TRANSITIONS_BEFORE_CONVERTING_TO_DICTIONARY
        {
            self.shape.set(self.shape().create_dictionary_transition(vm));
        }

        let shape = self.shape();
        if shape.is_dictionary() {
            shape.add_property_without_transition(vm, property_key, attributes);
        } else {
            self.shape
                .set(shape.create_put_transition(vm, property_key, attributes));
        }
        let new_offset = self.shape().property_count() - 1;
        self.ensure_named_storage_capacity(self.shape().property_count());
        self.put_direct(new_offset, value);
        Some(new_offset)
    }

    pub fn storage_delete(&self, vm: &Vm, property_key: &PropertyKey) {
        assert!(self.storage_has(property_key));

        if property_key.is_number() && self.indexed_has(property_key.as_number()) {
            self.indexed_delete(property_key.as_number());
            return;
        }

        if self.has_intrinsic_accessors() && property_key.is_string() {
            remove_intrinsic_accessor(vm, self, property_key);
        }

        let metadata = self.shape().lookup(property_key).expect("the object has the property");

        let shape = self.shape();
        if shape.is_dictionary() {
            shape.remove_property_without_transition(vm, property_key, metadata.offset);
        } else {
            self.shape.set(shape.create_delete_transition(vm, property_key));
        }
        // Shift remaining properties down to fill the gap.
        let remaining = self.shape().property_count() - metadata.offset;
        if remaining > 0 {
            assert!(metadata.offset + remaining < self.named_storage_capacity());
            let named_properties = self.named_properties.get();
            // SAFETY: Both ranges lie within the named storage, whose capacity was checked above.
            unsafe {
                core::ptr::copy(
                    named_properties.add(metadata.offset as usize + 1),
                    named_properties.add(metadata.offset as usize),
                    remaining as usize,
                );
            }
        }
    }

    pub fn set_prototype(&self, vm: &Vm, new_prototype: Option<Gc<Object>>) {
        if self.prototype() == new_prototype {
            return;
        }
        self.shape
            .set(self.shape().create_prototype_transition(vm, new_prototype));
    }

    // Non-standard methods

    pub fn for_each_own_property_with_enumerability(
        &self,
        vm: &Vm,
        mut callback: impl FnMut(&PropertyKey, bool) -> ThrowCompletionOr<()>,
    ) -> ThrowCompletionOr<()> {
        if self.eligible_for_own_property_enumeration_fast_path() {
            let keys = MarkedVec::with_capacity(
                vm,
                self.indexed_real_size()
                    + self.shape().property_count() as usize
                    + usize::from(self.has_magical_length_property()),
            );

            {
                let indices = self.indexed_indices();
                for index in indices {
                    let mut enumerable = true;
                    if self.indexed_storage_kind() == IndexedStorageKind::Dictionary
                        && let Some(result) = self.indexed_dictionary().borrow().get(index)
                    {
                        enumerable = result.attributes.is_enumerable();
                    }
                    keys.push((PropertyKey::from(index), enumerable));
                }
            }

            if self.has_magical_length_property() {
                keys.push((vm.names.length.clone(), false));
            }

            self.shape()
                .for_each_property_in_insertion_order(|property_key, metadata| {
                    if property_key.is_string() {
                        keys.push((property_key.clone(), metadata.attributes.is_enumerable()));
                    }
                    ControlFlow::Continue(())
                });

            for index in 0..keys.len() {
                let (property_key, enumerable) = keys.get(index).expect("the index is in bounds");
                callback(&property_key, enumerable)?;
            }
        } else {
            let keys = self.internal_own_property_keys(vm)?;
            for index in 0..keys.len() {
                let key = keys.get(index).expect("the index is in bounds");
                let property_key = PropertyKey::from_value(vm, key)?;
                if property_key.is_symbol() {
                    continue;
                }
                let descriptor = self.internal_get_own_property(vm, &property_key)?;
                let mut enumerable = false;
                if let Some(descriptor) = descriptor {
                    enumerable = descriptor
                        .enumerable
                        .expect("a property descriptor of an own property is complete");
                }
                callback(&property_key, enumerable)?;
            }
        }
        Ok(())
    }

    pub fn own_properties_count(&self) -> usize {
        self.indexed_real_size()
            + self.shape().property_count() as usize
            + usize::from(self.has_magical_length_property())
    }

    // Simple side-effect free property lookup, following the prototype chain. Non-standard.
    pub fn get_without_side_effects(&self, vm: &Vm, property_key: &PropertyKey) -> Value {
        let mut object = Some(self.as_gc());
        while let Some(current) = object {
            if let Some(value_and_attributes) = current.storage_get(vm, property_key) {
                return value_and_attributes.value;
            }
            object = current.prototype();
        }
        Value::UNDEFINED
    }

    #[allow(clippy::too_many_arguments)]
    pub fn define_native_function(
        &self,
        vm: &Vm,
        realm: Gc<Realm>,
        property_key: &PropertyKey,
        native_function: RawNativeFunctionPointer,
        length: i32,
        attribute: PropertyAttributes,
        builtin: Option<Builtin>,
    ) {
        let function = RawNativeFunction::create(vm, native_function, length, property_key, Some(realm), None, builtin);
        self.define_direct_property(vm, property_key, Value::from_object(function), attribute);
    }

    /// define_native_function() with a native function whose behaviour captures state, see NativeFunction::create().
    #[allow(clippy::too_many_arguments)]
    pub fn define_capturing_native_function<C, F>(
        &self,
        vm: &Vm,
        realm: Gc<Realm>,
        property_key: &PropertyKey,
        captures: C,
        native_function: F,
        length: i32,
        attribute: PropertyAttributes,
        builtin: Option<Builtin>,
    ) where
        C: Trace + 'static,
        F: Fn(&Vm, &C) -> ThrowCompletionOr<Value> + 'static,
    {
        let function = NativeFunction::create(
            vm,
            captures,
            native_function,
            length,
            property_key,
            Some(realm),
            None,
            builtin,
        );
        self.define_direct_property(vm, property_key, Value::from_object(function), attribute);
    }

    pub fn define_direct_property(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        value: Value,
        attributes: PropertyAttributes,
    ) {
        self.storage_set(vm, property_key, ValueAndAttributes::new(value, attributes));
    }

    /// Defines a property whose value an intrinsic accessor computes the first time the property is read, so that the
    /// intrinsics a realm's global object exposes are only created when a script uses them.
    pub fn define_intrinsic_accessor(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        attributes: PropertyAttributes,
        accessor: IntrinsicAccessor,
    ) {
        assert!(property_key.is_string());

        self.storage_set(vm, property_key, ValueAndAttributes::new(Value::UNDEFINED, attributes));

        self.set_has_intrinsic_accessors();
        vm.intrinsic_accessors()
            .borrow_mut()
            .entry(core::ptr::from_ref(self) as usize)
            .or_default()
            .insert(property_key.as_string().clone(), accessor);
    }

    pub fn get_engine_private_property(&self, key: Gc<Symbol>) -> Option<ValueAndAttributes> {
        assert!(key.is_private());
        self.storage_get_symbol(&PropertyKey::from(key))
    }

    pub fn set_engine_private_property(&self, vm: &Vm, key: Gc<Symbol>, value: Value) {
        assert!(key.is_private());
        self.storage_set(
            vm,
            &PropertyKey::from(key),
            ValueAndAttributes::new(value, PropertyAttributes::default()),
        );
    }

    pub fn delete_engine_private_property(&self, vm: &Vm, key: Gc<Symbol>) {
        assert!(key.is_private());
        let property_key = PropertyKey::from(key);
        if self.storage_has(&property_key) {
            self.storage_delete(vm, &property_key);
        }
    }

    pub fn define_native_accessor(
        &self,
        vm: &Vm,
        realm: Gc<Realm>,
        property_key: &PropertyKey,
        getter: RawNativeFunctionPointer,
        setter: RawNativeFunctionPointer,
        attribute: PropertyAttributes,
    ) {
        let getter_function = getter
            .is_some()
            .then(|| RawNativeFunction::create(vm, getter, 0, property_key, Some(realm), Some("get"), None).upcast());
        let setter_function = setter
            .is_some()
            .then(|| RawNativeFunction::create(vm, setter, 1, property_key, Some(realm), Some("set"), None).upcast());
        self.define_direct_accessor(vm, property_key, getter_function, setter_function, attribute);
    }

    pub fn define_direct_accessor(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        getter: Option<Gc<FunctionObject>>,
        setter: Option<Gc<FunctionObject>>,
        attributes: PropertyAttributes,
    ) {
        let existing_property = self.storage_get(vm, property_key).unwrap_or_default().value;
        if existing_property.is_accessor() {
            let accessor = existing_property.as_accessor();
            if getter.is_some() {
                accessor.set_getter(getter);
            }
            if setter.is_some() {
                accessor.set_setter(setter);
            }
        } else {
            let accessor = Accessor::create(vm, getter, setter, None);
            self.define_direct_property(vm, property_key, Value::from_accessor(accessor), attributes);
        }
    }

    // Cache the getter's result in an engine-private property on this object.
    pub fn define_direct_cached_accessor(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        getter: Option<Gc<FunctionObject>>,
        setter: Option<Gc<FunctionObject>>,
        attributes: PropertyAttributes,
    ) {
        self.clear_cached_accessor_value(vm, property_key);
        self.define_direct_accessor(vm, property_key, getter, setter, attributes);

        let property = self
            .storage_get(vm, property_key)
            .expect("the accessor was just defined");
        assert!(property.value.is_accessor());
        let accessor = property.value.as_accessor();
        if accessor.cached_value_key().is_none() {
            accessor.set_cached_value_key(Some(Symbol::create_private(vm)));
        }
    }

    pub fn clear_cached_accessor_value(&self, vm: &Vm, property_key: &PropertyKey) {
        let Some(property) = self.storage_get(vm, property_key) else {
            return;
        };
        if !property.value.is_accessor() {
            return;
        }
        let Some(cached_value_key) = property.value.as_accessor().cached_value_key() else {
            return;
        };
        self.delete_engine_private_property(vm, cached_value_key);
    }

    fn is_unimplemented_property(&self, property_key: &PropertyKey) -> bool {
        if !self.has_unimplemented_properties() || !property_key.is_string() {
            return false;
        }
        unimplemented_runtime_function(
            "the unimplemented properties of Object::define_unimplemented_property",
            0,
        )
    }

    // 20.1.2.3.1 ObjectDefineProperties ( O, Properties ), https://tc39.es/ecma262/#sec-objectdefineproperties
    pub fn define_properties(&self, vm: &Vm, properties: Value) -> ThrowCompletionOr<Gc<Object>> {
        // 1. Let props be ? ToObject(Properties).
        let props = properties.to_object(vm)?;

        // 2. Let keys be ? props.[[OwnPropertyKeys]]().
        let keys = props.internal_own_property_keys(vm)?;

        // 3. Let descriptors be a new empty List.
        let descriptors: MarkedVec<(PropertyKey, PropertyDescriptor)> = MarkedVec::new(vm);

        // 4. For each element nextKey of keys, do
        for index in 0..keys.len() {
            let next_key = keys.get(index).expect("the index is in bounds");
            let property_key = PropertyKey::from_value(vm, next_key).must();

            // a. Let propDesc be ? props.[[GetOwnProperty]](nextKey).
            let property_descriptor = props.internal_get_own_property(vm, &property_key)?;

            // b. If propDesc is not undefined and propDesc.[[Enumerable]] is true, then
            if let Some(property_descriptor) = property_descriptor
                && property_descriptor
                    .enumerable
                    .expect("a property descriptor of an own property is complete")
            {
                // i. Let descObj be ? Get(props, nextKey).
                let descriptor_object = props.get(vm, &property_key)?;

                // ii. Let desc be ? ToPropertyDescriptor(descObj).
                let descriptor = to_property_descriptor(vm, descriptor_object)?;

                // iii. Append the pair (a two element List) consisting of nextKey and desc to the end of descriptors.
                descriptors.push((property_key, descriptor));
            }
        }

        // 5. For each element pair of descriptors, do
        for index in 0..descriptors.len() {
            // a. Let P be the first element of pair.
            // b. Let desc be the second element of pair.
            let (name, mut descriptor) = descriptors.get(index).expect("the index is in bounds");

            // c. Perform ? DefinePropertyOrThrow(O, P, desc).
            self.define_property_or_throw(vm, &name, &mut descriptor)?;
        }

        // 6. Return O.
        Ok(self.as_gc())
    }

    // 14.7.5.9 EnumerateObjectProperties ( O ), https://tc39.es/ecma262/#sec-enumerate-object-properties
    /// Calls `callback` with each key until it returns something, which this then returns. Throws propagate as errors,
    /// where the C++ returns them as completions.
    pub fn enumerate_object_properties<T>(
        &self,
        vm: &Vm,
        mut callback: impl FnMut(Value) -> Option<T>,
    ) -> ThrowCompletionOr<Option<T>> {
        // 1. Return an Iterator object (27.1.1.2) whose next method iterates over all the String-valued keys of enumerable properties of O. The iterator object is never directly accessible to ECMAScript code. The mechanics and order of enumerating the properties is not specified but must conform to the rules specified below.
        //    * Returned property keys do not include keys that are Symbols.
        //    * Properties of the target object may be deleted during enumeration.
        //    * A property that is deleted before it is processed is ignored.
        //    * If new properties are added to the target object during enumeration, the newly added properties are not guaranteed to be processed in the active enumeration.
        //    * A property name will be returned at most once in any enumeration.
        //    * Enumerating the properties of the target object includes enumerating properties of its prototype, and the prototype of the prototype, and so on, recursively.
        //    * A property of a prototype is not processed if it has the same name as a property that has already been processed.

        let mut visited: HashSet<Utf16FlyString> = HashSet::new();

        let mut target = Some(self.as_gc());
        while let Some(current_target) = target {
            let own_keys = current_target.internal_own_property_keys(vm)?;
            for index in 0..own_keys.len() {
                let key = own_keys.get(index).expect("the index is in bounds");
                if !key.is_string() {
                    continue;
                }
                let property_key = to_utf16_fly_string(&key.as_string().utf16_string());
                if visited.contains(&property_key) {
                    continue;
                }
                let descriptor =
                    current_target.internal_get_own_property(vm, &PropertyKey::from(property_key.clone()))?;
                let Some(descriptor) = descriptor else {
                    continue;
                };
                visited.insert(property_key);
                if !descriptor
                    .enumerable
                    .expect("a property descriptor of an own property is complete")
                {
                    continue;
                }
                if let Some(completion) = callback(key) {
                    return Ok(Some(completion));
                }
            }

            target = current_target.internal_get_prototype_of(vm)?;
        }

        Ok(None)
    }

    // 7.1.1.1 OrdinaryToPrimitive ( O, hint ), https://tc39.es/ecma262/#sec-ordinarytoprimitive
    pub fn ordinary_to_primitive(&self, vm: &Vm, preferred_type: PreferredType) -> ThrowCompletionOr<Value> {
        assert!(preferred_type == PreferredType::String || preferred_type == PreferredType::Number);

        let method_names = if preferred_type == PreferredType::String {
            // 1. If hint is string, then
            // a. Let methodNames be « "toString", "valueOf" ».
            [vm.names.toString.clone(), vm.names.valueOf.clone()]
        } else {
            // 2. Else,
            // a. Let methodNames be « "valueOf", "toString" ».
            [vm.names.valueOf.clone(), vm.names.toString.clone()]
        };

        // 3. For each element name of methodNames, do
        for method_name in &method_names {
            // a. Let method be ? Get(O, name).
            let method = if *method_name == vm.names.toString {
                self.get_with_cache(
                    vm,
                    method_name,
                    vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::ObjectOrdinaryToPrimitiveToString),
                )?
            } else {
                debug_assert!(*method_name == vm.names.valueOf);
                self.get_with_cache(
                    vm,
                    method_name,
                    vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::ObjectOrdinaryToPrimitiveValueOf),
                )?
            };

            // b. If IsCallable(method) is true, then
            if method.is_function() {
                // i. Let result be ? Call(method, O).
                let result = call(vm, method, Value::from_object(self.as_gc()), &[])?;

                // ii. If Type(result) is not Object, return result.
                if !result.is_object() {
                    return Ok(result);
                }
            }
        }

        // 4. Throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::Convert,
            &[
                &"object",
                &if preferred_type == PreferredType::String {
                    "string"
                } else {
                    "number"
                },
            ],
        )
    }

    // Flags

    pub fn extensible(&self) -> bool {
        self.has_flag(object_flag::IS_EXTENSIBLE)
    }

    pub fn set_extensible(&self, value: bool) {
        if value {
            self.set_flag(object_flag::IS_EXTENSIBLE);
        } else {
            self.clear_flag(object_flag::IS_EXTENSIBLE);
        }
    }

    fn has_flag(&self, flag: u16) -> bool {
        self.flags.get() & flag != 0
    }

    fn set_flag(&self, flag: u16) {
        self.flags.set(self.flags.get() | flag);
    }

    fn clear_flag(&self, flag: u16) {
        self.flags.set(self.flags.get() & !flag);
    }

    // NOTE: Any subclass of Object that overrides property access slots ([[Get]], [[Set]] etc)
    //       to customize access to indexed properties (properties where the name is a positive integer)
    //       must return true for this, to opt out of optimizations that rely on assumptions that
    //       might not hold when property access behaves differently.
    pub fn may_interfere_with_indexed_property_access(&self) -> bool {
        self.has_flag(object_flag::MAY_INTERFERE_WITH_INDEXED_PROPERTY_ACCESS)
    }

    pub fn set_may_interfere_with_indexed_property_access(&self) {
        self.set_flag(object_flag::MAY_INTERFERE_WITH_INDEXED_PROPERTY_ACCESS);
    }

    // Objects with this flag must not participate in AddOwnProperty IC caching:
    // ordinary_set_with_own_descriptor() skips emitting AddOwnProperty metadata,
    // and AddOwnProperty cache-hit paths check the flag before consuming an
    // existing cache entry. Both sides are required: a plain object can share a
    // Shape* with a flagged object, e.g. Object.create(Element.prototype), and
    // prime a cache entry; the flagged object must still fail the consumption
    // check. Conversely, only checking consumption would leave permanently-dead
    // cache entries that evict useful ones.
    pub fn requires_slow_add_own_property(&self) -> bool {
        self.has_flag(object_flag::REQUIRES_SLOW_ADD_OWN_PROPERTY)
    }

    pub fn set_requires_slow_add_own_property(&self) {
        self.set_flag(object_flag::REQUIRES_SLOW_ADD_OWN_PROPERTY);
    }

    pub fn clear_requires_slow_add_own_property(&self) {
        self.clear_flag(object_flag::REQUIRES_SLOW_ADD_OWN_PROPERTY);
    }

    pub fn is_platform_object(&self) -> bool {
        self.has_flag(object_flag::IS_PLATFORM_OBJECT)
    }

    pub fn set_is_platform_object(&self) {
        self.set_flag(object_flag::IS_PLATFORM_OBJECT);
    }

    pub fn is_function(&self) -> bool {
        self.has_flag(object_flag::IS_FUNCTION)
    }

    pub fn set_is_function(&self) {
        self.set_flag(object_flag::IS_FUNCTION);
    }

    pub fn clear_is_function(&self) {
        self.clear_flag(object_flag::IS_FUNCTION);
    }

    pub fn is_raw_native_function(&self) -> bool {
        self.has_flag(object_flag::IS_RAW_NATIVE_FUNCTION)
    }

    pub fn set_is_raw_native_function(&self) {
        self.set_flag(object_flag::IS_RAW_NATIVE_FUNCTION);
    }

    pub fn is_direct_getter_function(&self) -> bool {
        self.has_flag(object_flag::IS_DIRECT_GETTER_FUNCTION)
    }

    pub fn set_is_direct_getter_function(&self) {
        self.set_flag(object_flag::IS_DIRECT_GETTER_FUNCTION);
    }

    pub fn has_global_object_flag(&self) -> bool {
        self.has_flag(object_flag::IS_GLOBAL_OBJECT)
    }

    pub fn set_global_object_flag(&self) {
        self.set_flag(object_flag::IS_GLOBAL_OBJECT);
    }

    pub fn is_ecmascript_function_object(&self) -> bool {
        self.has_flag(object_flag::IS_ECMASCRIPT_FUNCTION_OBJECT)
    }

    pub fn set_is_ecmascript_function_object(&self) {
        self.set_flag(object_flag::IS_ECMASCRIPT_FUNCTION_OBJECT);
    }

    // B.3.7 The [[IsHTMLDDA]] Internal Slot, https://tc39.es/ecma262/#sec-IsHTMLDDA-internal-slot
    pub fn is_htmldda(&self) -> bool {
        self.has_flag(object_flag::IS_HTMLDDA)
    }

    pub fn set_is_htmldda(&self) {
        self.set_flag(object_flag::IS_HTMLDDA);
    }

    pub fn has_magical_length_property(&self) -> bool {
        self.has_flag(object_flag::HAS_MAGICAL_LENGTH_PROPERTY)
    }

    pub fn set_has_magical_length_property(&self) {
        self.set_flag(object_flag::HAS_MAGICAL_LENGTH_PROPERTY);
    }

    pub fn is_typed_array(&self) -> bool {
        self.has_flag(object_flag::IS_TYPED_ARRAY)
    }

    pub fn set_is_typed_array(&self) {
        self.set_flag(object_flag::IS_TYPED_ARRAY);
    }

    pub fn has_intrinsic_accessors(&self) -> bool {
        self.has_flag(object_flag::HAS_INTRINSIC_ACCESSORS)
    }

    pub fn set_has_intrinsic_accessors(&self) {
        self.set_flag(object_flag::HAS_INTRINSIC_ACCESSORS);
    }

    fn has_unimplemented_properties(&self) -> bool {
        self.has_flag(object_flag::HAS_UNIMPLEMENTED_PROPERTIES)
    }

    pub fn has_parameter_map(&self) -> bool {
        self.shape().has_parameter_map()
    }

    pub fn shape(&self) -> Gc<Shape> {
        self.shape.get()
    }

    pub fn prototype(&self) -> Option<Gc<Object>> {
        self.shape().prototype()
    }

    pub fn convert_to_prototype_if_needed(&self, vm: &Vm) {
        if self.shape().is_prototype_shape() {
            return;
        }
        self.shape.set(self.shape().clone_for_prototype(vm));
    }

    // Named-property slot offsets come from shape lookups and from inline caches. A corrupted cache
    // can hand us an offset past the allocation, so we bound every direct slot access by the storage
    // capacity. This turns an out-of-bounds slot into a controlled crash instead of a memory
    // read/write primitive.
    pub fn get_direct(&self, index: u32) -> Value {
        assert!(index < self.named_storage_capacity());
        // SAFETY: The index is within the named storage.
        unsafe { self.named_properties.get().add(index as usize).read() }
    }

    pub fn put_direct(&self, index: u32, value: Value) {
        assert!(index < self.named_storage_capacity());
        // SAFETY: The index is within the named storage.
        unsafe { self.named_properties.get().add(index as usize).write(value) };
    }

    fn inline_named_storage_pointer(&self) -> *mut Value {
        // Cell<Value> is a transparent UnsafeCell, so the inline storage may be written through this pointer.
        core::ptr::from_ref(&self.inline_named_storage)
            .cast::<Value>()
            .cast_mut()
    }

    fn named_storage_is_inline(&self) -> bool {
        core::ptr::eq(self.named_properties.get(), self.inline_named_storage_pointer())
    }

    // Capacity of the current named-property storage in Values. Heap storage keeps its capacity in a
    // u32 just before the first element (see heap_value_storage); inline storage is fixed.
    fn named_storage_capacity(&self) -> u32 {
        if self.named_storage_is_inline() {
            return INLINE_NAMED_STORAGE_CAPACITY as u32;
        }
        heap_value_storage::capacity(self.named_properties.get())
    }

    fn ensure_named_storage_capacity(&self, needed: u32) {
        let is_inline = self.named_storage_is_inline();
        let old_capacity = self.named_storage_capacity();
        if needed <= old_capacity {
            return;
        }
        let new_capacity = needed.max(old_capacity.saturating_mul(2));
        if is_inline {
            let new_storage = heap_value_storage::allocate(new_capacity);
            for index in 0..new_capacity as usize {
                let value = if index < INLINE_NAMED_STORAGE_CAPACITY {
                    self.inline_named_storage[index].get()
                } else {
                    Value::UNDEFINED
                };
                // SAFETY: The index is within the new storage.
                unsafe { new_storage.add(index).write(value) };
            }
            self.named_properties.set(new_storage);
        } else {
            let new_storage = heap_value_storage::reallocate(self.named_properties.get(), new_capacity);
            for index in old_capacity..new_capacity {
                // SAFETY: The index is within the new storage.
                unsafe { new_storage.add(index as usize).write(Value::UNDEFINED) };
            }
            self.named_properties.set(new_storage);
        }
    }

    // Indexed property storage

    pub fn indexed_storage_kind(&self) -> IndexedStorageKind {
        self.indexed_storage_kind.get()
    }

    pub fn indexed_array_like_size(&self) -> u32 {
        self.indexed_array_like_size.get()
    }

    fn indexed_dictionary(&self) -> &GcRefCell<GenericIndexedPropertyStorage> {
        assert!(self.indexed_storage_kind() == IndexedStorageKind::Dictionary);
        // SAFETY: In dictionary mode the indexed elements pointer is the boxed storage, which the object owns until it
        // leaves dictionary mode.
        unsafe {
            &*self
                .indexed_elements
                .get()
                .cast::<GcRefCell<GenericIndexedPropertyStorage>>()
        }
    }

    // The number of packed elements that are really in the buffer. The logical size sits on the
    // Object while the capacity sits with the allocation, so the two can drift apart if the size
    // field is corrupted. Packed indexing goes through this, which keeps such a drift a wrong
    // answer rather than an access past the allocation.
    pub fn indexed_packed_element_count(&self) -> u32 {
        self.indexed_array_like_size().min(self.indexed_elements_capacity())
    }

    fn indexed_elements_capacity(&self) -> u32 {
        let elements = self.indexed_elements.get();
        if elements.is_null() {
            return 0;
        }
        assert!(matches!(
            self.indexed_storage_kind(),
            IndexedStorageKind::Packed | IndexedStorageKind::Holey
        ));
        heap_value_storage::capacity(elements)
    }

    fn indexed_element(&self, index: u32) -> Value {
        assert!(index < self.indexed_elements_capacity());
        // SAFETY: The index is within the elements buffer.
        unsafe { self.indexed_elements.get().add(index as usize).read() }
    }

    fn set_indexed_element(&self, index: u32, value: Value) {
        assert!(index < self.indexed_elements_capacity());
        // SAFETY: The index is within the elements buffer.
        unsafe { self.indexed_elements.get().add(index as usize).write(value) };
    }

    fn visit_indexed_elements(&self, count: u32, visitor: &mut Visitor) {
        if count == 0 {
            return;
        }
        // SAFETY: The count is within the elements buffer, and nothing writes it during a collection.
        visitor.visit_values(unsafe { core::slice::from_raw_parts(self.indexed_elements.get(), count as usize) });
    }

    fn allocate_indexed_elements(capacity: u32) -> *mut Value {
        let elements = heap_value_storage::allocate(capacity);
        for index in 0..capacity as usize {
            // SAFETY: The index is within the new buffer.
            unsafe { elements.add(index).write(Value::EMPTY) };
        }
        elements
    }

    fn free_indexed_elements(&self) {
        if self.indexed_storage_kind() == IndexedStorageKind::Dictionary {
            // SAFETY: In dictionary mode the indexed elements pointer came from Box::into_raw in transition_to_dictionary().
            drop(unsafe {
                Box::from_raw(
                    self.indexed_elements
                        .get()
                        .cast::<GcRefCell<GenericIndexedPropertyStorage>>(),
                )
            });
        } else {
            heap_value_storage::deallocate(self.indexed_elements.get());
        }
        self.indexed_elements.set(core::ptr::null_mut());
        self.indexed_storage_kind.set(IndexedStorageKind::None);
        self.indexed_array_like_size.set(0);
    }

    fn ensure_indexed_elements(&self, needed_capacity: u32) {
        if !self.indexed_elements.get().is_null() && self.indexed_elements_capacity() >= needed_capacity {
            return;
        }
        self.grow_indexed_elements(needed_capacity);
    }

    fn grow_indexed_elements(&self, needed_capacity: u32) {
        // Grow by at least 50% to reduce copying during dense fills.
        let old_capacity = self.indexed_elements_capacity();
        let new_capacity = needed_capacity
            .max(old_capacity.saturating_add(old_capacity / 2))
            .max(8);

        let new_elements = Self::allocate_indexed_elements(new_capacity);

        let old_elements = self.indexed_elements.get();
        if !old_elements.is_null() {
            let copy_count = old_capacity.min(needed_capacity);
            // SAFETY: Both buffers hold at least copy_count elements, and they do not overlap.
            unsafe { core::ptr::copy_nonoverlapping(old_elements, new_elements, copy_count as usize) };
            heap_value_storage::deallocate(old_elements);
        }

        self.indexed_elements.set(new_elements);
    }

    fn transition_to_dictionary(&self) {
        let mut dictionary = GenericIndexedPropertyStorage::default();

        let kind = self.indexed_storage_kind();
        if kind == IndexedStorageKind::Packed || kind == IndexedStorageKind::Holey {
            // Transfer existing elements
            let count = self.indexed_packed_element_count();
            for index in 0..count {
                let value = self.indexed_element(index);
                if !value.is_empty() {
                    dictionary.put(index, value, DEFAULT_ATTRIBUTES);
                }
            }
            heap_value_storage::deallocate(self.indexed_elements.get());
        }

        // Set the array_like_size on the dictionary
        dictionary.set_array_like_size(self.indexed_array_like_size() as usize);

        let dictionary = Box::into_raw(Box::new(GcRefCell::new(dictionary)));
        self.indexed_elements.set(dictionary.cast::<Value>());
        self.indexed_storage_kind.set(IndexedStorageKind::Dictionary);
    }

    #[cold]
    fn transition_to_packed(&self) {
        let elements = Self::allocate_indexed_elements(self.indexed_array_like_size());
        for (&index, element) in self.indexed_dictionary().borrow().sparse_elements() {
            // SAFETY: Every index of the dictionary is below its array-like size, the size of the new buffer.
            unsafe { elements.add(index as usize).write(element.value) };
        }

        self.free_indexed_elements_dictionary();
        self.indexed_elements.set(elements);
        self.indexed_storage_kind.set(IndexedStorageKind::Packed);
    }

    fn free_indexed_elements_dictionary(&self) {
        // SAFETY: In dictionary mode the indexed elements pointer came from Box::into_raw in transition_to_dictionary().
        drop(unsafe {
            Box::from_raw(
                self.indexed_elements
                    .get()
                    .cast::<GcRefCell<GenericIndexedPropertyStorage>>(),
            )
        });
        self.indexed_elements.set(core::ptr::null_mut());
    }

    pub fn indexed_get(&self, index: u32) -> Option<ValueAndAttributes> {
        match self.indexed_storage_kind() {
            IndexedStorageKind::None => None,
            IndexedStorageKind::Packed => {
                if index >= self.indexed_packed_element_count() {
                    return None;
                }
                Some(ValueAndAttributes::new(self.indexed_element(index), DEFAULT_ATTRIBUTES))
            }
            IndexedStorageKind::Holey => {
                if index >= self.indexed_array_like_size() {
                    return None;
                }
                if index >= self.indexed_elements_capacity() {
                    return None;
                }
                let value = self.indexed_element(index);
                if value.is_empty() {
                    return None;
                }
                Some(ValueAndAttributes::new(value, DEFAULT_ATTRIBUTES))
            }
            IndexedStorageKind::Dictionary => self.indexed_dictionary().borrow().get(index),
        }
    }

    #[cold]
    fn indexed_put_into_dictionary(&self, index: u32, value: Value, attributes: PropertyAttributes) {
        let promote = {
            let mut dictionary = self.indexed_dictionary().borrow_mut();
            let had_missing_entries = dictionary.size() < dictionary.array_like_size();
            dictionary.put(index, value, attributes);
            self.indexed_array_like_size.set(dictionary.array_like_size() as u32);
            had_missing_entries
                && dictionary.size() >= dictionary.array_like_size()
                && dictionary
                    .sparse_elements()
                    .values()
                    .all(|element| element.attributes == DEFAULT_ATTRIBUTES && !element.value.is_empty())
        };
        if promote {
            self.transition_to_packed();
        }
    }

    fn indexed_put_dictionary_and_update_size(&self, index: u32, value: Value, attributes: PropertyAttributes) {
        let mut dictionary = self.indexed_dictionary().borrow_mut();
        dictionary.put(index, value, attributes);
        self.indexed_array_like_size.set(dictionary.array_like_size() as u32);
    }

    pub fn indexed_put(&self, index: u32, value: Value, attributes: PropertyAttributes) {
        let storing_hole = value.is_empty();
        let kind = self.indexed_storage_kind();
        let mut materialized_elements = 0;
        if kind == IndexedStorageKind::Packed || kind == IndexedStorageKind::Holey {
            materialized_elements = self.indexed_packed_element_count();
        }

        if kind == IndexedStorageKind::Dictionary {
            self.indexed_put_into_dictionary(index, value, attributes);
            return;
        }

        // Non-default attributes require Dictionary mode
        if attributes != DEFAULT_ATTRIBUTES {
            self.transition_to_dictionary();
            self.indexed_put_dictionary_and_update_size(index, value, attributes);
            return;
        }

        // Check for sparse threshold
        if u64::from(index) > u64::from(materialized_elements) + u64::from(SPARSE_ARRAY_HOLE_THRESHOLD) {
            self.transition_to_dictionary();
            self.indexed_put_dictionary_and_update_size(index, value, attributes);
            return;
        }

        if kind == IndexedStorageKind::None {
            self.indexed_storage_kind.set(if storing_hole || index > 0 {
                IndexedStorageKind::Holey
            } else {
                IndexedStorageKind::Packed
            });
            let needed = index + 1;
            self.ensure_indexed_elements(needed);
            self.set_indexed_element(index, value);
            self.indexed_array_like_size
                .set(self.indexed_array_like_size().max(index + 1));
            return;
        }

        // Packed or Holey
        if index >= materialized_elements {
            self.ensure_indexed_elements(index + 1);
        }

        if index >= self.indexed_array_like_size() {
            // Growing
            let new_size = index + 1;

            if self.indexed_storage_kind() == IndexedStorageKind::Packed
                && (index > self.indexed_array_like_size() || storing_hole)
            {
                // Gap created
                self.indexed_storage_kind.set(IndexedStorageKind::Holey);
            }

            self.indexed_array_like_size.set(new_size);
        }

        if self.indexed_storage_kind() == IndexedStorageKind::Packed && storing_hole {
            self.indexed_storage_kind.set(IndexedStorageKind::Holey);
        }

        self.set_indexed_element(index, value);

        // Promote Holey -> Packed when filling the last hole.
        // Only check when writing to the last index to avoid O(N^2) scanning.
        if self.indexed_storage_kind() == IndexedStorageKind::Holey && index == self.indexed_array_like_size() - 1 {
            let available_elements = self.indexed_packed_element_count();
            let has_holes = (0..available_elements).any(|index| self.indexed_element(index).is_empty());
            if !has_holes && self.indexed_elements_capacity() >= self.indexed_array_like_size() {
                self.indexed_storage_kind.set(IndexedStorageKind::Packed);
            }
        }
    }

    pub fn indexed_has(&self, index: u32) -> bool {
        match self.indexed_storage_kind() {
            IndexedStorageKind::None => false,
            IndexedStorageKind::Packed => index < self.indexed_packed_element_count(),
            IndexedStorageKind::Holey => {
                index < self.indexed_array_like_size()
                    && index < self.indexed_elements_capacity()
                    && !self.indexed_element(index).is_empty()
            }
            IndexedStorageKind::Dictionary => self.indexed_dictionary().borrow().has_index(index),
        }
    }

    pub fn indexed_delete(&self, index: u32) {
        match self.indexed_storage_kind() {
            IndexedStorageKind::None => {}
            IndexedStorageKind::Packed => {
                assert!(index < self.indexed_array_like_size());
                if index >= self.indexed_elements_capacity() {
                    return;
                }
                self.set_indexed_element(index, Value::EMPTY);
                self.indexed_storage_kind.set(IndexedStorageKind::Holey);
            }
            IndexedStorageKind::Holey => {
                assert!(index < self.indexed_array_like_size());
                if index >= self.indexed_elements_capacity() {
                    return;
                }
                self.set_indexed_element(index, Value::EMPTY);
            }
            IndexedStorageKind::Dictionary => self.indexed_dictionary().borrow_mut().remove(index),
        }
    }

    pub fn set_indexed_array_like_size(&self, new_size: usize) -> bool {
        if new_size == self.indexed_array_like_size() as usize {
            return true;
        }

        if self.indexed_storage_kind() == IndexedStorageKind::Dictionary {
            let mut dictionary = self.indexed_dictionary().borrow_mut();
            let result = dictionary.set_array_like_size(new_size);
            self.indexed_array_like_size.set(dictionary.array_like_size() as u32);
            return result;
        }

        let old_size = self.indexed_array_like_size();
        let new_size = u32::try_from(new_size).expect("an array-like size fits in u32");

        if self.indexed_storage_kind() == IndexedStorageKind::None {
            if new_size == 0 {
                return true;
            }
            self.indexed_storage_kind.set(IndexedStorageKind::Holey);
            self.indexed_array_like_size.set(new_size);
            return true;
        }

        if new_size > old_size {
            if self.indexed_storage_kind() == IndexedStorageKind::Packed {
                self.indexed_storage_kind.set(IndexedStorageKind::Holey);
            }
            self.indexed_array_like_size.set(new_size);
            return true;
        }

        // Shrinking
        if new_size < old_size {
            let capacity = self.indexed_elements_capacity();
            for index in new_size..old_size.min(capacity) {
                self.set_indexed_element(index, Value::EMPTY);
            }
            self.indexed_array_like_size.set(new_size);
        }

        true
    }

    pub fn indexed_append(&self, value: Value, attributes: PropertyAttributes) {
        self.indexed_put(self.indexed_array_like_size(), value, attributes);
    }

    pub fn indexed_append_values(&self, values: &[Value]) {
        assert!(matches!(
            self.indexed_storage_kind(),
            IndexedStorageKind::None | IndexedStorageKind::Packed
        ));
        assert!(values.len() <= (u32::MAX - self.indexed_array_like_size()) as usize);

        if values.is_empty() {
            return;
        }

        let old_size = self.indexed_array_like_size();
        let values_size = values.len() as u32;
        let new_size = old_size + values_size;
        self.ensure_indexed_elements(new_size);

        // NB: The storage is packed before the elements are written, since writing checks the capacity of packed or
        //     holey storage.
        self.indexed_storage_kind.set(IndexedStorageKind::Packed);
        for (index, value) in values.iter().enumerate() {
            self.set_indexed_element(old_size + index as u32, *value);
        }

        self.indexed_array_like_size.set(new_size);
    }

    /// indexed_append(source.indexed_packed_elements_span()): appends the elements of an object whose indexed storage
    /// is packed, reading each from that storage as it is written.
    pub fn indexed_append_packed_elements_of(&self, source: &Object) {
        assert!(source.indexed_storage_kind() == IndexedStorageKind::Packed);
        let values_size = source.indexed_packed_element_count();
        assert!(matches!(
            self.indexed_storage_kind(),
            IndexedStorageKind::None | IndexedStorageKind::Packed
        ));
        assert!(values_size <= u32::MAX - self.indexed_array_like_size());

        if values_size == 0 {
            return;
        }

        let old_size = self.indexed_array_like_size();
        let new_size = old_size + values_size;
        self.ensure_indexed_elements(new_size);

        // NB: The storage is packed before the elements are written, since writing checks the capacity of packed or
        //     holey storage.
        self.indexed_storage_kind.set(IndexedStorageKind::Packed);
        for index in 0..values_size {
            self.set_indexed_element(old_size + index, source.indexed_element(index));
        }

        self.indexed_array_like_size.set(new_size);
    }

    pub fn indexed_take_first(&self) -> ValueAndAttributes {
        if self.indexed_storage_kind() == IndexedStorageKind::Dictionary {
            let mut dictionary = self.indexed_dictionary().borrow_mut();
            let result = dictionary.take_first();
            self.indexed_array_like_size.set(dictionary.array_like_size() as u32);
            return result;
        }

        assert!(self.indexed_array_like_size() > 0);
        if self.indexed_storage_kind() == IndexedStorageKind::None {
            self.indexed_array_like_size.set(self.indexed_array_like_size() - 1);
            return ValueAndAttributes::default();
        }

        let available_elements = self.indexed_packed_element_count();
        let first = if available_elements > 0 {
            self.indexed_element(0)
        } else {
            Value::EMPTY
        };

        if available_elements > 1 {
            let elements = self.indexed_elements.get();
            // SAFETY: Both ranges lie within the first available_elements elements of the buffer.
            unsafe { core::ptr::copy(elements.add(1), elements, available_elements as usize - 1) };
        }

        self.indexed_array_like_size.set(self.indexed_array_like_size() - 1);
        if available_elements > 0 {
            self.set_indexed_element(available_elements - 1, Value::EMPTY);
        }

        ValueAndAttributes::new(first, DEFAULT_ATTRIBUTES)
    }

    pub fn indexed_take_last(&self) -> ValueAndAttributes {
        if self.indexed_storage_kind() == IndexedStorageKind::Dictionary {
            let mut dictionary = self.indexed_dictionary().borrow_mut();
            let result = dictionary.take_last();
            self.indexed_array_like_size.set(dictionary.array_like_size() as u32);
            return result;
        }

        assert!(self.indexed_array_like_size() > 0);
        let new_size = self.indexed_array_like_size() - 1;
        self.indexed_array_like_size.set(new_size);
        if self.indexed_storage_kind() == IndexedStorageKind::None {
            return ValueAndAttributes::default();
        }
        if new_size >= self.indexed_elements_capacity() {
            return ValueAndAttributes::default();
        }

        let last = self.indexed_element(new_size);
        self.set_indexed_element(new_size, Value::EMPTY);

        if last.is_empty() {
            return ValueAndAttributes::default();
        }
        ValueAndAttributes::new(last, DEFAULT_ATTRIBUTES)
    }

    pub fn indexed_real_size(&self) -> usize {
        match self.indexed_storage_kind() {
            IndexedStorageKind::None => 0,
            IndexedStorageKind::Packed => self.indexed_packed_element_count() as usize,
            IndexedStorageKind::Holey => (0..self.indexed_packed_element_count())
                .filter(|index| !self.indexed_element(*index).is_empty())
                .count(),
            IndexedStorageKind::Dictionary => self.indexed_dictionary().borrow().size(),
        }
    }

    pub fn indexed_indices(&self) -> Vec<u32> {
        match self.indexed_storage_kind() {
            IndexedStorageKind::None => Vec::new(),
            IndexedStorageKind::Packed => (0..self.indexed_packed_element_count()).collect(),
            IndexedStorageKind::Holey => (0..self.indexed_packed_element_count())
                .filter(|index| !self.indexed_element(*index).is_empty())
                .collect(),
            IndexedStorageKind::Dictionary => {
                let mut indices: Vec<u32> = self
                    .indexed_dictionary()
                    .borrow()
                    .sparse_elements()
                    .keys()
                    .copied()
                    .collect();
                indices.sort_unstable();
                indices
            }
        }
    }

    /// Calls `callback` with the value of every element, in index order unless the storage is a dictionary. A
    /// dictionary stays borrowed meanwhile, so the callback must not change the object's indexed properties.
    pub fn indexed_for_each_value(&self, mut callback: impl FnMut(Value)) {
        match self.indexed_storage_kind() {
            IndexedStorageKind::None => {}
            IndexedStorageKind::Packed => {
                for index in 0..self.indexed_array_like_size() {
                    callback(self.indexed_element(index));
                }
            }
            IndexedStorageKind::Holey => {
                for index in 0..self.indexed_packed_element_count() {
                    let value = self.indexed_element(index);
                    if !value.is_empty() {
                        callback(value);
                    }
                }
            }
            IndexedStorageKind::Dictionary => {
                for element in self.indexed_dictionary().borrow().sparse_elements().values() {
                    callback(element.value);
                }
            }
        }
    }

    pub fn set_indexed_property_elements(&self, values: &[Value]) {
        self.free_indexed_elements();

        if values.is_empty() {
            return;
        }

        let size = u32::try_from(values.len()).expect("an array-like size fits in u32");
        self.indexed_storage_kind.set(IndexedStorageKind::Packed);
        self.indexed_array_like_size.set(size);
        self.indexed_elements.set(Self::allocate_indexed_elements(size));
        for (index, value) in values.iter().enumerate() {
            self.set_indexed_element(index as u32, *value);
        }
    }

    /// Replaces the indexed storage with `size` packed undefined elements, which the caller overwrites with
    /// set_packed_indexed_element().
    pub fn set_indexed_property_elements_to_undefined(&self, size: u32) {
        self.free_indexed_elements();

        if size == 0 {
            return;
        }

        let elements = heap_value_storage::allocate(size);
        for index in 0..size as usize {
            // SAFETY: The index is within the new buffer.
            unsafe { elements.add(index).write(Value::UNDEFINED) };
        }

        self.indexed_storage_kind.set(IndexedStorageKind::Packed);
        self.indexed_array_like_size.set(size);
        self.indexed_elements.set(elements);
    }

    // For FunctionPrototype.apply fast path
    pub fn indexed_packed_elements<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, Value> {
        assert!(self.indexed_storage_kind() == IndexedStorageKind::Packed);
        let count = self.indexed_packed_element_count();
        let elements = MarkedVec::with_capacity(vm, count as usize);
        for index in 0..count {
            elements.push(self.indexed_element(index));
        }
        elements
    }

    /// Writes one element of packed storage in place, as C++ writes through the span of
    /// set_indexed_property_elements_to_undefined().
    pub fn set_packed_indexed_element(&self, index: u32, value: Value) {
        assert!(self.indexed_storage_kind() == IndexedStorageKind::Packed);
        assert!(index < self.indexed_packed_element_count());
        self.set_indexed_element(index, value);
    }
}

fn create_data_property_for_set(
    vm: &Vm,
    receiver: &Object,
    property_key: &PropertyKey,
    value: Value,
    cacheable_metadata: Option<&mut CacheableSetPropertyMetadata>,
) -> ThrowCompletionOr<bool> {
    let no_existing_descriptor = None;
    let mut new_property_offset = None;
    let result = receiver.create_data_property(
        vm,
        property_key,
        value,
        Some(&mut new_property_offset),
        Some(&no_existing_descriptor),
    )?;
    if let Some(cacheable_metadata) = cacheable_metadata
        && let Some(new_property_offset) = new_property_offset
        && !receiver.shape().is_dictionary()
        && !receiver.requires_slow_add_own_property()
    {
        assert!(!property_key.is_number());
        *cacheable_metadata = CacheableSetPropertyMetadata {
            r#type: CacheableSetPropertyMetadataType::AddOwnProperty,
            property_offset: Some(new_property_offset),
            prototype: receiver.shape().prototype(),
            ..Default::default()
        };
    }
    Ok(result)
}

fn display_fly_string(string: &Utf16FlyString) -> String {
    crate::utf16::Utf16View::of_fly_string(string).to_utf8()
}

impl Object {
    /// Whether the object was allocated as a `T`, the Rust form of the C++ is<T>() on objects.
    pub fn is<T: GcCell + Extends<Object>>(&self) -> bool {
        self.class().is_subclass_of(T::CLASS)
    }
}
