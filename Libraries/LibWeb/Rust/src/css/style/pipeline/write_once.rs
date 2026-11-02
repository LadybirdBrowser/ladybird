/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A slot one thread writes once, which other threads read after it is written: what a the engine
//! pass keeps per element, written by the thread styling the element and read by those styling its
//! descendants. Writing it takes one atomic compare-exchange and one store, where a `OnceLock` goes
//! through `Once`.

use std::cell::UnsafeCell;
use std::mem::MaybeUninit;
use std::sync::atomic::{AtomicU8, Ordering};

const EMPTY: u8 = 0;
const WRITING: u8 = 1;
const WRITTEN: u8 = 2;

pub(super) struct WriteOnce<T> {
    state: AtomicU8,
    value: UnsafeCell<MaybeUninit<T>>,
}

impl<T> Default for WriteOnce<T> {
    fn default() -> Self {
        Self {
            state: AtomicU8::new(EMPTY),
            value: UnsafeCell::new(MaybeUninit::uninit()),
        }
    }
}

impl<T> WriteOnce<T> {
    pub(super) fn get(&self) -> Option<&T> {
        // SAFETY: A written value is initialized and never written again, and the acquire load
        //         sees the writes that made it.
        (self.state.load(Ordering::Acquire) == WRITTEN).then(|| unsafe { (*self.value.get()).assume_init_ref() })
    }

    /// Write the slot, or hand the value back where it was written before.
    pub(super) fn set(&self, value: T) -> Result<(), T> {
        if self
            .state
            .compare_exchange(EMPTY, WRITING, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            return Err(value);
        }
        // SAFETY: Only the thread that moved the slot from empty to writing writes it, and no
        //         reader reads it before it is marked written.
        unsafe { (*self.value.get()).write(value) };
        self.state.store(WRITTEN, Ordering::Release);
        Ok(())
    }
}

impl<T> WriteOnce<T> {
    /// Take the value, where the slot was written, leaving it empty.
    pub(super) fn take(&mut self) -> Option<T> {
        let written = *self.state.get_mut() == WRITTEN;
        *self.state.get_mut() = EMPTY;
        // SAFETY: A written value is initialized, and marking the slot empty keeps it from being
        //         read or dropped again.
        written.then(|| unsafe { self.value.get_mut().assume_init_read() })
    }
}

impl<T> Drop for WriteOnce<T> {
    fn drop(&mut self) {
        if *self.state.get_mut() == WRITTEN {
            // SAFETY: The value was written and is dropped once, with the slot.
            unsafe { self.value.get_mut().assume_init_drop() };
        }
    }
}

// SAFETY: The slot hands out shared references to its value across threads and drops it on the
//         thread dropping the slot, as a `OnceLock` does.
unsafe impl<T: Send + Sync> Sync for WriteOnce<T> {}
unsafe impl<T: Send> Send for WriteOnce<T> {}
