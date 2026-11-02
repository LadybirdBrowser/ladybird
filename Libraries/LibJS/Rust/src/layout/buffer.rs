/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

/// A heap buffer whose element pointer and length the interpreter reads directly, which a Vec does not promise to
/// keep at fixed offsets.
#[repr(C)]
pub struct InterpreterBuffer<T> {
    pub data: Cell<*mut T>,
    pub size: Cell<u32>,
    pub capacity: Cell<u32>,
}

const _: () = assert!(size_of::<InterpreterBuffer<u8>>() == 16);
