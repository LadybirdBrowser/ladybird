/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Identity hashes: a 32-bit hash of each cell that stays the same for as long as the cell lives, wherever the
//! collector moves it.
//!
//! A cell's identity hash is its compressed address at the time it is first hashed. Two bits of the header byte that
//! LibGC leaves free tell where the hash is: in an unhashed cell nowhere yet, in a hashed cell in the cell's address, and
//! in a cell that was hashed and then moved in the side table of its heap. Hashing an unhashed cell sets "hashed", so
//! that the collector knows to keep its hash when it moves the cell: under the copy's address in the side table, with
//! "moved" set on the copy. A copy that moves again has its entry rekeyed, and the entries of cells that died are
//! dropped after marking.
//!
//! Every hash of a cell's identity goes through identity_hash(), or through the inline copies of its fast path in C++
//! (GC::identity_hash()) and in LibJS's runtime, which hand the cells that moved to it.

use core::ffi::c_void;
use core::hash::{BuildHasherDefault, Hasher};
use std::collections::HashMap;

use crate::block::{BLOCK_SIZE, descriptor_for};
use crate::marker::COMPRESSED_POINTER_SHIFT;

/// Where a cell keeps its identity hash state, the header byte that LibGC leaves free.
pub const IDENTITY_HASH_STATE_OFFSET: usize = 8;
/// Set once the cell has been hashed, so its identity hash must survive moves.
pub const IDENTITY_HASH_HASHED: u8 = 1 << 0;
/// Set on a hashed cell that moved since it was hashed, whose hash is then in the side table of its heap.
pub const IDENTITY_HASH_MOVED: u8 = 1 << 1;

/// A cell's compressed address, which keys the side table.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct CompressedAddress(pub u32);

/// An identity hash.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct IdentityHash(pub u32);

/// OPTIMIZATION: The keys are compressed addresses, whose low bits differ the most, so a multiplication spreads them
///               well enough for the table.
#[derive(Default)]
struct CompressedAddressHasher(u64);

impl Hasher for CompressedAddressHasher {
    fn finish(&self) -> u64 {
        self.0
    }

    fn write(&mut self, bytes: &[u8]) {
        for &byte in bytes {
            self.0 = (self.0 ^ u64::from(byte)).wrapping_mul(0x9e37_79b9_7f4a_7c15);
        }
    }

    fn write_u32(&mut self, value: u32) {
        self.0 = u64::from(value).wrapping_mul(0x9e37_79b9_7f4a_7c15).rotate_left(32);
    }
}

/// The side table of a heap: the identity hashes of the hashed cells that moved, by their current address.
#[derive(Default)]
pub struct IdentityHashes {
    hashes: HashMap<CompressedAddress, IdentityHash, BuildHasherDefault<CompressedAddressHasher>>,
}

impl IdentityHashes {
    pub fn len(&self) -> usize {
        self.hashes.len()
    }

    pub fn is_empty(&self) -> bool {
        self.hashes.is_empty()
    }

    /// Notes that a cell with identity hash state `state` moved from `from` to `to`, and returns the state of the copy.
    pub fn did_move(&mut self, state: u8, from: CompressedAddress, to: CompressedAddress) -> u8 {
        if state & IDENTITY_HASH_HASHED == 0 {
            return state;
        }
        let hash = if state & IDENTITY_HASH_MOVED != 0 {
            self.hashes
                .remove(&from)
                .expect("a cell that moved since it was hashed has an entry")
        } else {
            IdentityHash(from.0)
        };
        self.hashes.insert(to, hash);
        state | IDENTITY_HASH_MOVED
    }

    /// Drops the entries of the cells that `is_live` says died.
    pub fn retain_live(&mut self, mut is_live: impl FnMut(CompressedAddress) -> bool) {
        self.hashes.retain(|&address, _| is_live(address));
    }

    fn get(&self, address: CompressedAddress) -> IdentityHash {
        *self
            .hashes
            .get(&address)
            .expect("a cell that moved since it was hashed has an entry")
    }
}

unsafe extern "C" {
    /// Where a LibGC heap keeps its side table, which CAPI.cpp defines.
    fn gc_heap_identity_hashes(heap: *mut c_void) -> *mut *mut c_void;
}

/// The side table of `heap`, which this creates if it has none yet.
///
/// # Safety
///
/// `heap` must be a live LibGC heap, used on this thread.
pub unsafe fn identity_hashes_of<'a>(heap: *mut c_void) -> &'a mut IdentityHashes {
    // SAFETY: The caller passes a live heap, whose table only its thread uses.
    unsafe {
        let slot = gc_heap_identity_hashes(heap).cast::<*mut IdentityHashes>();
        if (*slot).is_null() {
            *slot = Box::into_raw(Box::default());
        }
        &mut **slot
    }
}

/// Destroys a side table that rust_gc_identity_hash() or the mover created.
///
/// # Safety
///
/// `table` must be null or a table that nothing uses any more.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_identity_hashes_destroy(table: *mut IdentityHashes) {
    if !table.is_null() {
        // SAFETY: The caller passes a table this crate created and nothing uses.
        drop(unsafe { Box::from_raw(table) });
    }
}

/// The identity hash of the cell at `cell`.
///
/// # Safety
///
/// `cell` must be a live cell, of a heap that this thread uses.
#[inline]
pub unsafe fn identity_hash(heap_region_base: usize, cell: usize) -> IdentityHash {
    let state = (cell + IDENTITY_HASH_STATE_OFFSET) as *mut u8;
    let compressed = CompressedAddress((cell >> COMPRESSED_POINTER_SHIFT) as u32);
    // SAFETY: The caller passes a live cell, whose header has the state byte, and which only this thread touches.
    unsafe {
        let value = state.read();
        if value & IDENTITY_HASH_MOVED == 0 {
            if value & IDENTITY_HASH_HASHED == 0 {
                state.write(value | IDENTITY_HASH_HASHED);
            }
            return IdentityHash(compressed.0);
        }
        let heap = descriptor_for(heap_region_base, cell).heap();
        identity_hashes_of(heap).get(compressed)
    }
}

/// identity_hash(), for C++ and for LibJS's runtime, whose inline fast paths hand it the cells that moved.
///
/// # Safety
///
/// As for identity_hash().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_cell_identity_hash(heap_region_base: usize, cell: *const c_void) -> u32 {
    debug_assert!(
        cell.addr() & (BLOCK_SIZE - 1) != 0,
        "a cell is never at the start of its block"
    );
    // SAFETY: The caller passes a live cell.
    unsafe { identity_hash(heap_region_base, cell.addr()) }.0
}

/// Drops the entries of the cells that the collection that just marked found dead, from a heap's side table.
///
/// # Safety
///
/// `table` must be null or the side table of the heap that just marked, while its marks are valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_identity_hashes_drop_dead(table: *mut IdentityHashes, heap_region_base: usize) {
    // SAFETY: The caller passes null or a live table.
    let Some(table) = (unsafe { table.as_mut() }) else {
        return;
    };
    table.retain_live(|address| {
        let cell = heap_region_base + ((address.0 as usize) << COMPRESSED_POINTER_SHIFT);
        // SAFETY: Every entry is of a cell of the heap that marked, whose block is live until the cell is swept.
        unsafe { descriptor_for(heap_region_base, cell) }.is_marked(cell)
    });
}
