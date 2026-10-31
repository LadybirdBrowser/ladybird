/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Runtime/ByteLength.h: a byte or element length that is auto (tracks a resizable buffer) or detached.

use crate::layout::object::BYTE_LENGTH_LENGTH_INDEX;
pub use crate::layout::object::ByteLengthSlot;

/// Mirrors JS::ByteLength.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ByteLength {
    Auto,
    Detached,
    Length(usize),
}

const BYTE_LENGTH_AUTO_INDEX: u8 = 0;
const BYTE_LENGTH_DETACHED_INDEX: u8 = 1;

impl ByteLength {
    pub fn auto_() -> Self {
        Self::Auto
    }

    pub fn detached() -> Self {
        Self::Detached
    }

    pub fn is_auto(self) -> bool {
        self == Self::Auto
    }

    pub fn is_detached(self) -> bool {
        self == Self::Detached
    }

    pub fn length(self) -> usize {
        let Self::Length(length) = self else {
            panic!("ByteLength::length() of a byte length that has no length");
        };
        length
    }
}

impl From<usize> for ByteLength {
    fn from(length: usize) -> Self {
        Self::Length(length)
    }
}

impl ByteLengthSlot {
    pub fn new(byte_length: ByteLength) -> Self {
        let slot = Self {
            length: 0.into(),
            alternative_index: 0.into(),
        };
        slot.set(byte_length);
        slot
    }

    pub fn get(&self) -> ByteLength {
        match self.alternative_index.get() {
            BYTE_LENGTH_AUTO_INDEX => ByteLength::Auto,
            BYTE_LENGTH_DETACHED_INDEX => ByteLength::Detached,
            BYTE_LENGTH_LENGTH_INDEX => ByteLength::Length(self.length.get()),
            index => unreachable!("{index} is not an alternative of a ByteLength"),
        }
    }

    pub fn set(&self, byte_length: ByteLength) {
        let (index, length) = match byte_length {
            ByteLength::Auto => (BYTE_LENGTH_AUTO_INDEX, 0),
            ByteLength::Detached => (BYTE_LENGTH_DETACHED_INDEX, 0),
            ByteLength::Length(length) => (BYTE_LENGTH_LENGTH_INDEX, length),
        };
        self.length.set(length);
        self.alternative_index.set(index);
    }
}
