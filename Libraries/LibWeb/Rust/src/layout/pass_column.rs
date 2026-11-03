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
}

impl<T> Default for PassColumn<T> {
    fn default() -> Self {
        Self { chunks: Vec::new() }
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
        ChunkTable { chunks: &self.chunks }
    }

    pub(crate) fn row_mut(&mut self, index: usize) -> Option<&mut T> {
        Some(self.rows_of_chunk_mut(index / ROWS_PER_CHUNK)?[index % ROWS_PER_CHUNK].get_mut())
    }

    fn rows_of_chunk_mut(&mut self, chunk_index: usize) -> Option<&mut [UnsafeCell<T>; ROWS_PER_CHUNK]> {
        let chunk = *self.chunks.get_mut(chunk_index)?.pointer.get_mut();
        // SAFETY: `&mut self` rules out any other reference into the column's chunks.
        Some(&mut unsafe { chunk.as_mut() }?.rows)
    }
}

struct ChunkTable<'column, T> {
    chunks: &'column [OwnedChunk<T>],
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
