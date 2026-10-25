/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! GC objects, exceptions, and runtime types, as cells of the heap (see heap.rs), and how the collector traces them.
//!
//! References in the any hierarchy are encoded so that their kind shows in their low bits: zero is null, an i31 has
//! bit 0 set (its value is in the bits above), a host value converted from an externref has bits 1:0 equal to 0b10,
//! and GC objects and exceptions are the addresses of their cells. Converting between externref and anyref keeps the
//! bits. An embedder's host values should be the addresses of its cells with bit 1 set, so that the collector keeps
//! them alive; other host values are never followed.

use super::heap::{
    CELL_HEADER_SIZE, CellType, Heap, TRACE_FUNCTION, TRACE_NONE, TRACE_REFERENCE, Visitor, forget_host_object,
    object_size_class, visit_host_object,
};
use super::{Store, TagAddress, Value};
pub use crate::abi::EXCEPTION_CELL_SIZE;
use crate::abi::{
    EXCEPTION_KINDS_OFFSET, EXCEPTION_LENGTH_OFFSET, EXCEPTION_PAYLOAD_OFFSET, EXCEPTION_TAG_OFFSET,
    OBJECT_DATA_OFFSET, OBJECT_INLINE_DATA_OFFSET, OBJECT_LENGTH_OFFSET, RTT_KIND_ARRAY, RTT_KIND_FUNCTION,
    RTT_KIND_STRUCT, Rtt, STORAGE_32, STORAGE_64, STORAGE_FUNCTION_REFERENCE, STORAGE_I8, STORAGE_I16,
    STORAGE_REFERENCE, STORAGE_V128,
};
use crate::types::{CompositeType, HeapType, StorageType, SubType, ValType};
use core::ffi::c_void;

const STORE_ROOT_STORE_OFFSET: usize = 16;

/// The largest data an object can have.
const MAX_OBJECT_DATA_SIZE: u64 = 1 << 31;

unsafe fn read<T: Copy>(cell: *const c_void, offset: usize) -> T {
    unsafe { cell.cast::<u8>().add(offset).cast::<T>().read() }
}

unsafe fn write<T>(cell: *mut u8, offset: usize, value: T) {
    unsafe { cell.add(offset).cast::<T>().write(value) };
}

/// How the collector follows values of `ty` (TRACE_*). Concrete heap types are identities of `types`.
pub fn trace_of(ty: ValType, types: &[SubType]) -> u8 {
    let ValType::Ref(reference) = ty else {
        return TRACE_NONE;
    };
    match reference.heap {
        HeapType::Func | HeapType::NoFunc => TRACE_FUNCTION,
        HeapType::Concrete(identity) => match types.get(identity as usize).map(|ty| &ty.composite) {
            Some(CompositeType::Func(_)) => TRACE_FUNCTION,
            Some(_) => TRACE_REFERENCE,
            None => panic!("concrete heap types are identities of the store"),
        },
        _ => TRACE_REFERENCE,
    }
}

pub fn storage_kind(storage: StorageType, types: &[SubType]) -> u8 {
    match storage {
        StorageType::I8 => STORAGE_I8,
        StorageType::I16 => STORAGE_I16,
        StorageType::Val(ValType::V128) => STORAGE_V128,
        StorageType::Val(ValType::I32 | ValType::F32) => STORAGE_32,
        StorageType::Val(value) => match trace_of(value, types) {
            TRACE_REFERENCE => STORAGE_REFERENCE,
            TRACE_FUNCTION => STORAGE_FUNCTION_REFERENCE,
            _ => STORAGE_64,
        },
    }
}

/// How the collector follows a field of storage `kind`.
fn trace_of_storage(kind: u8) -> u8 {
    match kind {
        STORAGE_REFERENCE => TRACE_REFERENCE,
        STORAGE_FUNCTION_REFERENCE => TRACE_FUNCTION,
        _ => TRACE_NONE,
    }
}

fn leak<T>(values: Vec<T>) -> *const T {
    Box::leak(values.into_boxed_slice()).as_ptr()
}

unsafe fn free<T>(pointer: *const T, length: usize) {
    if length != 0 {
        unsafe {
            drop(Box::from_raw(core::ptr::slice_from_raw_parts_mut(
                pointer.cast_mut(),
                length,
            )));
        };
    }
}

/// The Rtt of a cell that holds one.
fn rtt_of_cell(cell: *mut u8) -> *mut Rtt {
    cell.wrapping_add(CELL_HEADER_SIZE).cast()
}

fn cell_of_rtt(rtt: *const Rtt) -> *mut u8 {
    rtt.cast::<u8>().cast_mut().wrapping_sub(CELL_HEADER_SIZE)
}

/// Allocate the runtime type of the store type `identity`, whose supertypes have runtime types already.
pub fn allocate_rtt(store: &Store, identity: u32) -> *const Rtt {
    let sub_type = &store.types[identity as usize];
    let mut display = match sub_type.supertypes.first() {
        Some(&supertype) => {
            let parent = store.rtt(supertype);
            unsafe { ak::slice_from_raw_parts(parent.display, parent.depth as usize + 1) }.to_vec()
        }
        None => Vec::new(),
    };
    display.push(identity);
    let (kind, field_kinds, field_offsets, size) = match &sub_type.composite {
        CompositeType::Func(_) => (RTT_KIND_FUNCTION, Vec::new(), Vec::new(), 0),
        CompositeType::Struct(fields) => {
            let kinds: Vec<u8> = fields
                .iter()
                .map(|field| storage_kind(field.storage, &store.types))
                .collect();
            let mut offsets = Vec::with_capacity(kinds.len());
            let mut offset = 0;
            for field in fields {
                offsets.push(offset);
                offset += field.storage.object_field_size();
            }
            (RTT_KIND_STRUCT, kinds, offsets, offset)
        }
        CompositeType::Array(element) => {
            let kind = storage_kind(element.storage, &store.types);
            let size = element.storage.object_field_size();
            (RTT_KIND_ARRAY, vec![kind], Vec::new(), size)
        }
    };
    let depth = display.len() as u32 - 1;
    let field_count = field_offsets.len() as u32;
    let kind_count = field_kinds.len();
    let cell = store.heap.allocate_cell(CellType::Rtt);
    let rtt = rtt_of_cell(cell);
    unsafe {
        rtt.write(Rtt {
            kind,
            depth,
            display: leak(display),
            field_offsets: leak(field_offsets),
            field_kinds: leak(field_kinds),
            size,
            field_count: kind_count as u32,
        });
        // The count of offsets and kinds differs for arrays, which have one kind and no offsets.
        debug_assert!(field_count as usize <= kind_count);
    }
    rtt
}

pub(super) unsafe extern "C" fn rtt_visit_edges(_: *mut c_void, _: *mut c_void) {}

pub(super) unsafe extern "C" fn rtt_destroy(cell: *mut c_void) {
    unsafe {
        let rtt = &*rtt_of_cell(cell.cast());
        free(rtt.display, rtt.depth as usize + 1);
        let kind_count = if rtt.kind == RTT_KIND_ARRAY {
            1
        } else {
            rtt.field_count as usize
        };
        free(rtt.field_kinds, kind_count);
        let offset_count = if rtt.kind == RTT_KIND_STRUCT {
            rtt.field_count as usize
        } else {
            0
        };
        free(rtt.field_offsets, offset_count);
    }
}

/// Allocate a zeroed object of the type `rtt` with `data_size` bytes of data and, for an array, `length` elements.
/// Returns zero if the data is too large, or if there is no memory for it.
pub fn allocate_object(heap: &Heap, rtt: *const Rtt, data_size: u64, length: u64) -> u64 {
    if data_size > MAX_OBJECT_DATA_SIZE {
        return 0;
    }
    let (cell, data) = match object_size_class(data_size as usize) {
        Some(class) => {
            let cell = heap.allocate_cell(CellType::Object(class));
            (cell, unsafe { cell.add(OBJECT_INLINE_DATA_OFFSET) })
        }
        None => {
            // The data lives outside the heap, after a word that holds the number of words after it. It counts towards
            // the next collection before the cell is allocated, which may collect.
            let words = data_size.div_ceil(8) as usize;
            let mut block = Vec::new();
            if block.try_reserve_exact(words + 1).is_err() {
                return 0;
            }
            block.resize(words + 1, 0u64);
            heap.did_allocate_external_memory((words + 1) * 8);
            let block = Box::leak(block.into_boxed_slice());
            block[0] = words as u64;
            let data = unsafe { block.as_mut_ptr().add(1) }.cast::<u8>();
            (heap.allocate_cell(CellType::Object(0)), data)
        }
    };
    unsafe {
        write(cell, 0, rtt as u64);
        write(cell, OBJECT_DATA_OFFSET, data as u64);
        write(cell, OBJECT_LENGTH_OFFSET, length);
    }
    cell as u64
}

/// The out-of-line data of an object and its size in words, if it has some.
unsafe fn out_of_line_data(cell: *const c_void) -> Option<(*mut u64, usize)> {
    unsafe {
        let data: *mut u64 = read(cell, OBJECT_DATA_OFFSET);
        if data.cast::<u8>().cast_const() == cell.cast::<u8>().add(OBJECT_INLINE_DATA_OFFSET) {
            return None;
        }
        let block = data.sub(1);
        Some((block, *block as usize + 1))
    }
}

pub(super) unsafe extern "C" fn object_visit_edges(cell: *mut c_void, visitor: *mut c_void) {
    let visitor = Visitor::new(visitor);
    unsafe {
        visit_host_object(cell, &visitor);
        let rtt: *const Rtt = read(cell, 0);
        visitor.visit_cell(cell_of_rtt(rtt));
        let rtt = &*rtt;
        let data: *const u8 = read(cell, OBJECT_DATA_OFFSET);
        if rtt.kind == RTT_KIND_STRUCT {
            for index in 0..rtt.field_count as usize {
                let trace = trace_of_storage(*rtt.field_kinds.add(index));
                if trace != TRACE_NONE {
                    let field = data.add(*rtt.field_offsets.add(index) as usize).cast::<u64>().read();
                    visitor.visit_value(field, trace);
                }
            }
        } else {
            let trace = trace_of_storage(*rtt.field_kinds);
            if trace != TRACE_NONE {
                let length: u64 = read(cell, OBJECT_LENGTH_OFFSET);
                for index in 0..length as usize {
                    visitor.visit_value(data.cast::<u64>().add(index).read(), trace);
                }
            }
        }
    }
}

pub(super) unsafe extern "C" fn object_destroy(cell: *mut c_void) {
    unsafe {
        forget_host_object(cell);
        if let Some((block, words)) = out_of_line_data(cell) {
            free(block, words);
        }
    }
}

pub(super) unsafe extern "C" fn object_external_memory_size(cell: *const c_void) -> usize {
    unsafe { out_of_line_data(cell).map_or(0, |(_, words)| words * 8) }
}

/// The number of elements of an array.
///
/// # Safety
///
/// `array` must be a live array reference.
pub unsafe fn array_length(array: u64) -> u32 {
    unsafe { read::<u64>(array as *const c_void, OBJECT_LENGTH_OFFSET) as u32 }
}

/// The address and storage kind of the element at `index` of an array of numbers (whose elements are neither
/// references nor v128s).
unsafe fn numeric_array_element(array: u64, index: u32) -> (*mut u8, u8) {
    unsafe {
        let cell = array as *const c_void;
        let rtt = &*read::<*const Rtt>(cell, 0);
        assert_eq!(rtt.kind, RTT_KIND_ARRAY, "the object is an array");
        let kind = *rtt.field_kinds;
        assert!(
            matches!(kind, STORAGE_I8 | STORAGE_I16 | STORAGE_32 | STORAGE_64),
            "the array's elements are numbers"
        );
        assert!(index < array_length(array), "the index is in bounds");
        let data: *mut u8 = read(cell, OBJECT_DATA_OFFSET);
        (data.add(index as usize * rtt.size as usize), kind)
    }
}

/// The bits of the element at `index` of an array of numbers, with packed elements zero-extended.
///
/// # Safety
///
/// `array` must be a live array reference.
pub unsafe fn array_element(array: u64, index: u32) -> u64 {
    unsafe {
        let (address, _) = numeric_array_element(array, index);
        address.cast::<u64>().read()
    }
}

/// Set the element at `index` of an array of numbers to `bits`, which packed elements wrap.
///
/// # Safety
///
/// `array` must be a live array reference.
pub unsafe fn set_array_element(array: u64, index: u32, bits: u64) {
    unsafe {
        let (address, kind) = numeric_array_element(array, index);
        let mask = match kind {
            STORAGE_I8 => 0xff,
            STORAGE_I16 => 0xffff,
            STORAGE_32 => 0xffff_ffff,
            _ => u64::MAX,
        };
        address.cast::<u64>().write(bits & mask);
    }
}

/// An exception: a tag and the values thrown with it, in operand stack order (the last value in the lowest slot).
pub struct Exception {
    pub tag: TagAddress,
    pub payload: &'static [u64],
}

/// Allocate an exception with the tag `tag` and the slots of `payload`, which the collector follows as `traces` says
/// (TRACE_*).
pub fn allocate_exception(heap: &Heap, tag: TagAddress, payload: &[u64], traces: &[u8]) -> u64 {
    let cell = heap.allocate_cell(CellType::Exception);
    let kinds: Vec<u8> = traces.to_vec();
    unsafe {
        write(cell, EXCEPTION_TAG_OFFSET, tag.as_ptr() as u64);
        write(cell, EXCEPTION_PAYLOAD_OFFSET, leak(payload.to_vec()) as u64);
        write(cell, EXCEPTION_LENGTH_OFFSET, payload.len() as u64);
        write(cell, EXCEPTION_KINDS_OFFSET, leak(kinds) as u64);
    }
    cell as u64
}

/// The exception an exception reference refers to.
pub fn exception(reference: u64) -> Exception {
    let cell = reference as *const c_void;
    unsafe {
        let tag: u64 = read(cell, EXCEPTION_TAG_OFFSET);
        let payload: *const u64 = read(cell, EXCEPTION_PAYLOAD_OFFSET);
        let length: u64 = read(cell, EXCEPTION_LENGTH_OFFSET);
        Exception {
            tag: TagAddress::from_ptr(tag as *const super::TagInstance),
            payload: if length == 0 {
                &[]
            } else {
                ak::slice_from_raw_parts(payload, length as usize)
            },
        }
    }
}

pub(super) unsafe extern "C" fn exception_visit_edges(cell: *mut c_void, visitor: *mut c_void) {
    let visitor = Visitor::new(visitor);
    unsafe {
        visit_host_object(cell, &visitor);
        let payload: *const u64 = read(cell, EXCEPTION_PAYLOAD_OFFSET);
        let length: u64 = read(cell, EXCEPTION_LENGTH_OFFSET);
        let kinds: *const u8 = read(cell, EXCEPTION_KINDS_OFFSET);
        let tag: *const super::TagInstance = read(cell, EXCEPTION_TAG_OFFSET);
        visitor.visit_cell(TagAddress::from_ptr(tag).cell());
        for index in 0..length as usize {
            visitor.visit_value(*payload.add(index), *kinds.add(index));
        }
    }
}

pub(super) unsafe extern "C" fn exception_destroy(cell: *mut c_void) {
    unsafe {
        forget_host_object(cell);
        let length: u64 = read(cell, EXCEPTION_LENGTH_OFFSET);
        free(read::<*const u64>(cell, EXCEPTION_PAYLOAD_OFFSET), length as usize);
        free(read::<*const u8>(cell, EXCEPTION_KINDS_OFFSET), length as usize);
    }
}

/// Allocate the cell through which the collector reaches what a store refers to.
pub(crate) fn allocate_store_root(heap: &Heap, store: *const Store) -> *mut u8 {
    let cell = heap.allocate_cell(CellType::StoreRoot);
    unsafe { write(cell, STORE_ROOT_STORE_OFFSET, store as u64) };
    cell
}

/// Detach a store's root cell from the store, which is going away, so that the collector no longer reaches it.
pub(crate) fn detach_store_root(cell: *mut u8) {
    unsafe { write(cell, STORE_ROOT_STORE_OFFSET, 0u64) };
}

pub(super) unsafe extern "C" fn store_root_destroy(cell: *mut c_void) {
    unsafe {
        let store: *mut Store = read(cell, STORE_ROOT_STORE_OFFSET);
        if let Some(store) = store.as_mut() {
            store.root_cell = std::ptr::null_mut();
        }
    }
}

pub(super) unsafe extern "C" fn store_root_visit_edges(cell: *mut c_void, visitor: *mut c_void) {
    unsafe {
        let store: *const Store = read(cell, STORE_ROOT_STORE_OFFSET);
        if let Some(store) = store.as_ref() {
            store.trace(&Visitor::new(visitor));
        }
    }
}

impl Store {
    pub fn rtt(&self, identity: u32) -> &Rtt {
        unsafe { &*self.rtt_pointers[identity as usize] }
    }

    /// Allocate a zeroed object of `data_size` bytes whose type is `rtt`, or return zero if it is too large.
    pub fn allocate_object(&mut self, rtt: *const Rtt, data_size: u64, length: u64) -> u64 {
        allocate_object(&self.heap, rtt, data_size, length)
    }

    /// Allocate a struct of the type `identity` with the given field values, or return zero if there is no memory
    /// for it.
    pub fn allocate_struct(&mut self, identity: u32, fields: &[Value]) -> u64 {
        let rtt: *const Rtt = self.rtt(identity);
        let size = u64::from(unsafe { (*rtt).size });
        let object = self.allocate_object(rtt, size, 0);
        if object == 0 {
            return 0;
        }
        let data = unsafe { read::<*mut u8>(object as *const c_void, OBJECT_DATA_OFFSET) };
        for (index, value) in fields.iter().enumerate() {
            unsafe {
                let offset = *(*rtt).field_offsets.add(index);
                let kind = *(*rtt).field_kinds.add(index);
                store_field(data.add(offset as usize), kind, *value);
            }
        }
        object
    }

    /// Allocate an array of the type `identity` with `values` as its elements, or return zero if it is too large.
    pub fn allocate_array(&mut self, identity: u32, values: &[Value]) -> u64 {
        let rtt: *const Rtt = self.rtt(identity);
        let (element_size, kind) = unsafe { (u64::from((*rtt).size), *(*rtt).field_kinds) };
        let object = self.allocate_object(rtt, values.len() as u64 * element_size, values.len() as u64);
        if object == 0 {
            return 0;
        }
        let data = unsafe { read::<*mut u8>(object as *const c_void, OBJECT_DATA_OFFSET) };
        for (index, value) in values.iter().enumerate() {
            unsafe { store_field(data.add(index * element_size as usize), kind, *value) };
        }
        object
    }

    /// Allocate an array of the type `identity` with `length` elements of `value`, or return zero if it is too large
    /// or there is no memory for it.
    pub fn allocate_filled_array(&mut self, identity: u32, value: Value, length: u32) -> u64 {
        let rtt: *const Rtt = self.rtt(identity);
        let (element_size, kind) = unsafe { (u64::from((*rtt).size), *(*rtt).field_kinds) };
        let object = self.allocate_object(rtt, u64::from(length) * element_size, u64::from(length));
        if object == 0 {
            return 0;
        }
        let data = unsafe { read::<*mut u8>(object as *const c_void, OBJECT_DATA_OFFSET) };
        for index in 0..length as usize {
            unsafe { store_field(data.add(index * element_size as usize), kind, value) };
        }
        object
    }

    /// Make an exception with `tag` and the values of `payload` (in operand stack order), and return its reference.
    pub fn allocate_exception(&mut self, tag: TagAddress, payload: &[u64]) -> u64 {
        let mut traces = Vec::with_capacity(payload.len());
        // The slots are in operand stack order, the last parameter first.
        for &param in self.tag(tag).ty.params.iter().rev() {
            traces.push(trace_of(param, &self.types));
            traces.extend(std::iter::repeat_n(TRACE_NONE, param.slot_count() as usize - 1));
        }
        // NB: The collector follows the payload and frees it as long as the tag's parameters say it is.
        assert_eq!(
            payload.len(),
            traces.len(),
            "an exception's payload has the slots of its tag's parameters"
        );
        allocate_exception(&self.heap, tag, payload, &traces)
    }

    /// The exception an exception reference refers to.
    pub fn exception(&self, reference: u64) -> Exception {
        exception(reference)
    }

    /// Visit what the store keeps: its runtime types, and the exception of a trap that is on its way out.
    pub fn trace(&self, visitor: &Visitor) {
        for &rtt in &self.rtt_pointers {
            visitor.visit_cell(cell_of_rtt(rtt));
        }
        if let Some(super::Trap::UncaughtException(exception)) = &self.pending_trap {
            visitor.visit_cell(*exception as *mut u8);
        }
    }
}

unsafe fn store_field(address: *mut u8, kind: u8, value: Value) {
    let (slots, _) = value.to_slots();
    let mask = match kind {
        STORAGE_I8 => 0xff,
        STORAGE_I16 => 0xffff,
        _ => u64::MAX,
    };
    unsafe {
        address.cast::<u64>().write(slots[0] & mask);
        if kind == STORAGE_V128 {
            address.cast::<u64>().add(1).write(slots[1]);
        }
    }
}

/// The i31 reference of a value.
pub fn i31_reference(value: i32) -> u64 {
    (u64::from(value as u32 & 0x7fff_ffff) << 1) | 1
}
