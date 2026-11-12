/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::marker::PhantomData;

use crate::layout::property_lookup_cache::EnvironmentCoordinate;

/// The bytes every instruction starts with.
#[repr(C)]
pub struct InstructionHeader {
    pub opcode: u8,
}

/// An operand: the index of its slot in the frame.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C, packed)]
pub struct Operand(pub u8);

impl Operand {
    pub fn index(self) -> usize {
        self.0 as usize
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C, packed)]
pub struct OptionalOperand(pub u8);

impl OptionalOperand {
    const NONE: u8 = u8::MAX;

    pub fn get(self) -> Option<Operand> {
        (self.0 != Self::NONE).then_some(Operand(self.0))
    }
}

/// The bytecode offset of a basic block.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C, packed)]
pub struct Label(pub u32);

#[derive(Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct OptionalLabel {
    pub label: Label,
    pub has_value: bool,
}

impl OptionalLabel {
    pub fn get(self) -> Option<Label> {
        self.has_value.then_some(self.label)
    }
}

/// The coordinate of a binding as an instruction holds it: a byte of hops and two bytes of binding index. The generator
/// looks bindings up by name where a coordinate does not fit.
#[derive(Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct BytecodeEnvironmentCoordinate {
    hops: u8,
    index: [u8; 2],
}

impl BytecodeEnvironmentCoordinate {
    pub fn get(self) -> EnvironmentCoordinate {
        EnvironmentCoordinate {
            hops: u32::from(self.hops),
            index: u32::from(u16::from_ne_bytes(self.index)),
        }
    }
}

macro_rules! define_table_index {
    ($($name:ident),*) => {
        $(
            #[derive(Clone, Copy, Debug, PartialEq, Eq)]
            #[repr(C, packed)]
            pub struct $name(pub [u8; 3]);

            impl $name {
                pub fn from_index(index: u32) -> Self {
                    Self(super::operand::name_index_bytes(index))
                }

                pub fn index(self) -> usize {
                    let [low, middle, high] = self.0;
                    u32::from_le_bytes([low, middle, high, 0]) as usize
                }
            }

            impl From<[u8; 3]> for $name {
                fn from(bytes: [u8; 3]) -> Self {
                    Self(bytes)
                }
            }
        )*
    };
}

define_table_index!(IdentifierTableIndex, PropertyKeyTableIndex, StringTableIndex);

/// A table index that may be absent, encoded with all bits set.
#[repr(C, packed)]
pub struct OptionalIndex<T>(pub [u8; 3], PhantomData<T>);

impl<T: From<[u8; 3]>> OptionalIndex<T> {
    pub fn get(&self) -> Option<T> {
        let raw = self.0;
        (raw != [u8::MAX; 3]).then(|| T::from(raw))
    }
}
