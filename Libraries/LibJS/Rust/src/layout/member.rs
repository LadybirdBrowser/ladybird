/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compressed cell pointers for the fields of cells. A field holds the cell's offset into the heap region in units of
//! the 8-byte cell alignment, which fits in 32 bits because the region is 32 GiB. Locals, parameters and return values
//! keep using the full-width `Gc`, which `get()` and `set()` convert from and to.
//!
//! The slot is an `UnsafeCell`, so that a collector can rewrite it behind a shared reference, and `set()` goes through
//! a write barrier hook, which does nothing for now.

use core::cell::UnsafeCell;
use core::marker::PhantomData;
use core::ptr::NonNull;

use super::cell::Gc;
use crate::layout_forward::heap_region_base;

/// How far a cell's offset into the heap region is shifted right to compress it: cells are 8-byte aligned.
pub const COMPRESSED_POINTER_SHIFT: u32 = 3;

/// The size of the heap region, which a compressed pointer covers whatever its 32 bits are.
pub const COMPRESSED_POINTER_RANGE: u64 = 1 << (32 + COMPRESSED_POINTER_SHIFT);

/// The compressed form of a pointer into the heap region. The region's base is aligned to its size, so the low 32 bits
/// of the shifted address are the shifted offset, and compressing needs no base.
#[inline(always)]
fn compress_address(address: usize) -> u32 {
    debug_assert!(
        (address.wrapping_sub(heap_region_base()) as u64) < COMPRESSED_POINTER_RANGE,
        "compressed pointers only point into the heap region"
    );
    debug_assert!(address.is_multiple_of(1 << COMPRESSED_POINTER_SHIFT));
    (address >> COMPRESSED_POINTER_SHIFT) as u32
}

/// The address a compressed pointer that is not null names, which is in the heap region whatever the 32 bits are.
///
/// Makes the compressed pointer live up to this point, and the decoded address from this point on. The conservative
/// stack scan recognizes a cell by its address or by its compressed pointer in either half of a word, but not by its
/// unshifted offset, which the compiler could otherwise keep instead while either is held across an allocation.
#[inline(always)]
fn decompress_address(compressed: u32) -> usize {
    let mut address = heap_region_base() + ((compressed as usize) << COMPRESSED_POINTER_SHIFT);
    // SAFETY: The assembly is empty: it only has the compiler put `compressed` and `address` in registers here.
    unsafe {
        core::arch::asm!(
            "/* {compressed} {address} */",
            compressed = in(reg) compressed as usize,
            address = inout(reg) address,
            options(nomem, nostack, preserves_flags),
        );
    }
    address
}

/// Where a collector that has to know about stores into cells, like a generational or incremental one, will hook in.
/// It does nothing for now.
#[inline(always)]
fn write_barrier(_slot: &UnsafeCell<u32>, _compressed: u32) {}

/// Ties a member to its cell type. The compressed pointer is the GC pointer, which the member traces itself, so the
/// marker holds none. Like a `Gc`, a member stays on its thread.
type CellMarker<T> = PhantomData<(fn() -> Gc<T>, *const ())>;

/// A compressed pointer to a cell, as a field of a cell or of storage that a cell traces. It is never null; see
/// `NullableMember` for one that can be.
#[repr(transparent)]
pub struct Member<T> {
    compressed: UnsafeCell<u32>,
    _cell: CellMarker<T>,
}

impl<T> Member<T> {
    #[inline(always)]
    pub fn new(cell: Gc<T>) -> Self {
        Self {
            compressed: UnsafeCell::new(compress_address(cell.as_ptr().addr())),
            _cell: PhantomData,
        }
    }

    #[inline(always)]
    pub fn get(&self) -> Gc<T> {
        let address = decompress_address(self.compressed());
        // SAFETY: The member was set to a cell, which a member keeps alive. A compressed pointer that is not null
        //         decodes to an address in the heap region, which is never null.
        unsafe { Gc::from_non_null(NonNull::new_unchecked(core::ptr::without_provenance_mut(address))) }
    }

    #[inline(always)]
    pub fn set(&self, cell: Gc<T>) {
        let compressed = compress_address(cell.as_ptr().addr());
        write_barrier(&self.compressed, compressed);
        // SAFETY: Cells are only used from one thread, and no reference into the slot outlives a call.
        unsafe { *self.compressed.get() = compressed };
    }

    /// The compressed pointer, for comparing cells without decoding them.
    #[inline(always)]
    pub fn compressed(&self) -> u32 {
        // SAFETY: Cells are only used from one thread, and no reference into the slot outlives a call.
        unsafe { *self.compressed.get() }
    }
}

/// OPTIMIZATION: Two members name the same cell exactly when their compressed pointers are equal, so they compare
///               without decoding.
impl<T> PartialEq for Member<T> {
    fn eq(&self, other: &Self) -> bool {
        self.compressed() == other.compressed()
    }
}

impl<T> Eq for Member<T> {}

impl<T> PartialEq<Gc<T>> for Member<T> {
    fn eq(&self, other: &Gc<T>) -> bool {
        self.get() == *other
    }
}

/// Hashes the decoded address, like `Gc`, so that a member and a `Gc` of the same cell hash the same.
impl<T> core::hash::Hash for Member<T> {
    fn hash<H: core::hash::Hasher>(&self, state: &mut H) {
        self.get().hash(state);
    }
}

impl<T> core::fmt::Debug for Member<T> {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(formatter, "Member({:p})", self.get().as_non_null())
    }
}

/// A compressed pointer to a cell or null, as a field of a cell or of storage that a cell traces. Null is 0, which
/// never names a cell because the bottom of the heap region is never mapped.
///
/// This is its own type rather than an `Option<Member<T>>`, which would take 8 bytes: the `UnsafeCell` hides the niche
/// that 0 would be.
#[repr(transparent)]
pub struct NullableMember<T> {
    compressed: UnsafeCell<u32>,
    _cell: CellMarker<T>,
}

impl<T> NullableMember<T> {
    #[inline(always)]
    pub const fn null() -> Self {
        Self {
            compressed: UnsafeCell::new(0),
            _cell: PhantomData,
        }
    }

    #[inline(always)]
    pub fn new(cell: Option<Gc<T>>) -> Self {
        Self {
            compressed: UnsafeCell::new(Self::compress(cell)),
            _cell: PhantomData,
        }
    }

    #[inline(always)]
    fn compress(cell: Option<Gc<T>>) -> u32 {
        cell.map_or(0, |cell| compress_address(cell.as_ptr().addr()))
    }

    #[inline(always)]
    pub fn get(&self) -> Option<Gc<T>> {
        let compressed = self.compressed();
        if compressed == 0 {
            return None;
        }
        let address = decompress_address(compressed);
        // SAFETY: The member was set to a cell, which a member keeps alive. A compressed pointer that is not null
        //         decodes to an address in the heap region, which is never null.
        Some(unsafe { Gc::from_non_null(NonNull::new_unchecked(core::ptr::without_provenance_mut(address))) })
    }

    /// The cell of a member that is known not to be null, without checking.
    ///
    /// # Safety
    ///
    /// The member must not be null.
    #[inline(always)]
    pub unsafe fn get_unchecked(&self) -> Gc<T> {
        debug_assert!(!self.is_null());
        let address = decompress_address(self.compressed());
        // SAFETY: The member was set to a cell, which a member keeps alive. A compressed pointer that is not null
        //         decodes to an address in the heap region, which is never null.
        unsafe { Gc::from_non_null(NonNull::new_unchecked(core::ptr::without_provenance_mut(address))) }
    }

    #[inline(always)]
    pub fn set(&self, cell: Option<Gc<T>>) {
        let compressed = Self::compress(cell);
        write_barrier(&self.compressed, compressed);
        // SAFETY: Cells are only used from one thread, and no reference into the slot outlives a call.
        unsafe { *self.compressed.get() = compressed };
    }

    #[inline(always)]
    pub fn take(&self) -> Option<Gc<T>> {
        let cell = self.get();
        self.set(None);
        cell
    }

    #[inline(always)]
    pub fn is_null(&self) -> bool {
        self.compressed() == 0
    }

    /// The compressed pointer, 0 for null, for comparing cells without decoding them.
    #[inline(always)]
    pub fn compressed(&self) -> u32 {
        // SAFETY: Cells are only used from one thread, and no reference into the slot outlives a call.
        unsafe { *self.compressed.get() }
    }
}

impl<T> Default for NullableMember<T> {
    fn default() -> Self {
        Self::null()
    }
}

/// OPTIMIZATION: Two members name the same cell, or are both null, exactly when their compressed pointers are equal,
///               so they compare without decoding.
impl<T> PartialEq for NullableMember<T> {
    fn eq(&self, other: &Self) -> bool {
        self.compressed() == other.compressed()
    }
}

impl<T> Eq for NullableMember<T> {}

impl<T> PartialEq<Option<Gc<T>>> for NullableMember<T> {
    fn eq(&self, other: &Option<Gc<T>>) -> bool {
        self.get() == *other
    }
}

/// Hashes like `Option<Gc<T>>`, so that a member and an `Option<Gc<T>>` of the same cell hash the same.
impl<T> core::hash::Hash for NullableMember<T> {
    fn hash<H: core::hash::Hasher>(&self, state: &mut H) {
        self.get().hash(state);
    }
}

impl<T> core::fmt::Debug for NullableMember<T> {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self.get() {
            Some(cell) => write!(formatter, "NullableMember({:p})", cell.as_non_null()),
            None => write!(formatter, "NullableMember(null)"),
        }
    }
}

const _: () = assert!(size_of::<Member<u8>>() == 4);
const _: () = assert!(align_of::<Member<u8>>() == 4);
const _: () = assert!(size_of::<NullableMember<u8>>() == 4);
const _: () = assert!(align_of::<NullableMember<u8>>() == 4);

#[cfg(test)]
mod tests {
    use super::*;

    const HEAP_REGION_BASE: usize = 0x7a00_0000_0000;

    struct FakeCell;

    fn cell_at(offset: usize) -> Gc<FakeCell> {
        let address = HEAP_REGION_BASE + offset;
        // SAFETY: The tests never dereference the pointer.
        unsafe { Gc::from_non_null(NonNull::new(core::ptr::without_provenance_mut(address)).unwrap()) }
    }

    fn hash_of(value: &impl core::hash::Hash) -> u64 {
        use core::hash::Hasher;
        let mut hasher = std::hash::DefaultHasher::new();
        value.hash(&mut hasher);
        hasher.finish()
    }

    #[test]
    fn region_base_is_aligned_to_the_compressed_range() {
        assert_eq!(heap_region_base(), HEAP_REGION_BASE);
        assert!((HEAP_REGION_BASE as u64).is_multiple_of(COMPRESSED_POINTER_RANGE));
    }

    #[test]
    fn round_trips_cells_across_the_region() {
        for offset in [8, 0x1_0000_0000, 0x4_0000_0010, 0x7_ffff_fff8] {
            let cell = cell_at(offset);
            let member = Member::new(cell);
            assert_eq!(member.compressed(), (offset >> COMPRESSED_POINTER_SHIFT) as u32);
            assert_eq!(member.get(), cell);
            let nullable = NullableMember::new(Some(cell));
            assert_eq!(nullable.compressed(), member.compressed());
            assert_eq!(nullable.get(), Some(cell));
        }
    }

    #[test]
    fn every_compressed_value_decodes_into_the_region() {
        for compressed in [0, 1, 0x8000_0000, u32::MAX] {
            let address = decompress_address(compressed);
            assert!(((address - HEAP_REGION_BASE) as u64) < COMPRESSED_POINTER_RANGE);
        }
    }

    #[test]
    fn set_replaces_the_cell() {
        let member = Member::new(cell_at(0x1_0000_0000));
        member.set(cell_at(0x2_0000_0008));
        assert_eq!(member.get(), cell_at(0x2_0000_0008));
    }

    #[test]
    fn null_is_zero() {
        let nullable = NullableMember::<FakeCell>::null();
        assert!(nullable.is_null());
        assert_eq!(nullable.compressed(), 0);
        assert_eq!(nullable.get(), None);
        assert_eq!(NullableMember::<FakeCell>::new(None).compressed(), 0);
        assert!(NullableMember::<FakeCell>::default().is_null());

        nullable.set(Some(cell_at(0x1_0000_0000)));
        assert!(!nullable.is_null());
        assert_eq!(nullable.take(), Some(cell_at(0x1_0000_0000)));
        assert!(nullable.is_null());
    }

    #[test]
    fn equality_and_hash_agree_with_gc() {
        let cell = cell_at(0x1_2345_6788);
        let member = Member::new(cell);
        assert_eq!(member, cell);
        assert_ne!(member, cell_at(0x1_2345_6790));
        assert_eq!(member, Member::new(cell));
        assert_ne!(member, Member::new(cell_at(0x1_2345_6790)));
        assert_eq!(hash_of(&member), hash_of(&cell));

        let nullable = NullableMember::new(Some(cell));
        assert_eq!(nullable, Some(cell));
        assert_ne!(nullable, None);
        assert_eq!(nullable, NullableMember::new(Some(cell)));
        assert_eq!(hash_of(&nullable), hash_of(&Some(cell)));
        assert_eq!(
            hash_of(&NullableMember::<FakeCell>::null()),
            hash_of(&None::<Gc<FakeCell>>)
        );
    }

    #[test]
    fn mixed_lookups_find_the_same_entry() {
        use std::collections::HashSet;
        let cell = cell_at(0x3_0000_0040);
        let set: HashSet<Gc<FakeCell>> = [cell].into_iter().collect();
        let member = Member::new(cell);
        assert!(set.contains(&member.get()));
        assert!(!set.contains(&Member::new(cell_at(0x3_0000_0048)).get()));
        assert_eq!(hash_of(&member), hash_of(set.iter().next().unwrap()));
    }

    #[test]
    fn debug_shows_the_address() {
        let cell = cell_at(0x1_0000_0000);
        assert_eq!(
            format!("{:?}", Member::new(cell)),
            format!("Member({:p})", cell.as_non_null())
        );
        assert_eq!(
            format!("{:?}", NullableMember::<FakeCell>::null()),
            "NullableMember(null)"
        );
    }
}
