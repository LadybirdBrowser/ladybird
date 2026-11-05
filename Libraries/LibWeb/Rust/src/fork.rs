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

/// The mark of the render state the host writes, which a fork of it, starting the mark anew as
/// [`ForkReset`], does not carry: `ForkReset<IsOriginal>` tells the state from its forks.
#[derive(Default, Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) struct IsOriginal(bool);

impl IsOriginal {
    pub(crate) const ORIGINAL: Self = Self(true);
}

impl ForkReset<IsOriginal> {
    /// The mark of the render state the host writes.
    pub(crate) fn original() -> Self {
        Self(IsOriginal::ORIGINAL)
    }

    /// Whether this is the render state the host writes, and not a fork of it.
    pub(crate) fn is_original(&self) -> bool {
        self.0 == IsOriginal::ORIGINAL
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

    /// The part, for writing, where no fork shares it.
    pub(crate) fn unshared_mut(&mut self) -> Option<&mut T> {
        std::sync::Arc::get_mut(&mut self.0)
    }

    /// Replaces the part, in place where no fork shares it.
    pub(crate) fn assign(&mut self, value: T) {
        match std::sync::Arc::get_mut(&mut self.0) {
            Some(part) => *part = value,
            None => self.0 = std::sync::Arc::new(value),
        }
    }

    /// Takes the part, leaving its default: the part moves out as it is where no fork shares it, and is copied where
    /// one does.
    pub(crate) fn take(&mut self) -> T
    where
        T: Default,
    {
        std::sync::Arc::unwrap_or_clone(std::mem::take(&mut self.0))
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
