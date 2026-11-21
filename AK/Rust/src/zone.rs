/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Memory for the data of one job, which all goes away at one time.
//!
//! A `Zone` is one range of address space that comes straight from the system. It gives out memory from its start to
//! its end and takes none of it back one piece at a time: `reset()` drops all the data in one step, and the memory
//! serves the next job. So a job that makes many lists and tables makes no block of a heap, the data of its steps is
//! side by side in memory, and all the memory can go back to the system at any time between two jobs (see `trim()`).
//!
//! A `ZoneVec` is a list in a zone that grows like a `Vec`. The data in a zone is plain: nothing in a zone is dropped,
//! so a zone and its lists do not hold types that have to be dropped (see `PLAIN`).

use core::cell::{Cell, RefCell};
use core::ffi::CStr;
use core::fmt;
use core::hash::{Hash, Hasher};
use core::marker::PhantomData;
use core::mem::{MaybeUninit, needs_drop};
use core::ops::{Deref, DerefMut};
use core::ptr::NonNull;

use crate::region::Region;

/// Memory for the data of one job. What it gives out lives until the zone is reset, which needs the zone to itself.
pub struct Zone {
    region: RefCell<Region>,
    /// Where the memory of the region starts, once it has some.
    base: Cell<NonNull<u8>>,
    /// How many bytes from the start of the region the zone can give out without growing: those of the region that
    /// are memory, but no more than the limit.
    usable: Cell<usize>,
    /// How many bytes the zone gives out at most. The region may have more, as it is a whole number of pages.
    limit: usize,
    /// How many bytes are given out. They are the first ones of the region.
    used: Cell<usize>,
}

/// Fails to compile for a type that has to be dropped. Nothing in a zone is dropped.
struct Plain<T>(PhantomData<T>);

impl<T> Plain<T> {
    const CHECK: () = assert!(
        !needs_drop::<T>() && size_of::<T>() != 0 && align_of::<T>() <= 4096,
        "a zone holds plain data only: types of a size, that nothing has to drop"
    );
}

// NB: Each call gives out other memory of the zone, so what the calls return can change at the same time.
#[allow(clippy::mut_from_ref)]
impl Zone {
    /// The memory a zone takes at least, and at most, when it has to grow.
    const MIN_GROWTH: usize = 64 * 1024;
    const MAX_GROWTH: usize = 16 * 1024 * 1024;

    /// A zone that gives out up to `limit` bytes. It takes no memory and no address space before it gives some out.
    /// `name` shows up for its memory in /proc/<pid>/smaps on Linux.
    ///
    /// The limit is not a budget: a zone that is asked for more panics. The owner of a zone makes the limit far
    /// above what its jobs may use, and stops a job that uses too much (see `used()`).
    pub fn new(limit: usize, name: &'static CStr) -> Self {
        let region = Region::new(limit, name);
        Self {
            base: Cell::new(region.base()),
            region: RefCell::new(region),
            usable: Cell::new(0),
            limit,
            used: Cell::new(0),
        }
    }

    /// How many bytes the zone has given out since it was last reset.
    pub fn used(&self) -> usize {
        self.used.get()
    }

    /// How many bytes of memory the zone has taken from the system.
    pub fn memory(&self) -> usize {
        self.region.borrow().committed()
    }

    /// Drops all the data in the zone. The zone keeps its memory.
    pub fn reset(&mut self) {
        self.used.set(0);
    }

    /// Gives memory back to the system: all that is past the data and past the first `keep` bytes.
    pub fn trim(&mut self, keep: usize) {
        let region = self.region.get_mut();
        region.decommit_past(self.used.get().max(keep.min(region.reserved())));
        self.usable.set(region.committed().min(self.limit));
    }

    /// Runs `job` with the zone and then drops all the data that `job` put in it. The zone must be empty of borrows,
    /// as for `reset()`, so this is for a zone that holds only the data of one step at a time.
    pub fn scope<R>(&mut self, job: impl for<'zone> FnOnce(&'zone Zone) -> R) -> R {
        let mark = self.used.get();
        let result = job(self);
        // NB: `job` cannot return what borrows from the zone, so nothing refers to the data past the mark.
        self.used.set(mark);
        result
    }

    /// Takes memory until at least the first `size` bytes of the region are memory.
    #[cold]
    fn grow(&self, size: usize) {
        let mut region = self.region.borrow_mut();
        assert!(size <= self.limit, "a zone gives out no more than {} bytes", self.limit);
        let step = region.committed().clamp(Self::MIN_GROWTH, Self::MAX_GROWTH);
        // NB: Memory that is taken and never written costs nothing on Unix, where a page becomes real when it is
        //     first written, so the steps can be large.
        let size = size.max(region.committed() + step).min(region.reserved());
        region.commit(size);
        self.base.set(region.base());
        self.usable.set(region.committed().min(self.limit));
    }

    /// Gives out `size` bytes at a multiple of `align`, which is a power of two and at most 4096.
    #[inline]
    fn take(&self, size: usize, align: usize) -> NonNull<u8> {
        // NB: The region starts at a multiple of 4096, so an offset that is aligned is an address that is aligned.
        let start = self.used.get().next_multiple_of(align);
        let end = start
            .checked_add(size)
            .expect("the data of a zone fits in the address space");
        if end > self.usable.get() {
            self.grow(end);
        }
        self.used.set(end);
        // SAFETY: `start` is at most `usable`, so the address is in the region or one past its memory.
        unsafe { self.base.get().add(start) }
    }

    /// Makes the slice of `len` values at `first` have room for `new_len` values, which is more, where it is. That
    /// works if the slice is the last thing that the zone gave out. What the slice holds stays.
    ///
    /// # Safety
    ///
    /// The zone must have given out the `len` values at `first` as one slice, and nothing but `first` may refer to
    /// them. If this returns a slice, `first` must not be used again.
    unsafe fn grow_in_place<T>(
        &self,
        first: NonNull<MaybeUninit<T>>,
        len: usize,
        new_len: usize,
    ) -> Option<&mut [MaybeUninit<T>]> {
        if len == 0 {
            return None;
        }
        // NB: The slice is memory of the region, so it starts at or after the base.
        let start = first.addr().get() - self.base.get().addr().get();
        if start + len * size_of::<T>() != self.used.get() {
            return None;
        }
        let end = new_len
            .checked_mul(size_of::<T>())
            .and_then(|size| start.checked_add(size))
            .expect("the data of a zone fits in the address space");
        if end > self.usable.get() {
            self.grow(end);
        }
        self.used.set(end);
        // SAFETY: The bytes from `start` to `end` are memory of the region: the slice of the caller, which starts at
        //         an address aligned for `T`, and the bytes after it, which the zone gives out here and to nothing
        //         else before it is reset. The caller has the only pointer to the slice and does not use it again,
        //         and the new slice is made from the address of the region, so it is the only way to these bytes.
        //         The first of them hold what the slice of the caller held.
        Some(unsafe {
            crate::slice_from_raw_parts_mut(self.base.get().add(start).cast::<MaybeUninit<T>>().as_ptr(), new_len)
        })
    }

    /// Room for `len` values of type `T`, with nothing written yet.
    #[inline]
    pub fn alloc_uninit_slice<T>(&self, len: usize) -> &mut [MaybeUninit<T>] {
        let () = Plain::<T>::CHECK;
        // NB: An empty slice takes no room, so it does not move or align where the next data goes, which could go
        //     past the limit.
        if len == 0 {
            return &mut [];
        }
        let size = len
            .checked_mul(size_of::<T>())
            .expect("the data of a zone fits in the address space");
        let first = self.take(size, align_of::<T>()).cast::<MaybeUninit<T>>();
        // SAFETY: take() gave out `size` bytes of the memory of the region at an address aligned for `T`, which it
        //         gives out to nothing else before the zone is reset, and a reset needs the zone to itself, so
        //         nothing else refers to them for as long as the zone is borrowed. The region keeps its memory where
        //         it is for as long as the zone lives. A `MaybeUninit` may hold any bytes.
        unsafe { crate::slice_from_raw_parts_mut(first.as_ptr(), len) }
    }

    /// `len` copies of `value`.
    pub fn alloc_slice_fill<T: Copy>(&self, len: usize, value: T) -> &mut [T] {
        let slice = self.alloc_uninit_slice(len);
        for place in slice.iter_mut() {
            place.write(value);
        }
        // SAFETY: Each element was written above.
        unsafe { slice.assume_init_mut() }
    }

    /// A copy of `values`.
    pub fn alloc_slice_copy<T: Copy>(&self, values: &[T]) -> &mut [T] {
        self.alloc_uninit_slice(values.len()).write_copy_of_slice(values)
    }

    /// The values that `values` gives, in order.
    pub fn alloc_slice_from_iter<T>(&self, values: impl IntoIterator<Item = T>) -> &mut [T] {
        ZoneVec::from_iter_in(values, self).into_slice()
    }

    /// `value`, in the zone.
    pub fn alloc<T>(&self, value: T) -> &mut T {
        let [place] = self.alloc_uninit_slice(1) else {
            unreachable!("the slice has one element");
        };
        place.write(value)
    }
}

// SAFETY: A zone owns its mapping, and no thread has a special tie to a mapping. What a zone gave out borrows the
//         zone, so a zone that moves to another thread has given out nothing that still lives.
unsafe impl Send for Zone {}

impl fmt::Debug for Zone {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "Zone({} bytes)", self.used())
    }
}

/// A list of plain values in a zone, which grows like a `Vec`.
///
/// When it has no room for one more element and is not the last thing the zone gave out, it moves to new memory of
/// the zone. The memory it had stays unused until the zone is reset. So a list that is made anew in each round of a
/// loop fills the zone: make such a list one time and clear it in each round.
pub struct ZoneVec<'zone, T: 'zone> {
    zone: &'zone Zone,
    /// The memory for the elements: room for `capacity` of them, which the zone gave out to this list and to nothing
    /// else, and of which the first `len` are written. A list with no capacity has no memory there.
    first: NonNull<T>,
    len: u32,
    capacity: u32,
    /// The list owns its elements.
    elements: PhantomData<T>,
}

impl<'zone, T> ZoneVec<'zone, T> {
    /// A list with no elements, which takes no memory before it has one.
    #[inline]
    pub fn new_in(zone: &'zone Zone) -> Self {
        let () = Plain::<T>::CHECK;
        Self {
            zone,
            first: NonNull::dangling(),
            len: 0,
            capacity: 0,
            elements: PhantomData,
        }
    }

    /// A list with no elements and room for `capacity` of them.
    #[inline]
    pub fn with_capacity_in(capacity: usize, zone: &'zone Zone) -> Self {
        let mut list = Self::new_in(zone);
        if capacity != 0 {
            list.set_buffer(zone.alloc_uninit_slice(capacity));
        }
        list
    }

    /// A list of the values that `values` gives, in order.
    pub fn from_iter_in(values: impl IntoIterator<Item = T>, zone: &'zone Zone) -> Self {
        let values = values.into_iter();
        let mut list = Self::with_capacity_in(values.size_hint().0, zone);
        list.extend(values);
        list
    }

    /// A list that is a copy of `values`.
    pub fn from_slice_in(values: &[T], zone: &'zone Zone) -> Self
    where
        T: Clone,
    {
        let mut list = Self::with_capacity_in(values.len(), zone);
        list.extend_from_slice(values);
        list
    }

    /// A list of `len` copies of `value`.
    pub fn from_elem_in(value: T, len: usize, zone: &'zone Zone) -> Self
    where
        T: Clone,
    {
        let mut list = Self::with_capacity_in(len, zone);
        list.resize(len, value);
        list
    }

    /// The zone that the list is in.
    pub fn zone(&self) -> &'zone Zone {
        self.zone
    }

    /// Takes the list out of its place, which gets a list with no elements in the same zone.
    pub fn take(&mut self) -> Self {
        core::mem::replace(self, Self::new_in(self.zone))
    }

    /// Makes the list a copy of `values`, in the memory it has if they fit.
    pub fn assign(&mut self, values: &[T])
    where
        T: Clone,
    {
        self.clear();
        self.extend_from_slice(values);
    }

    pub fn len(&self) -> usize {
        self.len as usize
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn capacity(&self) -> usize {
        self.capacity as usize
    }

    pub fn as_slice(&self) -> &[T] {
        self
    }

    pub fn as_mut_slice(&mut self) -> &mut [T] {
        self
    }

    /// The elements, for as long as the zone holds them. The list ends here.
    pub fn into_slice(self) -> &'zone mut [T] {
        // SAFETY: The first `len` elements are written, in memory that the zone keeps where it is and gives to nothing
        //         else for `'zone`. The list ends here, so the slice is the only way to them.
        unsafe { crate::slice_from_raw_parts_mut(self.first.as_ptr(), self.len()) }
    }

    /// Makes `buffer`, which the zone gave out, the memory of the list. The list has no elements in other memory.
    fn set_buffer(&mut self, buffer: &'zone mut [MaybeUninit<T>]) {
        self.capacity = u32::try_from(buffer.len()).expect("a list of a zone has room for fewer than 2^32 elements");
        self.first = NonNull::from(buffer).cast();
    }

    /// Makes room for `additional` more elements.
    #[inline]
    pub fn reserve(&mut self, additional: usize) {
        if additional > self.capacity() - self.len() {
            self.grow(additional);
        }
    }

    #[cold]
    #[inline(never)]
    fn grow(&mut self, additional: usize) {
        let needed = self
            .len()
            .checked_add(additional)
            .filter(|needed| *needed <= u32::MAX as usize)
            .expect("a list of a zone holds fewer than 2^32 elements");
        let capacity = needed.max(self.capacity() * 2).clamp(4, u32::MAX as usize);
        let first = self.first.cast::<MaybeUninit<T>>();
        // NB: A list that is the last thing the zone gave out grows where it is.
        // SAFETY: A list with capacity has its memory from the zone as one slice (see set_buffer()), and only the
        //         list refers to it. When a slice comes back, it takes the place of `first`.
        if let Some(buffer) = unsafe { self.zone.grow_in_place(first, self.capacity(), capacity) } {
            self.set_buffer(buffer);
            return;
        }
        let buffer = self.zone.alloc_uninit_slice::<T>(capacity);
        // SAFETY: The old memory holds `len` elements, the new memory has room for them, and they are different
        //         memory of the zone. The elements move: the old memory is not read again.
        unsafe { core::ptr::copy_nonoverlapping(first.as_ptr(), buffer.as_mut_ptr(), self.len()) };
        self.set_buffer(buffer);
    }

    #[inline]
    pub fn push(&mut self, value: T) {
        if self.len == self.capacity {
            self.grow(1);
        }
        // SAFETY: `capacity` is above `len`, so the place is in the memory of the list.
        unsafe { self.first.add(self.len()).write(value) };
        self.len += 1;
    }

    /// The memory of the list, which a caller may write elements to past the length before it sets the length (see
    /// `set_len()`).
    #[inline(always)]
    pub fn as_mut_ptr(&mut self) -> *mut T {
        self.first.as_ptr()
    }

    /// Makes the list `len` elements long, with what its memory holds.
    ///
    /// # Safety
    ///
    /// `len` must be at most the capacity, and the first `len` elements must be written.
    #[inline(always)]
    pub unsafe fn set_len(&mut self, len: usize) {
        debug_assert!(len <= self.capacity(), "a list is no longer than its capacity");
        self.len = len as u32;
    }

    pub fn pop(&mut self) -> Option<T> {
        if self.len == 0 {
            return None;
        }
        self.len -= 1;
        // SAFETY: The element was written, and `len` now leaves it out, so it is not read again.
        Some(unsafe { self.first.add(self.len()).read() })
    }

    /// Drops the elements from index `len` on. The list keeps its memory.
    pub fn truncate(&mut self, len: usize) {
        // NB: The elements need no drop.
        if len < self.len() {
            self.len = len as u32;
        }
    }

    /// Drops all elements. The list keeps its memory.
    pub fn clear(&mut self) {
        self.len = 0;
    }

    /// Puts `value` at `index` and moves the elements from there on up by one place.
    pub fn insert(&mut self, index: usize, value: T) {
        assert!(index <= self.len(), "the index of an insert is at most the length");
        if self.len == self.capacity {
            self.grow(1);
        }
        // SAFETY: The list has room for one more element. The elements from `index` on move up by one place, inside
        //         the memory of the list, and `value` goes to the place that they leave.
        unsafe {
            let place = self.first.add(index);
            core::ptr::copy(place.as_ptr(), place.as_ptr().add(1), self.len() - index);
            place.write(value);
        }
        self.len += 1;
    }

    /// Takes the element at `index` out and moves the elements after it down by one place.
    pub fn remove(&mut self, index: usize) -> T {
        assert!(index < self.len(), "the index of a removal is below the length");
        self.len -= 1;
        // SAFETY: The element at `index` was written. The elements after it move down over it, inside the memory of
        //         the list, and `len` now leaves the last place out.
        unsafe {
            let place = self.first.add(index);
            let value = place.read();
            core::ptr::copy(place.as_ptr().add(1), place.as_ptr(), self.len() - index);
            value
        }
    }

    /// Takes the element at `index` out and puts the last element in its place.
    pub fn swap_remove(&mut self, index: usize) -> T {
        assert!(index < self.len(), "the index of a removal is below the length");
        let last = self.len() - 1;
        self.swap(index, last);
        self.pop().expect("the list has an element")
    }

    /// Keeps the elements for which `keep` is true, in their order.
    pub fn retain(&mut self, mut keep: impl FnMut(&T) -> bool) {
        self.retain_mut(|value| keep(value));
    }

    /// Keeps the elements for which `keep` is true, in their order.
    pub fn retain_mut(&mut self, mut keep: impl FnMut(&mut T) -> bool) {
        let mut kept = 0;
        for index in 0..self.len() {
            if keep(&mut self[index]) {
                // NB: A swap and not a copy, so that each element is in the list one time even if `keep` panics.
                self.swap(index, kept);
                kept += 1;
            }
        }
        self.len = kept as u32;
    }

    /// Removes each element that is equal to the one before it.
    pub fn dedup(&mut self)
    where
        T: PartialEq,
    {
        let mut kept = 0;
        for index in 0..self.len() {
            if kept == 0 || self[index] != self[kept - 1] {
                // NB: As in retain_mut().
                self.swap(index, kept);
                kept += 1;
            }
        }
        self.len = kept as u32;
    }

    /// Makes the list `len` elements long: drops the elements from index `len` on, or adds copies of `value`.
    pub fn resize(&mut self, len: usize, value: T)
    where
        T: Clone,
    {
        if len <= self.len() {
            self.len = len as u32;
            return;
        }
        self.reserve(len - self.len());
        for index in self.len()..len {
            // SAFETY: reserve() made room for `len` elements, and the place is below that.
            unsafe { self.first.add(index).write(value.clone()) };
        }
        // NB: A clone that panics leaves the list as it was: the values written so far are plain, so nothing has to
        //     drop them.
        self.len = len as u32;
    }

    #[inline]
    pub fn extend_from_slice(&mut self, values: &[T])
    where
        T: Clone,
    {
        self.reserve(values.len());
        // SAFETY: reserve() made room for all of `values` after the elements of the list. That room is memory that
        //         the zone gave to this list, which nothing else refers to, and a `MaybeUninit` may hold any bytes.
        let places = unsafe {
            crate::slice_from_raw_parts_mut(
                self.first.add(self.len()).cast::<MaybeUninit<T>>().as_ptr(),
                values.len(),
            )
        };
        // NB: The length changes one time, after the copy, so that the copy of plain values is one block copy.
        places.write_clone_of_slice(values);
        self.len += values.len() as u32;
    }

    /// Moves all the elements of `other` to the end of the list.
    pub fn append(&mut self, other: &mut ZoneVec<'_, T>) {
        self.reserve(other.len());
        // SAFETY: The list has room for the elements of `other`, which is another list with its own memory. The
        //         elements move: `other` has none of them after this.
        unsafe {
            core::ptr::copy_nonoverlapping(other.first.as_ptr(), self.first.add(self.len()).as_ptr(), other.len());
        }
        self.len += other.len;
        other.len = 0;
    }

    /// Takes the elements from index `at` on out of the list, as a new list.
    pub fn split_off(&mut self, at: usize) -> Self {
        assert!(at <= self.len(), "the index of a split is at most the length");
        let mut tail = Self::with_capacity_in(self.len() - at, self.zone);
        // SAFETY: `tail` has room for the elements from `at` on, in other memory of the zone. The elements move:
        //         `len` leaves them out after this.
        unsafe { core::ptr::copy_nonoverlapping(self.first.add(at).as_ptr(), tail.first.as_ptr(), self.len() - at) };
        tail.len = self.len - at as u32;
        self.len = at as u32;
        tail
    }

    /// Takes all the elements out of the list, in order. The list keeps its memory.
    pub fn drain_all(&mut self) -> impl Iterator<Item = T> + '_ {
        let len = core::mem::take(&mut self.len) as usize;
        let first = self.first;
        (0..len).map(move |index| {
            // SAFETY: The first `len` elements were written, and the list has no elements now, so each is read one
            //         time. The list is borrowed for as long as the iterator lives, so nothing writes over them.
            unsafe { first.add(index).read() }
        })
    }
}

impl<T> Deref for ZoneVec<'_, T> {
    type Target = [T];

    fn deref(&self) -> &[T] {
        // SAFETY: The first `len` elements are written, in memory that the zone keeps for as long as the list lives,
        //         and that only the list refers to.
        unsafe { crate::slice_from_raw_parts(self.first.as_ptr(), self.len()) }
    }
}

impl<T> DerefMut for ZoneVec<'_, T> {
    fn deref_mut(&mut self) -> &mut [T] {
        // SAFETY: As in deref(), and the mutable borrow of the list is the only access to them.
        unsafe { crate::slice_from_raw_parts_mut(self.first.as_ptr(), self.len()) }
    }
}

impl<T> Extend<T> for ZoneVec<'_, T> {
    fn extend<I: IntoIterator<Item = T>>(&mut self, values: I) {
        let values = values.into_iter();
        self.reserve(values.size_hint().0);
        for value in values {
            self.push(value);
        }
    }
}

impl<'a, T: Copy + 'a> Extend<&'a T> for ZoneVec<'_, T> {
    fn extend<I: IntoIterator<Item = &'a T>>(&mut self, values: I) {
        self.extend(values.into_iter().copied());
    }
}

impl<T: Clone> Clone for ZoneVec<'_, T> {
    fn clone(&self) -> Self {
        Self::from_slice_in(self, self.zone)
    }
}

impl<T: PartialEq> PartialEq for ZoneVec<'_, T> {
    fn eq(&self, other: &Self) -> bool {
        **self == **other
    }
}

impl<T: Eq> Eq for ZoneVec<'_, T> {}

impl<T: PartialEq> PartialEq<[T]> for ZoneVec<'_, T> {
    fn eq(&self, other: &[T]) -> bool {
        **self == *other
    }
}

impl<T: PartialEq> PartialEq<&[T]> for ZoneVec<'_, T> {
    fn eq(&self, other: &&[T]) -> bool {
        **self == **other
    }
}

impl<T: PartialEq> PartialEq<Vec<T>> for ZoneVec<'_, T> {
    fn eq(&self, other: &Vec<T>) -> bool {
        **self == **other
    }
}

impl<T: PartialEq, const N: usize> PartialEq<[T; N]> for ZoneVec<'_, T> {
    fn eq(&self, other: &[T; N]) -> bool {
        **self == *other
    }
}

impl<T: Hash> Hash for ZoneVec<'_, T> {
    fn hash<H: Hasher>(&self, state: &mut H) {
        (**self).hash(state);
    }
}

impl<T: fmt::Debug> fmt::Debug for ZoneVec<'_, T> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.debug_list().entries(self.iter()).finish()
    }
}

impl<'a, T> IntoIterator for &'a ZoneVec<'_, T> {
    type Item = &'a T;
    type IntoIter = core::slice::Iter<'a, T>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<'a, T> IntoIterator for &'a mut ZoneVec<'_, T> {
    type Item = &'a mut T;
    type IntoIter = core::slice::IterMut<'a, T>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter_mut()
    }
}

/// The elements of a list that was taken apart, in order.
pub struct ZoneVecIntoIter<'zone, T> {
    /// The elements that are not given out yet, which are written: those from index `next` to index `end` of the
    /// memory at `first`, without the one at `end`. The zone keeps the memory for `'zone`.
    first: NonNull<T>,
    next: usize,
    end: usize,
    elements: PhantomData<(&'zone Zone, T)>,
}

impl<T> Iterator for ZoneVecIntoIter<'_, T> {
    type Item = T;

    fn next(&mut self) -> Option<T> {
        if self.next == self.end {
            return None;
        }
        self.next += 1;
        // SAFETY: The element was written, and the iterator gives each element out one time.
        Some(unsafe { self.first.add(self.next - 1).read() })
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        (self.end - self.next, Some(self.end - self.next))
    }
}

impl<T> DoubleEndedIterator for ZoneVecIntoIter<'_, T> {
    fn next_back(&mut self) -> Option<T> {
        if self.next == self.end {
            return None;
        }
        self.end -= 1;
        // SAFETY: As in next().
        Some(unsafe { self.first.add(self.end).read() })
    }
}

impl<T> ExactSizeIterator for ZoneVecIntoIter<'_, T> {}

impl<'zone, T> IntoIterator for ZoneVec<'zone, T> {
    type Item = T;
    type IntoIter = ZoneVecIntoIter<'zone, T>;

    fn into_iter(self) -> Self::IntoIter {
        ZoneVecIntoIter {
            first: self.first,
            next: 0,
            end: self.len(),
            elements: PhantomData,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_zone_takes_no_memory_before_it_gives_some_out() {
        let mut zone = Zone::new(1 << 20, c"test");
        assert_eq!(zone.memory(), 0);
        assert_eq!(zone.used(), 0);
        let values = zone.alloc_slice_fill(1000, 7u32);
        assert_eq!(values.len(), 1000);
        assert!(values.iter().all(|value| *value == 7));
        assert_eq!(zone.used(), 4000);
        assert!(zone.memory() >= 4000);
        zone.reset();
        assert_eq!(zone.used(), 0);
        zone.trim(0);
        assert_eq!(zone.memory(), 0);
    }

    #[test]
    fn an_empty_slice_takes_no_room() {
        let zone = Zone::new(4095, c"test");
        zone.alloc_slice_fill(4095, 1u8);
        assert!(zone.alloc_slice_copy::<u64>(&[]).is_empty());
        assert_eq!(zone.used(), 4095);
    }

    #[test]
    fn what_a_zone_gives_out_is_aligned_and_apart() {
        let zone = Zone::new(1 << 20, c"test");
        let byte = zone.alloc(1u8);
        let word = zone.alloc(2u64);
        let bytes = zone.alloc_slice_copy(&[3u8, 4, 5]);
        let wide = zone.alloc(6u128);
        assert_eq!(core::ptr::from_mut(word).addr() % align_of::<u64>(), 0);
        assert_eq!(core::ptr::from_mut(wide).addr() % align_of::<u128>(), 0);
        *byte += 10;
        *word += 10;
        bytes[1] += 10;
        *wide += 10;
        assert_eq!((*byte, *word, &*bytes, *wide), (11, 12, &[3u8, 14, 5][..], 16));
    }

    #[test]
    fn data_stays_in_place_while_a_zone_grows() {
        let zone = Zone::new(1 << 22, c"test");
        let first = zone.alloc_slice_fill(16, 9u64);
        let address = first.as_ptr();
        let mut total = 0;
        // NB: Miri runs this slowly, and has no pages: the zone grows there in the same way with less data.
        let len = if cfg!(miri) { 3_000 } else { 100_000 };
        for round in 0..32 {
            total += zone.alloc_slice_fill(len, round as u8).len();
        }
        assert!(zone.memory() >= total);
        assert_eq!(first.as_ptr(), address);
        assert!(first.iter().all(|value| *value == 9));
    }

    #[test]
    fn a_scope_drops_what_its_job_put_in_the_zone() {
        let mut zone = Zone::new(1 << 20, c"test");
        let sum = zone.scope(|zone| zone.alloc_slice_fill(100, 2u32).iter().sum::<u32>());
        assert_eq!(sum, 200);
        assert_eq!(zone.used(), 0);
        let memory = zone.memory();
        assert_eq!(zone.scope(|zone| zone.alloc_slice_fill(100, 3u32).len()), 100);
        assert_eq!(zone.memory(), memory);
    }

    #[test]
    #[should_panic(expected = "a zone gives out no more than")]
    fn a_zone_panics_at_its_limit() {
        let zone = Zone::new(8192, c"test");
        zone.alloc_slice_fill(8192, 0u8);
        zone.alloc(0u8);
    }

    #[test]
    #[should_panic(expected = "a zone gives out no more than 1 bytes")]
    fn a_zone_gives_out_no_more_than_its_limit_in_a_page() {
        let zone = Zone::new(1, c"test");
        zone.alloc(0u8);
        zone.alloc(0u8);
    }

    #[test]
    fn a_list_grows_like_a_vec() {
        let zone = Zone::new(1 << 20, c"test");
        let mut list = ZoneVec::new_in(&zone);
        assert!(list.is_empty());
        assert_eq!(zone.used(), 0);
        for value in 0..1000u32 {
            list.push(value);
        }
        assert_eq!(list.len(), 1000);
        assert!(list.iter().copied().eq(0..1000));
        assert_eq!(list.pop(), Some(999));
        list.truncate(10);
        list.insert(0, 77);
        list.insert(11, 88);
        assert_eq!(list, [77, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 88]);
        assert_eq!(list.remove(0), 77);
        assert_eq!(list.swap_remove(0), 0);
        assert_eq!(list, [88, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
        list.retain(|value| value % 2 == 0);
        assert_eq!(list, [88, 2, 4, 6, 8]);
        list.extend([8, 8, 1]);
        list.dedup();
        assert_eq!(list, [88, 2, 4, 6, 8, 1]);
        list.resize(8, 5);
        list.extend_from_slice(&[6, 6]);
        assert_eq!(list, [88, 2, 4, 6, 8, 1, 5, 5, 6, 6]);
        let mut tail = list.split_off(8);
        assert_eq!(tail, [6, 6]);
        list.append(&mut tail);
        assert!(tail.is_empty());
        assert_eq!(list.len(), 10);
        assert_eq!(list.drain_all().sum::<u32>(), 131);
        assert!(list.is_empty());
        list.clear();
        assert_eq!(list.pop(), None);
    }

    #[test]
    fn a_list_is_as_small_as_a_vec_and_shortens_its_lifetimes_like_one() {
        fn shorter<'long: 'short, 'short>(list: ZoneVec<'long, &'long u8>) -> ZoneVec<'short, &'short u8> {
            list
        }
        assert_eq!(size_of::<ZoneVec<'_, u8>>(), size_of::<Vec<u8>>());
        let zone = Zone::new(1 << 20, c"test");
        let byte = 5;
        assert_eq!(shorter(ZoneVec::from_slice_in(&[&byte], &zone)), [&5]);
    }

    #[test]
    fn the_last_list_of_a_zone_grows_where_it_is() {
        let zone = Zone::new(1 << 20, c"test");
        let mut list = ZoneVec::with_capacity_in(4, &zone);
        list.extend([1u64, 2, 3, 4]);
        let address = list.as_ptr();
        for value in 5..=1000 {
            list.push(value);
        }
        assert_eq!(list.as_ptr(), address);
        assert_eq!(zone.used(), list.capacity() * 8);
        // Another list takes the place after it, so that it has to move.
        let other = ZoneVec::from_slice_in(&[0u64], &zone);
        list.resize(list.capacity() + 1, 0);
        assert_ne!(list.as_ptr(), address);
        assert!(list.iter().take(1000).copied().eq(1..=1000));
        assert_eq!(other, [0]);
    }

    #[test]
    fn lists_hold_lists_and_copy_themselves() {
        let zone = Zone::new(1 << 20, c"test");
        let mut lists = ZoneVec::new_in(&zone);
        for len in 0..5u8 {
            lists.push(ZoneVec::from_iter_in(0..len, &zone));
        }
        let copy = lists.clone();
        lists[4].push(9);
        assert_eq!(copy[4], [0, 1, 2, 3]);
        assert_eq!(lists[4], [0, 1, 2, 3, 9]);
        assert_ne!(copy, lists);
        let lengths = ZoneVec::from_iter_in(lists.into_iter().map(|list| list.len()), &zone);
        assert_eq!(lengths.into_slice(), [0, 1, 2, 3, 5]);
        assert_eq!(zone.alloc_slice_from_iter((0..3).rev()), [2, 1, 0]);
    }
}
