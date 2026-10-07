/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a fork of a document's render state takes of each part of it.
//!
//! A fork is a copy of the render state as one frame left it, for the render clock to hover and present on while the
//! host goes on writing its own. Most parts copy as they are. A [`ForkReset`] part, such as a cache that holds nothing a
//! result depends on, starts the fork empty.

use std::ops::{Deref, DerefMut};

/// A part of the render state that a fork starts anew, as its default.
#[derive(Default)]
pub(crate) struct ForkReset<T: Default>(T);

impl<T: Default> ForkReset<T> {
    pub(crate) fn new(value: T) -> Self {
        Self(value)
    }
}

impl<T: Default> Clone for ForkReset<T> {
    fn clone(&self) -> Self {
        Self(T::default())
    }
}

impl<T: Default> Deref for ForkReset<T> {
    type Target = T;

    fn deref(&self) -> &T {
        &self.0
    }
}

impl<T: Default> DerefMut for ForkReset<T> {
    fn deref_mut(&mut self) -> &mut T {
        &mut self.0
    }
}

/// An atomic that a fork copies with the value it holds.
#[derive(Default, Debug)]
pub(crate) struct ForkCopied<T>(T);

impl<T> ForkCopied<T> {
    pub(crate) const fn new(value: T) -> Self {
        Self(value)
    }
}

impl<T> Deref for ForkCopied<T> {
    type Target = T;

    fn deref(&self) -> &T {
        &self.0
    }
}

impl<T> DerefMut for ForkCopied<T> {
    fn deref_mut(&mut self) -> &mut T {
        &mut self.0
    }
}

macro_rules! fork_copied_atomic {
    ($($atomic:ty),+) => {
        $(
            impl Clone for ForkCopied<$atomic> {
                fn clone(&self) -> Self {
                    Self(<$atomic>::new(self.0.load(std::sync::atomic::Ordering::Relaxed)))
                }
            }
        )+
    };
}

fork_copied_atomic!(
    std::sync::atomic::AtomicBool,
    std::sync::atomic::AtomicU8,
    std::sync::atomic::AtomicU32,
    std::sync::atomic::AtomicU64
);

/// A mutex that a fork copies with what it holds.
#[derive(Default, Debug)]
pub(crate) struct ForkLocked<T>(std::sync::Mutex<T>);

impl<T> ForkLocked<T> {
    pub(crate) const fn new(value: T) -> Self {
        Self(std::sync::Mutex::new(value))
    }
}

impl<T: Clone> Clone for ForkLocked<T> {
    fn clone(&self) -> Self {
        Self(std::sync::Mutex::new(
            self.0
                .lock()
                .expect("a forked lock is never held across a panic")
                .clone(),
        ))
    }
}

impl<T> Deref for ForkLocked<T> {
    type Target = std::sync::Mutex<T>;

    fn deref(&self) -> &std::sync::Mutex<T> {
        &self.0
    }
}

impl<T> DerefMut for ForkLocked<T> {
    fn deref_mut(&mut self) -> &mut std::sync::Mutex<T> {
        &mut self.0
    }
}

/// A part of the render state that a fork shares with the state it forked until either writes it: reading costs one
/// indirection, a fork one reference count, and the first write on either side after a fork copies the part.
#[derive(Default, Debug)]
pub(crate) struct ForkShared<T: Clone>(std::sync::Arc<T>);

impl<T: Clone> ForkShared<T> {
    pub(crate) fn new(value: T) -> Self {
        Self(std::sync::Arc::new(value))
    }
}

impl<T: Clone> Clone for ForkShared<T> {
    fn clone(&self) -> Self {
        Self(std::sync::Arc::clone(&self.0))
    }
}

impl<T: Clone> Deref for ForkShared<T> {
    type Target = T;

    fn deref(&self) -> &T {
        &self.0
    }
}

impl<T: Clone> DerefMut for ForkShared<T> {
    fn deref_mut(&mut self) -> &mut T {
        std::sync::Arc::make_mut(&mut self.0)
    }
}
