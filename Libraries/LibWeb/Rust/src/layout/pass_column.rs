/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::UnsafeCell;
use std::sync::atomic::{AtomicPtr, Ordering};

const ROWS_PER_CHUNK: usize = 128;

#[repr(align(64))]
struct PassChunk<T> {
    rows: [UnsafeCell<T>; ROWS_PER_CHUNK],
    #[cfg(debug_assertions)]
    borrows: [std::sync::atomic::AtomicI32; ROWS_PER_CHUNK],
}

impl<T: Default> PassChunk<T> {
    fn new() -> Box<Self> {
        Box::new(Self {
            rows: std::array::from_fn(|_| UnsafeCell::new(T::default())),
            #[cfg(debug_assertions)]
            borrows: std::array::from_fn(|_| std::sync::atomic::AtomicI32::new(0)),
        })
    }
}

struct OwnedChunk<T> {
    pointer: AtomicPtr<PassChunk<T>>,
    owns: std::marker::PhantomData<Box<PassChunk<T>>>,
}

impl<T> Default for OwnedChunk<T> {
    fn default() -> Self {
        Self {
            pointer: AtomicPtr::default(),
            owns: std::marker::PhantomData,
        }
    }
}

impl<T> Drop for OwnedChunk<T> {
    fn drop(&mut self) {
        let chunk = *self.pointer.get_mut();
        if !chunk.is_null() {
            // SAFETY: A non-null chunk pointer came from `Box::into_raw` in `make_chunk`, and only this slot owns it.
            drop(unsafe { Box::from_raw(chunk) });
        }
    }
}

pub(crate) struct PassColumn<T> {
    chunks: Vec<OwnedChunk<T>>,
    rows_are_slots: bool,
}

// SAFETY: A pass writes a row only from the run that lays out the row's box. The runs laid out at once on several
// threads lay out disjoint subtrees, and outside their own subtree they only read the rows of their ancestors, whose
// runs wait for them. So no row is written while another thread reaches it, and rows several threads read at once are
// Sync. Debug builds check that no row is written while it is reached otherwise.
unsafe impl<T: Send + Sync> Sync for PassColumn<T> {}

#[derive(Default)]
pub(crate) struct ExclusiveRow<T>(T);

// SAFETY: An ExclusiveRow hands out its contents only through a mutable reference, so threads never share them.
unsafe impl<T: Send> Sync for ExclusiveRow<T> {}

impl<T> ExclusiveRow<T> {
    pub(crate) fn get_mut(&mut self) -> &mut T {
        &mut self.0
    }
}

impl<T> Default for PassColumn<T> {
    fn default() -> Self {
        Self {
            chunks: Vec::new(),
            rows_are_slots: true,
        }
    }
}

impl<T> PassColumn<T> {
    pub(crate) fn with_rows_of_no_slot() -> Self {
        Self {
            rows_are_slots: false,
            ..Self::default()
        }
    }

    fn empty_like<OtherRow>(&self) -> PassColumn<OtherRow> {
        PassColumn {
            rows_are_slots: self.rows_are_slots,
            ..PassColumn::default()
        }
    }
}

impl<T: Default> PassColumn<T> {
    pub(crate) fn cover_slots(&mut self, slot_count: usize) {
        let chunk_count = slot_count.div_ceil(ROWS_PER_CHUNK);
        if self.chunks.len() < chunk_count {
            self.chunks.resize_with(chunk_count, Default::default);
        }
    }

    #[inline]
    pub(crate) fn with_row<R>(&self, index: usize, read: impl FnOnce(Option<&T>) -> R) -> R {
        self.table().with_row(index, read)
    }

    #[inline]
    pub(crate) fn with_row_mut<R>(&self, index: usize, write: impl FnOnce(&mut T) -> R) -> R {
        self.table().with_row_mut(index, write)
    }

    #[inline]
    fn table(&self) -> ChunkTable<'_, T> {
        ChunkTable {
            chunks: &self.chunks,
            rows_are_slots: self.rows_are_slots,
        }
    }

    pub(crate) fn row_mut(&mut self, index: usize) -> Option<&mut T> {
        Some(self.rows_of_chunk_mut(index / ROWS_PER_CHUNK)?[index % ROWS_PER_CHUNK].get_mut())
    }

    fn rows_of_chunk_mut(&mut self, chunk_index: usize) -> Option<&mut [UnsafeCell<T>; ROWS_PER_CHUNK]> {
        let chunk = *self.chunks.get_mut(chunk_index)?.pointer.get_mut();
        // SAFETY: `&mut self` rules out any other reference into the column's chunks.
        Some(&mut unsafe { chunk.as_mut() }?.rows)
    }

    fn covered_rows(&self) -> usize {
        self.chunks.len() * ROWS_PER_CHUNK
    }
}

struct ChunkTable<'column, T> {
    chunks: &'column [OwnedChunk<T>],
    rows_are_slots: bool,
}

impl<T> Clone for ChunkTable<'_, T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T> Copy for ChunkTable<'_, T> {}

impl<'column, T: Default> ChunkTable<'column, T> {
    #[inline]
    fn with_row<R>(self, index: usize, read: impl FnOnce(Option<&T>) -> R) -> R {
        if self.rows_are_slots {
            super::parallel_layout::check_row_reached_by_forked_task(index, false);
        }
        let Some(chunk) = self.chunk_for_reading(index / ROWS_PER_CHUNK) else {
            return read(None);
        };
        let _borrow = RowBorrow::shared(chunk, index % ROWS_PER_CHUNK);
        // SAFETY: Only the run laying out the row's box writes the row, and not while this read runs (`RowBorrow`
        // checks that in debug builds).
        read(Some(unsafe { &*chunk.rows[index % ROWS_PER_CHUNK].get() }))
    }

    #[inline]
    fn with_row_mut<R>(self, index: usize, write: impl FnOnce(&mut T) -> R) -> R {
        if self.rows_are_slots {
            super::parallel_layout::check_row_reached_by_forked_task(index, true);
        }
        let chunk = self.chunk_for_writing(index / ROWS_PER_CHUNK);
        let _borrow = RowBorrow::exclusive(chunk, index % ROWS_PER_CHUNK);
        // SAFETY: Only the run laying out the row's box reaches the row while it writes it (`RowBorrow` checks that in
        // debug builds).
        write(unsafe { &mut *chunk.rows[index % ROWS_PER_CHUNK].get() })
    }

    #[inline]
    fn chunk_for_reading(self, chunk_index: usize) -> Option<&'column PassChunk<T>> {
        let chunk = self.chunks.get(chunk_index)?.pointer.load(Ordering::Acquire);
        // SAFETY: A non-null chunk pointer stays valid until its column drops, which the borrow of the column rules
        // out.
        unsafe { chunk.as_ref() }
    }

    #[inline]
    fn chunk_for_writing(self, chunk_index: usize) -> &'column PassChunk<T> {
        let slot = &self
            .chunks
            .get(chunk_index)
            .expect("a pass writes only rows of slots the arena held as the pass began")
            .pointer;
        let chunk = slot.load(Ordering::Acquire);
        // SAFETY: As in `chunk_for_reading`.
        unsafe { chunk.as_ref() }.unwrap_or_else(|| make_chunk(slot))
    }
}

#[cold]
#[inline(never)]
fn make_chunk<T: Default>(slot: &AtomicPtr<PassChunk<T>>) -> &PassChunk<T> {
    let made = Box::into_raw(PassChunk::new());
    match slot.compare_exchange(std::ptr::null_mut(), made, Ordering::AcqRel, Ordering::Acquire) {
        // SAFETY: The slot now owns `made`, which stays valid until its column drops.
        Ok(_) => unsafe { &*made },
        Err(made_by_another_run) => {
            // SAFETY: `made` was never shared.
            drop(unsafe { Box::from_raw(made) });
            // SAFETY: As in `ChunkTable::chunk_for_reading`.
            unsafe { &*made_by_another_run }
        }
    }
}

#[derive(Default)]
pub(crate) struct PassFlags {
    flags: PassColumn<bool>,
    chunks_holding_raised_flags: Vec<std::sync::atomic::AtomicBool>,
}

impl PassFlags {
    pub(crate) fn cover_slots(&mut self, slot_count: usize) {
        self.flags.cover_slots(slot_count);
        let chunk_count = slot_count.div_ceil(ROWS_PER_CHUNK);
        if self.chunks_holding_raised_flags.len() < chunk_count {
            self.chunks_holding_raised_flags
                .resize_with(chunk_count, Default::default);
        }
    }

    #[inline]
    pub(crate) fn raise(&self, index: usize) {
        self.flags.with_row_mut(index, |flag| *flag = true);
        let chunk_holds_raised_flags = &self.chunks_holding_raised_flags[index / ROWS_PER_CHUNK];
        if !chunk_holds_raised_flags.load(std::sync::atomic::Ordering::Relaxed) {
            chunk_holds_raised_flags.store(true, std::sync::atomic::Ordering::Relaxed);
        }
    }

    pub(crate) fn retain_raised_flags(&mut self, mut keeps_flag_raised: impl FnMut(usize) -> bool) {
        let Self {
            flags,
            chunks_holding_raised_flags,
        } = self;
        for (chunk_index, chunk_holds_raised_flags) in chunks_holding_raised_flags.iter_mut().enumerate() {
            if !*chunk_holds_raised_flags.get_mut() {
                continue;
            }
            let Some(rows) = flags.rows_of_chunk_mut(chunk_index) else {
                continue;
            };
            let mut holds_raised_flags = false;
            for (row_index, flag) in rows.iter_mut().enumerate() {
                if *flag.get_mut() {
                    *flag.get_mut() = keeps_flag_raised(chunk_index * ROWS_PER_CHUNK + row_index);
                    holds_raised_flags |= *flag.get_mut();
                }
            }
            *chunk_holds_raised_flags.get_mut() = holds_raised_flags;
        }
    }

    pub(crate) fn copy(&self) -> Self {
        let mut copy = Self::default();
        copy.cover_slots(self.flags.covered_rows());
        for (chunk_index, chunk_holds_raised_flags) in self.chunks_holding_raised_flags.iter().enumerate() {
            if !chunk_holds_raised_flags.load(std::sync::atomic::Ordering::Relaxed) {
                continue;
            }
            for index in chunk_index * ROWS_PER_CHUNK..(chunk_index + 1) * ROWS_PER_CHUNK {
                if self.flags.with_row(index, |flag| flag.is_some_and(|flag| *flag)) {
                    copy.raise(index);
                }
            }
        }
        copy
    }
}

pub(crate) struct ForkSharedRows<T> {
    host_rows: std::sync::Arc<HostRows<T>>,
    chunk_table_of_the_host: ChunkTableOfTheHost<T>,
    fork_side: Option<ForkSide<T>>,
}

struct ChunkTableOfTheHost<T> {
    chunks: std::ptr::NonNull<OwnedChunk<T>>,
    chunk_count: usize,
    rows_are_slots: bool,
}

// SAFETY: It points into the chunk table of the host's column, which is Send and Sync as `HostRows` is.
unsafe impl<T: Send> Send for ChunkTableOfTheHost<T> {}
// SAFETY: As above.
unsafe impl<T: Send + Sync> Sync for ChunkTableOfTheHost<T> {}

impl<T> ChunkTableOfTheHost<T> {
    fn of(column: &PassColumn<T>) -> Self {
        Self {
            chunks: std::ptr::NonNull::new(column.chunks.as_ptr().cast_mut()).expect("a Vec's pointer is never null"),
            chunk_count: column.chunks.len(),
            rows_are_slots: column.rows_are_slots,
        }
    }

    fn of_a_fork() -> Self {
        Self {
            chunks: std::ptr::NonNull::dangling(),
            chunk_count: 0,
            rows_are_slots: false,
        }
    }
}

struct HostRows<T> {
    rows: UnsafeCell<PassColumn<T>>,
    fork_generation_each_row_was_saved_for: UnsafeCell<PassColumn<u32>>,
    live_fork: UnsafeCell<Option<std::sync::Arc<RowsSavedForFork<T>>>>,
    fork_generations_handed_out: UnsafeCell<u32>,
}

// SAFETY: The host's state and its forks reach these rows only while they hold their document's render state, which
// one thread at a time does, and they never hold it at once with another thread laying out the same rows: a fork is
// made from the host between its jobs, and a dropped fork only lets go of its `Arc`. Within one layout pass, a row and
// its saved copy are written only by the run that lays out the row's box.
unsafe impl<T: Send + Sync> Sync for HostRows<T> {}

struct RowsSavedForFork<T> {
    generation: u32,
    rows_the_host_held_at_fork: usize,
    rows: PassColumn<Option<T>>,
}

// SAFETY: The host writes these rows and its fork reads them, never at once, as for `HostRows`.
unsafe impl<T: Send + Sync> Sync for RowsSavedForFork<T> {}

struct ForkSide<T> {
    rows_saved_by_host: std::sync::Arc<RowsSavedForFork<T>>,
    own_rows: PassColumn<Option<T>>,
    reads_host_rows: bool,
}

impl<T> Default for ForkSharedRows<T> {
    fn default() -> Self {
        Self::with_host_rows(PassColumn::default(), PassColumn::default())
    }
}

impl<T> ForkSharedRows<T> {
    pub(crate) fn with_rows_of_no_slot() -> Self {
        Self::with_host_rows(PassColumn::with_rows_of_no_slot(), PassColumn::with_rows_of_no_slot())
    }

    fn with_host_rows(rows: PassColumn<T>, fork_generation_each_row_was_saved_for: PassColumn<u32>) -> Self {
        Self {
            chunk_table_of_the_host: ChunkTableOfTheHost::of(&rows),
            host_rows: std::sync::Arc::new(HostRows {
                rows: UnsafeCell::new(rows),
                fork_generation_each_row_was_saved_for: UnsafeCell::new(fork_generation_each_row_was_saved_for),
                live_fork: UnsafeCell::new(None),
                fork_generations_handed_out: UnsafeCell::new(0),
            }),
            fork_side: None,
        }
    }
}

impl<T: Clone + Default> ForkSharedRows<T> {
    #[inline]
    fn host_rows(&self) -> &PassColumn<T> {
        // SAFETY: The host's column grows only in `cover_slots`, while neither the host nor a fork lays out.
        unsafe { &*self.host_rows.rows.get() }
    }

    #[inline]
    fn host_table_of_the_host(&self) -> ChunkTable<'_, T> {
        debug_assert!(self.fork_side.is_none());
        ChunkTable {
            // SAFETY: `chunk_table_of_the_host` points into the chunk table of the host's column, which `host_rows`
            // keeps alive. Only the host's `cover_slots` grows that table, and it points this at the grown table.
            chunks: unsafe {
                ak::slice_from_raw_parts(
                    self.chunk_table_of_the_host.chunks.as_ptr(),
                    self.chunk_table_of_the_host.chunk_count,
                )
            },
            rows_are_slots: self.chunk_table_of_the_host.rows_are_slots,
        }
    }

    #[inline]
    fn live_fork(&self) -> Option<&RowsSavedForFork<T>> {
        // SAFETY: The live fork changes only as the host makes a fork or covers its rows, between its passes.
        unsafe { (*self.host_rows.live_fork.get()).as_deref() }
    }

    pub(crate) fn fork(&self) -> Self {
        assert!(
            self.fork_side.is_none(),
            "a render state is forked only from the host's state"
        );
        let rows_the_host_held_at_fork = self.host_rows().covered_rows();
        // SAFETY: The host makes a fork between its jobs, while no fork lays out.
        let generation = unsafe {
            let handed_out = &mut *self.host_rows.fork_generations_handed_out.get();
            *handed_out += 1;
            *handed_out
        };
        let mut rows = self.host_rows().empty_like();
        rows.cover_slots(rows_the_host_held_at_fork);
        let rows_saved_by_host = std::sync::Arc::new(RowsSavedForFork {
            generation,
            rows_the_host_held_at_fork,
            rows,
        });
        // SAFETY: As above.
        unsafe {
            *self.host_rows.live_fork.get() = Some(std::sync::Arc::clone(&rows_saved_by_host));
        }
        Self {
            host_rows: std::sync::Arc::clone(&self.host_rows),
            chunk_table_of_the_host: ChunkTableOfTheHost::of_a_fork(),
            fork_side: Some(ForkSide {
                rows_saved_by_host,
                own_rows: self.host_rows().empty_like(),
                reads_host_rows: true,
            }),
        }
    }

    pub(crate) fn cover_slots(&mut self, slot_count: usize) {
        match &mut self.fork_side {
            None => {
                // SAFETY: The host covers its rows between its passes, while no fork lays out.
                unsafe {
                    (*self.host_rows.rows.get()).cover_slots(slot_count);
                    (*self.host_rows.fork_generation_each_row_was_saved_for.get()).cover_slots(slot_count);
                    let live_fork = &mut *self.host_rows.live_fork.get();
                    if live_fork
                        .as_ref()
                        .is_some_and(|fork| std::sync::Arc::strong_count(fork) == 1)
                    {
                        *live_fork = None;
                    }
                }
                self.chunk_table_of_the_host = ChunkTableOfTheHost::of(self.host_rows());
            }
            Some(fork_side) => {
                fork_side.own_rows.cover_slots(slot_count);
                // SAFETY: The fork covers its rows between its passes, while the host does not lay out.
                let live_fork = unsafe { &*self.host_rows.live_fork.get() };
                if !live_fork
                    .as_ref()
                    .is_some_and(|live_fork| std::sync::Arc::ptr_eq(live_fork, &fork_side.rows_saved_by_host))
                {
                    fork_side.reads_host_rows = false;
                }
            }
        }
    }

    #[inline]
    pub(crate) fn with_row<R>(&self, index: usize, read: impl FnOnce(Option<&T>) -> R) -> R {
        let Some(fork_side) = &self.fork_side else {
            return self.host_table_of_the_host().with_row(index, read);
        };
        fork_side.own_rows.with_row(index, |own_row| match own_row {
            Some(Some(row)) => read(Some(row)),
            _ => self.with_row_the_fork_was_made_with(fork_side, index, read),
        })
    }

    fn with_row_the_fork_was_made_with<R>(
        &self,
        fork_side: &ForkSide<T>,
        index: usize,
        read: impl FnOnce(Option<&T>) -> R,
    ) -> R {
        let saved = &fork_side.rows_saved_by_host;
        saved.rows.with_row(index, |saved_row| match saved_row {
            Some(Some(row)) => read(Some(row)),
            _ if fork_side.reads_host_rows && index < saved.rows_the_host_held_at_fork => {
                self.host_rows().with_row(index, read)
            }
            _ => read(None),
        })
    }

    #[inline]
    pub(crate) fn with_row_mut<R>(&self, index: usize, write: impl FnOnce(&mut T) -> R) -> R {
        let Some(fork_side) = &self.fork_side else {
            if let Some(live_fork) = self.live_fork()
                && index < live_fork.rows_the_host_held_at_fork
            {
                self.save_row_for(live_fork, index);
            }
            return self.host_table_of_the_host().with_row_mut(index, write);
        };
        fork_side.own_rows.with_row_mut(index, |own_row| {
            write(own_row.get_or_insert_with(|| {
                self.with_row_the_fork_was_made_with(fork_side, index, |row| row.cloned().unwrap_or_default())
            }))
        })
    }

    fn save_row_for(&self, live_fork: &RowsSavedForFork<T>, index: usize) {
        // SAFETY: As for `host_rows`.
        let saved_for = unsafe { &*self.host_rows.fork_generation_each_row_was_saved_for.get() };
        saved_for.with_row_mut(index, |saved_for| {
            if *saved_for == live_fork.generation {
                return;
            }
            *saved_for = live_fork.generation;
            live_fork.rows.with_row_mut(index, |saved_row| {
                if saved_row.is_none() {
                    *saved_row = Some(
                        self.host_table_of_the_host()
                            .with_row(index, |row| row.cloned().unwrap_or_default()),
                    );
                }
            });
        });
    }

    pub(crate) fn row_mut(&mut self, index: usize) -> Option<&mut T> {
        if self.fork_side.is_some() {
            self.with_row_mut(index, |_| ());
            return self.fork_side.as_mut()?.own_rows.row_mut(index)?.as_mut();
        }
        if let Some(live_fork) = self.live_fork()
            && index < live_fork.rows_the_host_held_at_fork
        {
            self.save_row_for(live_fork, index);
        }
        // SAFETY: The host writes its rows between its passes, while no fork lays out, and `&mut self` rules out any
        // other reference into them from the host.
        unsafe { &mut *self.host_rows.rows.get() }.row_mut(index)
    }

    pub(crate) fn reset_row_where(&mut self, index: usize, holds: impl Fn(&T) -> bool) {
        if self.fork_side.is_none() && self.live_fork().is_none() {
            // SAFETY: As in `row_mut`.
            if let Some(row) = unsafe { &mut *self.host_rows.rows.get() }.row_mut(index)
                && holds(row)
            {
                *row = T::default();
            }
            return;
        }
        if self.with_row(index, |row| row.is_some_and(&holds)) {
            self.with_row_mut(index, |row| *row = T::default());
        }
    }
}

struct RowBorrow<'chunk> {
    #[cfg(debug_assertions)]
    state: &'chunk std::sync::atomic::AtomicI32,
    #[cfg(not(debug_assertions))]
    chunk: std::marker::PhantomData<&'chunk ()>,
}

impl<'chunk> RowBorrow<'chunk> {
    #[inline]
    fn shared<T>(chunk: &'chunk PassChunk<T>, row: usize) -> Self {
        #[cfg(debug_assertions)]
        {
            let state = &chunk.borrows[row];
            let previous = state.fetch_add(1, std::sync::atomic::Ordering::Acquire);
            assert!(previous >= 0, "a pass column row is read while it is written");
            Self { state }
        }
        #[cfg(not(debug_assertions))]
        {
            let _ = (chunk, row);
            Self {
                chunk: std::marker::PhantomData,
            }
        }
    }

    #[inline]
    fn exclusive<T>(chunk: &'chunk PassChunk<T>, row: usize) -> Self {
        #[cfg(debug_assertions)]
        {
            let state = &chunk.borrows[row];
            let previous = state.swap(-1, std::sync::atomic::Ordering::Acquire);
            assert!(
                previous == 0,
                "a pass column row is written while it is reached otherwise"
            );
            Self { state }
        }
        #[cfg(not(debug_assertions))]
        {
            let _ = (chunk, row);
            Self {
                chunk: std::marker::PhantomData,
            }
        }
    }
}

#[cfg(debug_assertions)]
impl Drop for RowBorrow<'_> {
    fn drop(&mut self) {
        let state = self.state.load(std::sync::atomic::Ordering::Relaxed);
        if state < 0 {
            self.state.store(0, std::sync::atomic::Ordering::Release);
        } else {
            self.state.fetch_sub(1, std::sync::atomic::Ordering::Release);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::ForkSharedRows;

    fn row(rows: &ForkSharedRows<u32>, index: usize) -> u32 {
        rows.with_row(index, |row| row.copied().unwrap_or_default())
    }

    fn write(rows: &ForkSharedRows<u32>, index: usize, value: u32) {
        rows.with_row_mut(index, |row| *row = value);
    }

    #[test]
    fn a_fork_reads_the_rows_the_host_held_when_it_was_made() {
        let mut host = ForkSharedRows::default();
        host.cover_slots(300);
        write(&host, 5, 1);
        write(&host, 200, 2);
        let mut fork = host.fork();
        fork.cover_slots(300);
        host.cover_slots(300);
        write(&host, 5, 10);
        write(&host, 7, 11);
        write(&host, 250, 12);
        write(&host, 260, 13);
        assert_eq!([5, 7, 200, 250, 260].map(|index| row(&fork, index)), [1, 0, 2, 0, 0]);
        assert_eq!(
            [5, 7, 200, 250, 260].map(|index| row(&host, index)),
            [10, 11, 2, 12, 13]
        );
    }

    #[test]
    fn a_fork_keeps_its_own_writes_over_rows_the_host_saves_for_it_later() {
        let mut host = ForkSharedRows::default();
        host.cover_slots(10);
        write(&host, 5, 1);
        write(&host, 6, 2);
        let mut fork = host.fork();
        fork.cover_slots(10);
        write(&fork, 5, 20);
        host.cover_slots(10);
        write(&host, 5, 10);
        write(&host, 6, 30);
        assert_eq!([5, 6].map(|index| row(&fork, index)), [20, 2]);
        assert_eq!([5, 6].map(|index| row(&host, index)), [10, 30]);
    }

    #[test]
    fn rows_past_those_the_host_held_when_the_fork_was_made_read_empty_in_the_fork() {
        let mut host = ForkSharedRows::default();
        host.cover_slots(100);
        let mut fork = host.fork();
        host.cover_slots(300);
        write(&host, 200, 5);
        fork.cover_slots(300);
        assert_eq!(row(&fork, 200), 0);
        write(&fork, 201, 7);
        assert_eq!(row(&fork, 201), 7);
        assert_eq!([200, 201].map(|index| row(&host, index)), [5, 0]);
    }

    #[test]
    fn an_earlier_fork_stops_reading_the_host_rows_once_a_newer_fork_is_made() {
        let mut host = ForkSharedRows::default();
        host.cover_slots(10);
        write(&host, 5, 1);
        write(&host, 6, 2);
        let mut earlier = host.fork();
        host.cover_slots(10);
        write(&host, 5, 10);
        let mut newer = host.fork();
        host.cover_slots(10);
        write(&host, 6, 20);
        earlier.cover_slots(10);
        newer.cover_slots(10);
        assert_eq!([5, 6].map(|index| row(&earlier, index)), [1, 0]);
        assert_eq!([5, 6].map(|index| row(&newer, index)), [10, 2]);
    }

    #[test]
    fn the_host_stops_saving_rows_once_its_fork_is_dropped() {
        let mut host = ForkSharedRows::<u32>::default();
        host.cover_slots(10);
        let fork = host.fork();
        host.cover_slots(10);
        assert!(host.live_fork().is_some());
        drop(fork);
        host.cover_slots(10);
        assert!(host.live_fork().is_none());
    }
}
