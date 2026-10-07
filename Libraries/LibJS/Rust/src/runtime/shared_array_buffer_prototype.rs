/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::{HandledByHost, Vm};
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{construct, species_constructor};
use crate::runtime::array_buffer::{ArrayBuffer, ZeroFillNewBytes};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_value;
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct SharedArrayBufferPrototype {
    base: Object,
}

define_object_class!(SharedArrayBufferPrototype, extends: [Object], methods: {
    initialize: SharedArrayBufferPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn this_array_buffer(vm: &Vm) -> ThrowCompletionOr<Gc<ArrayBuffer>> {
    typed_this_value::<ArrayBuffer>(vm, "SharedArrayBuffer")
}

impl SharedArrayBufferPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<SharedArrayBufferPrototype> {
        realm.create_object(
            vm,
            SharedArrayBufferPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let configurable = PropertyAttributes::new(Attribute::CONFIGURABLE);
        object.define_native_accessor(
            vm,
            realm,
            &names.byteLength,
            raw_native!(SharedArrayBufferPrototype::byte_length_getter),
            None,
            configurable,
        );
        object.define_native_function(
            vm,
            realm,
            &names.grow,
            raw_native!(SharedArrayBufferPrototype::grow),
            1,
            attr,
            None,
        );
        object.define_native_accessor(
            vm,
            realm,
            &names.growable,
            raw_native!(SharedArrayBufferPrototype::growable_getter),
            None,
            configurable,
        );
        object.define_native_accessor(
            vm,
            realm,
            &names.maxByteLength,
            raw_native!(SharedArrayBufferPrototype::max_byte_length),
            None,
            configurable,
        );
        object.define_native_function(
            vm,
            realm,
            &names.slice,
            raw_native!(SharedArrayBufferPrototype::slice),
            2,
            attr,
            None,
        );

        // 25.2.5.7 SharedArrayBuffer.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-sharedarraybuffer.prototype.toString
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                names.SharedArrayBuffer.as_string(),
            )),
            configurable,
        );
    }

    // 25.2.5.1 get SharedArrayBuffer.prototype.byteLength, https://tc39.es/ecma262/#sec-get-sharedarraybuffer.prototype.bytelength
    fn byte_length_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[ArrayBufferData]]).
        let array_buffer_object = this_array_buffer(vm)?;

        // 3. If IsSharedArrayBuffer(O) is false, throw a TypeError exception.
        if !array_buffer_object.is_shared_array_buffer() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotASharedArrayBuffer, &[]);
        }

        // 4. Let length be O.[[ArrayBufferByteLength]].
        // 5. Return 𝔽(length).
        Ok(Value::from_f64(array_buffer_object.byte_length() as f64))
    }

    // 25.2.5.3 SharedArrayBuffer.prototype.grow ( newLength ), https://tc39.es/ecma262/#sec-sharedarraybuffer.prototype.grow
    fn grow(vm: &Vm) -> ThrowCompletionOr<Value> {
        let new_length = vm.argument(0);

        // 1. Let O be the this value.
        let array_buffer_object = this_array_buffer(vm)?;

        // 2. Perform ? RequireInternalSlot(O, [[ArrayBufferMaxByteLength]]).
        if array_buffer_object.is_fixed_length() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::FixedArrayBuffer, &[]);
        }

        // 3. If IsSharedArrayBuffer(O) is false, throw a TypeError exception.
        if !array_buffer_object.is_shared_array_buffer() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotASharedArrayBuffer, &[]);
        }

        // 4. Let newByteLength be ? ToIndex(newLength).
        let new_byte_length = new_length.to_index(vm)? as usize;

        // 5. Let hostHandled be ? HostGrowSharedArrayBuffer(O, newByteLength).
        let host_handled = vm.host_grow_shared_array_buffer()(vm, &array_buffer_object, new_byte_length)?;

        // 6. If hostHandled is handled, return undefined.
        if host_handled == HandledByHost::Handled {
            return Ok(Value::UNDEFINED);
        }

        // FIXME: 7. Let AR be the Agent Record of the surrounding agent.
        // FIXME: 8. Let isLittleEndian be AR.[[LittleEndian]].
        // FIXME: 9. Let byteLengthBlock be O.[[ArrayBufferByteLengthData]].
        // FIXME: 10. Let currentByteLengthRawBytes be GetRawBytesFromSharedBlock(byteLengthBlock, 0, biguint64, true, seq-cst).
        // FIXME: 11. Let newByteLengthRawBytes be NumericToRawBytes(biguint64, ℤ(newByteLength), isLittleEndian).
        // FIXME: 12. Repeat,
        // FIXME:         a. NOTE: This is a compare-and-exchange loop to ensure that parallel, racing grows of the same buffer are totally ordered, are not lost, and do not silently do nothing. The loop exits if it was able to attempt to grow uncontended.
        // FIXME:         b. Let currentByteLength be ℝ(RawBytesToNumeric(biguint64, currentByteLengthRawBytes, isLittleEndian)).
        let current_byte_length = array_buffer_object.byte_length();

        //                c. If newByteLength = currentByteLength, return undefined.
        if new_byte_length == current_byte_length {
            return Ok(Value::UNDEFINED);
        }

        //                d. If newByteLength < currentByteLength or newByteLength > O.[[ArrayBufferMaxByteLength]], throw a RangeError exception.
        if new_byte_length < current_byte_length {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::ByteLengthLessThanPreviousByteLength,
                &[&new_byte_length, &current_byte_length],
            );
        }
        if new_byte_length > array_buffer_object.max_byte_length() {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::ByteLengthExceedsMaxByteLength,
                &[&new_byte_length, &array_buffer_object.max_byte_length()],
            );
        }

        // FIXME:         e. Let byteLengthDelta be newByteLength - currentByteLength.
        // FIXME:         f. If it is impossible to create a new Shared Data Block value consisting of byteLengthDelta bytes, throw a RangeError exception.
        // FIXME:         g. NOTE: No new Shared Data Block is constructed and used here. The observable behaviour of growable SharedArrayBuffers is specified by allocating a max-sized Shared Data Block at construction time, and this step captures the requirement that implementations that run out of memory must throw a RangeError.
        // FIXME:         h. Let readByteLengthRawBytes be AtomicCompareExchangeInSharedBlock(byteLengthBlock, 0, 8, currentByteLengthRawBytes, newByteLengthRawBytes).
        // FIXME:         i. If ByteListEqual(readByteLengthRawBytes, currentByteLengthRawBytes) is true, return undefined.
        // FIXME:         j. Set currentByteLengthRawBytes to readByteLengthRawBytes.

        if array_buffer_object
            .try_resize(vm, new_byte_length, ZeroFillNewBytes::Yes)
            .is_err()
        {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::NotEnoughMemoryToAllocate,
                &[&new_byte_length],
            );
        }

        Ok(Value::UNDEFINED)
    }

    // 25.2.5.4 get SharedArrayBuffer.prototype.growable, https://tc39.es/ecma262/#sec-get-sharedarraybuffer.prototype.growable
    fn growable_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[ArrayBufferData]]).
        let array_buffer_object = this_array_buffer(vm)?;

        // 3. If IsSharedArrayBuffer(O) is false, throw a TypeError exception.
        if !array_buffer_object.is_shared_array_buffer() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotASharedArrayBuffer, &[]);
        }

        // 4. If IsFixedLengthArrayBuffer(O) is false, return true; otherwise return false.
        Ok(Value::from_bool(!array_buffer_object.is_fixed_length()))
    }

    // 25.2.5.5 get SharedArrayBuffer.prototype.maxByteLength, https://tc39.es/ecma262/#sec-get-sharedarraybuffer.prototype.maxbytelength
    fn max_byte_length(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[ArrayBufferData]]).
        let array_buffer_object = this_array_buffer(vm)?;

        // 3. If IsSharedArrayBuffer(O) is false, throw a TypeError exception.
        if !array_buffer_object.is_shared_array_buffer() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotASharedArrayBuffer, &[]);
        }

        // 4. If IsFixedLengthArrayBuffer(O) is true, then
        //        a. Let length be O.[[ArrayBufferByteLength]].
        // 5. Else,
        //        a. Let length be O.[[ArrayBufferMaxByteLength]].
        let length = if array_buffer_object.is_fixed_length() {
            array_buffer_object.byte_length()
        } else {
            array_buffer_object.max_byte_length()
        };

        // 6. Return 𝔽(length).
        Ok(Value::from_f64(length as f64))
    }

    // 25.2.5.6 SharedArrayBuffer.prototype.slice ( start, end ), https://tc39.es/ecma262/#sec-sharedarraybuffer.prototype.slice
    fn slice(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a native function runs in a realm");

        let start = vm.argument(0);
        let end = vm.argument(1);

        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[ArrayBufferData]]).
        let array_buffer_object = this_array_buffer(vm)?;

        // 3. If IsSharedArrayBuffer(O) is false, throw a TypeError exception.
        if !array_buffer_object.is_shared_array_buffer() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotASharedArrayBuffer, &[]);
        }

        // 4. Let len be O.[[ArrayBufferByteLength]].
        let length = array_buffer_object.byte_length() as f64;

        // 5. Let relativeStart be ? ToIntegerOrInfinity(start).
        let relative_start = start.to_integer_or_infinity(vm)?;

        // 6. If relativeStart is -∞, let first be 0.
        let first = if relative_start == f64::NEG_INFINITY {
            0.0
        }
        // 7. Else if relativeStart < 0, let first be max(len + relativeStart, 0).
        else if relative_start < 0.0 {
            (length + relative_start).max(0.0)
        }
        // 8. Else, let first be min(relativeStart, len).
        else {
            relative_start.min(length)
        };

        // 9. If end is undefined, let relativeEnd be len; else let relativeEnd be ? ToIntegerOrInfinity(end).
        let relative_end = if end.is_undefined() {
            length
        } else {
            end.to_integer_or_infinity(vm)?
        };

        // 10. If relativeEnd is -∞, let final be 0.
        let final_ = if relative_end == f64::NEG_INFINITY {
            0.0
        }
        // 11. Else if relativeEnd < 0, let final be max(len + relativeEnd, 0).
        else if relative_end < 0.0 {
            (length + relative_end).max(0.0)
        }
        // 12. Else, let final be min(relativeEnd, len).
        else {
            relative_end.min(length)
        };

        // 13. Let newLen be max(final - first, 0).
        let new_length = (final_ - first).max(0.0);

        // 14. Let ctor be ? SpeciesConstructor(O, %SharedArrayBuffer%).
        let constructor = species_constructor(
            vm,
            &array_buffer_object,
            realm.intrinsics().shared_array_buffer_constructor(vm),
        )?;

        // 15. Let new be ? Construct(ctor, « 𝔽(newLen) »).
        let new_array_buffer = construct(vm, constructor, &[Value::from_f64(new_length)], None)?;

        // 16. Perform ? RequireInternalSlot(new, [[ArrayBufferData]]).
        let Some(new_array_buffer_object) = new_array_buffer.downcast::<ArrayBuffer>() else {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::SpeciesConstructorDidNotCreate,
                &[&"an ArrayBuffer"],
            );
        };

        // 17. If IsSharedArrayBuffer(new) is true, throw a TypeError exception.
        if !new_array_buffer_object.is_shared_array_buffer() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotASharedArrayBuffer, &[]);
        }

        // 18. If new.[[ArrayBufferData]] is O.[[ArrayBufferData]], throw a TypeError exception.
        if new_array_buffer == array_buffer_object.upcast::<Object>() {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::SpeciesConstructorReturned,
                &[&"same ArrayBuffer instance"],
            );
        }

        // 19. If new.[[ArrayBufferByteLength]] < newLen, throw a TypeError exception.
        if (new_array_buffer_object.byte_length() as f64) < new_length {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::SpeciesConstructorReturned,
                &[&"an ArrayBuffer smaller than requested"],
            );
        }

        // 22. Perform CopyDataBlockBytes(toBuf, 0, fromBuf, first, newLen).
        array_buffer_object.copy_data_to(&new_array_buffer_object, first as usize, 0, new_length as usize);

        // 23. Return new.
        Ok(Value::from_object(new_array_buffer_object))
    }
}
