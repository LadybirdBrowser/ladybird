/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! GC objects: their runtime types, layout, and allocation (see `Rtt` in abi.rs for the layout).
//!
//! References in the any hierarchy are encoded so that their kind shows in their low bits: zero is null, an i31 has
//! bit 0 set (its value is in the bits above), a host value converted from an externref has bits 1:0 equal to 0b10,
//! and GC objects are 8-byte aligned addresses. Converting between externref and anyref keeps the bits.

use super::{Store, Value};
use crate::abi::{
    ARRAY_ELEMENTS_OFFSET, RTT_KIND_ARRAY, RTT_KIND_FUNCTION, RTT_KIND_STRUCT, Rtt, STORAGE_32, STORAGE_64, STORAGE_I8,
    STORAGE_I16, STORAGE_V128, STRUCT_FIELDS_OFFSET,
};
use crate::types::{CompositeType, StorageType, ValType};

/// An Rtt and the arrays it points at.
pub struct RttStorage {
    pub rtt: Rtt,
    display: Box<[u32]>,
    _field_offsets: Box<[u32]>,
    _field_kinds: Box<[u8]>,
}

pub fn storage_kind(storage: StorageType) -> u8 {
    match storage {
        StorageType::I8 => STORAGE_I8,
        StorageType::I16 => STORAGE_I16,
        StorageType::Val(ValType::V128) => STORAGE_V128,
        StorageType::Val(ValType::I32 | ValType::F32) => STORAGE_32,
        StorageType::Val(_) => STORAGE_64,
    }
}

fn storage_size(kind: u8) -> u32 {
    if kind == STORAGE_V128 { 16 } else { 8 }
}

impl RttStorage {
    /// The runtime type of the store type `identity`, whose supertypes have runtime types already.
    pub fn new(store: &Store, identity: u32) -> Box<Self> {
        let sub_type = &store.types[identity as usize];
        let mut display = match sub_type.supertypes.first() {
            Some(&supertype) => store.rtts[supertype as usize].display.to_vec(),
            None => Vec::new(),
        };
        display.push(identity);
        let (kind, field_kinds, field_offsets, size) = match &sub_type.composite {
            CompositeType::Func(_) => (RTT_KIND_FUNCTION, Vec::new(), Vec::new(), 0),
            CompositeType::Struct(fields) => {
                let kinds: Vec<u8> = fields.iter().map(|field| storage_kind(field.storage)).collect();
                let mut offsets = Vec::with_capacity(kinds.len());
                let mut offset = STRUCT_FIELDS_OFFSET as u32;
                for &kind in &kinds {
                    offsets.push(offset);
                    offset += storage_size(kind);
                }
                (RTT_KIND_STRUCT, kinds, offsets, offset)
            }
            CompositeType::Array(element) => {
                let kind = storage_kind(element.storage);
                (RTT_KIND_ARRAY, vec![kind], Vec::new(), storage_size(kind))
            }
        };
        let display = display.into_boxed_slice();
        let field_offsets = field_offsets.into_boxed_slice();
        let field_kinds = field_kinds.into_boxed_slice();
        Box::new(Self {
            rtt: Rtt {
                kind,
                depth: display.len() as u32 - 1,
                display: display.as_ptr(),
                field_offsets: field_offsets.as_ptr(),
                field_kinds: field_kinds.as_ptr(),
                size,
                field_count: field_offsets.len() as u32,
            },
            display,
            _field_offsets: field_offsets,
            _field_kinds: field_kinds,
        })
    }
}

/// The largest object the runtime allocates.
const MAX_OBJECT_SIZE: u64 = 1 << 31;

impl Store {
    pub fn rtt(&self, identity: u32) -> &Rtt {
        &self.rtts[identity as usize].rtt
    }

    /// Allocate a zeroed object of `size` bytes whose type is `rtt`, or return zero if it is too large.
    pub fn allocate_object(&mut self, rtt: *const Rtt, size: u64) -> u64 {
        if size > MAX_OBJECT_SIZE {
            return 0;
        }
        let mut words = vec![0u64; size.div_ceil(8) as usize].into_boxed_slice();
        words[0] = rtt as u64;
        let object = words.as_mut_ptr() as u64;
        self.objects.push(words);
        object
    }

    /// Allocate a struct of the type `identity` with the given field values.
    pub fn allocate_struct(&mut self, identity: u32, fields: &[Value]) -> u64 {
        let rtt: *const Rtt = self.rtt(identity);
        let size = u64::from(unsafe { (*rtt).size });
        let object = self.allocate_object(rtt, size);
        for (index, value) in fields.iter().enumerate() {
            unsafe {
                let offset = *(*rtt).field_offsets.add(index);
                let kind = *(*rtt).field_kinds.add(index);
                store_field(object + u64::from(offset), kind, *value);
            }
        }
        object
    }

    /// Allocate an array of the type `identity` with `values` as its elements, or return zero if it is too large.
    pub fn allocate_array(&mut self, identity: u32, values: &[Value]) -> u64 {
        let rtt: *const Rtt = self.rtt(identity);
        let (element_size, kind) = unsafe { (u64::from((*rtt).size), *(*rtt).field_kinds) };
        let size = ARRAY_ELEMENTS_OFFSET as u64 + values.len() as u64 * element_size;
        let object = self.allocate_object(rtt, size);
        if object == 0 {
            return 0;
        }
        unsafe {
            *((object + 8) as *mut u64) = values.len() as u64;
            for (index, value) in values.iter().enumerate() {
                store_field(
                    object + ARRAY_ELEMENTS_OFFSET as u64 + index as u64 * element_size,
                    kind,
                    *value,
                );
            }
        }
        object
    }
}

unsafe fn store_field(address: u64, kind: u8, value: Value) {
    let mut slots = Vec::with_capacity(2);
    value.push_slots(&mut slots);
    let mask = match kind {
        STORAGE_I8 => 0xff,
        STORAGE_I16 => 0xffff,
        _ => u64::MAX,
    };
    unsafe {
        *(address as *mut u64) = slots[0] & mask;
        if kind == STORAGE_V128 {
            *((address + 8) as *mut u64) = slots[1];
        }
    }
}

/// The i31 reference of a value.
pub fn i31_reference(value: i32) -> u64 {
    (u64::from(value as u32 & 0x7fff_ffff) << 1) | 1
}
