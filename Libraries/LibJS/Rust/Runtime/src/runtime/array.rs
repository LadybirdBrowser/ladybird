/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use libjs_runtime_macros::Trace;

use crate::gc::class::{Class, GcCell, define_cell};
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::call_function_object;
use crate::runtime::array_prototype::array_merge_sort;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::object::{
    CacheableSetPropertyMetadata, IndexedStorageKind, MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS,
    Object, ObjectMethods, PropertyLookupPhase, allocate_object,
};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::DEFAULT_ATTRIBUTES;
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::value::{TriState, is_less_than};
use crate::utf16::Utf16View;

/// The Array exotic object. Its "length" is not a stored property: it is the size of the indexed storage, which the
/// interpreter reads through the object's magical length flag.
#[repr(C)]
#[derive(Trace)]
pub struct Array {
    base: Object,
    realm: Cell<Gc<Realm>>,
    length_writable: Cell<bool>,
    is_proxy_target: Cell<bool>,
}

pub static ARRAY_OBJECT_METHODS: ObjectMethods = ObjectMethods {
    internal_get_own_property: Array::internal_get_own_property,
    is_cacheable_for_property_absence: |_| false,
    internal_set: Array::internal_set,
    internal_define_own_property: Array::internal_define_own_property,
    internal_has_property: Array::internal_has_property,
    internal_delete: Array::internal_delete,
    internal_own_property_keys: Array::internal_own_property_keys,
    ..ORDINARY_OBJECT_METHODS
};

define_cell!(Array, Object, extends: [Object], methods: ARRAY_OBJECT_METHODS);

impl core::ops::Deref for Array {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Object {
    pub fn is_array_exotic_object(&self) -> bool {
        self.is::<Array>()
    }
}

/// The array an Array internal method was called on.
fn as_array(object: &Object) -> &Array {
    assert!(object.is_array_exotic_object());
    // SAFETY: The object is an Array, which starts with its Object.
    unsafe { &*core::ptr::from_ref(object).cast::<Array>() }
}

impl Array {
    fn new(vm: &Vm, realm: Gc<Realm>, prototype: Gc<Object>) -> Self {
        Self::new_with_class(vm, Self::CLASS, realm, prototype)
    }

    /// Array(Realm&, Object& prototype), for `class`, which is Array or a class that extends it.
    pub fn new_with_class(vm: &Vm, class: &'static Class, realm: Gc<Realm>, prototype: Gc<Object>) -> Self {
        let array = Self {
            base: Object::new_with_prototype(vm, class, prototype, MayInterfereWithIndexedPropertyAccess::No),
            realm: Cell::new(realm),
            length_writable: Cell::new(true),
            is_proxy_target: Cell::new(false),
        };
        array.base.set_has_magical_length_property();
        array
    }

    // 10.4.2.2 ArrayCreate ( length [ , proto ] ), https://tc39.es/ecma262/#sec-arraycreate
    pub fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        length: u64,
        prototype: Option<Gc<Object>>,
    ) -> ThrowCompletionOr<Gc<Array>> {
        // 1. If length > 2^32 - 1, throw a RangeError exception.
        if length > u64::from(u32::MAX) {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"array"]);
        }

        // 2. If proto is not present, set proto to %Array.prototype%.
        let prototype = prototype.unwrap_or_else(|| realm.intrinsics().array_prototype(vm));

        // 3. Let A be MakeBasicObject(« [[Prototype]], [[Extensible]] »).
        // 4. Set A.[[Prototype]] to proto.
        // 5. Set A.[[DefineOwnProperty]] as specified in 10.4.2.1.
        let array = allocate_object(vm, Self::new(vm, realm, prototype));

        // 6. Perform ! OrdinaryDefineOwnProperty(A, "length", PropertyDescriptor { [[Value]]: 𝔽(length), [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: false }).
        let mut descriptor = PropertyDescriptor {
            value: Some(Value::from_f64(length as f64)),
            writable: Some(true),
            enumerable: Some(false),
            configurable: Some(false),
            ..Default::default()
        };
        array
            .internal_define_own_property(vm, &vm.names.length.clone(), &mut descriptor, None)
            .must();

        // 7. Return A.
        Ok(array)
    }

    // 7.3.18 CreateArrayFromList ( elements ), https://tc39.es/ecma262/#sec-createarrayfromlist
    pub fn create_from(vm: &Vm, realm: Gc<Realm>, elements: &[Value]) -> Gc<Array> {
        // 1. Let array be ! ArrayCreate(0).
        let array = Self::create(vm, realm, 0, None).must();

        // 2. Let n be 0.
        // 3. For each element e of elements, do
        // a. Perform ! CreateDataPropertyOrThrow(array, ! ToString(𝔽(n)), e).
        // b. Set n to n + 1.
        // OPTIMIZATION: These are consecutive default data properties, so initialize the packed
        //               indexed storage in one allocation instead of defining them individually.
        array.set_indexed_property_elements(elements);

        // 4. Return array.
        array
    }

    /// CreateArrayFromList for a list that is kept alive while the array is allocated.
    pub fn create_from_list(vm: &Vm, realm: Gc<Realm>, elements: &MarkedVec<'_, Value>) -> Gc<Array> {
        let array = Self::create(vm, realm, 0, None).must();
        let size = u32::try_from(elements.len()).expect("an array-like size fits in u32");
        array.set_indexed_property_elements_to_undefined(size);
        for index in 0..size {
            array.set_packed_indexed_element(index, elements.get(index as usize).expect("the index is in bounds"));
        }
        array
    }

    pub fn length_is_writable(&self) -> bool {
        self.length_writable.get()
    }

    pub fn is_proxy_target(&self) -> bool {
        self.is_proxy_target.get()
    }

    pub fn set_is_proxy_target(&self, is_proxy_target: bool) {
        self.is_proxy_target.set(is_proxy_target);
    }

    // Packed arrays have no holes, so the prototype chain is irrelevant:
    // every index [0, size) is an own data property.
    pub fn is_simple_packed_array(&self) -> bool {
        !self.is_proxy_target()
            && !self.may_interfere_with_indexed_property_access()
            && self.indexed_storage_kind() == IndexedStorageKind::Packed
    }

    // 10.4.2.4 ArraySetLength ( A, Desc ), https://tc39.es/ecma262/#sec-arraysetlength
    fn set_length(&self, vm: &Vm, property_descriptor: &PropertyDescriptor) -> ThrowCompletionOr<bool> {
        // 1. If Desc does not have a [[Value]] field, then
        // a. Return ! OrdinaryDefineOwnProperty(A, "length", Desc).
        // 2. Let newLenDesc be a copy of Desc.
        // NOTE: Handled by step 16

        let mut new_length = self.indexed_array_like_size() as usize;
        if let Some(value) = property_descriptor.value {
            // 3. Let newLen be ? ToUint32(Desc.[[Value]]).
            new_length = value.to_u32(vm)? as usize;
            // 4. Let numberLen be ? ToNumber(Desc.[[Value]]).
            let number_length = value.to_number(vm)?;
            // 5. If newLen is not the same value as numberLen, throw a RangeError exception.
            if new_length as f64 != number_length.as_f64() {
                return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"array"]);
            }
        }

        // 6. Set newLenDesc.[[Value]] to newLen.
        // 7. Let oldLenDesc be OrdinaryGetOwnProperty(A, "length").
        // 8. Assert: IsDataDescriptor(oldLenDesc) is true.
        // 9. Assert: oldLenDesc.[[Configurable]] is false.
        // 10. Let oldLen be oldLenDesc.[[Value]].
        // 11. If newLen ≥ oldLen, then
        // a. Return ! OrdinaryDefineOwnProperty(A, "length", newLenDesc).
        // 12. If oldLenDesc.[[Writable]] is false, return false.
        // NOTE: Handled by step 16

        // 13. If newLenDesc does not have a [[Writable]] field or newLenDesc.[[Writable]] true, let newWritable be true.
        // 14. Else,
        // a. NOTE: Setting the [[Writable]] attribute to false is deferred in case any elements cannot be deleted.
        // b. Let newWritable be false.
        let new_writable = property_descriptor.writable.unwrap_or(true);

        // c. Set newLenDesc.[[Writable]] to true.
        // 15. Let succeeded be ! OrdinaryDefineOwnProperty(A, "length", newLenDesc).
        // 16. If succeeded is false, return false.
        // NOTE: Because the length property does not actually exist calling OrdinaryDefineOwnProperty
        // will result in unintended behavior, so instead we only implement here the small subset of
        // checks performed inside of it that would have mattered to us:

        // 10.1.6.3 ValidateAndApplyPropertyDescriptor ( O, P, extensible, Desc, current ), https://tc39.es/ecma262/#sec-validateandapplypropertydescriptor
        // 5. If current.[[Configurable]] is false, then
        // a. If Desc has a [[Configurable]] field and Desc.[[Configurable]] is true, return false.
        if property_descriptor.configurable == Some(true) {
            return Ok(false);
        }
        // b. If Desc has an [[Enumerable]] field and SameValue(Desc.[[Enumerable]], current.[[Enumerable]]) is false, return false.
        if property_descriptor.enumerable == Some(true) {
            return Ok(false);
        }
        // c. If IsGenericDescriptor(Desc) is false and SameValue(IsAccessorDescriptor(Desc), IsAccessorDescriptor(current)) is false, return false.
        if !property_descriptor.is_generic_descriptor() && property_descriptor.is_accessor_descriptor() {
            return Ok(false);
        }
        // NOTE: Step d. doesn't apply here.
        // e. Else if current.[[Writable]] is false, then
        if !self.length_writable.get() {
            // i. If Desc has a [[Writable]] field and Desc.[[Writable]] is true, return false.
            if property_descriptor.writable == Some(true) {
                return Ok(false);
            }
            // ii. If Desc has a [[Value]] field and SameValue(Desc.[[Value]], current.[[Value]]) is false, return false.
            if new_length != self.indexed_array_like_size() as usize {
                return Ok(false);
            }
        }

        // 17. For each own property key P of A that is an array index, whose numeric value is greater than or equal to newLen, in descending numeric index order, do
        // a. Let deleteSucceeded be ! A.[[Delete]](P).
        // b. If deleteSucceeded is false, then
        // i. Set newLenDesc.[[Value]] to ! ToUint32(P) + 1𝔽.
        let success = self.set_indexed_array_like_size(new_length);

        // ii. If newWritable is false, set newLenDesc.[[Writable]] to false.
        // iii. Perform ! OrdinaryDefineOwnProperty(A, "length", newLenDesc).
        // NOTE: Handled by step 18

        // 18. If newWritable is false, then
        // a. Set succeeded to ! OrdinaryDefineOwnProperty(A, "length", PropertyDescriptor { [[Writable]]: false }).
        // b. Assert: succeeded is true.
        if !new_writable {
            self.length_writable.set(false);
        }

        // NOTE: Continuation of step #17
        // iv. Return false.
        if !success {
            return Ok(false);
        }

        // 19. Return true.
        Ok(true)
    }

    pub fn default_prototype_chain_intact(&self) -> bool {
        let realm = self.realm.get();
        let Some(array_prototype) = self.shape().prototype() else {
            return false;
        };
        if array_prototype != realm.array_prototype() {
            return false;
        }
        if array_prototype.indexed_array_like_size() != 0
            || array_prototype.may_interfere_with_indexed_property_access()
        {
            return false;
        }

        let Some(object_prototype) = array_prototype.shape().prototype() else {
            return false;
        };
        if object_prototype != realm.object_prototype() {
            return false;
        }
        if object_prototype.indexed_array_like_size() != 0
            || object_prototype.may_interfere_with_indexed_property_access()
        {
            return false;
        }

        object_prototype.shape().prototype().is_none()
    }

    // NON-STANDARD: Used to return the value of the ephemeral length property
    fn internal_get_own_property(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
    ) -> ThrowCompletionOr<Option<PropertyDescriptor>> {
        let array = as_array(object);

        // OPTIMIZATION: Fast path for arrays with non-Dictionary indexed storage.
        if property_key.is_number() && array.indexed_storage_kind() != IndexedStorageKind::Dictionary {
            let Some(result) = array.indexed_get(property_key.as_number()) else {
                return Ok(None);
            };
            return Ok(Some(PropertyDescriptor {
                value: Some(result.value),
                writable: Some(true),
                enumerable: Some(true),
                configurable: Some(true),
                ..Default::default()
            }));
        }

        if property_key.is_string() && property_key.as_string() == vm.names.length.as_string() {
            return Ok(Some(PropertyDescriptor {
                value: Some(Value::from_f64(f64::from(array.indexed_array_like_size()))),
                writable: Some(array.length_writable.get()),
                enumerable: Some(false),
                configurable: Some(false),
                ..Default::default()
            }));
        }

        array.ordinary_get_own_property(vm, property_key)
    }

    fn internal_set(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
        value: Value,
        receiver: Value,
        cacheable_metadata: Option<&mut CacheableSetPropertyMetadata>,
        phase: PropertyLookupPhase,
    ) -> ThrowCompletionOr<bool> {
        let array = as_array(object);

        // Fast path for arrays with intact prototype chain
        if receiver.is_object()
            && receiver.as_object() == array.as_gc()
            && !array.is_proxy_target()
            && array.default_prototype_chain_intact()
        {
            if property_key.is_number() && array.indexed_storage_kind() != IndexedStorageKind::Dictionary {
                let index = property_key.as_number();
                let property_descriptor = array.internal_get_own_property(vm, property_key)?;
                match property_descriptor {
                    None => {
                        if !array.is_extensible(vm)? {
                            return Ok(false);
                        }
                        if index >= array.indexed_array_like_size() && !array.length_writable.get() {
                            return Ok(false);
                        }
                        array.indexed_put(index, value, DEFAULT_ATTRIBUTES);
                        return Ok(true);
                    }
                    Some(property_descriptor) if property_descriptor.is_data_descriptor() => {
                        if property_descriptor.writable == Some(false) {
                            return Ok(false);
                        }
                        array.indexed_put(index, value, DEFAULT_ATTRIBUTES);
                        return Ok(true);
                    }
                    Some(_) => {}
                }
            } else if *property_key == vm.names.length {
                let mut property_descriptor = array
                    .internal_get_own_property(vm, property_key)?
                    .expect("an array has a length");
                if property_descriptor.writable == Some(false) {
                    return Ok(false);
                }
                property_descriptor.value = Some(value);
                return array.set_length(vm, &property_descriptor);
            }
        }

        array.ordinary_set(vm, property_key, value, receiver, cacheable_metadata, phase)
    }

    // 10.4.2.1 [[DefineOwnProperty]] ( P, Desc ), https://tc39.es/ecma262/#sec-array-exotic-objects-defineownproperty-p-desc
    fn internal_define_own_property(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
        property_descriptor: &mut PropertyDescriptor,
        precomputed_get_own_property: Option<&Option<PropertyDescriptor>>,
    ) -> ThrowCompletionOr<bool> {
        let array = as_array(object);

        // 1. If P is "length", then
        if property_key.is_string() && property_key.as_string() == vm.names.length.as_string() {
            // a. Return ? ArraySetLength(A, Desc).
            return array.set_length(vm, property_descriptor);
        }

        // 2. Else if P is an array index, then
        if property_key.is_number() {
            // a. Let oldLenDesc be OrdinaryGetOwnProperty(A, "length").
            // b. Assert: IsDataDescriptor(oldLenDesc) is true.
            // c. Assert: oldLenDesc.[[Configurable]] is false.
            // d. Let oldLen be oldLenDesc.[[Value]].
            // e. Assert: oldLen is a non-negative integral Number.
            // f. Let index be ! ToUint32(P).

            // g. If index ≥ oldLen and oldLenDesc.[[Writable]] is false, return false.
            if property_key.as_number() >= array.indexed_array_like_size() && !array.length_writable.get() {
                return Ok(false);
            }

            // h. Let succeeded be ! OrdinaryDefineOwnProperty(A, P, Desc).
            let mut succeeded = true;
            let attributes = property_descriptor.attributes();
            // OPTIMIZATION: Fast path for arrays with non-Dictionary indexed storage and default attributes.
            if property_descriptor.is_data_descriptor()
                && attributes == DEFAULT_ATTRIBUTES
                && array.indexed_storage_kind() != IndexedStorageKind::Dictionary
            {
                if !array.extensible() {
                    let existing_descriptor = array.internal_get_own_property(vm, property_key)?;
                    if existing_descriptor.is_none() {
                        return Ok(false);
                    }
                }

                array.indexed_put(
                    property_key.as_number(),
                    property_descriptor
                        .value
                        .expect("a data descriptor with default attributes has a value"),
                    DEFAULT_ATTRIBUTES,
                );
            } else {
                succeeded = array
                    .ordinary_define_own_property(vm, property_key, property_descriptor, precomputed_get_own_property)
                    .must();
            }

            // i. If succeeded is false, return false.
            if !succeeded {
                return Ok(false);
            }

            // j. If index ≥ oldLen, then
            // i. Set oldLenDesc.[[Value]] to index + 1𝔽.
            // ii. Set succeeded to ! OrdinaryDefineOwnProperty(A, "length", oldLenDesc).
            // iii. Assert: succeeded is true.

            // k. Return true.
            return Ok(true);
        }

        // 3. Return ? OrdinaryDefineOwnProperty(A, P, Desc).
        array.ordinary_define_own_property(vm, property_key, property_descriptor, precomputed_get_own_property)
    }

    // NON-STANDARD: Fast path to quickly check if an indexed property exists in array without holes
    fn internal_has_property(object: &Object, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<bool> {
        let array = as_array(object);
        if property_key.is_number()
            && !array.is_proxy_target()
            && array.indexed_storage_kind() == IndexedStorageKind::Packed
            && property_key.as_number() < array.indexed_array_like_size()
        {
            return Ok(true);
        }
        array.ordinary_has_property(vm, property_key)
    }

    // NON-STANDARD: Used to reject deletes to ephemeral (non-configurable) length property
    fn internal_delete(object: &Object, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<bool> {
        let array = as_array(object);
        if property_key.is_string() && property_key.as_string() == vm.names.length.as_string() {
            return Ok(false);
        }
        array.ordinary_delete(vm, property_key)
    }

    // NON-STANDARD: Used to inject the ephemeral length property's key
    fn internal_own_property_keys<'vm>(object: &Object, vm: &'vm Vm) -> ThrowCompletionOr<MarkedVec<'vm, Value>> {
        let array = as_array(object);
        let keys = array.ordinary_own_property_keys(vm)?;
        // FIXME: This is pretty expensive, find a better way to do this
        keys.insert(
            array.indexed_real_size(),
            Value::from_string(PrimitiveString::create_from_fly_string(vm, vm.names.length.as_string())),
        );
        Ok(keys)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Holes {
    SkipHoles,
    ReadThroughHoles,
}

// 23.1.3.30.1 SortIndexedProperties ( obj, len, SortCompare, holes ), https://tc39.es/ecma262/#sec-sortindexedproperties
pub fn sort_indexed_properties<'vm>(
    vm: &'vm Vm,
    object: &Object,
    length: u64,
    sort_compare: &dyn Fn(Value, Value) -> ThrowCompletionOr<f64>,
    holes: Holes,
) -> ThrowCompletionOr<MarkedVec<'vm, Value>> {
    // 1. Let items be a new empty List.
    let items = MarkedVec::new(vm);

    // 2. Let k be 0.
    // 3. Repeat, while k < len,
    for k in 0..length {
        // a. Let Pk be ! ToString(𝔽(k)).
        let property_key = PropertyKey::from_number(k);

        // b. If holes is skip-holes, then
        let k_read = if holes == Holes::SkipHoles {
            // i. Let kRead be ? HasProperty(obj, Pk).
            object.has_property(vm, &property_key)?
        }
        // c. Else,
        else {
            // i. Assert: holes is read-through-holes.
            assert!(holes == Holes::ReadThroughHoles);

            // ii. Let kRead be true.
            true
        };

        // d. If kRead is true, then
        if k_read {
            // i. Let kValue be ? Get(obj, Pk).
            let k_value = object.get(vm, &property_key)?;

            // ii. Append kValue to items.
            items.push(k_value);
        }

        // e. Set k to k + 1.
    }

    // 4. Sort items using an implementation-defined sequence of calls to SortCompare. If any such call returns an abrupt completion, stop before performing any further calls to SortCompare or steps in this algorithm and return that Completion Record.

    // Perform sorting by merge sort. This isn't as efficient compared to quick sort, but
    // quicksort can't be used in all cases because the spec requires Array.prototype.sort()
    // to be stable. FIXME: when initially scanning through the array, maintain a flag
    // for if an unstable sort would be indistinguishable from a stable sort (such as just
    // just strings or numbers), and in that case use quick sort instead for better performance.
    array_merge_sort(vm, sort_compare, &items)?;

    // 5. Return items.
    Ok(items)
}

/// AK::Utf16View::operator<=>: the code units in order, then the lengths.
fn compare_code_units(lhs: Utf16View<'_>, rhs: Utf16View<'_>) -> f64 {
    let common_length = lhs.length_in_code_units().min(rhs.length_in_code_units());
    for index in 0..common_length {
        let lhs_code_unit = lhs.code_unit_at(index);
        let rhs_code_unit = rhs.code_unit_at(index);
        if lhs_code_unit != rhs_code_unit {
            return if lhs_code_unit < rhs_code_unit { -1.0 } else { 1.0 };
        }
    }
    match lhs.length_in_code_units().cmp(&rhs.length_in_code_units()) {
        core::cmp::Ordering::Less => -1.0,
        core::cmp::Ordering::Equal => 0.0,
        core::cmp::Ordering::Greater => 1.0,
    }
}

// 23.1.3.30.2 CompareArrayElements ( x, y, comparefn ), https://tc39.es/ecma262/#sec-comparearrayelements
pub fn compare_array_elements(
    vm: &Vm,
    x: Value,
    y: Value,
    comparefn: Option<Gc<FunctionObject>>,
) -> ThrowCompletionOr<f64> {
    // 1. If x and y are both undefined, return +0𝔽.
    if x.is_undefined() && y.is_undefined() {
        return Ok(0.0);
    }

    // 2. If x is undefined, return 1𝔽.
    if x.is_undefined() {
        return Ok(1.0);
    }

    // 3. If y is undefined, return -1𝔽.
    if y.is_undefined() {
        return Ok(-1.0);
    }

    // 4. If comparefn is not undefined, then
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

    // OPTIMIZATION: When both operands are already Strings, ToString is the identity, so we can compare their
    //               UTF-16 views directly. This preserves any cached UTF-16 on the original PrimitiveStrings
    //               across sort comparisons and skips the ToPrimitive + IsLessThan detour.
    if x.is_string() && y.is_string() {
        let x_primitive_string = x.as_string();
        let y_primitive_string = y.as_string();
        return Ok(compare_code_units(
            x_primitive_string.utf16_string_view(),
            y_primitive_string.utf16_string_view(),
        ));
    }

    // 5. Let xString be ? ToString(x).
    let x_string = PrimitiveString::create(vm, x.to_utf16_string(vm)?);

    // 6. Let yString be ? ToString(y).
    let y_string = PrimitiveString::create(vm, y.to_utf16_string(vm)?);

    // 7. Let xSmaller be ! IsLessThan(xString, yString, true).
    let x_smaller = is_less_than(vm, Value::from_string(x_string), Value::from_string(y_string), true).must();

    // 8. If xSmaller is true, return -1𝔽.
    if x_smaller == TriState::True {
        return Ok(-1.0);
    }

    // 9. Let ySmaller be ! IsLessThan(yString, xString, true).
    let y_smaller = is_less_than(vm, Value::from_string(y_string), Value::from_string(x_string), true).must();

    // 10. If ySmaller is true, return 1𝔽.
    if y_smaller == TriState::True {
        return Ok(1.0);
    }

    // 11. Return +0𝔽.
    Ok(0.0)
}

// The class tables point at code that uses the heap, so this links only against LibGC.
