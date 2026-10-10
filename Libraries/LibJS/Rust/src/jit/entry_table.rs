/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The JIT entry table: where calls into the JIT code of an executable go, and who owns that code, outside the GC heap.
//!
//! Every executable the VM registers while the JIT is on gets a slot for its life. The slot holds the entry that
//! calls of the executable's functions jump to (its JIT code, or else an entry that has the interpreter run the
//! frame), the executable it belongs to, and the executable's JIT code. Executables keep only their slot number, which
//! JIT code and the runtime mask into the table and check against the slot's owner. Nothing in the GC heap points at
//! JIT code, so a corrupted executable can at worst name the slot of another executable, which the owner check
//! catches.

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;

use super::code::JitCode;

/// How many slots the table has, a power of two. Executables registered while it is full never get JIT code.
pub const JIT_ENTRY_TABLE_CAPACITY: usize = 1 << 20;

/// Slot numbers are masked with this, which keeps them inside the table.
pub const JIT_ENTRY_SLOT_MASK: u32 = (JIT_ENTRY_TABLE_CAPACITY - 1) as u32;

/// Where the owners start, as a byte offset from the entries.
pub const JIT_ENTRY_TABLE_OWNERS_OFFSET: usize = JIT_ENTRY_TABLE_CAPACITY * 8;

/// Where the code starts, as a byte offset from the entries.
const JIT_ENTRY_TABLE_CODE_OFFSET: usize = 2 * JIT_ENTRY_TABLE_CAPACITY * 8;

const JIT_ENTRY_TABLE_SIZE: usize = 3 * JIT_ENTRY_TABLE_CAPACITY * 8;

/// The slot of executables without one. Its entry has the interpreter run the frame, and it has no owner.
pub const NO_JIT_ENTRY_SLOT: u32 = 0;

/// One reserved mapping of three parallel arrays, indexed by slot: the entries (`*const c_void`), the owners (the
/// address of the owning executable's head, as `u64`) and the code (`Option<Box<JitCode>>`). Its address stays the
/// same for the VM's life, so JIT code embeds it.
pub struct JitEntryTable {
    base: NonNull<u8>,
    /// What freed slots and new executables' slots get as their entry.
    not_compiled_entry: *const c_void,
    /// The first slot no executable ever had.
    next_slot: Cell<u32>,
    free_slots: RefCell<Vec<u32>>,
}

impl JitEntryTable {
    pub fn new(not_compiled_entry: *const c_void) -> Self {
        // SAFETY: A fresh anonymous mapping, which no one else uses. Its pages read as zeroes until written: no owner
        //         and no code in every slot.
        let base = unsafe {
            libc::mmap(
                core::ptr::null_mut(),
                JIT_ENTRY_TABLE_SIZE,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_NORESERVE,
                -1,
                0,
            )
        };
        assert!(base != libc::MAP_FAILED, "mapping the JIT entry table failed");
        let table = Self {
            base: NonNull::new(base.cast()).expect("mmap does not return null"),
            not_compiled_entry,
            next_slot: Cell::new(NO_JIT_ENTRY_SLOT + 1),
            free_slots: RefCell::default(),
        };
        table.entry_cell(NO_JIT_ENTRY_SLOT).set(not_compiled_entry);
        table
    }

    /// The address of the entries, which JIT code embeds.
    pub fn address(&self) -> u64 {
        self.base.as_ptr() as u64
    }

    /// The address of the slot's entry.
    pub fn entry_address(&self, slot: u32) -> u64 {
        self.address() + 8 * u64::from(slot & JIT_ENTRY_SLOT_MASK)
    }

    fn entry_cell(&self, slot: u32) -> &Cell<*const c_void> {
        // SAFETY: The masked slot is inside the entries, which live as long as the table.
        unsafe { &*self.base.as_ptr().add(8 * (slot & JIT_ENTRY_SLOT_MASK) as usize).cast() }
    }

    fn owner_cell(&self, slot: u32) -> &Cell<u64> {
        // SAFETY: The masked slot is inside the owners, which live as long as the table.
        unsafe {
            &*self
                .base
                .as_ptr()
                .add(JIT_ENTRY_TABLE_OWNERS_OFFSET + 8 * (slot & JIT_ENTRY_SLOT_MASK) as usize)
                .cast()
        }
    }

    fn code_cell(&self, slot: u32) -> &Cell<Option<Box<JitCode>>> {
        // SAFETY: The masked slot is inside the code array, which lives as long as the table.
        unsafe {
            &*self
                .base
                .as_ptr()
                .add(JIT_ENTRY_TABLE_CODE_OFFSET + 8 * (slot & JIT_ENTRY_SLOT_MASK) as usize)
                .cast()
        }
    }

    /// A slot for the executable whose head is at `owner`, with the not compiled entry, or `NO_JIT_ENTRY_SLOT` if the
    /// table is full.
    pub fn allocate(&self, owner: u64) -> u32 {
        let slot = match self.free_slots.borrow_mut().pop() {
            Some(slot) => slot,
            None => {
                let slot = self.next_slot.get();
                if slot as usize == JIT_ENTRY_TABLE_CAPACITY {
                    return NO_JIT_ENTRY_SLOT;
                }
                self.next_slot.set(slot + 1);
                slot
            }
        };
        self.entry_cell(slot).set(self.not_compiled_entry);
        self.owner_cell(slot).set(owner);
        slot
    }

    /// Frees the slot of the executable whose head is at `owner`, which died, and drops its code.
    pub fn free(&self, slot: u32, owner: u64) {
        let slot = slot & JIT_ENTRY_SLOT_MASK;
        if slot == NO_JIT_ENTRY_SLOT {
            return;
        }
        assert!(
            self.is_owned_by(slot, owner),
            "only the owner of a JIT entry slot frees it"
        );
        drop(self.code_cell(slot).take());
        self.entry_cell(slot).set(self.not_compiled_entry);
        self.owner_cell(slot).set(0);
        self.free_slots.borrow_mut().push(slot);
    }

    /// Whether the slot belongs to the executable whose head is at `owner`.
    pub fn is_owned_by(&self, slot: u32, owner: u64) -> bool {
        owner != 0 && self.owner_cell(slot).get() == owner
    }

    /// Makes calls through the slot enter `entry`, or else have the interpreter run the frame.
    pub fn set_entry(&self, slot: u32, entry: Option<*const c_void>) {
        self.entry_cell(slot).set(entry.unwrap_or(self.not_compiled_entry));
    }

    /// The code attached to the slot.
    pub fn code(&self, slot: u32) -> Option<&JitCode> {
        // SAFETY: The code stays in the slot until set_code() or free() replaces it, which callers do not do while
        //         they use the reference.
        unsafe { (*self.code_cell(slot).as_ptr()).as_deref() }
    }

    /// Attaches `code` to the slot, and returns the code it had.
    pub fn set_code(&self, slot: u32, code: Option<Box<JitCode>>) -> Option<Box<JitCode>> {
        assert_ne!(slot & JIT_ENTRY_SLOT_MASK, NO_JIT_ENTRY_SLOT);
        self.code_cell(slot).replace(code)
    }
}

impl Drop for JitEntryTable {
    fn drop(&mut self) {
        for slot in 0..self.next_slot.get() {
            drop(self.code_cell(slot).take());
        }
        // SAFETY: The table's mapping, which nothing uses anymore.
        unsafe {
            libc::munmap(self.base.as_ptr().cast(), JIT_ENTRY_TABLE_SIZE);
        }
    }
}
