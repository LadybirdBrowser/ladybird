/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_abi::value as nan_box;

/// A NaN-boxed JS::Value.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(transparent)]
pub struct Value(pub u64);

impl Value {
    pub const EMPTY: Value = Value(nan_box::EMPTY_VALUE);
    pub const UNDEFINED: Value = Value(nan_box::UNDEFINED_VALUE);
    pub const NULL: Value = Value(nan_box::NULL_VALUE);
    pub const FALSE: Value = Value(nan_box::FALSE_VALUE);
    pub const TRUE: Value = Value(nan_box::TRUE_VALUE);
}

const _: () = assert!(size_of::<Value>() == 8);
