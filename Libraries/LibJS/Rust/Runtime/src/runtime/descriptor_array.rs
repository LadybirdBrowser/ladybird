/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::hash::BuildHasher;
use core::ops::ControlFlow;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::runtime::property_attributes::PropertyAttributes;
use crate::runtime::property_key::PropertyKey;

/// Where a shape keeps a property: its slot in the object's named storage and its attributes.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PropertyMetadata {
    pub offset: u32,
    pub attributes: PropertyAttributes,
}

#[derive(Clone)]
pub struct Entry {
    pub property_key: PropertyKey,
    pub offset: u32,
    pub attributes: u16,
    pub enum_index: u16,
}

impl Entry {
    pub fn metadata(&self) -> PropertyMetadata {
        PropertyMetadata {
            offset: self.offset,
            attributes: PropertyAttributes::new(self.attributes as u8),
        }
    }
}

/// The properties of the shapes along a chain of transitions, which share one array. A shape sees the entries whose
/// enum index is below its property count. Entries are sorted by key hash, and the enum indices keep insertion order.
#[repr(C)]
pub struct DescriptorArray {
    header: CellHeader,
    entries: GcRefCell<Vec<Entry>>,
    entry_indices_by_enum_index: GcRefCell<Vec<u32>>,
}

define_cell!(DescriptorArray, Other);

// SAFETY: The keys of the entries are the only cells a descriptor array reaches.
unsafe impl Trace for DescriptorArray {
    fn trace(&self, visitor: &mut Visitor) {
        for entry in self.entries.borrow().iter() {
            entry.property_key.trace(visitor);
        }
    }
}

pub const MAX_DESCRIPTOR_COUNT: u32 = u16::MAX as u32 + 1;

fn property_key_hash(property_key: &PropertyKey) -> u32 {
    foldhash::fast::FixedState::default().hash_one(property_key) as u32
}

fn find(entries: &[Entry], property_key: &PropertyKey, descriptor_count: u32) -> Option<usize> {
    let is_match =
        |entry: &Entry| u32::from(entry.enum_index) < descriptor_count && entry.property_key == *property_key;

    if entries.len() <= 16 {
        return entries.iter().position(is_match);
    }

    let hash = property_key_hash(property_key);
    let low = entries.partition_point(|entry| property_key_hash(&entry.property_key) < hash);
    entries[low..]
        .iter()
        .take_while(|entry| property_key_hash(&entry.property_key) == hash)
        .position(is_match)
        .map(|index| low + index)
}

fn find_insertion_index(entries: &[Entry], property_key: &PropertyKey) -> usize {
    let hash = property_key_hash(property_key);
    entries.partition_point(|entry| property_key_hash(&entry.property_key) <= hash)
}

fn set_entry(
    entries: &mut Vec<Entry>,
    entry_indices_by_enum_index: &mut Vec<u32>,
    property_key: &PropertyKey,
    metadata: PropertyMetadata,
    enum_index: u32,
) {
    assert!(enum_index < MAX_DESCRIPTOR_COUNT);
    if let Some(existing_index) = find(entries, property_key, enum_index + 1) {
        let entry = &mut entries[existing_index];
        entry.offset = metadata.offset;
        entry.attributes = u16::from(metadata.attributes.bits());
        return;
    }

    let insertion_index = find_insertion_index(entries, property_key);
    let insertion_index_u32 = u32::try_from(insertion_index).expect("the insertion index fits in u32");
    assert!(enum_index as usize == entry_indices_by_enum_index.len());
    for entry_index in entry_indices_by_enum_index.iter_mut() {
        if *entry_index >= insertion_index_u32 {
            *entry_index += 1;
        }
    }
    entries.insert(
        insertion_index,
        Entry {
            property_key: property_key.clone(),
            offset: metadata.offset,
            attributes: u16::from(metadata.attributes.bits()),
            enum_index: enum_index as u16,
        },
    );
    entry_indices_by_enum_index.push(insertion_index_u32);
}

impl DescriptorArray {
    fn from_parts(entries: Vec<Entry>, entry_indices_by_enum_index: Vec<u32>) -> Self {
        Self {
            header: CellHeader::for_class(Self::CLASS),
            entries: GcRefCell::new(entries),
            entry_indices_by_enum_index: GcRefCell::new(entry_indices_by_enum_index),
        }
    }

    pub fn create(vm: &Vm) -> Gc<DescriptorArray> {
        vm.heap().allocate(Self::from_parts(Vec::new(), Vec::new()))
    }

    /// The first `descriptor_count` properties of `other`, in its insertion order.
    pub fn create_copy(vm: &Vm, other: &DescriptorArray, descriptor_count: u32) -> Gc<DescriptorArray> {
        assert!(descriptor_count <= MAX_DESCRIPTOR_COUNT);
        let mut entries = Vec::with_capacity(descriptor_count as usize);
        let mut entry_indices_by_enum_index = Vec::with_capacity(descriptor_count as usize);
        other.for_each_in_insertion_order(
            |property_key, metadata| {
                let enum_index = entries.len() as u32;
                set_entry(
                    &mut entries,
                    &mut entry_indices_by_enum_index,
                    property_key,
                    metadata,
                    enum_index,
                );
                ControlFlow::Continue(())
            },
            descriptor_count,
        );
        // The keys are clones of the keys `other` holds, which keeps their symbols alive while this allocates.
        vm.heap()
            .allocate(Self::from_parts(entries, entry_indices_by_enum_index))
    }

    pub fn size(&self) -> u32 {
        self.entries.borrow().len() as u32
    }

    pub fn lookup(&self, property_key: &PropertyKey, descriptor_count: u32) -> Option<PropertyMetadata> {
        let entries = self.entries.borrow();
        let index = find(&entries, property_key, descriptor_count)?;
        Some(entries[index].metadata())
    }

    pub fn set(&self, property_key: &PropertyKey, metadata: PropertyMetadata, enum_index: u32) {
        set_entry(
            &mut self.entries.borrow_mut(),
            &mut self.entry_indices_by_enum_index.borrow_mut(),
            property_key,
            metadata,
            enum_index,
        );
    }

    pub fn set_attributes(&self, property_key: &PropertyKey, attributes: PropertyAttributes, descriptor_count: u32) {
        let mut entries = self.entries.borrow_mut();
        let index = find(&entries, property_key, descriptor_count).expect("the property has a descriptor");
        entries[index].attributes = u16::from(attributes.bits());
    }

    pub fn remove(&self, property_key: &PropertyKey, descriptor_count: u32) {
        let mut entries = self.entries.borrow_mut();
        let mut entry_indices_by_enum_index = self.entry_indices_by_enum_index.borrow_mut();
        let index = find(&entries, property_key, descriptor_count).expect("the property has a descriptor");

        let removed_offset = entries[index].offset;
        let removed_enum_index = entries[index].enum_index;
        entries.remove(index);
        entry_indices_by_enum_index.remove(usize::from(removed_enum_index));
        let index = index as u32;
        for entry_index in entry_indices_by_enum_index.iter_mut() {
            if *entry_index > index {
                *entry_index -= 1;
            }
        }

        for entry in entries.iter_mut() {
            if u32::from(entry.enum_index) >= descriptor_count {
                continue;
            }
            if entry.offset > removed_offset {
                entry.offset -= 1;
            }
            if entry.enum_index > removed_enum_index {
                entry.enum_index -= 1;
            }
        }
    }

    /// Calls `callback` with the first `descriptor_count` properties in insertion order, until it breaks. The array
    /// stays borrowed meanwhile, so the callback must not allocate or call anything that takes the VM.
    pub fn for_each_in_insertion_order(
        &self,
        mut callback: impl FnMut(&PropertyKey, PropertyMetadata) -> ControlFlow<()>,
        descriptor_count: u32,
    ) {
        let entries = self.entries.borrow();
        let entry_indices_by_enum_index = self.entry_indices_by_enum_index.borrow();
        assert!(descriptor_count as usize <= entry_indices_by_enum_index.len());
        for enum_index in 0..descriptor_count {
            let entry = &entries[entry_indices_by_enum_index[enum_index as usize] as usize];
            assert!(u32::from(entry.enum_index) == enum_index);
            if callback(&entry.property_key, entry.metadata()).is_break() {
                return;
            }
        }
    }
}
