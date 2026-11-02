/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::marker::PhantomData;

/// A heap buffer whose element pointer and length the interpreter reads directly, which a Vec does not promise to
/// keep at fixed offsets.
#[repr(C)]
pub struct InterpreterBuffer<T> {
    pub data: Cell<*mut T>,
    pub size: Cell<u32>,
    pub capacity: Cell<u32>,
}

const _: () = assert!(size_of::<InterpreterBuffer<u8>>() == 16);

/// A boxed slice in one word, which the interpreter can read the elements of directly: the pointer to the first
/// element, or null if there are none. The length is in a word in front of the elements.
#[repr(transparent)]
pub struct ThinSlice<T> {
    pub data: *mut T,
    _elements: PhantomData<T>,
}

const _: () = assert!(size_of::<ThinSlice<u8>>() == 8);

impl<T> ThinSlice<T> {
    pub const fn empty() -> Self {
        Self::from_data(core::ptr::null_mut())
    }

    /// `data` must be null, or point at elements that have their length in front of them and that this slice owns.
    pub(crate) const fn from_data(data: *mut T) -> Self {
        Self {
            data,
            _elements: PhantomData,
        }
    }
}

impl<T> Default for ThinSlice<T> {
    fn default() -> Self {
        Self::empty()
    }
}
