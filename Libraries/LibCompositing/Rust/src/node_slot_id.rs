/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The identity of a layout node's slot in the arena: a 24-bit index and a generation, which the
//! visual context tree carries to name the box a node was built for.

pub const INVALID_NODE_SLOT_INDEX: u32 = u32::MAX;

const NODE_SLOT_INDEX_BITS: u32 = 24;
const NODE_SLOT_INDEX_MASK: u32 = (1 << NODE_SLOT_INDEX_BITS) - 1;
/// cbindgen:ignore
pub const MAX_NODE_SLOT_COUNT: u32 = NODE_SLOT_INDEX_MASK;

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
#[repr(C)]
pub struct NodeSlotId {
    pub index: u32,
}

impl NodeSlotId {
    pub const INVALID: Self = Self {
        index: INVALID_NODE_SLOT_INDEX,
    };

    pub fn new(index: u32, generation: u8) -> Self {
        assert!(
            index < MAX_NODE_SLOT_COUNT,
            "layout node arena exhausted its 24-bit slot index space"
        );
        assert_ne!(generation, 0, "layout node arena slot generation must be nonzero");
        Self {
            index: index | (u32::from(generation) << NODE_SLOT_INDEX_BITS),
        }
    }

    pub fn slot_index(self) -> u32 {
        self.index & NODE_SLOT_INDEX_MASK
    }

    pub fn generation(self) -> u8 {
        (self.index >> NODE_SLOT_INDEX_BITS) as u8
    }

    pub fn is_invalid(self) -> bool {
        self == Self::INVALID
    }
}

impl Default for NodeSlotId {
    fn default() -> Self {
        Self::INVALID
    }
}
