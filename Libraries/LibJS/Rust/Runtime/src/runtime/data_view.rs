/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::runtime::array_buffer::{ArrayBuffer, Order, array_buffer_byte_length};
use crate::runtime::byte_length::ByteLength;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct DataView {
    base: Object,
    viewed_array_buffer: Cell<Gc<ArrayBuffer>>,
    #[gc(untraced)]
    byte_length: Cell<ByteLength>,
    byte_offset: Cell<usize>,
}

define_cell!(DataView, Object, extends: [Object]);

impl Deref for DataView {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl DataView {
    /// DataView(GC::Ptr<ArrayBuffer>, ByteLength byte_length, size_t byte_offset, Object& prototype)
    pub fn new(
        vm: &Vm,
        viewed_buffer: Gc<ArrayBuffer>,
        byte_length: ByteLength,
        byte_offset: usize,
        prototype: Gc<Object>,
    ) -> DataView {
        DataView {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            viewed_array_buffer: Cell::new(viewed_buffer),
            byte_length: Cell::new(byte_length),
            byte_offset: Cell::new(byte_offset),
        }
    }

    pub fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        viewed_buffer: Gc<ArrayBuffer>,
        byte_length: ByteLength,
        byte_offset: usize,
    ) -> Gc<DataView> {
        let prototype = realm.intrinsics().data_view_prototype(vm);
        realm.create_object(
            vm,
            DataView::new(vm, viewed_buffer, byte_length, byte_offset, prototype),
        )
    }

    pub fn viewed_array_buffer(&self) -> Gc<ArrayBuffer> {
        self.viewed_array_buffer.get()
    }

    pub fn byte_length(&self) -> ByteLength {
        self.byte_length.get()
    }

    /// NB: Like C++, this truncates the byte offset to a u32.
    pub fn byte_offset(&self) -> u32 {
        self.byte_offset.get() as u32
    }
}

// 25.3.1.1 DataView With Buffer Witness Records, https://tc39.es/ecma262/#sec-dataview-with-buffer-witness-records
#[derive(Clone, Copy, Debug)]
pub struct DataViewWithBufferWitness {
    pub object: Gc<DataView>,                  // [[Object]]
    pub cached_buffer_byte_length: ByteLength, // [[CachedBufferByteLength]]
}

// 25.3.1.2 MakeDataViewWithBufferWitnessRecord ( obj, order ), https://tc39.es/ecma262/#sec-makedataviewwithbufferwitnessrecord
pub fn make_data_view_with_buffer_witness_record(data_view: Gc<DataView>, order: Order) -> DataViewWithBufferWitness {
    // 1. Let buffer be obj.[[ViewedArrayBuffer]].
    let buffer = data_view.viewed_array_buffer();

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

    // 4. Return the DataView With Buffer Witness Record { [[Object]]: obj, [[CachedBufferByteLength]]: byteLength }.
    DataViewWithBufferWitness {
        object: data_view,
        cached_buffer_byte_length: byte_length,
    }
}

// 25.3.1.3 GetViewByteLength ( viewRecord ), https://tc39.es/ecma262/#sec-getviewbytelength
pub fn get_view_byte_length(view_record: &DataViewWithBufferWitness) -> u32 {
    // 1. Assert: IsViewOutOfBounds(viewRecord) is false.
    assert!(!is_view_out_of_bounds(view_record));

    // 2. Let view be viewRecord.[[Object]].
    let view = view_record.object;

    // 3. If view.[[ByteLength]] is not auto, return view.[[ByteLength]].
    if !view.byte_length().is_auto() {
        return view.byte_length().length();
    }

    // 4. Assert: IsFixedLengthArrayBuffer(view.[[ViewedArrayBuffer]]) is false.
    assert!(!view.viewed_array_buffer().is_fixed_length());

    // 5. Let byteOffset be view.[[ByteOffset]].
    let byte_offset = view.byte_offset();

    // 6. Let byteLength be viewRecord.[[CachedBufferByteLength]].
    let byte_length = view_record.cached_buffer_byte_length;

    // 7. Assert: byteLength is not detached.
    assert!(!byte_length.is_detached());

    // 8. Return byteLength - byteOffset.
    byte_length.length().wrapping_sub(byte_offset)
}

// 25.3.1.4 IsViewOutOfBounds ( viewRecord ), https://tc39.es/ecma262/#sec-isviewoutofbounds
pub fn is_view_out_of_bounds(view_record: &DataViewWithBufferWitness) -> bool {
    // 1. Let view be viewRecord.[[Object]].
    let view = view_record.object;

    // 2. Let bufferByteLength be viewRecord.[[CachedBufferByteLength]].
    let buffer_byte_length = view_record.cached_buffer_byte_length;

    // 3. Assert: IsDetachedBuffer(view.[[ViewedArrayBuffer]]) is true if and only if bufferByteLength is detached.
    assert!(view.viewed_array_buffer().is_detached() == buffer_byte_length.is_detached());

    // 4. If bufferByteLength is detached, return true.
    if buffer_byte_length.is_detached() {
        return true;
    }

    // 5. Let byteOffsetStart be view.[[ByteOffset]].
    let byte_offset_start = view.byte_offset();

    // 6. If view.[[ByteLength]] is auto, then
    let byte_offset_end = if view.byte_length().is_auto() {
        // a. Let byteOffsetEnd be bufferByteLength.
        buffer_byte_length.length()
    }
    // 7. Else,
    else {
        // a. Let byteOffsetEnd be byteOffsetStart + view.[[ByteLength]].
        // NB: Like C++, this is computed in 32 bits.
        byte_offset_start.wrapping_add(view.byte_length().length())
    };

    // 8. If byteOffsetStart > bufferByteLength or byteOffsetEnd > bufferByteLength, return true.
    if byte_offset_start > buffer_byte_length.length() || byte_offset_end > buffer_byte_length.length() {
        return true;
    }

    // 9. NOTE: 0-length DataViews are not considered out-of-bounds.
    // 10. Return false.
    false
}
