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
use crate::runtime::array_buffer::{
    ArrayBuffer, PreserveResizability, array_buffer_copy_and_detach, create_byte_data_block,
};
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
use crate::runtime::value::same_value;

#[repr(C)]
#[derive(Trace)]
pub struct ArrayBufferPrototype {
    base: Object,
}

define_object_class!(ArrayBufferPrototype, extends: [Object], methods: {
    initialize: ArrayBufferPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn this_array_buffer(vm: &Vm) -> ThrowCompletionOr<Gc<ArrayBuffer>> {
    typed_this_value::<ArrayBuffer>(vm, "ArrayBuffer")
}

impl ArrayBufferPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ArrayBufferPrototype> {
        realm.create_object(
            vm,
            ArrayBufferPrototype {
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
            raw_native!(ArrayBufferPrototype::byte_length_getter),
            None,
            configurable,
        );
        object.define_native_accessor(
            vm,
            realm,
            &names.detached,
            raw_native!(ArrayBufferPrototype::detached_getter),
            None,
            configurable,
        );
        object.define_native_accessor(
            vm,
            realm,
            &names.maxByteLength,
            raw_native!(ArrayBufferPrototype::max_byte_length),
            None,
            configurable,
        );
        object.define_native_accessor(
            vm,
            realm,
            &names.resizable,
            raw_native!(ArrayBufferPrototype::resizable),
            None,
            configurable,
        );
        object.define_native_function(
            vm,
            realm,
            &names.resize,
            raw_native!(ArrayBufferPrototype::resize),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.slice,
            raw_native!(ArrayBufferPrototype::slice),
            2,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.transfer,
            raw_native!(ArrayBufferPrototype::transfer),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.transferToFixedLength,
            raw_native!(ArrayBufferPrototype::transfer_to_fixed_length),
            0,
            attr,
            None,
        );

        // 25.1.6.7 ArrayBuffer.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-arraybuffer.prototype-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                names.ArrayBuffer.as_string(),
            )),
            configurable,
        );
    }

    // 25.1.6.1 get ArrayBuffer.prototype.byteLength, https://tc39.es/ecma262/#sec-get-arraybuffer.prototype.bytelength
    fn byte_length_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[ArrayBufferData]]).
        let array_buffer_object = this_array_buffer(vm)?;

        // 3. If IsSharedArrayBuffer(O) is true, throw a TypeError exception.
        if array_buffer_object.is_shared_array_buffer() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::SharedArrayBuffer, &[]);
        }

        // NOTE: These steps are done in byte_length()
        // 4. If IsDetachedBuffer(O) is true, return +0𝔽.
        // 5. Let length be O.[[ArrayBufferByteLength]].
        // 6. Return 𝔽(length).
        Ok(Value::from_f64(array_buffer_object.byte_length() as f64))
    }

    // 25.1.6.3 get ArrayBuffer.prototype.detached, https://tc39.es/ecma262/#sec-get-arraybuffer.prototype.detached
    fn detached_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[ArrayBufferData]]).
        let array_buffer_object = this_array_buffer(vm)?;

        // 3. If IsSharedArrayBuffer(O) is true, throw a TypeError exception.
        if array_buffer_object.is_shared_array_buffer() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::SharedArrayBuffer, &[]);
        }

        // 4. Return IsDetachedBuffer(O).
        Ok(Value::from_bool(array_buffer_object.is_detached()))
    }

    // 25.1.6.4 get ArrayBuffer.prototype.maxByteLength, https://tc39.es/ecma262/#sec-get-arraybuffer.prototype.maxbytelength
    fn max_byte_length(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[ArrayBufferData]]).
        let array_buffer_object = this_array_buffer(vm)?;

        // 3. If IsSharedArrayBuffer(O) is true, throw a TypeError exception.
        if array_buffer_object.is_shared_array_buffer() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::SharedArrayBuffer, &[]);
        }

        // 4. If IsDetachedBuffer(O) is true, return +0𝔽.
        if array_buffer_object.is_detached() {
            return Ok(Value::from_i32(0));
        }

        // 5. If IsFixedLengthArrayBuffer(O) is true, then
        let length = if array_buffer_object.is_fixed_length() {
            // a. Let length be O.[[ArrayBufferByteLength]].
            array_buffer_object.byte_length()
        }
        // 6. Else,
        else {
            // a. Let length be O.[[ArrayBufferMaxByteLength]].
            array_buffer_object.max_byte_length()
        };

        // 7. Return 𝔽(length).
        Ok(Value::from_f64(length as f64))
    }

    // 25.1.6.5 get ArrayBuffer.prototype.resizable, https://tc39.es/ecma262/#sec-get-arraybuffer.prototype.resizable
    fn resizable(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[ArrayBufferData]]).
        let array_buffer_object = this_array_buffer(vm)?;

        // 3. If IsSharedArrayBuffer(O) is true, throw a TypeError exception.
        if array_buffer_object.is_shared_array_buffer() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::SharedArrayBuffer, &[]);
        }

        // 4. If IsFixedLengthArrayBuffer(O) is false, return true; otherwise return false.
        Ok(Value::from_bool(!array_buffer_object.is_fixed_length()))
    }

    // 25.1.6.6 ArrayBuffer.prototype.resize ( newLength ), https://tc39.es/ecma262/#sec-arraybuffer.prototype.resize
    fn resize(vm: &Vm) -> ThrowCompletionOr<Value> {
        let new_length = vm.argument(0);

        // 1. Let O be the this value.
        let array_buffer_object = this_array_buffer(vm)?;

        // 2. Perform ? RequireInternalSlot(O, [[ArrayBufferMaxByteLength]]).
        if array_buffer_object.is_fixed_length() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::FixedArrayBuffer, &[]);
        }

        // 3. If IsSharedArrayBuffer(O) is true, throw a TypeError exception.
        if array_buffer_object.is_shared_array_buffer() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::SharedArrayBuffer, &[]);
        }

        // 4. Let newByteLength be ? ToIndex(newLength).
        let new_byte_length = new_length.to_index(vm)? as usize;

        // 5. If IsDetachedBuffer(O) is true, throw a TypeError exception.
        if array_buffer_object.is_detached() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::DetachedArrayBuffer, &[]);
        }

        // 6. If newByteLength > O.[[ArrayBufferMaxByteLength]], throw a RangeError exception.
        if new_byte_length > array_buffer_object.max_byte_length() {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::ByteLengthExceedsMaxByteLength,
                &[&new_byte_length, &array_buffer_object.max_byte_length()],
            );
        }

        // 7. Let hostHandled be ? HostResizeArrayBuffer(O, newByteLength).
        let host_handled = vm.host_resize_array_buffer()(vm, &array_buffer_object, new_byte_length)?;

        // 8. If hostHandled is handled, return undefined.
        if host_handled == HandledByHost::Handled {
            return Ok(Value::UNDEFINED);
        }

        // 9. Let oldBlock be O.[[ArrayBufferData]].
        // NOTE: oldBlock is read directly in step 12.

        // 10. Let newBlock be ? CreateByteDataBlock(newByteLength).
        let new_block = create_byte_data_block(vm, new_byte_length, None)?;

        // 11. Let copyLength be min(newByteLength, O.[[ArrayBufferByteLength]]).
        let copy_length = new_byte_length.min(array_buffer_object.byte_length());

        // 12. Perform CopyDataBlockBytes(newBlock, 0, oldBlock, 0, copyLength).
        array_buffer_object.copy_data_to_block(&new_block, 0, 0, copy_length);

        // 13. NOTE: Neither creation of the new Data Block nor copying from the old Data Block are observable. Implementations may implement this method as in-place growth or shrinkage.

        // 14. Set O.[[ArrayBufferData]] to newBlock.
        array_buffer_object.set_data_block(vm, new_block);

        // 15. Set O.[[ArrayBufferByteLength]] to newByteLength.

        // 16. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 25.1.6.7 ArrayBuffer.prototype.slice ( start, end ), https://tc39.es/ecma262/#sec-arraybuffer.prototype.slice
    fn slice(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a native function runs in a realm");

        let start = vm.argument(0);
        let end = vm.argument(1);

        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[ArrayBufferData]]).
        let array_buffer_object = this_array_buffer(vm)?;

        // 3. If IsSharedArrayBuffer(O) is true, throw a TypeError exception.
        if array_buffer_object.is_shared_array_buffer() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::SharedArrayBuffer, &[]);
        }

        // 4. If IsDetachedBuffer(O) is true, throw a TypeError exception.
        if array_buffer_object.is_detached() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::DetachedArrayBuffer, &[]);
        }

        // 5. Let len be O.[[ArrayBufferByteLength]].
        let length = array_buffer_object.byte_length() as f64;

        // 6. Let relativeStart be ? ToIntegerOrInfinity(start).
        let relative_start = start.to_integer_or_infinity(vm)?;

        // 7. If relativeStart is -∞, let first be 0.
        let first = if relative_start == f64::NEG_INFINITY {
            0.0
        }
        // 8. Else if relativeStart < 0, let first be max(len + relativeStart, 0).
        else if relative_start < 0.0 {
            (length + relative_start).max(0.0)
        }
        // 9. Else, let first be min(relativeStart, len).
        else {
            relative_start.min(length)
        };

        // 10. If end is undefined, let relativeEnd be len; else let relativeEnd be ? ToIntegerOrInfinity(end).
        let relative_end = if end.is_undefined() {
            length
        } else {
            end.to_integer_or_infinity(vm)?
        };

        // 11. If relativeEnd is -∞, let final be 0.
        let final_ = if relative_end == f64::NEG_INFINITY {
            0.0
        }
        // 12. Else if relativeEnd < 0, let final be max(len + relativeEnd, 0).
        else if relative_end < 0.0 {
            (length + relative_end).max(0.0)
        }
        // 13. Else, let final be min(relativeEnd, len).
        else {
            relative_end.min(length)
        };

        // 14. Let newLen be max(final - first, 0).
        let new_length = (final_ - first).max(0.0);

        // 15. Let ctor be ? SpeciesConstructor(O, %ArrayBuffer%).
        let constructor = species_constructor(
            vm,
            &array_buffer_object,
            realm.intrinsics().array_buffer_constructor(vm),
        )?;

        // 16. Let new be ? Construct(ctor, « 𝔽(newLen) »).
        let new_array_buffer = construct(vm, constructor, &[Value::from_f64(new_length)], None)?;

        // 17. Perform ? RequireInternalSlot(new, [[ArrayBufferData]]).
        let Some(new_array_buffer_object) = new_array_buffer.downcast::<ArrayBuffer>() else {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::SpeciesConstructorDidNotCreate,
                &[&"an ArrayBuffer"],
            );
        };

        // 18. If IsSharedArrayBuffer(new) is true, throw a TypeError exception.
        if new_array_buffer_object.is_shared_array_buffer() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::SharedArrayBuffer, &[]);
        }

        // 19. If IsDetachedBuffer(new) is true, throw a TypeError exception.
        if new_array_buffer_object.is_detached() {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::SpeciesConstructorReturned,
                &[&"a detached ArrayBuffer"],
            );
        }

        // 20. If SameValue(new, O) is true, throw a TypeError exception.
        if same_value(
            Value::from_object(new_array_buffer_object),
            Value::from_object(array_buffer_object),
        ) {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::SpeciesConstructorReturned,
                &[&"same ArrayBuffer instance"],
            );
        }

        // 21. If new.[[ArrayBufferByteLength]] < newLen, throw a TypeError exception.
        if (new_array_buffer_object.byte_length() as f64) < new_length {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::SpeciesConstructorReturned,
                &[&"an ArrayBuffer smaller than requested"],
            );
        }

        // 22. NOTE: Side-effects of the above steps may have detached or resized O.

        // 23. If IsDetachedBuffer(O) is true, throw a TypeError exception.
        if array_buffer_object.is_detached() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::DetachedArrayBuffer, &[]);
        }

        // 24. Let fromBuf be O.[[ArrayBufferData]].
        // 25. Let toBuf be new.[[ArrayBufferData]].
        // NOTE: fromBuf and toBuf are read directly in step 27b.

        // 26. Let currentLen be O.[[ArrayBufferByteLength]].
        let current_length = array_buffer_object.byte_length() as f64;

        // 27. If first < currentLen, then
        if first < current_length {
            // a. Let count be min(newLen, currentLen - first).
            let count = new_length.min(current_length - first);

            // b. Perform CopyDataBlockBytes(toBuf, 0, fromBuf, first, count).
            array_buffer_object.copy_data_to(&new_array_buffer_object, first as usize, 0, count as usize);
        }

        // 28. Return new.
        Ok(Value::from_object(new_array_buffer_object))
    }

    // 25.1.6.8 ArrayBuffer.prototype.transfer ( [ newLength ] ), https://tc39.es/ecma262/#sec-arraybuffer.prototype.transfer
    fn transfer(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        let array_buffer_object = this_array_buffer(vm)?;

        // 2. Return ? ArrayBufferCopyAndDetach(O, newLength, PRESERVE-RESIZABILITY).
        let new_length = vm.argument(0);
        Ok(Value::from_object(array_buffer_copy_and_detach(
            vm,
            array_buffer_object,
            new_length,
            PreserveResizability::PreserveResizability,
        )?))
    }

    // 25.1.6.9 ArrayBuffer.prototype.transferToFixedLength ( [ newLength ] ), https://tc39.es/ecma262/#sec-arraybuffer.prototype.transfertofixedlength
    fn transfer_to_fixed_length(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        let array_buffer_object = this_array_buffer(vm)?;

        // 2. Return ? ArrayBufferCopyAndDetach(O, newLength, FIXED-LENGTH).
        let new_length = vm.argument(0);
        Ok(Value::from_object(array_buffer_copy_and_detach(
            vm,
            array_buffer_object,
            new_length,
            PreserveResizability::FixedLength,
        )?))
    }
}
