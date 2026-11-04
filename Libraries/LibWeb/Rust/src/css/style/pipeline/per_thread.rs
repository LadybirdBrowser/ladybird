/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::sync::Mutex;

/// What each thread a pass runs on keeps between passes, by thread, the calling thread's last: a slot is made the first
/// time its thread asks for it, so a document, or a fork of its render state, holds only what its threads used.
pub(super) struct PerThread<T>([std::sync::OnceLock<Box<Mutex<T>>>; crate::style_layout_pool::MAX_THREAD_COUNT + 1]);

impl<T> Default for PerThread<T> {
    fn default() -> Self {
        Self(std::array::from_fn(|_| std::sync::OnceLock::new()))
    }
}

impl<T: Default> PerThread<T> {
    /// What thread `index` keeps, made where it is not yet, or none past the last thread.
    pub(super) fn get(&self, index: usize) -> Option<&Mutex<T>> {
        self.0.get(index).map(|slot| &**slot.get_or_init(Box::default))
    }

    /// What the threads that asked keep.
    pub(super) fn made(&self) -> impl Iterator<Item = &Mutex<T>> {
        self.0.iter().filter_map(|slot| slot.get().map(|made| &**made))
    }

    /// How many threads may keep anything.
    pub(super) fn len(&self) -> usize {
        self.0.len()
    }
}
