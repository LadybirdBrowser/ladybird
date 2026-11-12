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

/// The name table index that the field `field` of an instruction in `bytecode` holds, if it holds one. An index too
/// large for the field is in the WideNameIndices before the instruction, which only the bytecode can reach.
fn name_index_in(field: &[u8; 2], bytecode: &[u8]) -> Option<usize> {
    let at = field.as_ptr().addr().wrapping_sub(bytecode.as_ptr().addr());
    assert!(at < bytecode.len(), "the name table index is a field of the bytecode");
    super::operand::read_optional_name_index(bytecode, at).map(|index| index as usize)
}

macro_rules! define_table_index {
    ($($name:ident),*) => {
        $(
            /// The field of an instruction that holds an index into a name table of its executable.
            #[derive(Debug)]
            #[repr(C, packed)]
            pub struct $name([u8; 2]);

            impl $name {
                /// The index the field holds, which is a field of an instruction in `bytecode`.
                pub fn index_in(&self, bytecode: &[u8]) -> usize {
                    name_index_in(&self.0, bytecode).expect("the field holds a name table index")
                }
            }
        )*
    };
}

define_table_index!(IdentifierTableIndex, PropertyKeyTableIndex, StringTableIndex);

/// The field of an instruction that holds an index into a name table of its executable, if it holds one.
#[repr(C, packed)]
pub struct OptionalIndex<T>([u8; 2], PhantomData<T>);

impl<T> OptionalIndex<T> {
    /// The index the field holds, which is a field of an instruction in `bytecode`.
    pub fn index_in(&self, bytecode: &[u8]) -> Option<usize> {
        name_index_in(&self.0, bytecode)
    }
}
