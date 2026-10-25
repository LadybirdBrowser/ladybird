/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The layout of smallvec 1.x: the spill is a bare pointer, so the type structure does not show that it owns items.

use core::mem::{ManuallyDrop, MaybeUninit};
use core::ptr::NonNull;

pub trait Array {
    type Item;
}

impl<T, const N: usize> Array for [T; N] {
    type Item = T;
}

union SmallVecData<A: Array> {
    inline: ManuallyDrop<MaybeUninit<A>>,
    heap: (NonNull<A::Item>, usize),
}

pub struct SmallVec<A: Array> {
    capacity: usize,
    data: SmallVecData<A>,
}

impl<A: Array> SmallVec<A> {
    pub fn new() -> Self {
        Self {
            capacity: 0,
            data: SmallVecData {
                inline: ManuallyDrop::new(MaybeUninit::uninit()),
            },
        }
    }
}
