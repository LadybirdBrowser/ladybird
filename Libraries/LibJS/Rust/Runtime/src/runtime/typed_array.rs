/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::{ControlFlow, Deref};

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::gc::class::{Class, GcCell, define_cell};
use crate::gc::root::MarkedVec;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::{Object, TYPED_ARRAY_CACHED_DATA_OFFSET_INVALID, typed_array_kind};
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    CanonicalIndexMode, IntrinsicDefaultPrototype, call_function_object, canonical_numeric_index_string, construct,
    get_prototype_from_constructor, length_of_array_like,
};
use crate::runtime::array_buffer::{
    ArrayBuffer, ElementType, INVALID_DATA_OFFSET, Order, ReadWriteModifyOperation, allocate_array_buffer,
    array_buffer_byte_length, clone_array_buffer,
};
use crate::runtime::byte_length::{ByteLength, ByteLengthSlot};
use crate::runtime::canonical_index::{CanonicalIndex, CanonicalIndexType};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::{ErrorKind, RangeError};
use crate::runtime::error_types::{AkDouble, ErrorType};
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::iterator::{get_iterator_from_method_impl, iterator_to_list};
use crate::runtime::native_function::{NativeFunction, define_native_function_class};
use crate::runtime::object::{
    CacheableGetPropertyMetadata, CacheableSetPropertyMetadata, MayInterfereWithIndexedPropertyAccess,
    ORDINARY_OBJECT_METHODS, ObjectMethods, PropertyLookupPhase, ShouldThrowExceptions, define_object_class,
};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::uint8_array::{Uint8ArrayConstructorHelpers, Uint8ArrayPrototypeHelpers};
use crate::runtime::value::same_value;

pub use crate::layout::object::TypedArrayBase;

/// TypedArrayBase::ContentType.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ContentType {
    BigInt,
    Number,
}

/// TypedArrayBase::Kind, in the order of JS_ENUMERATE_TYPED_ARRAYS, which the interpreter reads.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Kind {
    Uint8Array = typed_array_kind::UINT8,
    Uint8ClampedArray = typed_array_kind::UINT8_CLAMPED,
    Uint16Array = typed_array_kind::UINT16,
    Uint32Array = typed_array_kind::UINT32,
    BigUint64Array = typed_array_kind::BIG_UINT64,
    Int8Array = typed_array_kind::INT8,
    Int16Array = typed_array_kind::INT16,
    Int32Array = typed_array_kind::INT32,
    BigInt64Array = typed_array_kind::BIG_INT64,
    Float16Array = typed_array_kind::FLOAT16,
    Float32Array = typed_array_kind::FLOAT32,
    Float64Array = typed_array_kind::FLOAT64,
}

impl Kind {
    /// The kind with the value of TypedArrayBase::Kind, which the embedding ABI passes as JS_LAYOUT_TYPED_ARRAY_KIND_*.
    pub fn from_u8(kind: u8) -> Self {
        match kind {
            typed_array_kind::UINT8 => Self::Uint8Array,
            typed_array_kind::UINT8_CLAMPED => Self::Uint8ClampedArray,
            typed_array_kind::UINT16 => Self::Uint16Array,
            typed_array_kind::UINT32 => Self::Uint32Array,
            typed_array_kind::BIG_UINT64 => Self::BigUint64Array,
            typed_array_kind::INT8 => Self::Int8Array,
            typed_array_kind::INT16 => Self::Int16Array,
            typed_array_kind::INT32 => Self::Int32Array,
            typed_array_kind::BIG_INT64 => Self::BigInt64Array,
            typed_array_kind::FLOAT16 => Self::Float16Array,
            typed_array_kind::FLOAT32 => Self::Float32Array,
            typed_array_kind::FLOAT64 => Self::Float64Array,
            _ => unreachable!("{kind} is not a typed array kind"),
        }
    }

    /// The Element Type of Table 71 for the typed array, the T of the C++ TypedArray<T>.
    pub fn element_type(self) -> ElementType {
        match self {
            Self::Uint8Array => ElementType::Uint8,
            Self::Uint8ClampedArray => ElementType::Uint8Clamped,
            Self::Uint16Array => ElementType::Uint16,
            Self::Uint32Array => ElementType::Uint32,
            Self::BigUint64Array => ElementType::BigUint64,
            Self::Int8Array => ElementType::Int8,
            Self::Int16Array => ElementType::Int16,
            Self::Int32Array => ElementType::Int32,
            Self::BigInt64Array => ElementType::BigInt64,
            Self::Float16Array => ElementType::Float16,
            Self::Float32Array => ElementType::Float32,
            Self::Float64Array => ElementType::Float64,
        }
    }

    pub fn content_type(self) -> ContentType {
        match self {
            Self::BigInt64Array | Self::BigUint64Array => ContentType::BigInt,
            _ => ContentType::Number,
        }
    }
}

/// The parts of a TypedArrayBase the interpreter does not read.
#[derive(Trace)]
pub struct TypedArrayBaseStorage {
    viewed_array_buffer: Cell<Option<Gc<ArrayBuffer>>>,
    #[gc(untraced)]
    byte_length: Cell<ByteLength>,
    #[gc(untraced)]
    content_type: Cell<ContentType>,
}

pub static TYPED_ARRAY_BASE_METHODS: ObjectMethods = ObjectMethods {
    internal_prevent_extensions: TypedArrayBase::internal_prevent_extensions,
    internal_get_own_property: TypedArrayBase::internal_get_own_property,
    internal_has_property: TypedArrayBase::internal_has_property,
    internal_define_own_property: TypedArrayBase::internal_define_own_property,
    internal_get: TypedArrayBase::internal_get,
    is_cacheable_for_property_absence: |_| false,
    internal_set: TypedArrayBase::internal_set,
    internal_delete: TypedArrayBase::internal_delete,
    internal_own_property_keys: TypedArrayBase::internal_own_property_keys,
    eligible_for_own_property_enumeration_fast_path: |_| false,
    ..ORDINARY_OBJECT_METHODS
};

define_cell!(TypedArrayBase, Object, extends: [Object], methods: TYPED_ARRAY_BASE_METHODS);

// SAFETY: The viewed buffer in the storage is the only cell a TypedArrayBase adds to its Object.
unsafe impl Trace for TypedArrayBase {
    fn trace(&self, visitor: &mut Visitor) {
        self.base.trace(visitor);
        self.storage.trace(visitor);
    }
}

impl Deref for TypedArrayBase {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

/// The typed array an internal method of a typed array was called on.
fn as_typed_array(object: &Object) -> &TypedArrayBase {
    assert!(object.is::<TypedArrayBase>());
    // SAFETY: The object is a TypedArrayBase, which starts with its Object.
    unsafe { &*core::ptr::from_ref(object).cast::<TypedArrayBase>() }
}

impl TypedArrayBase {
    /// TypedArray<T>(Object& prototype, u32 array_length, ArrayBuffer& array_buffer, Kind kind), for `class`, which is
    /// the class of the typed array of `kind`.
    fn new(
        vm: &Vm,
        class: &'static Class,
        prototype: Gc<Object>,
        array_length: u32,
        array_buffer: Gc<ArrayBuffer>,
        kind: Kind,
    ) -> TypedArrayBase {
        let element_size = kind.element_type().size() as u8;
        let object = Object::new_with_prototype(vm, class, prototype, MayInterfereWithIndexedPropertyAccess::Yes);
        object.set_is_typed_array();
        let typed_array = TypedArrayBase {
            base: object,
            kind: Cell::new(kind as u8),
            element_size: Cell::new(element_size),
            array_length: ByteLengthSlot::new(ByteLength::Length(0)),
            byte_offset: Cell::new(0),
            cached_data_offset: Cell::new(TYPED_ARRAY_CACHED_DATA_OFFSET_INVALID),
            storage: TypedArrayBaseStorage {
                viewed_array_buffer: Cell::new(None),
                byte_length: Cell::new(ByteLength::Length(0)),
                content_type: Cell::new(kind.content_type()),
            },
        };
        assert!(array_length.checked_mul(u32::from(element_size)).is_some());
        // NB: C++ caches the data offset here, which the view can only do once it is a cell. Its creators do that
        //     with update_cached_data_offset() right after allocating it.
        typed_array.storage.viewed_array_buffer.set(Some(array_buffer));
        if array_length > 0 {
            assert!(!array_buffer.is_detached() && array_buffer.byte_length() > 0);
        }
        typed_array.array_length.set(ByteLength::Length(array_length));
        // NB: Like C++, the byte length starts out as the length of the whole buffer, truncated to a u32.
        typed_array
            .storage
            .byte_length
            .set(ByteLength::Length(array_buffer.byte_length() as u32));
        typed_array
    }

    pub fn array_length(&self) -> ByteLength {
        self.array_length.get()
    }

    pub fn byte_length(&self) -> ByteLength {
        self.storage.byte_length.get()
    }

    pub fn byte_offset(&self) -> u32 {
        self.byte_offset.get()
    }

    pub fn content_type(&self) -> ContentType {
        self.storage.content_type.get()
    }

    pub fn viewed_array_buffer(&self) -> Gc<ArrayBuffer> {
        self.storage
            .viewed_array_buffer
            .get()
            .expect("a typed array views a buffer")
    }

    // Cached cage offset: viewed_array_buffer->data_offset() + byte_offset.
    // invalid_cached_data_offset means "not cached, use slow path".
    pub fn cached_data_offset(&self) -> usize {
        self.cached_data_offset.get()
    }

    pub fn set_array_length(&self, length: ByteLength) {
        self.array_length.set(length);
    }

    pub fn set_byte_length(&self, length: ByteLength) {
        self.storage.byte_length.set(length);
    }

    pub fn set_byte_offset(&self, vm: &Vm, offset: u32) {
        self.byte_offset.set(offset);
        self.update_cached_data_offset(vm);
    }

    pub fn set_viewed_array_buffer(&self, vm: &Vm, array_buffer: Gc<ArrayBuffer>) {
        let previous_array_buffer = self.storage.viewed_array_buffer.replace(Some(array_buffer));
        // A cached offset means the view is registered with the buffer it was cached for, which is not this one.
        if previous_array_buffer != Some(array_buffer) {
            self.invalidate_cached_data_offset();
        }
        self.update_cached_data_offset(vm);
    }

    pub fn invalidate_cached_data_offset(&self) {
        self.cached_data_offset.set(TYPED_ARRAY_CACHED_DATA_OFFSET_INVALID);
    }

    pub fn kind(&self) -> Kind {
        Kind::from_u8(self.kind.get())
    }

    pub fn element_size(&self) -> u32 {
        u32::from(self.element_size.get())
    }

    pub fn element_name(&self, vm: &Vm) -> Utf16FlyString {
        let names = &vm.names;
        let name = match self.kind() {
            Kind::Uint8Array => &names.Uint8Array,
            Kind::Uint8ClampedArray => &names.Uint8ClampedArray,
            Kind::Uint16Array => &names.Uint16Array,
            Kind::Uint32Array => &names.Uint32Array,
            Kind::BigUint64Array => &names.BigUint64Array,
            Kind::Int8Array => &names.Int8Array,
            Kind::Int16Array => &names.Int16Array,
            Kind::Int32Array => &names.Int32Array,
            Kind::BigInt64Array => &names.BigInt64Array,
            Kind::Float16Array => &names.Float16Array,
            Kind::Float32Array => &names.Float32Array,
            Kind::Float64Array => &names.Float64Array,
        };
        name.as_string().clone()
    }

    /// Cell::class_name(), the name of the typed array's class.
    pub fn class_name(&self) -> &'static str {
        self.class().class_name()
    }

    // 25.1.3.11 IsUnclampedIntegerElementType ( type ), https://tc39.es/ecma262/#sec-isunclampedintegerelementtype
    pub fn is_unclamped_integer_element_type(&self) -> bool {
        matches!(
            self.kind(),
            Kind::Int8Array
                | Kind::Uint8Array
                | Kind::Int16Array
                | Kind::Uint16Array
                | Kind::Int32Array
                | Kind::Uint32Array
        )
    }

    // 25.1.3.12 IsBigIntElementType ( type ), https://tc39.es/ecma262/#sec-isbigintelementtype
    pub fn is_bigint_element_type(&self) -> bool {
        matches!(self.kind(), Kind::BigInt64Array | Kind::BigUint64Array)
    }

    // 25.1.3.16 GetValueFromBuffer ( arrayBuffer, byteIndex, type, isTypedArray, order [ , isLittleEndian ] ), https://tc39.es/ecma262/#sec-getvaluefrombuffer
    pub fn get_value_from_buffer(&self, vm: &Vm, byte_index: usize, order: Order) -> Value {
        self.viewed_array_buffer()
            .get_value(vm, byte_index, self.kind().element_type(), true, order, true)
    }

    // 25.1.3.18 SetValueInBuffer ( arrayBuffer, byteIndex, type, value, isTypedArray, order [ , isLittleEndian ] ), https://tc39.es/ecma262/#sec-setvalueinbuffer
    pub fn set_value_in_buffer(&self, vm: &Vm, byte_index: usize, value: Value, order: Order) {
        self.viewed_array_buffer()
            .set_value(vm, byte_index, self.kind().element_type(), value, true, order, true);
    }

    // 25.1.3.19 GetModifySetValueInBuffer ( arrayBuffer, byteIndex, type, value, op ), https://tc39.es/ecma262/#sec-getmodifysetvalueinbuffer
    pub fn get_modify_set_value_in_buffer(
        &self,
        vm: &Vm,
        byte_index: usize,
        value: Value,
        operation: ReadWriteModifyOperation,
    ) -> Value {
        self.viewed_array_buffer().get_modify_set_value(
            vm,
            byte_index,
            self.kind().element_type(),
            value,
            operation,
            true,
        )
    }

    pub fn intrinsic_constructor(&self, vm: &Vm, realm: Gc<Realm>) -> Gc<FunctionObject> {
        intrinsic_constructor_of(vm, realm, self.kind())
    }

    // OPTIMIZATION: Fast-path factories used by TypedArraySpeciesCreate when the resolved species constructor is the
    // default intrinsic. These bypass the public TypedArray constructor (which would otherwise allocate a throwaway
    // ArrayBuffer in the `is_object()` branch of its argument handling).
    pub fn create_default(
        &self,
        vm: &Vm,
        realm: Gc<Realm>,
        array_length: u32,
    ) -> ThrowCompletionOr<Gc<TypedArrayBase>> {
        let kind = self.kind();
        validate_typed_array_length(vm, array_length as usize, kind.element_type().size())?;
        create_typed_array(vm, realm, kind, array_length)
    }

    pub fn create_default_view_on_buffer(
        &self,
        vm: &Vm,
        realm: Gc<Realm>,
        buffer: Gc<ArrayBuffer>,
    ) -> Gc<TypedArrayBase> {
        create_typed_array_on_buffer(vm, realm, self.kind(), 0, buffer)
    }

    /// TypedArrayBase::create_from_slots(): restores a view from its [[ArrayLength]], [[ByteLength]] and [[ByteOffset]],
    /// as StructuredDeserialize does. The caller must already have checked that the view fits inside the buffer.
    pub fn create_from_slots(
        vm: &Vm,
        realm: Gc<Realm>,
        kind: Kind,
        array_buffer: Gc<ArrayBuffer>,
        array_length: ByteLength,
        byte_length: ByteLength,
        byte_offset: u32,
    ) -> Gc<TypedArrayBase> {
        let element_size = kind.element_type().size() as u32;
        assert!(array_length.is_auto() == byte_length.is_auto());
        assert!(byte_offset.is_multiple_of(element_size));
        if !array_length.is_auto() {
            let byte_length_of_array_length = array_length
                .length()
                .checked_mul(element_size)
                .expect("the byte length of the array length fits in 32 bits");
            assert!(byte_length_of_array_length == byte_length.length());
        }

        let typed_array = create_typed_array_on_buffer(vm, realm, kind, 0, array_buffer);
        typed_array.set_array_length(array_length);
        typed_array.set_byte_length(byte_length);
        typed_array.set_byte_offset(vm, byte_offset);
        typed_array
    }

    pub fn update_cached_data_offset(&self, vm: &Vm) {
        let Some(viewed_array_buffer) = self.storage.viewed_array_buffer.get() else {
            self.invalidate_cached_data_offset();
            return;
        };
        if !viewed_array_buffer.can_cache_typed_array_view_data_offset() {
            self.invalidate_cached_data_offset();
            return;
        }

        let data_offset = viewed_array_buffer.data_offset();
        if data_offset == INVALID_DATA_OFFSET {
            self.invalidate_cached_data_offset();
            return;
        }

        let cached_data_offset = data_offset
            .checked_add(self.byte_offset() as usize)
            .expect("the cached data offset does not overflow");

        // NB: C++ appends the view to the buffer's intrusive list every time, which moves it if it is listed already.
        //     A view with a cached offset is registered with its buffer, so only one without needs registering.
        if self.cached_data_offset() == TYPED_ARRAY_CACHED_DATA_OFFSET_INVALID {
            let view = self
                .as_gc()
                .downcast::<TypedArrayBase>()
                .expect("a typed array is a TypedArrayBase");
            viewed_array_buffer.register_cached_typed_array_view(vm, view);
        }
        self.cached_data_offset.set(cached_data_offset);
    }

    // 10.4.5.1 [[PreventExtensions]] ( ), https://tc39.es/ecma262/#sec-typedarray-preventextensions
    fn internal_prevent_extensions(object: &Object, vm: &Vm) -> ThrowCompletionOr<bool> {
        // 1. NOTE: The extensibility-related invariants specified in 6.1.7.3 do not allow this method to return true
        //    when O can gain (or lose and then regain) properties, which might occur for properties with integer index
        //    names when its underlying buffer is resized.

        // 2. If IsTypedArrayFixedLength(O) is false, return false.
        if !is_typed_array_fixed_length(as_typed_array(object)) {
            return Ok(false);
        }

        // 3. Return OrdinaryPreventExtensions(O).
        object.ordinary_prevent_extensions(vm)
    }

    // 10.4.5.2 [[GetOwnProperty]] ( P ), https://tc39.es/ecma262/#sec-integer-indexed-exotic-objects-getownproperty-p
    fn internal_get_own_property(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
    ) -> ThrowCompletionOr<Option<PropertyDescriptor>> {
        // NOTE: If the property name is a number type (An implementation-defined optimized
        // property key type), it can be treated as a string property that will transparently be
        // converted into a canonical numeric index.

        // 1. If P is a String, then
        // NOTE: This includes an implementation-defined optimization, see note above!
        if property_key.is_string() || property_key.is_number() {
            // a. Let numericIndex be CanonicalNumericIndexString(P).
            let numeric_index =
                canonical_numeric_index_string(property_key, CanonicalIndexMode::DetectNumericRoundtrip);
            // b. If numericIndex is not undefined, then
            if !numeric_index.is_undefined() {
                // i. Let value be TypedArrayGetElement(O, numericIndex).
                let value = typed_array_get_element(vm, as_typed_array(object), numeric_index);

                // ii. If value is undefined, return undefined.
                if value.is_undefined() {
                    return Ok(None);
                }

                // iii. Return the PropertyDescriptor { [[Value]]: value, [[Writable]]: true, [[Enumerable]]: true, [[Configurable]]: true }.
                return Ok(Some(PropertyDescriptor {
                    value: Some(value),
                    writable: Some(true),
                    enumerable: Some(true),
                    configurable: Some(true),
                    ..Default::default()
                }));
            }
        }

        // 2. Return OrdinaryGetOwnProperty(O, P).
        object.ordinary_get_own_property(vm, property_key)
    }

    // 10.4.5.3 [[HasProperty]] ( P ), https://tc39.es/ecma262/#sec-integer-indexed-exotic-objects-hasproperty-p
    fn internal_has_property(object: &Object, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<bool> {
        // NOTE: If the property name is a number type (An implementation-defined optimized
        // property key type), it can be treated as a string property that will transparently be
        // converted into a canonical numeric index.

        // 1. If P is a String, then
        // NOTE: This includes an implementation-defined optimization, see note above!
        if property_key.is_string() || property_key.is_number() {
            // a. Let numericIndex be CanonicalNumericIndexString(P).
            let numeric_index =
                canonical_numeric_index_string(property_key, CanonicalIndexMode::DetectNumericRoundtrip);
            // b. If numericIndex is not undefined, return IsValidIntegerIndex(O, numericIndex).
            if !numeric_index.is_undefined() {
                return Ok(is_valid_integer_index(as_typed_array(object), numeric_index));
            }
        }

        // 2. Return ? OrdinaryHasProperty(O, P).
        object.ordinary_has_property(vm, property_key)
    }

    // 10.4.5.4 [[DefineOwnProperty]] ( P, Desc ), https://tc39.es/ecma262/#sec-integer-indexed-exotic-objects-defineownproperty-p-desc
    fn internal_define_own_property(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
        property_descriptor: &mut PropertyDescriptor,
        precomputed_get_own_property: Option<&Option<PropertyDescriptor>>,
    ) -> ThrowCompletionOr<bool> {
        // NOTE: If the property name is a number type (An implementation-defined optimized
        // property key type), it can be treated as a string property that will transparently be
        // converted into a canonical numeric index.

        // 1. If P is a String, then
        // NOTE: This includes an implementation-defined optimization, see note above!
        if property_key.is_string() || property_key.is_number() {
            // a. Let numericIndex be CanonicalNumericIndexString(P).
            let numeric_index =
                canonical_numeric_index_string(property_key, CanonicalIndexMode::DetectNumericRoundtrip);
            // b. If numericIndex is not undefined, then
            if !numeric_index.is_undefined() {
                let typed_array = as_typed_array(object);

                // i. If IsValidIntegerIndex(O, numericIndex) is false, return false.
                if !is_valid_integer_index(typed_array, numeric_index) {
                    return Ok(false);
                }

                // ii. If Desc has a [[Configurable]] field and if Desc.[[Configurable]] is false, return false.
                if property_descriptor.configurable == Some(false) {
                    return Ok(false);
                }

                // iii. If Desc has an [[Enumerable]] field and if Desc.[[Enumerable]] is false, return false.
                if property_descriptor.enumerable == Some(false) {
                    return Ok(false);
                }

                // iv. If IsAccessorDescriptor(Desc) is true, return false.
                if property_descriptor.is_accessor_descriptor() {
                    return Ok(false);
                }

                // v. If Desc has a [[Writable]] field and if Desc.[[Writable]] is false, return false.
                if property_descriptor.writable == Some(false) {
                    return Ok(false);
                }

                // vi. If Desc has a [[Value]] field, perform ? TypedArraySetElement(O, numericIndex, Desc.[[Value]]).
                if let Some(value) = property_descriptor.value {
                    typed_array_set_element(vm, typed_array, numeric_index, value)?;
                }

                // vii. Return true.
                return Ok(true);
            }
        }

        // 2. Return ! OrdinaryDefineOwnProperty(O, P, Desc).
        object.ordinary_define_own_property(vm, property_key, property_descriptor, precomputed_get_own_property)
    }

    // 10.4.5.5 [[Get]] ( P, Receiver ), https://tc39.es/ecma262/#sec-typedarray-get
    fn internal_get(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
        receiver: Value,
        cacheable_metadata: Option<&mut CacheableGetPropertyMetadata>,
        phase: PropertyLookupPhase,
    ) -> ThrowCompletionOr<Value> {
        assert!(!receiver.is_empty());

        // NOTE: If the property name is a number type (An implementation-defined optimized
        // property key type), it can be treated as a string property that will transparently be
        // converted into a canonical numeric index.

        // 1. If P is a String, then
        // NOTE: This includes an implementation-defined optimization, see note above!
        if property_key.is_string() || property_key.is_number() {
            // a. Let numericIndex be CanonicalNumericIndexString(P).
            let numeric_index =
                canonical_numeric_index_string(property_key, CanonicalIndexMode::DetectNumericRoundtrip);
            // b. If numericIndex is not undefined, then
            if !numeric_index.is_undefined() {
                // i. Return TypedArrayGetElement(O, numericIndex).
                return Ok(typed_array_get_element(vm, as_typed_array(object), numeric_index));
            }
        }

        // 2. Return ? OrdinaryGet(O, P, Receiver).
        object.ordinary_get(vm, property_key, receiver, cacheable_metadata, phase)
    }

    // 10.4.5.6 [[Set]] ( P, V, Receiver ), https://tc39.es/ecma262/#sec-integer-indexed-exotic-objects-set-p-v-receiver
    fn internal_set(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
        value: Value,
        receiver: Value,
        _cacheable_metadata: Option<&mut CacheableSetPropertyMetadata>,
        _phase: PropertyLookupPhase,
    ) -> ThrowCompletionOr<bool> {
        assert!(!value.is_empty());
        assert!(!receiver.is_empty());

        // NOTE: If the property name is a number type (An implementation-defined optimized
        // property key type), it can be treated as a string property that will transparently be
        // converted into a canonical numeric index.

        // 1. If P is a String, then
        // NOTE: This includes an implementation-defined optimization, see note above!
        if property_key.is_string() || property_key.is_number() {
            // a. Let numericIndex be CanonicalNumericIndexString(P).
            let numeric_index =
                canonical_numeric_index_string(property_key, CanonicalIndexMode::DetectNumericRoundtrip);
            // b. If numericIndex is not undefined, then
            if !numeric_index.is_undefined() {
                let typed_array = as_typed_array(object);

                // i. If SameValue(O, Receiver) is true, then
                if same_value(Value::from_object(object.as_gc()), receiver) {
                    // 1. Perform ? TypedArraySetElement(O, numericIndex, V).
                    typed_array_set_element(vm, typed_array, numeric_index, value)?;

                    // 2. Return true.
                    return Ok(true);
                }

                // ii. If IsValidIntegerIndex(O, numericIndex) is false, return true.
                if !is_valid_integer_index(typed_array, numeric_index) {
                    return Ok(true);
                }
            }
        }

        // 2. Return ? OrdinarySet(O, P, V, Receiver).
        // NB: Like C++, this passes neither the cacheable metadata nor the lookup phase on.
        object.ordinary_set(
            vm,
            property_key,
            value,
            receiver,
            None,
            PropertyLookupPhase::OwnProperty,
        )
    }

    // 10.4.5.7 [[Delete]] ( P ), https://tc39.es/ecma262/#sec-integer-indexed-exotic-objects-delete-p
    fn internal_delete(object: &Object, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<bool> {
        // NOTE: If the property name is a number type (An implementation-defined optimized
        // property key type), it can be treated as a string property that will transparently be
        // converted into a canonical numeric index.

        // 1. If P is a String, then
        // NOTE: This includes an implementation-defined optimization, see note above!
        if property_key.is_string() || property_key.is_number() {
            // a. Let numericIndex be CanonicalNumericIndexString(P).
            let numeric_index =
                canonical_numeric_index_string(property_key, CanonicalIndexMode::DetectNumericRoundtrip);
            // b. If numericIndex is not undefined, then
            if !numeric_index.is_undefined() {
                // i. If IsValidIntegerIndex(O, numericIndex) is false, return true; else return false.
                return Ok(!is_valid_integer_index(as_typed_array(object), numeric_index));
            }
        }

        // 2. Return ? OrdinaryDelete(O, P).
        object.ordinary_delete(vm, property_key)
    }

    // 10.4.5.8 [[OwnPropertyKeys]] ( ), https://tc39.es/ecma262/#sec-integer-indexed-exotic-objects-ownpropertykeys
    #[allow(
        clippy::unnecessary_wraps,
        reason = "[[OwnPropertyKeys]] can throw for other objects"
    )]
    fn internal_own_property_keys<'vm>(object: &Object, vm: &'vm Vm) -> ThrowCompletionOr<MarkedVec<'vm, Value>> {
        let typed_array = as_typed_array(object);

        // 1. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(O, seq-cst).
        let typed_array_record = make_typed_array_with_buffer_witness_record(typed_array, Order::SeqCst);

        // 2. Let keys be a new empty List.
        let keys = MarkedVec::new(vm);

        // 3. If IsTypedArrayOutOfBounds(taRecord) is false, then
        if !is_typed_array_out_of_bounds(&typed_array_record) {
            // a. Let length be TypedArrayLength(taRecord).
            let length = typed_array_length(&typed_array_record);

            // b. For each integer i such that 0 ≤ i < length, in ascending order, do
            for i in 0..length {
                // i. Append ! ToString(𝔽(i)) to keys.
                keys.push(Value::from_string(PrimitiveString::create_from_unsigned_integer(
                    vm,
                    u64::from(i),
                )));
            }
        }

        // The keys are copied out first, since turning them into values allocates.
        let string_keys = MarkedVec::new(vm);
        let symbol_keys = MarkedVec::new(vm);
        object.shape().for_each_property_in_insertion_order(|property_key, _| {
            if property_key.is_string() {
                string_keys.push(property_key.clone());
            } else if property_key.is_symbol() && !property_key.is_private() {
                symbol_keys.push(property_key.clone());
            }
            ControlFlow::Continue(())
        });

        // 4. For each own property key P of O such that P is a String and P is not an integer index, in ascending chronological order of property creation, do
        for index in 0..string_keys.len() {
            let property_key: PropertyKey = string_keys.get(index).expect("the index is in bounds");
            // a. Append P to keys.
            keys.push(property_key.to_value(vm));
        }

        // 5. For each own property key P of O such that P is a Symbol, in ascending chronological order of property creation, do
        for index in 0..symbol_keys.len() {
            let property_key: PropertyKey = symbol_keys.get(index).expect("the index is in bounds");
            // a. Append P to keys.
            keys.push(property_key.to_value(vm));
        }

        // 6. Return keys.
        Ok(keys)
    }
}

// 10.4.5.9 TypedArray With Buffer Witness Records, https://tc39.es/ecma262/#sec-typedarray-with-buffer-witness-records
#[derive(Clone, Copy, Debug)]
pub struct TypedArrayWithBufferWitness {
    pub object: Gc<TypedArrayBase>,            // [[Object]]
    pub cached_buffer_byte_length: ByteLength, // [[CachedBufferByteLength]]
}

// 10.4.5.10 MakeTypedArrayWithBufferWitnessRecord ( obj, order ), https://tc39.es/ecma262/#sec-maketypedarraywithbufferwitnessrecord
pub fn make_typed_array_with_buffer_witness_record(
    typed_array: &TypedArrayBase,
    order: Order,
) -> TypedArrayWithBufferWitness {
    // 1. Let buffer be obj.[[ViewedArrayBuffer]].
    let buffer = typed_array.viewed_array_buffer();

    // 2. If IsDetachedBuffer(buffer) is true, then
    let byte_length = if buffer.is_detached() {
        // a. Let byteLength be detached.
        ByteLength::detached()
    }
    // 3. Else,
    else {
        // a. Let byteLength be ArrayBufferByteLength(buffer, order).
        // NB: Like C++, the length is truncated to a u32.
        ByteLength::Length(array_buffer_byte_length(&buffer, order) as u32)
    };

    // 4. Return the TypedArray With Buffer Witness Record { [[Object]]: obj, [[CachedBufferByteLength]]: byteLength }.
    TypedArrayWithBufferWitness {
        // SAFETY: Typed arrays only exist as cells.
        object: unsafe { Gc::from_ref(typed_array) },
        cached_buffer_byte_length: byte_length,
    }
}

// 10.4.5.12 TypedArrayByteLength ( taRecord ), https://tc39.es/ecma262/#sec-typedarraybytelength
pub fn typed_array_byte_length(typed_array_record: &TypedArrayWithBufferWitness) -> u32 {
    // 1. If IsTypedArrayOutOfBounds(taRecord) is true, return 0.
    if is_typed_array_out_of_bounds(typed_array_record) {
        return 0;
    }

    // 2. Let length be TypedArrayLength(taRecord).
    let length = typed_array_length(typed_array_record);

    // 3. If length = 0, return 0.
    if length == 0 {
        return 0;
    }

    // 4. Let O be taRecord.[[Object]].
    let object = typed_array_record.object;

    // 5. If O.[[ByteLength]] is not auto, return O.[[ByteLength]].
    if !object.byte_length().is_auto() {
        return object.byte_length().length();
    }

    // 6. Let elementSize be TypedArrayElementSize(O).
    let element_size = object.element_size();

    // 7. Return length × elementSize.
    length.wrapping_mul(element_size)
}

// 10.4.5.13 TypedArrayLength ( taRecord ), https://tc39.es/ecma262/#sec-typedarraylength
pub fn typed_array_length(typed_array_record: &TypedArrayWithBufferWitness) -> u32 {
    // 1. Assert: IsTypedArrayOutOfBounds(taRecord) is false.
    assert!(!is_typed_array_out_of_bounds(typed_array_record));

    // 2. Let O be taRecord.[[Object]].
    let object = typed_array_record.object;

    // 3. If O.[[ArrayLength]] is not auto, return O.[[ArrayLength]].
    if !object.array_length().is_auto() {
        return object.array_length().length();
    }

    // 4. Assert: IsFixedLengthArrayBuffer(O.[[ViewedArrayBuffer]]) is false.
    assert!(!object.viewed_array_buffer().is_fixed_length());

    // 5. Let byteOffset be O.[[ByteOffset]].
    let byte_offset = object.byte_offset();

    // 6. Let elementSize be TypedArrayElementSize(O).
    let element_size = object.element_size();

    // 7. Let byteLength be taRecord.[[CachedBufferByteLength]].
    let byte_length = typed_array_record.cached_buffer_byte_length;

    // 8. Assert: byteLength is not detached.
    assert!(!byte_length.is_detached());

    // 9. Return floor((byteLength - byteOffset) / elementSize).
    byte_length.length().wrapping_sub(byte_offset) / element_size
}

// 10.4.5.14 IsTypedArrayOutOfBounds ( taRecord ), https://tc39.es/ecma262/#sec-istypedarrayoutofbounds
pub fn is_typed_array_out_of_bounds(typed_array_record: &TypedArrayWithBufferWitness) -> bool {
    // 1. Let O be taRecord.[[Object]].
    let object = typed_array_record.object;

    // 2. Let bufferByteLength be taRecord.[[CachedBufferByteLength]].
    let buffer_byte_length = typed_array_record.cached_buffer_byte_length;

    // 3. Assert: IsDetachedBuffer(O.[[ViewedArrayBuffer]]) is true if and only if bufferByteLength is detached.
    assert!(object.viewed_array_buffer().is_detached() == buffer_byte_length.is_detached());

    // 4. If bufferByteLength is detached, return true.
    if buffer_byte_length.is_detached() {
        return true;
    }

    // 5. Let byteOffsetStart be O.[[ByteOffset]].
    let byte_offset_start = object.byte_offset();

    // 6. If O.[[ArrayLength]] is auto, then
    let byte_offset_end = if object.array_length().is_auto() {
        // a. Let byteOffsetEnd be bufferByteLength.
        buffer_byte_length.length()
    }
    // 7. Else,
    else {
        // a. Let elementSize be TypedArrayElementSize(O).
        let element_size = object.element_size();

        // b. Let byteOffsetEnd be byteOffsetStart + O.[[ArrayLength]] × elementSize.
        // NB: Like C++, this is computed in 32 bits.
        byte_offset_start.wrapping_add(object.array_length().length().wrapping_mul(element_size))
    };

    // 8. If byteOffsetStart > bufferByteLength or byteOffsetEnd > bufferByteLength, return true.
    if byte_offset_start > buffer_byte_length.length() || byte_offset_end > buffer_byte_length.length() {
        return true;
    }

    // 9. NOTE: 0-length TypedArrays are not considered out-of-bounds.
    // 10. Return false.
    false
}

// 10.4.5.15 IsTypedArrayFixedLength ( O ), https://tc39.es/ecma262/#sec-istypedarrayfixedlength
pub fn is_typed_array_fixed_length(typed_array: &TypedArrayBase) -> bool {
    // 1. If O.[[ArrayLength]] is AUTO, return false.
    if typed_array.array_length().is_auto() {
        return false;
    }

    // 2. Let buffer be O.[[ViewedArrayBuffer]].
    let buffer = typed_array.viewed_array_buffer();

    // 3. If IsFixedLengthArrayBuffer(buffer) is false and IsSharedArrayBuffer(buffer) is false, return false.
    if !buffer.is_fixed_length() && !buffer.is_shared_array_buffer() {
        return false;
    }

    // 4. Return true.
    true
}

// 10.4.5.16 IsValidIntegerIndex ( O, index ), https://tc39.es/ecma262/#sec-isvalidintegerindex
pub fn is_valid_integer_index(typed_array: &TypedArrayBase, property_index: CanonicalIndex) -> bool {
    // 1. If IsDetachedBuffer(O.[[ViewedArrayBuffer]]) is true, return false.
    let buffer = typed_array.viewed_array_buffer();
    if buffer.is_detached() {
        return false;
    }

    // 2. If IsIntegralNumber(index) is false, return false.
    // 3. If index is -0𝔽, return false.
    if !property_index.is_index() {
        return false;
    }

    // OPTIMIZATION: For TypedArrays with non-resizable ArrayBuffers, we can avoid most of the work performed by
    //               IsValidIntegerIndex. We just need to check whether the array itself is out-of-bounds and if
    //               the provided index is within the array bounds.
    let array_length = typed_array.array_length();
    if !array_length.is_auto() {
        let byte_length = array_buffer_byte_length(&buffer, Order::Unordered);
        // NB: Like C++, the end is computed in 32 bits.
        let byte_offset_end = typed_array
            .byte_offset()
            .wrapping_add(array_length.length().wrapping_mul(typed_array.element_size()));

        return typed_array.byte_offset() as usize <= byte_length
            && byte_offset_end as usize <= byte_length
            && property_index.as_index() < array_length.length();
    }

    is_valid_integer_index_slow_case(typed_array, property_index)
}

pub fn is_valid_integer_index_slow_case(typed_array: &TypedArrayBase, property_index: CanonicalIndex) -> bool {
    // 4. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(O, unordered).
    let typed_array_record = make_typed_array_with_buffer_witness_record(typed_array, Order::Unordered);

    // 5. NOTE: Bounds checking is not a synchronizing operation when O's backing buffer is a growable SharedArrayBuffer.

    // 6. If IsTypedArrayOutOfBounds(taRecord) is true, return false.
    if is_typed_array_out_of_bounds(&typed_array_record) {
        return false;
    }

    // 7. Let length be TypedArrayLength(taRecord).
    let length = typed_array_length(&typed_array_record);

    // 8. If ℝ(index) < 0 or ℝ(index) ≥ length, return false.
    if property_index.as_index() >= length {
        return false;
    }

    // 9. Return true.
    true
}

// 10.4.5.17 TypedArrayGetElement ( O, index ), https://tc39.es/ecma262/#sec-typedarraygetelement
pub fn typed_array_get_element(vm: &Vm, typed_array: &TypedArrayBase, property_index: CanonicalIndex) -> Value {
    // 1. If IsValidIntegerIndex(O, index) is false, return undefined.
    if !is_valid_integer_index(typed_array, property_index) {
        return Value::UNDEFINED;
    }

    // 2. Let offset be O.[[ByteOffset]].
    let offset = typed_array.byte_offset();

    // 3. Let elementSize be TypedArrayElementSize(O).
    // 4. Let byteIndexInBuffer be (ℝ(index) × elementSize) + offset.
    let byte_index_in_buffer = (property_index.as_index() as usize)
        .checked_mul(typed_array.element_size() as usize)
        .and_then(|byte_index| byte_index.checked_add(offset as usize));
    // FIXME: Not exactly sure what we should do when overflow occurs.
    //        Just return as if it's an invalid index for now.
    let Some(byte_index_in_buffer) = byte_index_in_buffer else {
        eprintln!("typed_array_get_element(): byte_index_in_buffer overflowed, returning as if it's an invalid index.");
        return Value::UNDEFINED;
    };

    // 5. Let elementType be TypedArrayElementType(O).
    // 6. Return GetValueFromBuffer(O.[[ViewedArrayBuffer]], byteIndexInBuffer, elementType, true, unordered).
    typed_array.get_value_from_buffer(vm, byte_index_in_buffer, Order::Unordered)
}

// 10.4.5.18 TypedArraySetElement ( O, index, value ), https://tc39.es/ecma262/#sec-typedarraysetelement
// NOTE: In error cases, the function will return as if it succeeded.
pub fn typed_array_set_element(
    vm: &Vm,
    typed_array: &TypedArrayBase,
    property_index: CanonicalIndex,
    value: Value,
) -> ThrowCompletionOr<()> {
    assert!(!value.is_empty());

    // 1. If O.[[ContentType]] is BigInt, let numValue be ? ToBigInt(value).
    let num_value = if typed_array.content_type() == ContentType::BigInt {
        Value::from_bigint(value.to_bigint(vm)?)
    }
    // 2. Otherwise, let numValue be ? ToNumber(value).
    else {
        value.to_number(vm)?
    };

    // 3. If IsValidIntegerIndex(O, index) is true, then
    // NOTE: Inverted for flattened logic.
    if !is_valid_integer_index(typed_array, property_index) {
        return Ok(());
    }

    // a. Let offset be O.[[ByteOffset]].
    let offset = typed_array.byte_offset();

    // b. Let elementSize be TypedArrayElementSize(O).
    // c. Let byteIndexInBuffer be (ℝ(index) × elementSize) + offset.
    let byte_index_in_buffer = (property_index.as_index() as usize)
        .checked_mul(typed_array.element_size() as usize)
        .and_then(|byte_index| byte_index.checked_add(offset as usize));
    // FIXME: Not exactly sure what we should do when overflow occurs.
    //        Just return as if it succeeded for now.
    let Some(byte_index_in_buffer) = byte_index_in_buffer else {
        eprintln!("typed_array_set_element(): byte_index_in_buffer overflowed, returning as if succeeded.");
        return Ok(());
    };

    // d. Let elementType be TypedArrayElementType(O).
    // e. Perform SetValueInBuffer(O.[[ViewedArrayBuffer]], byteIndexInBuffer, elementType, numValue, true, unordered).
    typed_array.set_value_in_buffer(vm, byte_index_in_buffer, num_value, Order::Unordered);

    // 4. Return unused.
    Ok(())
}

pub fn typed_array_from(vm: &Vm, typed_array_value: Value) -> ThrowCompletionOr<Gc<TypedArrayBase>> {
    let this_object = typed_array_value.to_object(vm)?;
    if !this_object.is_typed_array() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"TypedArray"]);
    }

    Ok(this_object
        .downcast::<TypedArrayBase>()
        .expect("an object with the typed array flag is a typed array"))
}

pub fn validate_typed_array_length(vm: &Vm, array_length: usize, element_size: usize) -> ThrowCompletionOr<()> {
    if array_length > i32::MAX as usize / element_size {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"typed array"]);
    }
    if array_length.checked_mul(element_size).is_none() {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"typed array"]);
    }
    Ok(())
}

// 22.2.5.1.3 InitializeTypedArrayFromArrayBuffer, https://tc39.es/ecma262/#sec-initializetypedarrayfromarraybuffer
pub fn initialize_typed_array_from_array_buffer(
    vm: &Vm,
    typed_array: &TypedArrayBase,
    array_buffer: Gc<ArrayBuffer>,
    byte_offset: Value,
    length: Value,
) -> ThrowCompletionOr<()> {
    // 1. Let elementSize be TypedArrayElementSize(O).
    let element_size = typed_array.element_size() as usize;

    // 2. Let offset be ? ToIndex(byteOffset).
    let offset = byte_offset.to_index(vm)? as usize;

    // 3. If offset modulo elementSize ≠ 0, throw a RangeError exception.
    if !offset.is_multiple_of(element_size) {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TypedArrayInvalidByteOffset,
            &[&typed_array.class_name(), &element_size, &offset],
        );
    }

    // 4. Let bufferIsFixedLength be IsFixedLengthArrayBuffer(buffer).
    let buffer_is_fixed_length = array_buffer.is_fixed_length();

    let mut new_length = 0usize;
    // 5. If length is not undefined, then
    if !length.is_undefined() {
        // a. Let newLength be ? ToIndex(length).
        new_length = length.to_index(vm)? as usize;
    }

    // 6. If IsDetachedBuffer(buffer) is true, throw a TypeError exception.
    if array_buffer.is_detached() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::DetachedArrayBuffer, &[]);
    }

    // 7. Let bufferByteLength be ArrayBufferByteLength(buffer, seq-cst).
    let buffer_byte_length = array_buffer_byte_length(&array_buffer, Order::SeqCst);

    // 8. If length is undefined and bufferIsFixedLength is false, then
    if length.is_undefined() && !buffer_is_fixed_length {
        // a. If offset > bufferByteLength, throw a RangeError exception.
        if offset > buffer_byte_length {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::TypedArrayOutOfRangeByteOffset,
                &[&offset, &buffer_byte_length],
            );
        }

        // b. Set O.[[ByteLength]] to auto.
        typed_array.set_byte_length(ByteLength::auto_());

        // c. Set O.[[ArrayLength]] to auto.
        typed_array.set_array_length(ByteLength::auto_());
    }
    // 9. Else,
    else {
        let new_byte_length: usize;

        // a. If length is undefined, then
        if length.is_undefined() {
            // i. If bufferByteLength modulo elementSize ≠ 0, throw a RangeError exception.
            if !buffer_byte_length.is_multiple_of(element_size) {
                return vm.throw_completion(
                    ErrorKind::RangeError,
                    ErrorType::TypedArrayInvalidBufferLength,
                    &[&typed_array.class_name(), &element_size, &buffer_byte_length],
                );
            }

            // ii. Let newByteLength be bufferByteLength - offset.
            // iii. If newByteLength < 0, throw a RangeError exception.
            if offset > buffer_byte_length {
                return vm.throw_completion(
                    ErrorKind::RangeError,
                    ErrorType::TypedArrayOutOfRangeByteOffset,
                    &[&offset, &buffer_byte_length],
                );
            }
            new_byte_length = buffer_byte_length - offset;
        }
        // b. Else,
        else {
            // i. Let newByteLength be newLength × elementSize.
            let Some(byte_length) = new_length.checked_mul(element_size) else {
                return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"typed array"]);
            };
            new_byte_length = byte_length;

            // ii. If offset + newByteLength > bufferByteLength, throw a RangeError exception.
            let Some(new_byte_end) = offset.checked_add(new_byte_length) else {
                return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"typed array"]);
            };

            if new_byte_end > buffer_byte_length {
                return vm.throw_completion(
                    ErrorKind::RangeError,
                    ErrorType::TypedArrayOutOfRangeByteOffsetOrLength,
                    &[&offset, &new_byte_end, &buffer_byte_length],
                );
            }
        }

        let new_array_length = new_byte_length / element_size;
        validate_typed_array_length(vm, new_array_length, element_size)?;

        // c. Set O.[[ByteLength]] to newByteLength.
        typed_array.set_byte_length(ByteLength::Length(new_byte_length as u32));

        // d. Set O.[[ArrayLength]] to newByteLength / elementSize.
        typed_array.set_array_length(ByteLength::Length(new_array_length as u32));
    }

    // 10. Set O.[[ViewedArrayBuffer]] to buffer.
    typed_array.set_viewed_array_buffer(vm, array_buffer);

    // 11. Set O.[[ByteOffset]] to offset.
    if offset > u32::MAX as usize {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"typed array"]);
    }
    typed_array.set_byte_offset(vm, offset as u32);

    // 12. Return unused.
    Ok(())
}

// 23.2.5.1.2 InitializeTypedArrayFromTypedArray ( O, srcArray ), https://tc39.es/ecma262/#sec-initializetypedarrayfromtypedarray
fn initialize_typed_array_from_typed_array(
    vm: &Vm,
    typed_array: &TypedArrayBase,
    source_array: &TypedArrayBase,
) -> ThrowCompletionOr<()> {
    let realm = vm.current_realm().expect("a typed array is initialized in a realm");

    // 1. Let srcData be srcArray.[[ViewedArrayBuffer]].
    let source_data = source_array.viewed_array_buffer();

    // 2. Let elementType be TypedArrayElementType(O).
    let element_type = typed_array.element_name(vm);

    // 3. Let elementSize be TypedArrayElementSize(O).
    let element_size = typed_array.element_size() as usize;

    // 4. Let srcType be TypedArrayElementType(srcArray).
    let source_type = source_array.element_name(vm);

    // 5. Let srcElementSize be TypedArrayElementSize(srcArray).
    let source_element_size = source_array.element_size() as u64;

    // 6. Let srcByteOffset be srcArray.[[ByteOffset]].
    let source_byte_offset = source_array.byte_offset();

    // 7. Let srcRecord be MakeTypedArrayWithBufferWitnessRecord(srcArray, seq-cst).
    let source_record = make_typed_array_with_buffer_witness_record(source_array, Order::SeqCst);

    // 8. If IsTypedArrayOutOfBounds(srcRecord) is true, throw a TypeError exception.
    if is_typed_array_out_of_bounds(&source_record) {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::BufferOutOfBounds, &[&"TypedArray"]);
    }

    // 9. Let elementLength be TypedArrayLength(srcRecord).
    let element_length = typed_array_length(&source_record);

    // 10. Let byteLength be elementSize × elementLength.
    let Some(byte_length) = element_size.checked_mul(element_length as usize) else {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"typed array"]);
    };

    // 11. If elementType is srcType, then
    let data = if element_type == source_type {
        // a. Let data be ? CloneArrayBuffer(srcData, srcByteOffset, byteLength).
        clone_array_buffer(vm, source_data, source_byte_offset as usize, byte_length)?
    }
    // 12. Else,
    else {
        // a. Let data be ? AllocateArrayBuffer(%ArrayBuffer%, byteLength).
        let data = allocate_array_buffer(vm, realm.intrinsics().array_buffer_constructor(vm), byte_length, None)?;

        // b. If srcArray.[[ContentType]] is not O.[[ContentType]], throw a TypeError exception.
        if source_array.content_type() != typed_array.content_type() {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::TypedArrayContentTypeMismatch,
                &[&typed_array.class_name(), &source_array.class_name()],
            );
        }

        // c. Let srcByteIndex be srcByteOffset.
        let mut source_byte_index = u64::from(source_byte_offset);

        // d. Let targetByteIndex be 0.
        let mut target_byte_index: u64 = 0;

        // e. Let count be elementLength.
        // f. Repeat, while count > 0,
        for _ in 0..element_length {
            // i. Let value be GetValueFromBuffer(srcData, srcByteIndex, srcType, true, unordered).
            let value = source_array.get_value_from_buffer(vm, source_byte_index as usize, Order::Unordered);

            // ii. Perform SetValueInBuffer(data, targetByteIndex, elementType, value, true, unordered).
            data.set_value(
                vm,
                target_byte_index as usize,
                typed_array.kind().element_type(),
                value,
                true,
                Order::Unordered,
                true,
            );

            // iii. Set srcByteIndex to srcByteIndex + srcElementSize.
            source_byte_index += source_element_size;

            // iv. Set targetByteIndex to targetByteIndex + elementSize.
            target_byte_index += element_size as u64;

            // v. Set count to count - 1.
        }
        data
    };

    // 13. Set O.[[ViewedArrayBuffer]] to data.
    typed_array.set_viewed_array_buffer(vm, data);

    // 14. Set O.[[ByteLength]] to byteLength.
    typed_array.set_byte_length(ByteLength::Length(byte_length as u32));

    // 15. Set O.[[ByteOffset]] to 0.
    typed_array.set_byte_offset(vm, 0);

    // 16. Set O.[[ArrayLength]] to elementLength.
    typed_array.set_array_length(ByteLength::Length(element_length));

    // 17. Return unused.
    Ok(())
}

// 23.2.5.1.6 AllocateTypedArrayBuffer ( O, length ), https://tc39.es/ecma262/#sec-allocatetypedarraybuffer
fn allocate_typed_array_buffer(vm: &Vm, typed_array: &TypedArrayBase, length: usize) -> ThrowCompletionOr<()> {
    let realm = vm.current_realm().expect("a typed array is initialized in a realm");

    // Enforce 2GB "Excessive Length" limit
    if length > i32::MAX as usize / typed_array.kind().element_type().size() {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"typed array"]);
    }

    // 1. Assert: O.[[ViewedArrayBuffer]] is undefined.

    // 2. Let elementSize be TypedArrayElementSize(O).
    let element_size = typed_array.element_size() as usize;
    if element_size.checked_mul(length).is_none() {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"typed array"]);
    }

    // 3. Let byteLength be elementSize × length.
    let byte_length = element_size * length;

    // 4. Let data be ? AllocateArrayBuffer(%ArrayBuffer%, byteLength).
    let data = allocate_array_buffer(vm, realm.intrinsics().array_buffer_constructor(vm), byte_length, None)?;

    // 5. Set O.[[ViewedArrayBuffer]] to data.
    typed_array.set_viewed_array_buffer(vm, data);

    // 6. Set O.[[ByteLength]] to byteLength.
    typed_array.set_byte_length(ByteLength::Length(byte_length as u32));

    // 7. Set O.[[ByteOffset]] to 0.
    typed_array.set_byte_offset(vm, 0);

    // 8. Set O.[[ArrayLength]] to length.
    typed_array.set_array_length(ByteLength::Length(length as u32));

    // 9. Return unused.
    Ok(())
}

// 23.2.5.1.5 InitializeTypedArrayFromArrayLike, https://tc39.es/ecma262/#sec-initializetypedarrayfromarraylike
fn initialize_typed_array_from_array_like(
    vm: &Vm,
    typed_array: &TypedArrayBase,
    array_like: &Object,
) -> ThrowCompletionOr<()> {
    // 1. Let len be ? LengthOfArrayLike(arrayLike).
    let length = length_of_array_like(vm, array_like)?;

    // 2. Perform ? AllocateTypedArrayBuffer(O, len).
    allocate_typed_array_buffer(vm, typed_array, length as usize)?;

    // 3. Let k be 0.
    // 4. Repeat, while k < len,
    for k in 0..length {
        // a. Let Pk be ! ToString(𝔽(k)).
        // b. Let kValue be ? Get(arrayLike, Pk).
        let k_value = array_like.get(vm, &PropertyKey::from_number(k))?;

        // c. Perform ? Set(O, Pk, kValue, true).
        typed_array.set(vm, &PropertyKey::from_number(k), k_value, ShouldThrowExceptions::Yes)?;

        // d. Set k to k + 1.
    }

    // 5. Return unused.
    Ok(())
}

// 23.2.5.1.4 InitializeTypedArrayFromList, https://tc39.es/ecma262/#sec-initializetypedarrayfromlist
fn initialize_typed_array_from_list(
    vm: &Vm,
    typed_array: &TypedArrayBase,
    list: &MarkedVec<'_, Value>,
) -> ThrowCompletionOr<()> {
    // 1. Let len be the number of elements in values.
    let length = list.len();

    // 2. Perform ? AllocateTypedArrayBuffer(O, len).
    allocate_typed_array_buffer(vm, typed_array, length)?;

    // 3. Let k be 0.
    // 4. Repeat, while k < len,
    for k in 0..length {
        // a. Let Pk be ! ToString(𝔽(k)).
        // b. Let kValue be the first element of values and remove that element from values.
        let value = list.get(k).expect("the index is in bounds");

        // c. Perform ? Set(O, Pk, kValue, true).
        typed_array.set(
            vm,
            &PropertyKey::from_number(k as u64),
            value,
            ShouldThrowExceptions::Yes,
        )?;

        // d. Set k to k + 1.
    }

    // 5. Assert: values is now an empty List.
    // 6. Return unused.
    Ok(())
}

// 23.2.4.2 TypedArrayCreate ( constructor, argumentList ), https://tc39.es/ecma262/#typedarray-create
pub fn typed_array_create(
    vm: &Vm,
    constructor: Gc<FunctionObject>,
    arguments: &[Value],
) -> ThrowCompletionOr<Gc<TypedArrayBase>> {
    let first_argument = (arguments.len() == 1 && arguments[0].is_number()).then(|| arguments[0].as_f64());

    // 1. Let newTypedArray be ? Construct(constructor, argumentList).
    let new_typed_array = construct(vm, constructor, arguments, None)?;

    // 2. Let taRecord be ? ValidateTypedArray(newTypedArray, seq-cst).
    let typed_array_record = validate_typed_array(vm, &new_typed_array, Order::SeqCst)?;

    // 3. If the number of elements in argumentList is 1 and argumentList[0] is a Number, then
    if let Some(first_argument) = first_argument {
        // a. If IsTypedArrayOutOfBounds(taRecord) is true, throw a TypeError exception.
        if is_typed_array_out_of_bounds(&typed_array_record) {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::BufferOutOfBounds, &[&"TypedArray"]);
        }

        // b. Let length be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record);

        // c. If length < ℝ(argumentList[0]), throw a TypeError exception.
        if f64::from(length) < first_argument {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::InvalidLength, &[&"typed array"]);
        }
    }

    // 4. Return newTypedArray.
    Ok(typed_array_record.object)
}

// 23.2.4.3 TypedArrayCreateSameType ( exemplar, argumentList ), https://tc39.es/ecma262/#sec-typedarray-create-same-type
pub fn typed_array_create_same_type(
    vm: &Vm,
    exemplar: &TypedArrayBase,
    arguments: &[Value],
) -> ThrowCompletionOr<Gc<TypedArrayBase>> {
    let realm = vm.current_realm().expect("a typed array is created in a realm");

    // 1. Let constructor be the intrinsic object associated with the constructor name exemplar.[[TypedArrayName]] in Table 68.
    let constructor = exemplar.intrinsic_constructor(vm, realm);

    // 2. Let result be ? TypedArrayCreate(constructor, argumentList).
    let result = typed_array_create(vm, constructor, arguments)?;

    // 3. Assert: result has [[TypedArrayName]] and [[ContentType]] internal slots.
    // 4. Assert: result.[[ContentType]] is exemplar.[[ContentType]].
    // 5. Return result.
    Ok(result)
}

// 23.2.4.4 ValidateTypedArray ( O ), https://tc39.es/ecma262/#sec-validatetypedarray
pub fn validate_typed_array(vm: &Vm, object: &Object, order: Order) -> ThrowCompletionOr<TypedArrayWithBufferWitness> {
    // 1. Perform ? RequireInternalSlot(O, [[TypedArrayName]]).
    if !object.is_typed_array() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"TypedArray"]);
    }

    // 2. Assert: O has a [[ViewedArrayBuffer]] internal slot.
    let typed_array = as_typed_array(object);

    // 3. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(O, order).
    let typed_array_record = make_typed_array_with_buffer_witness_record(typed_array, order);

    // 4. If IsTypedArrayOutOfBounds(taRecord) is true, throw a TypeError exception.
    if is_typed_array_out_of_bounds(&typed_array_record) {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::BufferOutOfBounds, &[&"TypedArray"]);
    }

    // 5. Return taRecord.
    Ok(typed_array_record)
}

// 23.2.4.7 CompareTypedArrayElements ( x, y, comparefn ), https://tc39.es/ecma262/#sec-typedarray-create-same-type
pub fn compare_typed_array_elements(
    vm: &Vm,
    x: Value,
    y: Value,
    comparefn: Option<Gc<FunctionObject>>,
) -> ThrowCompletionOr<f64> {
    // 1. Assert: x is a Number and y is a Number, or x is a BigInt and y is a BigInt.
    assert!((x.is_number() && y.is_number()) || (x.is_bigint() && y.is_bigint()));

    // 2. If comparefn is not undefined, then
    if let Some(comparefn) = comparefn {
        // a. Let v be ? ToNumber(? Call(comparefn, undefined, « x, y »)).
        let value = call_function_object(vm, comparefn, Value::UNDEFINED, &[x, y])?;
        let value_number = value.to_number(vm)?;

        // b. If v is NaN, return +0𝔽.
        if value_number.is_nan() {
            return Ok(0.0);
        }

        // c. Return v.
        return Ok(value_number.as_f64());
    }

    // 3. If x and y are both NaN, return +0𝔽.
    if x.is_nan() && y.is_nan() {
        return Ok(0.0);
    }

    // 4. If x is NaN, return 1𝔽.
    if x.is_nan() {
        return Ok(1.0);
    }

    // 5. If y is NaN, return -1𝔽.
    if y.is_nan() {
        return Ok(-1.0);
    }

    // 6. If x < y, return -1𝔽.
    let is_less_than = if x.is_bigint() {
        x.as_bigint().big_integer() < y.as_bigint().big_integer()
    } else {
        x.as_f64() < y.as_f64()
    };
    if is_less_than {
        return Ok(-1.0);
    }

    // 7. If x > y, return 1𝔽.
    let is_greater_than = if x.is_bigint() {
        x.as_bigint().big_integer() > y.as_bigint().big_integer()
    } else {
        x.as_f64() > y.as_f64()
    };
    if is_greater_than {
        return Ok(1.0);
    }

    // 8. If x is -0𝔽 and y is +0𝔽, return -1𝔽.
    if x.is_negative_zero() && y.is_positive_zero() {
        return Ok(-1.0);
    }

    // 9. If x is +0𝔽 and y is -0𝔽, return 1𝔽.
    if x.is_positive_zero() && y.is_negative_zero() {
        return Ok(1.0);
    }

    // 10. Return +0𝔽.
    Ok(0.0)
}

/// CanonicalIndex::from_double(): the index of a double, or a RangeError if it does not fit in a u32.
pub fn canonical_index_from_double(
    vm: &Vm,
    index_type: CanonicalIndexType,
    index: f64,
) -> ThrowCompletionOr<CanonicalIndex> {
    if index < f64::from(u32::MIN) || index > f64::from(u32::MAX) {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TypedArrayInvalidIntegerIndex,
            &[&AkDouble(index)],
        );
    }
    Ok(CanonicalIndex::new(index_type, index as u32))
}

/// Generates the class of each kind of typed array, of its prototype and of its constructor, and the functions that
/// stand in for the C++ virtual functions that differ between them.
macro_rules! define_typed_arrays {
    ($($class:ident, $snake:ident, $prototype:ident, $prototype_accessor:ident, $constructor:ident, $constructor_accessor:ident;)*) => {
        $(
            #[repr(C)]
            #[derive(Trace)]
            pub struct $class {
                base: TypedArrayBase,
            }

            define_cell!($class, Object, extends: [TypedArrayBase, Object]);

            impl Deref for $class {
                type Target = TypedArrayBase;

                fn deref(&self) -> &TypedArrayBase {
                    &self.base
                }
            }

            impl $class {
                /// ClassName::create(Realm&, u32 length, FunctionObject& new_target)
                pub fn create_with_new_target(
                    vm: &Vm,
                    realm: Gc<Realm>,
                    length: u32,
                    new_target: Gc<FunctionObject>,
                ) -> ThrowCompletionOr<Gc<$class>> {
                    let prototype = get_prototype_from_constructor(vm, new_target, Intrinsics::$prototype_accessor)?;
                    let element_size = Kind::$class.element_type().size();
                    let array_buffer =
                        ArrayBuffer::create(vm, realm, length as usize * element_size, crate::runtime::array_buffer::Shared::No)?;
                    let typed_array = realm.create_object(
                        vm,
                        $class {
                            base: TypedArrayBase::new(vm, Self::CLASS, prototype, length, array_buffer, Kind::$class),
                        },
                    );
                    typed_array.update_cached_data_offset(vm);
                    Ok(typed_array)
                }

                /// ClassName::create(Realm&, u32 length)
                pub fn create(vm: &Vm, realm: Gc<Realm>, length: u32) -> ThrowCompletionOr<Gc<$class>> {
                    let element_size = Kind::$class.element_type().size();
                    let array_buffer =
                        ArrayBuffer::create(vm, realm, length as usize * element_size, crate::runtime::array_buffer::Shared::No)?;
                    Ok(Self::create_on_buffer(vm, realm, length, array_buffer))
                }

                /// ClassName::create(Realm&, u32 length, ArrayBuffer& buffer)
                pub fn create_on_buffer(
                    vm: &Vm,
                    realm: Gc<Realm>,
                    length: u32,
                    array_buffer: Gc<ArrayBuffer>,
                ) -> Gc<$class> {
                    let prototype = realm.intrinsics().$prototype_accessor(vm);
                    let typed_array = realm.create_object(
                        vm,
                        $class {
                            base: TypedArrayBase::new(vm, Self::CLASS, prototype, length, array_buffer, Kind::$class),
                        },
                    );
                    typed_array.update_cached_data_offset(vm);
                    typed_array
                }
            }

            #[repr(C)]
            #[derive(Trace)]
            pub struct $prototype {
                base: Object,
            }

            define_object_class!($prototype, extends: [Object], methods: {
                initialize: $prototype::initialize,
                ..ORDINARY_OBJECT_METHODS
            });

            impl $prototype {
                pub fn create(vm: &Vm, realm: Gc<Realm>, typed_array_prototype: Gc<Object>) -> Gc<$prototype> {
                    realm.create_object(
                        vm,
                        $prototype {
                            base: Object::new_with_prototype(
                                vm,
                                Self::CLASS,
                                typed_array_prototype,
                                MayInterfereWithIndexedPropertyAccess::Yes,
                            ),
                        },
                    )
                }

                fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
                    object.define_direct_property(
                        vm,
                        &vm.names.BYTES_PER_ELEMENT,
                        Value::from_i32(Kind::$class.element_type().size() as i32),
                        PropertyAttributes::new(0),
                    );

                    if Kind::$class == Kind::Uint8Array {
                        Uint8ArrayPrototypeHelpers::initialize(vm, realm, object);
                    }
                }
            }

            #[repr(C)]
            #[derive(Trace)]
            pub struct $constructor {
                base: NativeFunction,
            }

            define_native_function_class!(
                $constructor,
                initialize: $constructor::initialize,
                call: $constructor::call,
                construct: $constructor::construct
            );

            impl $constructor {
                pub fn create(vm: &Vm, realm: Gc<Realm>, typed_array_constructor: Gc<Object>) -> Gc<$constructor> {
                    realm.create_object(
                        vm,
                        $constructor {
                            base: NativeFunction::new_with_name(
                                vm,
                                Self::CLASS,
                                vm.names.$class.as_string().clone(),
                                typed_array_constructor,
                            ),
                        },
                    )
                }

                fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
                    // 23.2.6.2 TypedArray.prototype, https://tc39.es/ecma262/#sec-typedarray.prototype
                    object.define_direct_property(
                        vm,
                        &vm.names.prototype,
                        Value::from_object(realm.intrinsics().$prototype_accessor(vm)),
                        PropertyAttributes::new(0),
                    );

                    // 23.2.6.1 TypedArray.BYTES_PER_ELEMENT, https://tc39.es/ecma262/#sec-typedarray.bytes_per_element
                    object.define_direct_property(
                        vm,
                        &vm.names.BYTES_PER_ELEMENT,
                        Value::from_i32(Kind::$class.element_type().size() as i32),
                        PropertyAttributes::new(0),
                    );

                    object.define_direct_property(
                        vm,
                        &vm.names.length,
                        Value::from_i32(3),
                        PropertyAttributes::new(Attribute::CONFIGURABLE),
                    );

                    if Kind::$class == Kind::Uint8Array {
                        Uint8ArrayConstructorHelpers::initialize(vm, realm, object);
                    }
                }

                // 23.2.5.1 TypedArray ( ...args ), https://tc39.es/ecma262/#sec-typedarray
                fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
                    vm.throw_completion(ErrorKind::TypeError, ErrorType::ConstructorWithoutNew, &[&vm.names.$class])
                }

                // 23.2.5.1 TypedArray ( ...args ), https://tc39.es/ecma262/#sec-typedarray
                fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
                    construct_typed_array(vm, Kind::$class, new_target)
                }
            }
        )*

        /// realm.intrinsics().snake_name##_constructor() for the typed array of `kind`.
        pub fn intrinsic_constructor_of(vm: &Vm, realm: Gc<Realm>, kind: Kind) -> Gc<FunctionObject> {
            match kind {
                $(Kind::$class => realm.intrinsics().$constructor_accessor(vm),)*
            }
        }

        /// The %TypedArray.prototype% of the typed array of `kind`, as an intrinsic default prototype.
        pub fn intrinsic_prototype_of(kind: Kind) -> IntrinsicDefaultPrototype {
            match kind {
                $(Kind::$class => Intrinsics::$prototype_accessor,)*
            }
        }

        /// ClassName::create(Realm&, u32 length, FunctionObject& new_target) for the typed array of `kind`.
        fn create_typed_array_with_new_target(
            vm: &Vm,
            realm: Gc<Realm>,
            kind: Kind,
            length: u32,
            new_target: Gc<FunctionObject>,
        ) -> ThrowCompletionOr<Gc<TypedArrayBase>> {
            match kind {
                $(Kind::$class => Ok($class::create_with_new_target(vm, realm, length, new_target)?.upcast()),)*
            }
        }

        /// ClassName::create(Realm&, u32 length) for the typed array of `kind`.
        pub fn create_typed_array(
            vm: &Vm,
            realm: Gc<Realm>,
            kind: Kind,
            length: u32,
        ) -> ThrowCompletionOr<Gc<TypedArrayBase>> {
            match kind {
                $(Kind::$class => Ok($class::create(vm, realm, length)?.upcast()),)*
            }
        }

        /// ClassName::create(Realm&, u32 length, ArrayBuffer&) for the typed array of `kind`.
        pub fn create_typed_array_on_buffer(
            vm: &Vm,
            realm: Gc<Realm>,
            kind: Kind,
            length: u32,
            array_buffer: Gc<ArrayBuffer>,
        ) -> Gc<TypedArrayBase> {
            match kind {
                $(Kind::$class => $class::create_on_buffer(vm, realm, length, array_buffer).upcast(),)*
            }
        }
    };
}

define_typed_arrays! {
    Uint8Array, uint8_array, Uint8ArrayPrototype, uint8_array_prototype, Uint8ArrayConstructor, uint8_array_constructor;
    Uint8ClampedArray, uint8_clamped_array, Uint8ClampedArrayPrototype, uint8_clamped_array_prototype, Uint8ClampedArrayConstructor, uint8_clamped_array_constructor;
    Uint16Array, uint16_array, Uint16ArrayPrototype, uint16_array_prototype, Uint16ArrayConstructor, uint16_array_constructor;
    Uint32Array, uint32_array, Uint32ArrayPrototype, uint32_array_prototype, Uint32ArrayConstructor, uint32_array_constructor;
    BigUint64Array, big_uint64_array, BigUint64ArrayPrototype, big_uint64_array_prototype, BigUint64ArrayConstructor, big_uint64_array_constructor;
    Int8Array, int8_array, Int8ArrayPrototype, int8_array_prototype, Int8ArrayConstructor, int8_array_constructor;
    Int16Array, int16_array, Int16ArrayPrototype, int16_array_prototype, Int16ArrayConstructor, int16_array_constructor;
    Int32Array, int32_array, Int32ArrayPrototype, int32_array_prototype, Int32ArrayConstructor, int32_array_constructor;
    BigInt64Array, big_int64_array, BigInt64ArrayPrototype, big_int64_array_prototype, BigInt64ArrayConstructor, big_int64_array_constructor;
    Float16Array, float16_array, Float16ArrayPrototype, float16_array_prototype, Float16ArrayConstructor, float16_array_constructor;
    Float32Array, float32_array, Float32ArrayPrototype, float32_array_prototype, Float32ArrayConstructor, float32_array_constructor;
    Float64Array, float64_array, Float64ArrayPrototype, float64_array_prototype, Float64ArrayConstructor, float64_array_constructor;
}

/// The construct() of each ConstructorName in TypedArray.cpp, for the typed array of `kind`.
// 23.2.5.1 TypedArray ( ...args ), https://tc39.es/ecma262/#sec-typedarray
fn construct_typed_array(vm: &Vm, kind: Kind, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
    let realm = vm.current_realm().expect("a constructor runs in a realm");

    if vm.argument_count() == 0 {
        return Ok(create_typed_array_with_new_target(vm, realm, kind, 0, new_target)?.upcast());
    }

    let first_argument = vm.argument(0);
    if first_argument.is_object() {
        let typed_array = create_typed_array_with_new_target(vm, realm, kind, 0, new_target)?;
        let first_object = first_argument.as_object();
        if first_object.is_typed_array() {
            let arg_typed_array = as_typed_array(&first_object);
            initialize_typed_array_from_typed_array(vm, &typed_array, arg_typed_array)?;
        } else if let Some(array_buffer) = first_object.downcast::<ArrayBuffer>() {
            initialize_typed_array_from_array_buffer(vm, &typed_array, array_buffer, vm.argument(1), vm.argument(2))?;
        } else if let Some(iterator) =
            first_argument.get_method(vm, &PropertyKey::from(vm.well_known_symbols().iterator))?
        {
            let values = iterator_to_list(vm, &get_iterator_from_method_impl(vm, first_argument, iterator)?)?;
            initialize_typed_array_from_list(vm, &typed_array, &values)?;
        } else {
            initialize_typed_array_from_array_like(vm, &typed_array, &first_object)?;
        }
        return Ok(typed_array.upcast());
    }

    let array_length = match first_argument.to_index(vm) {
        Ok(array_length) => array_length as usize,
        Err(error) => {
            if error.value().is_object() && error.value().as_object().is::<RangeError>() {
                // Re-throw more specific RangeError
                return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"typed array"]);
            }
            return Err(error);
        }
    };
    validate_typed_array_length(vm, array_length, kind.element_type().size())?;
    Ok(create_typed_array_with_new_target(vm, realm, kind, array_length as u32, new_target)?.upcast())
}

/// The typed array of an object whose flags say it is one.
pub fn typed_array_of_object(object: &Object) -> &TypedArrayBase {
    as_typed_array(object)
}
