/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Columns whose published generations share storage with the column that goes on changing.
//!
//! A [`CowColumn`] keeps its rows in fixed-size chunks behind [`Arc`]s, and its chunks in groups
//! behind [`Arc`]s, under a spine behind an [`Arc`]. A column holds whole chunks, and a row it was
//! never asked to hold reads as the default. Publishing shares the spine, so a [`ColumnSnapshot`]
//! costs one reference count however large the rows are, and so does dropping it. A write copies
//! the spine, the row's group and the row's chunk only where a snapshot still shares them, so the
//! cost of a generation follows the chunks written after it was published rather than the size of
//! the column. Neither is deep-copied: a snapshot's clone shares its spine, and so does a column's
//! clone, its fork. Both are `Send` and `Sync` when the row type is: the column is written through
//! `&mut`, and a snapshot never changes.
//!
//! A column is written only through [`CowColumn::set`] and [`RowMut`], and both compare the row
//! they write with the row a snapshot shares before copying the chunk. So a write that leaves a row
//! as it was never copies a chunk, whichever caller makes it.

use std::ops::{Deref, DerefMut};
use std::sync::Arc;

/// A chunk starts on a cache line of its own, so rows do not share a line with the reference
/// counts or with another chunk.
#[derive(Clone)]
#[repr(align(64))]
struct Chunk<T, const CHUNK: usize>([T; CHUNK]);

/// How many chunks a group holds: a write after a publication copies this many chunk pointers.
const GROUP: usize = 64;

/// The chunks of a group, the column's last group holding fewer than it has room for.
struct Group<T, const CHUNK: usize>([Option<Arc<Chunk<T, CHUNK>>>; GROUP]);

impl<T, const CHUNK: usize> Clone for Group<T, CHUNK> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}

type Spine<T, const CHUNK: usize> = Arc<[Arc<Group<T, CHUNK>>]>;

fn chunk_of<T, const CHUNK: usize>(spine: &Spine<T, CHUNK>, chunk_index: usize) -> Option<&Arc<Chunk<T, CHUNK>>> {
    spine.get(chunk_index / GROUP)?.0[chunk_index % GROUP].as_ref()
}

fn row_of<T, const CHUNK: usize>(spine: &Spine<T, CHUNK>, index: usize) -> Option<&T> {
    Some(&chunk_of(spine, index / CHUNK)?.0[index % CHUNK])
}

/// The chunk at `chunk_index` of `spine`, for the caller to make its own.
fn chunk_slot_mut<T, const CHUNK: usize>(
    spine: &mut Spine<T, CHUNK>,
    chunk_index: usize,
) -> &mut Option<Arc<Chunk<T, CHUNK>>> {
    let group = Arc::make_mut(&mut Arc::make_mut(spine)[chunk_index / GROUP]);
    &mut group.0[chunk_index % GROUP]
}

pub(crate) struct CowColumn<T, const CHUNK: usize> {
    spine: Spine<T, CHUNK>,
    /// The chunk the spine holds at each index, so that reaching a row does not walk the spine
    /// and the chunk's group. The spine keeps each alive, and whatever puts another chunk in a
    /// slot of the spine (`grow_to`, and `owned_row` copying one a snapshot shares) updates it.
    chunks: Vec<*mut Chunk<T, CHUNK>>,
    /// Whether each chunk is known to be unshared, with its group and the spine: the column made
    /// them writable after it last published, and nothing but publishing shares them.
    unshared: Vec<std::sync::atomic::AtomicBool>,
    /// Advanced as the column is written. See [`Self::version`].
    version: u64,
}

// SAFETY: `chunks` points only into what `spine` holds, and the column hands out its rows only
// through `&self` and `&mut self`, as the spine alone would.
unsafe impl<T: Send + Sync, const CHUNK: usize> Send for CowColumn<T, CHUNK> {}
// SAFETY: As above.
unsafe impl<T: Send + Sync, const CHUNK: usize> Sync for CowColumn<T, CHUNK> {}

/// A generation of a [`CowColumn`], as it was when published. It does not see later writes.
pub(crate) struct ColumnSnapshot<T, const CHUNK: usize> {
    spine: Spine<T, CHUNK>,
}

impl<T, const CHUNK: usize> Default for CowColumn<T, CHUNK> {
    fn default() -> Self {
        Self {
            spine: Arc::new([]),
            chunks: Vec::new(),
            unshared: Vec::new(),
            version: 0,
        }
    }
}

impl<T, const CHUNK: usize> Default for ColumnSnapshot<T, CHUNK> {
    fn default() -> Self {
        Self { spine: Arc::new([]) }
    }
}

impl<T, const CHUNK: usize> Clone for ColumnSnapshot<T, CHUNK> {
    fn clone(&self) -> Self {
        Self {
            spine: Arc::clone(&self.spine),
        }
    }
}

/// The bytes of the spine, groups and chunks of `spine` that `sizer` reaches for the first time,
/// with what `row_heap` says each row of such a chunk holds.
fn spine_heap_size<T, const CHUNK: usize>(
    spine: &Spine<T, CHUNK>,
    sizer: &mut crate::heap_size::HeapSizer,
    row_heap: &mut impl FnMut(&T, &mut crate::heap_size::HeapSizer) -> usize,
) -> usize {
    use crate::heap_size::ARC_HEADER_BYTES;
    if !sizer.first_sight(Arc::as_ptr(spine)) {
        return 0;
    }
    let mut bytes = ARC_HEADER_BYTES + size_of_val::<[Arc<Group<T, CHUNK>>]>(spine);
    for group in spine.iter() {
        if !sizer.first_sight(Arc::as_ptr(group)) {
            continue;
        }
        bytes += ARC_HEADER_BYTES + size_of::<Group<T, CHUNK>>();
        for chunk in group.0.iter().flatten() {
            if !sizer.first_sight(Arc::as_ptr(chunk)) {
                continue;
            }
            // NB: The chunk's alignment pads the reference counts in front of it to a cache line.
            bytes += size_of::<Chunk<T, CHUNK>>().max(ARC_HEADER_BYTES)
                + align_of::<Chunk<T, CHUNK>>()
                + chunk.0.iter().map(|row| row_heap(row, sizer)).sum::<usize>();
        }
    }
    bytes
}

impl<T: Clone + Default, const CHUNK: usize> CowColumn<T, CHUNK> {
    /// The bytes the column holds that `sizer` reaches for the first time, with what `row_heap`
    /// says each row holds.
    pub(crate) fn heap_size(
        &self,
        sizer: &mut crate::heap_size::HeapSizer,
        mut row_heap: impl FnMut(&T, &mut crate::heap_size::HeapSizer) -> usize,
    ) -> usize {
        crate::heap_size::vec_bytes(&self.chunks)
            + crate::heap_size::vec_bytes(&self.unshared)
            + spine_heap_size(&self.spine, sizer, &mut row_heap)
    }

    const CHUNK_IS_NOT_EMPTY: () = assert!(CHUNK > 0);

    #[inline]
    pub(crate) fn get(&self, index: usize) -> Option<&T> {
        let chunk = *self.chunks.get(index / CHUNK)?;
        // SAFETY: The spine holds the chunk, and nothing writes it while `&self` is borrowed.
        Some(unsafe { &(*chunk).0[index % CHUNK] })
    }

    /// The row at `index` in a chunk made the column's own, copying the spine, the chunk's group
    /// and the chunk where a snapshot shares them.
    fn owned_row(&mut self, index: usize) -> Option<&mut T> {
        let chunk_index = index / CHUNK;
        let unshared = self.unshared.get_mut(chunk_index)?.get_mut();
        self.version += 1;
        if !*unshared {
            let chunk = chunk_slot_mut(&mut self.spine, chunk_index).as_mut()?;
            Arc::make_mut(chunk);
            self.chunks[chunk_index] = Arc::as_ptr(chunk).cast_mut();
            *unshared = true;
        }
        // SAFETY: The chunk, its group and the spine have not been shared since `make_mut` above
        // made them unique: only `publish` shares them, and it forgets which chunks are unshared.
        // `&mut self` keeps any other reference into the column from being live.
        Some(unsafe { &mut (*self.chunks[chunk_index]).0[index % CHUNK] })
    }

    /// Whether the row at `index` is in a chunk no snapshot shares, marking the chunk so if it is.
    fn owns_chunk_of(&mut self, index: usize) -> bool {
        let chunk_index = index / CHUNK;
        if *self.unshared[chunk_index].get_mut() {
            return true;
        }
        let owned = Arc::get_mut(&mut self.spine)
            .and_then(|spine| Arc::get_mut(&mut spine[chunk_index / GROUP]))
            .and_then(|group| Arc::get_mut(group.0[chunk_index % GROUP].as_mut()?))
            .is_some();
        if owned {
            *self.unshared[chunk_index].get_mut() = true;
            self.version += 1;
        }
        owned
    }

    /// Grows the column to hold at least `len` rows, the new ones default. A column never
    /// shrinks: rows are reset in place instead.
    pub(crate) fn grow_to(&mut self, len: usize) {
        let () = Self::CHUNK_IS_NOT_EMPTY;
        while self.unshared.len() * CHUNK < len {
            let chunk_index = self.unshared.len();
            if chunk_index.is_multiple_of(GROUP) {
                let group = Arc::new(Group(std::array::from_fn(|_| None)));
                self.spine = self.spine.iter().cloned().chain([group]).collect();
            }
            let chunk = Arc::new(Chunk(std::array::from_fn(|_| T::default())));
            self.chunks.push(Arc::as_ptr(&chunk).cast_mut());
            *chunk_slot_mut(&mut self.spine, chunk_index) = Some(chunk);
            self.unshared.push(std::sync::atomic::AtomicBool::new(true));
            self.version += 1;
        }
    }

    /// How many rows the column holds: whole chunks, the rows it was never asked for reading as the default.
    pub(crate) fn len(&self) -> usize {
        self.chunks.len() * CHUNK
    }

    /// How far the column has been written. A snapshot reads as the column does for as long as the version stays what
    /// it was when the snapshot was published, whoever published since: every write advances it, except one to a chunk
    /// the column already made its own after it last published, whose first write did.
    pub(crate) fn version(&self) -> u64 {
        self.version
    }

    /// Forgets which chunks the column owns, once something else shares them.
    fn forget_unshared(&self) {
        for unshared in &self.unshared {
            unshared.store(false, std::sync::atomic::Ordering::Relaxed);
        }
    }

    /// This generation of the column, sharing all of it with the column until the column writes.
    pub(crate) fn publish(&mut self) -> ColumnSnapshot<T, CHUNK> {
        self.forget_unshared();
        ColumnSnapshot {
            spine: Arc::clone(&self.spine),
        }
    }
}

/// A fork of a column shares all of it with the column until either writes, as a snapshot does: it costs one
/// reference count, and each side copies a chunk the other shares the first time it writes there.
impl<T: Clone + Default, const CHUNK: usize> Clone for CowColumn<T, CHUNK> {
    fn clone(&self) -> Self {
        self.forget_unshared();
        Self {
            spine: Arc::clone(&self.spine),
            chunks: self.chunks.clone(),
            unshared: self
                .unshared
                .iter()
                .map(|_| std::sync::atomic::AtomicBool::new(false))
                .collect(),
            version: self.version,
        }
    }
}

impl<T: Clone + Default + PartialEq, const CHUNK: usize> CowColumn<T, CHUNK> {
    /// Sets the row at `index`: in place in a chunk the column owns, and in a chunk a snapshot
    /// shares only if the row changes, copying the chunk then. `None` if the column does not hold
    /// the row.
    pub(crate) fn set(&mut self, index: usize, value: T) -> Option<()> {
        self.get(index)?;
        if self.owns_chunk_of(index) || *self.get(index)? != value {
            *self.owned_row(index)? = value;
        }
        Some(())
    }

    /// Sets the row at `index` as [`Self::set`] does, answering what the row held. `None` if the column does not hold
    /// the row.
    pub(crate) fn replace(&mut self, index: usize, value: T) -> Option<T> {
        self.get(index)?;
        if self.owns_chunk_of(index) || *self.get(index)? != value {
            return Some(std::mem::replace(self.owned_row(index)?, value));
        }
        Some(value)
    }

    /// The row at `index`, for writing. See [`RowMut`].
    pub(crate) fn row_mut(&mut self, index: usize) -> Option<RowMut<&mut Self, T, CHUNK>> {
        RowMut::new(self, index)
    }
}

/// A row of a [`CowColumn`], for writing. While a snapshot shares the row's chunk, the writes go
/// to a copy of the row, and the row is set from it when the guard drops, copying the chunk only if
/// the row changed. In a chunk the column owns, they go to the row in place. `C` is how the guard
/// holds the column: `&mut` it, or a `RefMut` of it.
pub(crate) struct RowMut<C, T, const CHUNK: usize>
where
    C: DerefMut<Target = CowColumn<T, CHUNK>>,
    T: Clone + Default + PartialEq,
{
    column: C,
    index: usize,
    staged: Option<T>,
}

impl<C, T, const CHUNK: usize> RowMut<C, T, CHUNK>
where
    C: DerefMut<Target = CowColumn<T, CHUNK>>,
    T: Clone + Default + PartialEq,
{
    /// The row at `index` of the column `column` holds. `None` if the column does not hold it.
    pub(crate) fn new(mut column: C, index: usize) -> Option<Self> {
        column.get(index)?;
        let staged = if column.owns_chunk_of(index) {
            None
        } else {
            column.get(index).cloned()
        };
        Some(Self { column, index, staged })
    }
}

impl<C, T, const CHUNK: usize> Deref for RowMut<C, T, CHUNK>
where
    C: DerefMut<Target = CowColumn<T, CHUNK>>,
    T: Clone + Default + PartialEq,
{
    type Target = T;

    #[inline]
    fn deref(&self) -> &T {
        match &self.staged {
            Some(row) => row,
            None => self.column.get(self.index).expect("the guard's row is in the column"),
        }
    }
}

impl<C, T, const CHUNK: usize> DerefMut for RowMut<C, T, CHUNK>
where
    C: DerefMut<Target = CowColumn<T, CHUNK>>,
    T: Clone + Default + PartialEq,
{
    #[inline]
    fn deref_mut(&mut self) -> &mut T {
        match &mut self.staged {
            Some(row) => row,
            None => self
                .column
                .owned_row(self.index)
                .expect("the guard's row is in the column"),
        }
    }
}

impl<C, T, const CHUNK: usize> Drop for RowMut<C, T, CHUNK>
where
    C: DerefMut<Target = CowColumn<T, CHUNK>>,
    T: Clone + Default + PartialEq,
{
    fn drop(&mut self) {
        if let Some(row) = self.staged.take() {
            self.column.set(self.index, row);
        }
    }
}

impl<T, const CHUNK: usize> ColumnSnapshot<T, CHUNK> {
    /// Whether `other` is this very generation of the column: one no write came between.
    pub(crate) fn is_same_generation_as(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.spine, &other.spine)
    }

    /// The bytes of the snapshot's chunks that `sizer` reaches for the first time, with what
    /// `row_heap` says each row holds.
    pub(crate) fn heap_size(
        &self,
        sizer: &mut crate::heap_size::HeapSizer,
        mut row_heap: impl FnMut(&T, &mut crate::heap_size::HeapSizer) -> usize,
    ) -> usize {
        spine_heap_size(&self.spine, sizer, &mut row_heap)
    }

    /// How many rows the snapshot has room for: every row index below it may be read.
    pub(crate) fn slot_capacity(&self) -> usize {
        self.spine.last().map_or(0, |last| {
            let in_last = last.0.iter().take_while(|chunk| chunk.is_some()).count();
            ((self.spine.len() - 1) * GROUP + in_last) * CHUNK
        })
    }

    #[inline]
    pub(crate) fn get(&self, index: usize) -> Option<&T> {
        row_of(&self.spine, index)
    }
}

/// Whether two rows hold the same shared payload: the same allocation, or two that `same_value`
/// says are equal.
pub(crate) fn same_payload<T: ?Sized>(
    a: Option<&Arc<T>>,
    b: Option<&Arc<T>>,
    same_value: impl FnOnce(&T, &T) -> bool,
) -> bool {
    match (a, b) {
        (Some(a), Some(b)) => Arc::ptr_eq(a, b) || same_value(a, b),
        (None, None) => true,
        _ => false,
    }
}

const _: () = {
    const fn assert_send_and_sync<T: Send + Sync>() {}
    assert_send_and_sync::<CowColumn<u64, 64>>();
    assert_send_and_sync::<ColumnSnapshot<u64, 64>>();
};

/// A vector whose fork shares its storage, as a [`CowColumn`]'s does: cloning it costs a reference count per chunk,
/// and each side copies a chunk the other shares the first time it writes there. Rows past `len` read as the default
/// and are reset to it before the vector grows over them again.
pub(crate) struct CowVec<T, const CHUNK: usize> {
    column: CowColumn<T, CHUNK>,
    len: usize,
}

impl<T, const CHUNK: usize> Default for CowVec<T, CHUNK> {
    fn default() -> Self {
        Self {
            column: CowColumn::default(),
            len: 0,
        }
    }
}

impl<T: Clone + Default, const CHUNK: usize> Clone for CowVec<T, CHUNK> {
    fn clone(&self) -> Self {
        Self {
            column: self.column.clone(),
            len: self.len,
        }
    }
}

impl<T: Clone + Default, const CHUNK: usize> CowVec<T, CHUNK> {
    pub(crate) fn len(&self) -> usize {
        self.len
    }

    /// The bytes the vector holds that `sizer` reaches for the first time, with what `row_heap` says each row holds.
    pub(crate) fn heap_size(
        &self,
        sizer: &mut crate::heap_size::HeapSizer,
        row_heap: impl FnMut(&T, &mut crate::heap_size::HeapSizer) -> usize,
    ) -> usize {
        self.column.heap_size(sizer, row_heap)
    }

    /// Every row, in order.
    pub(crate) fn iter(&self) -> impl Iterator<Item = &T> {
        (0..self.len).filter_map(|index| self.column.get(index))
    }

    #[inline]
    pub(crate) fn get(&self, index: usize) -> Option<&T> {
        if index >= self.len {
            return None;
        }
        self.column.get(index)
    }

    /// The row at `index`, for writing, copying its chunk where a fork shares it.
    #[inline]
    pub(crate) fn get_mut(&mut self, index: usize) -> Option<&mut T> {
        if index >= self.len {
            return None;
        }
        self.column.owned_row(index)
    }

    pub(crate) fn push(&mut self, value: T) {
        let index = self.len;
        self.resize_with_default(index + 1);
        self[index] = value;
    }

    /// Grows the vector to `len` rows, the new ones default, or truncates it to them.
    pub(crate) fn resize_with_default(&mut self, len: usize) {
        if len < self.len {
            for index in len..self.len {
                if self.column.get(index).is_some() {
                    *self.column.owned_row(index).expect("the row is in the column") = T::default();
                }
            }
        }
        self.column.grow_to(len);
        self.len = len;
    }
}

impl<T: Clone + Default, const CHUNK: usize> std::ops::Index<usize> for CowVec<T, CHUNK> {
    type Output = T;

    #[inline]
    fn index(&self, index: usize) -> &T {
        self.get(index).expect("index within the vector")
    }
}

impl<T: Clone + Default, const CHUNK: usize> std::ops::IndexMut<usize> for CowVec<T, CHUNK> {
    #[inline]
    fn index_mut(&mut self, index: usize) -> &mut T {
        self.get_mut(index).expect("index within the vector")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    type Column = CowColumn<u32, 4>;

    fn column_of(values: &[u32]) -> Column {
        let mut column = Column::default();
        column.grow_to(values.len());
        for (index, &value) in values.iter().enumerate() {
            column.set(index, value).unwrap();
        }
        column
    }

    fn rows(snapshot: &ColumnSnapshot<u32, 4>, count: usize) -> Vec<u32> {
        (0..count).map(|index| *snapshot.get(index).unwrap()).collect()
    }

    fn set(column: &mut Column, index: usize, value: u32) {
        column.set(index, value).unwrap();
    }

    fn chunk(spine: &Spine<u32, 4>, chunk_index: usize) -> &Arc<Chunk<u32, 4>> {
        chunk_of(spine, chunk_index).unwrap()
    }

    #[test]
    fn a_column_reads_back_what_was_written() {
        let mut column = column_of(&[1, 2, 3, 4, 5, 6]);
        set(&mut column, 4, 50);
        assert_eq!(
            (0..6).map(|index| *column.get(index).unwrap()).collect::<Vec<_>>(),
            [1, 2, 3, 4, 50, 6]
        );
    }

    #[test]
    fn a_column_holds_whole_chunks() {
        let mut column = column_of(&[1, 2, 3]);
        assert_eq!(column.get(3), Some(&0));
        assert_eq!(column.get(4), None);
        assert_eq!(column.set(4, 1), None);
        assert!(column.row_mut(4).is_none());
        assert_eq!(column.publish().get(4), None);
    }

    #[test]
    fn growing_fills_with_defaults() {
        let mut column = column_of(&[7]);
        column.grow_to(9);
        assert_eq!(column.get(0), Some(&7));
        assert_eq!(column.get(11), Some(&0));
        assert_eq!(column.get(12), None);
        column.grow_to(3);
        assert_eq!(column.get(11), Some(&0));
    }

    #[test]
    fn a_snapshot_keeps_its_generation_while_the_column_changes() {
        let mut column = column_of(&[1, 2, 3, 4, 5, 6]);
        let first = column.publish();
        set(&mut column, 1, 20);
        column.grow_to(7);
        set(&mut column, 6, 7);
        let second = column.publish();
        set(&mut column, 5, 60);
        assert_eq!(rows(&first, 6), [1, 2, 3, 4, 5, 6]);
        assert_eq!(rows(&second, 7), [1, 20, 3, 4, 5, 6, 7]);
        assert_eq!(rows(&column.publish(), 7), [1, 20, 3, 4, 5, 60, 7]);
    }

    #[test]
    fn a_write_copies_only_the_chunk_a_snapshot_shares_and_only_once() {
        let mut column = column_of(&[1, 2, 3, 4, 5, 6, 7, 8]);
        let snapshot = column.publish();
        set(&mut column, 5, 60);
        assert!(Arc::ptr_eq(chunk(&column.spine, 0), chunk(&snapshot.spine, 0)));
        assert!(!Arc::ptr_eq(chunk(&column.spine, 1), chunk(&snapshot.spine, 1)));
        let copied = Arc::as_ptr(chunk(&column.spine, 1));
        set(&mut column, 6, 70);
        assert_eq!(Arc::as_ptr(chunk(&column.spine, 1)), copied);
        assert_eq!(rows(&snapshot, 8), [1, 2, 3, 4, 5, 6, 7, 8]);
    }

    #[test]
    fn a_snapshot_shares_the_column_and_a_write_copies_only_its_group() {
        let mut column = column_of(&[1; 4 * GROUP + 4]);
        let snapshot = column.publish();
        assert!(Arc::ptr_eq(&column.spine, &snapshot.spine));
        assert!(Arc::ptr_eq(&snapshot.clone().spine, &snapshot.spine));
        set(&mut column, 4 * GROUP, 2);
        assert!(!Arc::ptr_eq(&column.spine, &snapshot.spine));
        assert!(Arc::ptr_eq(&column.spine[0], &snapshot.spine[0]));
        assert!(!Arc::ptr_eq(&column.spine[1], &snapshot.spine[1]));
        assert!(Arc::ptr_eq(chunk(&column.spine, 1), chunk(&snapshot.spine, 1)));
        assert_eq!(snapshot.slot_capacity(), 4 * GROUP + 4);
        assert_eq!(snapshot.get(4 * GROUP), Some(&1));
        assert_eq!(column.publish().get(4 * GROUP), Some(&2));
    }

    #[test]
    fn a_write_that_changes_nothing_copies_no_chunk() {
        let mut column = column_of(&[1, 2, 3, 4, 5, 6, 7, 8]);
        let snapshot = column.publish();
        set(&mut column, 5, 6);
        *column.row_mut(6).unwrap() = 7;
        {
            let mut row = column.row_mut(4).unwrap();
            *row = 50;
            *row = 5;
        }
        assert!(Arc::ptr_eq(chunk(&column.spine, 1), chunk(&snapshot.spine, 1)));
        *column.row_mut(6).unwrap() += 1;
        assert!(!Arc::ptr_eq(chunk(&column.spine, 1), chunk(&snapshot.spine, 1)));
        assert_eq!(rows(&snapshot, 8), [1, 2, 3, 4, 5, 6, 7, 8]);
        assert_eq!(rows(&column.publish(), 8), [1, 2, 3, 4, 5, 6, 8, 8]);
    }

    #[test]
    fn a_row_guard_writes_in_place_once_the_column_owns_the_chunk() {
        let mut column = column_of(&[1, 2, 3, 4]);
        let snapshot = column.publish();
        set(&mut column, 0, 10);
        let copied = Arc::as_ptr(chunk(&column.spine, 0));
        {
            let mut row = column.row_mut(1).unwrap();
            assert!(row.staged.is_none());
            *row = 20;
        }
        assert_eq!(Arc::as_ptr(chunk(&column.spine, 0)), copied);
        assert_eq!(rows(&snapshot, 4), [1, 2, 3, 4]);
        assert_eq!(rows(&column.publish(), 4), [10, 20, 3, 4]);
    }

    #[test]
    fn a_chunk_whose_snapshot_is_gone_is_written_in_place() {
        let mut column = column_of(&[1, 2, 3, 4]);
        drop(column.publish());
        let in_place = Arc::as_ptr(chunk(&column.spine, 0));
        set(&mut column, 2, 30);
        assert_eq!(Arc::as_ptr(chunk(&column.spine, 0)), in_place);
    }

    #[test]
    fn a_snapshot_can_be_read_on_another_thread() {
        let mut column = column_of(&[1, 2, 3, 4, 5]);
        let snapshot = column.publish();
        let reader = std::thread::spawn(move || rows(&snapshot, 5));
        set(&mut column, 0, 10);
        assert_eq!(reader.join().unwrap(), [1, 2, 3, 4, 5]);
    }
}
