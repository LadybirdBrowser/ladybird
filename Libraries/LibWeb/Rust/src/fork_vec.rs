/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A vector whose fork costs a few reference counts, and which costs what a [`Vec`] does while no fork is alive.
//!
//! The vector keeps its rows flat. A fork shares them, and the vector it was made from goes on writing them in place,
//! saving a row's old value in a log the first time it writes the row. So a fork reads a row from what it wrote itself,
//! then from the logs from its own on, and then from the rows themselves, and sees the vector as it was when forked.
//! A log goes with the last fork that reads it, and the rows it saved with it, and writes then only pay to find that no
//! log is there.
//!
//! A row that owns nothing beside itself is saved with the rows next to it, a segment of them, the first time the vector
//! writes a row of the segment, so a vector writing many rows near each other saves a few segments rather than each row.
//!
//! The render state and its forks live in the render owner's slot, which one thread holds at a time, and the threads of a
//! pass only read. That is what lets a fork read the rows without a lock. See [`crate::render_state`]. A fork may be
//! dropped once its thread no longer holds the slot, so the vector takes a reference to a log for as long as it writes it.

use crate::css::style::fast_hash::FastMap;
use std::cell::{Cell, UnsafeCell};
use std::sync::{Arc, OnceLock, Weak};

/// What a vector saved of its rows while a fork made from it, or the log after it, was alive.
struct Log<T> {
    rows_at_fork: usize,
    /// Each saved row by its index, or where the vector saves rows with their segment (see [`ForkVec::SAVES_SEGMENTS`]),
    /// each saved segment by its index. In a box of its own, so that a reference a fork holds stays good as the log
    /// grows.
    saved: UnsafeCell<FastMap<usize, Box<[T]>>>,
    /// The log of the next fork, which saved what this log leaves out from when that fork was made.
    next: UnsafeCell<Option<Arc<Log<T>>>>,
}

struct ForkSide<T> {
    log: Arc<Log<T>>,
    own: FastMap<usize, T>,
    len: usize,
    /// What a row past those the fork was made with reads as until the fork writes it: growing a fork takes no rows.
    default_row: OnceLock<T>,
}

/// How many rows a segment of a vector holds: a row that owns nothing beside itself is saved with its segment.
const ROWS_PER_SEGMENT: usize = 256;

pub(crate) struct ForkVec<T> {
    /// The rows of the vector, which a fork reads none of here: its `len` is 0, so it leaves the fast path.
    rows: *mut T,
    len: usize,
    storage: Arc<UnsafeCell<Vec<T>>>,
    /// The log of the newest fork, which only the forks keep alive.
    log: UnsafeCell<Option<Weak<Log<T>>>>,
    /// How many rows the newest fork was made with, and 0 without a log: a write to a row past them saves nothing, and
    /// so does not ask whether a fork still reads the log.
    rows_at_newest_fork: Cell<usize>,
    /// Which segments the log of the newest fork saved, a bit each, where the vector saves rows with their segment: a
    /// write to a row of one of them saves nothing again.
    segments_saved_since_newest_fork: UnsafeCell<Vec<u64>>,
    fork: Option<Box<ForkSide<T>>>,
}

// SAFETY: A vector and its forks are reached only by the thread that holds the render owner's slot, and by the threads of
// a pass, which only read them, while it waits, but for a fork dropped past the hold, which only lets go of its `Arc`s.
// See the module documentation.
unsafe impl<T: Send + Sync> Send for ForkVec<T> {}
// SAFETY: As above.
unsafe impl<T: Send + Sync> Sync for ForkVec<T> {}

impl<T> Default for ForkVec<T> {
    fn default() -> Self {
        Self {
            rows: std::ptr::null_mut(),
            len: 0,
            storage: Arc::default(),
            log: UnsafeCell::new(None),
            rows_at_newest_fork: Cell::new(0),
            segments_saved_since_newest_fork: UnsafeCell::default(),
            fork: None,
        }
    }
}

impl<T: Clone + Default> ForkVec<T> {
    /// Whether a row is saved with its segment: a row that owns nothing beside itself copies as its bytes do, so its
    /// segment copies for little more than one row, and a vector writes many rows near each other, as a build filling
    /// the free slots of a chunk does.
    const SAVES_SEGMENTS: bool = !std::mem::needs_drop::<T>();

    fn storage_mut(&mut self) -> &mut Vec<T> {
        // SAFETY: `&mut self` and holding the slot rule out any other reference into the rows.
        unsafe { &mut *self.storage.get() }
    }

    fn note_storage_moved(&mut self) {
        let rows = self.storage_mut();
        let (pointer, len) = (rows.as_mut_ptr(), rows.len());
        self.rows = pointer;
        self.len = len;
    }

    #[inline]
    pub(crate) fn len(&self) -> usize {
        match &self.fork {
            None => self.len,
            Some(fork) => fork.len,
        }
    }

    #[inline]
    pub(crate) fn get(&self, index: usize) -> Option<&T> {
        if index < self.len {
            // SAFETY: `rows` and `len` follow the storage, which `&self` keeps from moving.
            return Some(unsafe { &*self.rows.add(index) });
        }
        self.get_on_fork(index)
    }

    #[cold]
    fn get_on_fork(&self, index: usize) -> Option<&T> {
        let fork = self.fork.as_deref()?;
        if index >= fork.len {
            return None;
        }
        if let Some(row) = fork.own.get(&index) {
            return Some(row);
        }
        if index >= fork.log.rows_at_fork {
            return Some(fork.default_row.get_or_init(T::default));
        }
        let mut log = &fork.log;
        loop {
            // SAFETY: Only the vector writes the log, and not while this reference lives: see the module documentation.
            if let Some(saved) = unsafe { &*log.saved.get() }.get(&Self::saved_key(index)) {
                return saved.get(Self::saved_offset(index));
            }
            // SAFETY: As above.
            match unsafe { &*log.next.get() } {
                Some(next) => log = next,
                // SAFETY: A row no log holds is as it was when the fork was made, and as above.
                None => return unsafe { &*self.storage.get() }.get(index),
            }
        }
    }

    #[inline]
    pub(crate) fn get_mut(&mut self, index: usize) -> Option<&mut T> {
        if index < self.len {
            self.save_row(index);
            // SAFETY: `rows` and `len` follow the storage, and `&mut self` rules out any other reference into it.
            return Some(unsafe { &mut *self.rows.add(index) });
        }
        self.get_mut_on_fork(index)
    }

    /// Whether another side reads the row at `index` as this one does: a fork of the vector, or the vector this is a
    /// fork of, where neither wrote the row since the fork.
    pub(crate) fn shares_row(&mut self, index: usize) -> bool {
        if let Some(fork) = &self.fork {
            return index < fork.log.rows_at_fork
                && !fork.own.contains_key(&index)
                && Arc::strong_count(&self.storage) > 1;
        }
        let Some(log) = self.live_log() else {
            return false;
        };
        if index >= log.rows_at_fork {
            return false;
        }
        // SAFETY: Only the vector writes the log, and `&mut self` rules out any other reference into it.
        !unsafe { &*log.saved.get() }.contains_key(&Self::saved_key(index))
    }

    /// The log of the newest fork, forgetting it once no fork reads it.
    fn live_log(&mut self) -> Option<Arc<Log<T>>> {
        let log = self.log.get_mut().as_ref().and_then(Weak::upgrade);
        if log.is_none() {
            *self.log.get_mut() = None;
            self.rows_at_newest_fork.set(0);
        }
        log
    }

    #[inline]
    fn save_row(&mut self, index: usize) {
        if index < self.rows_at_newest_fork.get() {
            self.save_row_in_log(index);
        }
    }

    /// Where a log keeps what it saved of the row at `index`, and where in that the row is.
    fn saved_key(index: usize) -> usize {
        if Self::SAVES_SEGMENTS {
            index / ROWS_PER_SEGMENT
        } else {
            index
        }
    }

    fn saved_offset(index: usize) -> usize {
        if Self::SAVES_SEGMENTS {
            index % ROWS_PER_SEGMENT
        } else {
            0
        }
    }

    fn segment_saved_since_newest_fork(&mut self, segment: usize) -> bool {
        Self::SAVES_SEGMENTS
            && self
                .segments_saved_since_newest_fork
                .get_mut()
                .get(segment / 64)
                .is_some_and(|word| word & (1 << (segment % 64)) != 0)
    }

    #[cold]
    fn save_row_in_log(&mut self, index: usize) {
        if self.segment_saved_since_newest_fork(index / ROWS_PER_SEGMENT) {
            return;
        }
        // NB: The last fork reading the log may go on another thread at any moment, so this holds it.
        let Some(log) = self.live_log() else {
            return;
        };
        if index >= log.rows_at_fork {
            return;
        }
        if Self::SAVES_SEGMENTS {
            let segment = index / ROWS_PER_SEGMENT;
            // NB: The segment holds every row of it the fork reads: a write to any of them, a pop included, saves it
            //     before it changes.
            // SAFETY: Only the vector writes the log, and `&mut self` rules out any other reference into it.
            unsafe { &mut *log.saved.get() }.entry(segment).or_insert_with(|| {
                let rows = self.storage_mut();
                let start = segment * ROWS_PER_SEGMENT;
                rows[start..rows.len().min(start + ROWS_PER_SEGMENT)].into()
            });
            let saved = self.segments_saved_since_newest_fork.get_mut();
            if saved.len() <= segment / 64 {
                saved.resize(segment / 64 + 1, 0);
            }
            saved[segment / 64] |= 1 << (segment % 64);
            return;
        }
        // SAFETY: Only the vector writes the log, and `&mut self` rules out any other reference into it.
        let saved = unsafe { &mut *log.saved.get() };
        // SAFETY: `index` is a row of the storage, which the caller checked.
        saved
            .entry(index)
            .or_insert_with(|| Box::new([unsafe { (*self.rows.add(index)).clone() }]));
    }

    #[cold]
    fn get_mut_on_fork(&mut self, index: usize) -> Option<&mut T> {
        let fork = self.fork.as_deref()?;
        if index >= fork.len {
            return None;
        }
        if !fork.own.contains_key(&index) {
            let row = self.get_on_fork(index).cloned().unwrap_or_default();
            self.fork.as_deref_mut()?.own.insert(index, row);
        }
        self.fork.as_deref_mut()?.own.get_mut(&index)
    }

    pub(crate) fn push(&mut self, value: T) {
        if let Some(fork) = self.fork.as_deref_mut() {
            fork.own.insert(fork.len, value);
            fork.len += 1;
            return;
        }
        self.storage_mut().push(value);
        self.note_storage_moved();
    }

    /// Makes room for the vector to grow to `len` rows without moving its rows again, for rows about to be written.
    pub(crate) fn reserve_for_len(&mut self, len: usize) {
        if self.fork.is_some() {
            return;
        }
        let rows = self.storage_mut();
        let capacity = rows.capacity();
        let additional = len.saturating_sub(rows.len());
        rows.reserve(additional);
        if rows.capacity() == capacity {
            return;
        }
        // OPTIMIZATION: The room is fresh memory for rows about to be written, which take its pages at once rather
        //               than a fault at a time.
        ak::populate_room_for_write(rows, additional);
        self.note_storage_moved();
    }

    /// Makes room in the log of the newest fork, while a fork reads it, for the old values of `rows` more rows, which
    /// writes are about to save: the log then grows once rather than as they come.
    pub(crate) fn reserve_log(&mut self, rows: usize) {
        if self.rows_at_newest_fork.get() == 0 {
            return;
        }
        let Some(log) = self.live_log() else {
            return;
        };
        // SAFETY: Only the vector writes the log, and `&mut self` rules out any other reference into it. The rows it
        //         saved are boxes of their own, which growing the log does not move.
        let entries = if Self::SAVES_SEGMENTS {
            rows.min(log.rows_at_fork.div_ceil(ROWS_PER_SEGMENT))
        } else {
            rows
        };
        unsafe { &mut *log.saved.get() }.reserve(entries);
    }

    /// Appends `rows`, in order.
    pub(crate) fn extend_from_slice(&mut self, rows: &[T]) {
        if let Some(fork) = self.fork.as_deref_mut() {
            for row in rows {
                fork.own.insert(fork.len, row.clone());
                fork.len += 1;
            }
            return;
        }
        self.storage_mut().extend_from_slice(rows);
        self.note_storage_moved();
    }

    pub(crate) fn pop(&mut self) -> Option<T> {
        if let Some(fork) = self.fork.as_deref() {
            let index = fork.len.checked_sub(1)?;
            let row = self.get_on_fork(index).cloned();
            let fork = self.fork.as_deref_mut()?;
            fork.own.remove(&index);
            fork.len = index;
            return row;
        }
        let index = self.len.checked_sub(1)?;
        self.save_row(index);
        let row = self.storage_mut().pop();
        self.note_storage_moved();
        row
    }

    /// Grows the vector to `len` rows, the new ones default, or truncates it to them.
    pub(crate) fn resize_with_default(&mut self, len: usize) {
        while self.len() > len {
            self.pop();
        }
        if let Some(fork) = self.fork.as_deref_mut() {
            // NB: A row the fork was made with reads as what it holds there, so it takes a default of its own.
            while fork.len < len.min(fork.log.rows_at_fork) {
                fork.own.insert(fork.len, T::default());
                fork.len += 1;
            }
            fork.len = fork.len.max(len);
            return;
        }
        self.storage_mut().resize_with(len, T::default);
        self.note_storage_moved();
    }

    /// Empties the vector, answering its rows in order.
    pub(crate) fn take_rows(&mut self) -> Vec<T> {
        if self.fork.is_some() || self.live_log().is_some() {
            let rows = self.iter().cloned().collect();
            // NB: What a fork reads stays as it was, in the rows this vector leaves behind.
            *self = Self::default();
            return rows;
        }
        let rows = std::mem::take(self.storage_mut());
        self.note_storage_moved();
        rows
    }

    /// The bytes of the rows themselves, without what they hold on the heap.
    pub(crate) fn row_bytes(&self) -> usize {
        // SAFETY: Holding the slot rules out a write to the storage.
        unsafe { &*self.storage.get() }.capacity() * size_of::<T>()
    }

    /// Every row, in order.
    pub(crate) fn iter(&self) -> impl Iterator<Item = &T> {
        (0..self.len()).filter_map(|index| self.get(index))
    }
}

/// A fork of the vector, which shares its rows. A fork of a fork takes rows of its own.
impl<T: Clone + Default> Clone for ForkVec<T> {
    fn clone(&self) -> Self {
        if self.fork.is_some() {
            return self.iter().cloned().collect();
        }
        let log = Arc::new(Log {
            rows_at_fork: self.len,
            saved: UnsafeCell::default(),
            next: UnsafeCell::new(None),
        });
        // SAFETY: The thread that holds the slot makes the fork, so nothing else reaches the log. See the module
        // documentation.
        let current = unsafe { &mut *self.log.get() };
        let previous = current.replace(Arc::downgrade(&log));
        self.rows_at_newest_fork.set(self.len);
        if Self::SAVES_SEGMENTS {
            // SAFETY: As above.
            // NB: The words take their room as segments are saved, so that a fork allocates nothing for them.
            unsafe { &mut *self.segments_saved_since_newest_fork.get() }.fill(0);
        }
        if let Some(previous) = previous.as_ref().and_then(Weak::upgrade) {
            // SAFETY: As above.
            unsafe { *previous.next.get() = Some(Arc::clone(&log)) };
        }
        Self {
            rows: std::ptr::null_mut(),
            len: 0,
            storage: Arc::clone(&self.storage),
            log: UnsafeCell::new(None),
            rows_at_newest_fork: Cell::new(0),
            segments_saved_since_newest_fork: UnsafeCell::default(),
            fork: Some(Box::new(ForkSide {
                log,
                own: FastMap::default(),
                len: self.len,
                default_row: OnceLock::new(),
            })),
        }
    }
}

impl<T: Clone + Default> FromIterator<T> for ForkVec<T> {
    fn from_iter<I: IntoIterator<Item = T>>(rows: I) -> Self {
        let mut vector = Self::default();
        *vector.storage_mut() = rows.into_iter().collect();
        vector.note_storage_moved();
        vector
    }
}

impl<T: Clone + Default> std::ops::Index<usize> for ForkVec<T> {
    type Output = T;

    #[inline]
    fn index(&self, index: usize) -> &T {
        self.get(index).expect("index within the vector")
    }
}

impl<T: Clone + Default> std::ops::IndexMut<usize> for ForkVec<T> {
    #[inline]
    fn index_mut(&mut self, index: usize) -> &mut T {
        self.get_mut(index).expect("index within the vector")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rows(vector: &ForkVec<u32>) -> Vec<u32> {
        vector.iter().copied().collect()
    }

    #[test]
    fn a_fork_sees_the_vector_as_it_was_when_forked() {
        let mut vector: ForkVec<u32> = [1, 2, 3].into_iter().collect();
        let fork = vector.clone();
        vector[0] = 10;
        vector.push(4);
        vector.pop();
        vector.pop();
        vector.push(30);
        assert_eq!(rows(&vector), [10, 2, 30]);
        assert_eq!(rows(&fork), [1, 2, 3]);
    }

    #[test]
    fn a_fork_writes_its_own_rows() {
        let mut vector: ForkVec<u32> = [1, 2, 3].into_iter().collect();
        let mut fork = vector.clone();
        fork[1] = 20;
        fork.push(4);
        vector[2] = 30;
        assert_eq!(rows(&fork), [1, 20, 3, 4]);
        assert_eq!(rows(&vector), [1, 2, 30]);
        assert_eq!(fork.pop(), Some(4));
        fork.resize_with_default(5);
        assert_eq!(rows(&fork), [1, 20, 3, 0, 0]);
    }

    #[test]
    fn a_fork_grows_past_its_rows_without_taking_any() {
        let mut vector: ForkVec<u32> = [1, 2, 3].into_iter().collect();
        let mut fork = vector.clone();
        vector.push(4);
        fork.resize_with_default(1_000_000);
        assert!(fork.fork.as_ref().unwrap().own.is_empty());
        assert_eq!(fork.len(), 1_000_000);
        assert_eq!((fork[2], fork[3], fork[999_999]), (3, 0, 0));
        assert!(!fork.shares_row(3));
        fork[500] = 5;
        assert_eq!(fork[500], 5);
        fork.resize_with_default(2);
        assert_eq!(rows(&fork), [1, 2]);
        fork.resize_with_default(600);
        assert_eq!((fork[2], fork[500]), (0, 0));
    }

    #[test]
    fn a_fork_copies_a_row_the_first_time_it_writes_it() {
        use std::sync::atomic::{AtomicUsize, Ordering};

        #[derive(Default)]
        struct Counted(Arc<AtomicUsize>);

        impl Clone for Counted {
            fn clone(&self) -> Self {
                self.0.fetch_add(1, Ordering::Relaxed);
                Self(Arc::clone(&self.0))
            }
        }

        let copies = Arc::new(AtomicUsize::new(0));
        let vector: ForkVec<Counted> = [Counted(Arc::clone(&copies))].into_iter().collect();
        let mut fork = vector.clone();
        fork.get_mut(0);
        fork.get_mut(0);
        assert_eq!(copies.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn an_earlier_fork_reads_through_the_logs_of_later_ones() {
        let mut vector: ForkVec<u32> = [1, 2].into_iter().collect();
        let first = vector.clone();
        vector[0] = 10;
        let second = vector.clone();
        vector[0] = 100;
        vector[1] = 200;
        assert_eq!(rows(&first), [1, 2]);
        assert_eq!(rows(&second), [10, 2]);
        drop(second);
        vector[1] = 2000;
        assert_eq!(rows(&first), [1, 2]);
    }

    #[test]
    fn the_vector_stops_logging_once_its_forks_are_gone() {
        let mut vector: ForkVec<u32> = [1].into_iter().collect();
        drop(vector.clone());
        vector[0] = 2;
        assert!(vector.log.get_mut().is_none());
    }

    #[test]
    fn the_rows_a_log_saved_go_with_its_last_fork() {
        let row = Arc::new(1);
        let mut vector: ForkVec<Arc<u32>> = [Arc::clone(&row)].into_iter().collect();
        let fork = vector.clone();
        vector[0] = Arc::default();
        assert_eq!(Arc::strong_count(&row), 2);
        drop(fork);
        assert_eq!(Arc::strong_count(&row), 1);
    }

    #[test]
    fn a_fork_shares_a_row_until_the_vector_writes_it() {
        // NB: A row that owns something is saved alone. See `a_fork_reads_the_segments_the_vector_saved`.
        let mut vector: ForkVec<Option<Arc<u32>>> = (0..4).map(|row| Some(Arc::new(row))).collect();
        assert!(!vector.shares_row(1));
        let fork = vector.clone();
        assert!(vector.shares_row(1));
        let mut other = vector.clone();
        assert!(other.shares_row(1));
        other[1] = None;
        assert!(!other.shares_row(1));
        drop(other);
        vector[1] = None;
        assert!(!vector.shares_row(1));
        assert!(vector.shares_row(2));
        vector.push(None);
        assert!(!vector.shares_row(4));
        drop(fork);
        assert!(!vector.shares_row(2));
    }

    #[test]
    fn a_fork_reads_the_segments_the_vector_saved() {
        let mut vector: ForkVec<u32> = (0..1000).collect();
        let first = vector.clone();
        for row in 300..400 {
            vector[row] = 0;
        }
        let second = vector.clone();
        vector[310] = 1;
        vector[600] = 2;
        while vector.len() > 500 {
            vector.pop();
        }
        vector.push(3);
        assert!(!vector.shares_row(256) && vector.shares_row(0));
        assert_eq!((first[310], first[399], first[600], first[999]), (310, 399, 600, 999));
        assert_eq!((second[300], second[310], second[400], second[600]), (0, 0, 400, 600));
        assert_eq!((vector[310], vector[500], vector.len()), (1, 3, 501));
    }

    #[test]
    fn taking_the_rows_leaves_a_fork_its_rows() {
        let mut vector: ForkVec<u32> = [1, 2].into_iter().collect();
        let fork = vector.clone();
        assert_eq!(vector.take_rows(), [1, 2]);
        vector.push(3);
        assert_eq!(rows(&fork), [1, 2]);
        assert_eq!(rows(&vector), [3]);
    }
}
