/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::marker::PhantomData;

use libjs_abi::value as nan_box;

/// A NaN-boxed JS::Value. Only libjs_rust makes values from bits, as bits can name a cell that is not there. Values
/// stay on the thread of their VM, as cells do.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(transparent)]
pub struct Value(pub(crate) u64, PhantomData<*const ()>);

impl Value {
    pub const EMPTY: Value = Value::from_bits(nan_box::EMPTY_VALUE);
    pub const UNDEFINED: Value = Value::from_bits(nan_box::UNDEFINED_VALUE);
    pub const NULL: Value = Value::from_bits(nan_box::NULL_VALUE);
    pub const FALSE: Value = Value::from_bits(nan_box::FALSE_VALUE);
    pub const TRUE: Value = Value::from_bits(nan_box::TRUE_VALUE);

    pub(crate) const fn from_bits(bits: u64) -> Value {
        Value(bits, PhantomData)
    }

    /// # Safety
    ///
    /// `bits` must be a NaN-boxed value, such as one C++ code hands over, and a cell it names must be live.
    pub const unsafe fn from_raw(bits: u64) -> Value {
        Value::from_bits(bits)
    }

    /// The NaN-boxed bits, for handing the value to C++ code.
    pub const fn to_raw(self) -> u64 {
        self.0
    }
}

const _: () = assert!(size_of::<Value>() == 8);
