/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::marker::PhantomData;

/// The bytes every instruction starts with.
#[repr(C)]
pub struct InstructionHeader {
    pub opcode: u8,
    pub strict: bool,
}

/// An operand, encoded as a 3-bit kind above the index of its slot in the frame.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(transparent)]
pub struct Operand(pub u32);

impl Operand {
    const INDEX_MASK: u32 = 0x1fff_ffff;

    pub fn index(self) -> usize {
        (self.0 & Self::INDEX_MASK) as usize
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(transparent)]
pub struct OptionalOperand(pub u32);

impl OptionalOperand {
    const NONE: u32 = u32::MAX;

    pub fn get(self) -> Option<Operand> {
        (self.0 != Self::NONE).then_some(Operand(self.0))
    }
}

/// The bytecode offset of a basic block.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(transparent)]
pub struct Label(pub u32);

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct OptionalLabel {
    pub label: Label,
    pub has_value: bool,
}

impl OptionalLabel {
    pub fn get(self) -> Option<Label> {
        self.has_value.then_some(self.label)
    }
}

macro_rules! define_table_index {
    ($($name:ident),*) => {
        $(
            #[derive(Clone, Copy, Debug, PartialEq, Eq)]
            #[repr(transparent)]
            pub struct $name(pub u32);

            impl From<u32> for $name {
                fn from(index: u32) -> Self {
                    Self(index)
                }
            }
        )*
    };
}

define_table_index!(
    IdentifierTableIndex,
    PropertyKeyTableIndex,
    StringTableIndex,
    RegexTableIndex
);

/// A table index that may be absent, encoded with all bits set.
#[repr(transparent)]
pub struct OptionalIndex<T>(pub u32, PhantomData<T>);

impl<T: From<u32>> OptionalIndex<T> {
    pub fn get(&self) -> Option<T> {
        (self.0 != u32::MAX).then(|| T::from(self.0))
    }
}
