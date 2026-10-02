/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The layout tree's shape as the paint side reads it.
//!
//! A node's links, kind, paint facts and style live in its [`NodeData`], which tree builds, style
//! installs and invalidation write in place. The fields the paint side reads of a node are
//! [`ShapeCell`]s. They read like a `Cell`, but only a [`ShapeWriter`] or `&mut` writes one, and only
//! a [`Chunk`] hands out either, marking the node in the chunk when a write changes it. So the
//! chunk knows which of its nodes the paint side may see differently since it last looked, and a
//! write it would miss does not compile.

use super::layout_node_arena::SLOTS_PER_CHUNK;
use super::node_data::{NodeData, NodeKind, NodeSlotId, StylePayloadsRef};
use std::cell::Cell;
use std::ops::Deref;

/// A field of a node the paint side reads. It reads like a `Cell`, and is written through a
/// [`ShapeWriter`] or through `&mut`, both of which mark the node in its chunk.
#[repr(transparent)]
pub(crate) struct ShapeCell<T>(Cell<T>);

impl<T: Copy> ShapeCell<T> {
    pub(crate) const fn new(value: T) -> Self {
        Self(Cell::new(value))
    }

    #[inline]
    pub(crate) fn get(&self) -> T {
        self.0.get()
    }

    #[inline]
    pub(crate) fn get_mut(&mut self) -> &mut T {
        self.0.get_mut()
    }

    #[inline]
    fn set(&self, value: T) {
        self.0.set(value);
    }
}

/// Writes the shape of one node, marking it in its chunk when a write changes it. It reads as the
/// node's [`NodeData`], so the fields the paint side does not read are written through it as
/// before.
pub(crate) struct ShapeWriter<'a> {
    data: &'a NodeData,
    written_rows: &'a Cell<u64>,
    row_bit: u64,
}

impl Deref for ShapeWriter<'_> {
    type Target = NodeData;

    fn deref(&self) -> &NodeData {
        self.data
    }
}

impl ShapeWriter<'_> {
    /// Writes a field, marking the node only for a value the field does not already hold.
    #[inline]
    fn write<T: Copy + PartialEq>(&self, field: &ShapeCell<T>, value: T) {
        if field.get() != value {
            self.written_rows.set(self.written_rows.get() | self.row_bit);
            field.set(value);
        }
    }

    pub(crate) fn set_parent(&self, parent: NodeSlotId) {
        self.write(&self.data.parent, parent);
    }

    pub(crate) fn set_first_child(&self, child: NodeSlotId) {
        self.write(&self.data.first_child, child);
    }

    pub(crate) fn set_next_sibling(&self, sibling: NodeSlotId) {
        self.write(&self.data.next_sibling, sibling);
    }

    pub(crate) fn set_kind(&self, kind: NodeKind) {
        self.write(&self.data.kind, kind);
    }

    pub(crate) fn set_generated_for(&self, generated_for: u8) {
        self.write(&self.data.generated_for, generated_for);
    }

    pub(crate) fn set_flags(&self, flags: u32) {
        self.write(&self.data.flags, flags);
    }

    pub(crate) fn set_dom_paint_facts(&self, facts: u8) {
        self.write(&self.data.dom_paint_facts, facts);
    }

    pub(crate) fn set_compositor_animation_frame_kinds(&self, kinds: u8) {
        self.write(&self.data.compositor_animation_frame_kinds, kinds);
    }

    pub(crate) fn set_style(&self, style: StylePayloadsRef) {
        self.write(&self.data.style, style);
    }
}

/// A run of the arena's nodes. It is the only way to reach a node's data, so it knows which of its
/// nodes may have changed since the paint side last looked.
// NodeData is sized to fit cache lines evenly; the aligned chunk keeps every densely-strided slot
// line-aligned, and per-slot bookkeeping lives in a parallel array so it stays that way.
#[repr(align(64))]
pub(crate) struct Chunk {
    slots: [NodeData; SLOTS_PER_CHUNK],
    /// One bit per node whose shape may have been written since the paint side last looked.
    written_rows: [Cell<u64>; SLOTS_PER_CHUNK / 64],
}

impl Chunk {
    pub(crate) fn new() -> Box<Self> {
        // SAFETY: Every slot is written with NodeData::default() and every mark word with all bits
        // set before the chunk is exposed. The chunk is built in place on the heap because it is
        // far too large for the stack.
        unsafe {
            let mut chunk = Box::<Self>::new_uninit();
            let slots = &raw mut (*chunk.as_mut_ptr()).slots;
            for offset in 0..SLOTS_PER_CHUNK {
                (&raw mut (*slots)[offset]).write(NodeData::default());
            }
            (&raw mut (*chunk.as_mut_ptr()).written_rows).write(std::array::from_fn(|_| Cell::new(u64::MAX)));
            chunk.assume_init()
        }
    }

    /// Where the chunk's first node is, which never changes.
    pub(crate) fn slots_address(&self) -> usize {
        (&raw const self.slots) as usize
    }

    #[inline]
    pub(crate) fn slot(&self, offset: usize) -> &NodeData {
        &self.slots[offset]
    }

    #[inline]
    pub(crate) fn slot_mut(&mut self, offset: usize) -> &mut NodeData {
        *self.written_rows[offset / 64].get_mut() |= 1 << (offset % 64);
        &mut self.slots[offset]
    }

    #[inline]
    pub(crate) fn write_shape(&self, offset: usize) -> ShapeWriter<'_> {
        ShapeWriter {
            data: &self.slots[offset],
            written_rows: &self.written_rows[offset / 64],
            row_bit: 1 << (offset % 64),
        }
    }

    /// Whether the node at `offset` may have been written since the marks were last taken.
    #[cfg(test)]
    pub(crate) fn is_marked(&self, offset: usize) -> bool {
        self.written_rows[offset / 64].get() & (1 << (offset % 64)) != 0
    }

    /// Clears every mark.
    #[cfg(test)]
    pub(crate) fn clear_marks(&self) {
        for word in &self.written_rows {
            word.set(0);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_write_that_changes_a_node_marks_it_and_one_that_does_not_leaves_it_unmarked() {
        let mut chunk = Chunk::new();
        assert!(chunk.is_marked(0), "a new chunk's nodes are all marked");
        chunk.clear_marks();

        let shape = chunk.write_shape(3);
        shape.set_kind(NodeKind::Unset);
        shape.set_parent(NodeSlotId::INVALID);
        shape.set_flags(0);
        assert!(!chunk.is_marked(3));

        chunk.write_shape(3).set_kind(NodeKind::BlockContainer);
        assert!(chunk.is_marked(3));
        assert!(!chunk.is_marked(2) && !chunk.is_marked(4));

        chunk.write_shape(70).set_dom_paint_facts(1);
        assert!(chunk.is_marked(70));
        assert!(!chunk.is_marked(71));

        *chunk.slot_mut(100).slot_generation.get_mut() = 1;
        assert!(chunk.is_marked(100));
        assert_eq!(chunk.slot(3).kind.get(), NodeKind::BlockContainer);
    }
}
