/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The layout tree's shape as the paint side reads it, published copy-on-write.
//!
//! A node's links, kind, paint facts and style live in its [`NodeData`], which tree builds, style
//! installs and invalidation write in place. A recording that read them there would read a tree
//! the next layout writes. The arena therefore keeps a [`CowColumn`] of [`PaintNode`]s beside its
//! node chunks, and brings the rows of every node written since it last published up to date when
//! it publishes again, so publishing costs the nodes written since, not the whole tree.
//!
//! The fields a [`PaintNode`] copies are [`ShapeCell`]s. They read like a `Cell`, but only a
//! [`ShapeWriter`] or `&mut` writes one, and only a [`Chunk`] hands out either, marking the node in
//! the chunk when a write changes it. A write the next publication would miss does not compile.

use super::layout_node_arena::SLOTS_PER_CHUNK;
use super::node_data::{NodeData, NodeFlag, NodeKind, NodeSlotId, PaintNode, StylePayloadsRef};
use crate::cow_column::{ColumnSnapshot, CowColumn};
use crate::css::style::tree::StyleNodeID;
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

/// How far the arena's nodes have been written, which tells a reader of the rows published before that they moved on.
#[derive(Default)]
pub(crate) struct ShapeWrites {
    all: Cell<u64>,
    identity: Cell<u64>,
    style: Cell<u64>,
    /// One bit per chunk holding a node marked since the last publication, so that publishing visits those alone.
    written_chunks: Vec<Cell<u64>>,
}

impl ShapeWrites {
    /// Every write that changed a node's shape, or the style record or node kept beside it.
    pub(crate) fn all(&self) -> u64 {
        self.all.get()
    }

    /// The writes among them that changed what a row is: its slot's generation, its kind, what it is generated for,
    /// its [`NodeFlag::IDENTITY`] flags, and the node whose style it carries. Installing a style changes none of it.
    pub(crate) fn identity(&self) -> u64 {
        self.identity.get()
    }

    /// The writes among them that gave a row a style record, but for a row they made, which changes what the row is.
    pub(crate) fn style(&self) -> u64 {
        self.style.get()
    }

    fn note(&self) {
        self.all.set(self.all.get() + 1);
    }

    /// Notes a write that gave a row a style record.
    pub(crate) fn note_style(&self) {
        self.note();
        self.style.set(self.style.get() + 1);
    }

    /// Notes a write that changed what a row is.
    pub(crate) fn note_identity(&self) {
        self.note();
        self.identity.set(self.identity.get() + 1);
    }

    /// Makes room for the chunk at `chunk_index`, whose nodes are all marked as it is made.
    pub(crate) fn add_chunk(&mut self, chunk_index: usize) {
        self.written_chunks.resize_with(chunk_index / 64 + 1, Cell::default);
        self.note_chunk_written(chunk_index);
    }

    fn note_chunk_written(&self, chunk_index: usize) {
        let word = &self.written_chunks[chunk_index / 64];
        word.set(word.get() | 1 << (chunk_index % 64));
    }

    /// The chunks holding a node marked since the last publication, which it forgets.
    fn take_written_chunks(&self) -> impl Iterator<Item = usize> + '_ {
        self.written_chunks.iter().enumerate().flat_map(|(word_index, word)| {
            let mut chunks = word.replace(0);
            std::iter::from_fn(move || {
                (chunks != 0).then(|| {
                    let chunk = chunks.trailing_zeros() as usize;
                    chunks &= chunks - 1;
                    word_index * 64 + chunk
                })
            })
        })
    }
}

/// Writes the shape of one node, marking it in its chunk for the next publication when a write
/// changes it. It reads as the
/// node's [`NodeData`], so the fields the paint side does not read are written through it as
/// before.
pub(crate) struct ShapeWriter<'a> {
    data: &'a NodeData,
    written_rows: &'a Cell<u64>,
    row_bit: u64,
    chunk_index: usize,
    writes: &'a ShapeWrites,
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
            self.mark();
            field.set(value);
        }
    }

    /// Like [`Self::write`], for a field that says what the row is.
    #[inline]
    fn write_identity<T: Copy + PartialEq>(&self, field: &ShapeCell<T>, value: T) {
        if field.get() != value {
            self.mark_identity();
            field.set(value);
        }
    }

    /// Marks the node for the next publication, for a write to what the arena keeps beside it.
    pub(crate) fn mark(&self) {
        self.mark_row();
        self.writes.note();
    }

    /// Like [`Self::mark`], for a write that changes what the row is.
    pub(crate) fn mark_identity(&self) {
        self.mark_row();
        self.writes.note_identity();
    }

    fn mark_row(&self) {
        self.written_rows.set(self.written_rows.get() | self.row_bit);
        self.writes.note_chunk_written(self.chunk_index);
    }

    pub(crate) fn set_parent(&self, parent: NodeSlotId) {
        self.write(&self.data.parent, parent);
    }

    pub(crate) fn set_first_child(&self, child: NodeSlotId) {
        self.write(&self.data.first_child, child);
    }

    pub(crate) fn set_last_child(&self, child: NodeSlotId) {
        self.write(&self.data.last_child, child);
    }

    pub(crate) fn set_previous_sibling(&self, sibling: NodeSlotId) {
        self.write(&self.data.previous_sibling, sibling);
    }

    pub(crate) fn set_next_sibling(&self, sibling: NodeSlotId) {
        self.write(&self.data.next_sibling, sibling);
    }

    pub(crate) fn set_kind(&self, kind: NodeKind) {
        self.write_identity(&self.data.kind, kind);
    }

    pub(crate) fn set_generated_for(&self, generated_for: u8) {
        self.write_identity(&self.data.generated_for, generated_for);
    }

    pub(crate) fn set_flags(&self, flags: u32) {
        if (self.data.flags.get() ^ flags) & NodeFlag::IDENTITY != 0 {
            self.write_identity(&self.data.flags, flags);
        } else {
            self.write(&self.data.flags, flags);
        }
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

    /// The node at `offset` of this chunk, the arena's chunk at `chunk_index`, marked for the next publication.
    #[inline]
    pub(crate) fn slot_mut(&mut self, chunk_index: usize, offset: usize, writes: &ShapeWrites) -> &mut NodeData {
        *self.written_rows[offset / 64].get_mut() |= 1 << (offset % 64);
        writes.note_chunk_written(chunk_index);
        &mut self.slots[offset]
    }

    #[inline]
    pub(crate) fn write_shape<'a>(
        &'a self,
        chunk_index: usize,
        offset: usize,
        writes: &'a ShapeWrites,
    ) -> ShapeWriter<'a> {
        ShapeWriter {
            data: &self.slots[offset],
            written_rows: &self.written_rows[offset / 64],
            row_bit: 1 << (offset % 64),
            chunk_index,
            writes,
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

/// How many rows a chunk of the published column holds. A node written after a publication copies
/// the chunk the publication shares, while the nodes a layout writes lie scattered over the arena,
/// so a chunk holds far fewer rows than one of the arena's own.
pub(crate) const PUBLISHED_ROWS_PER_CHUNK: usize = 32;

/// The arena's column of what the paint side reads of every node, which it publishes from.
#[derive(Default)]
pub(crate) struct TreeShape {
    nodes: CowColumn<PaintNode, PUBLISHED_ROWS_PER_CHUNK>,
}

impl TreeShape {
    /// Brings the rows of every node written since the last publication up to date and publishes
    /// the column. A row whose node did not change is not written, so a chunk an earlier
    /// publication shares is copied only for a change.
    pub(crate) fn publish(
        &mut self,
        chunks: &[Box<Chunk>],
        style_records: &[Cell<u64>],
        style_nodes: &[Cell<Option<StyleNodeID>>],
        writes: &ShapeWrites,
    ) -> ColumnSnapshot<PaintNode, PUBLISHED_ROWS_PER_CHUNK> {
        self.nodes.grow_to(chunks.len() * SLOTS_PER_CHUNK);
        for chunk_index in writes.take_written_chunks() {
            let chunk = &chunks[chunk_index];
            for (word_index, word) in chunk.written_rows.iter().enumerate() {
                let mut written = word.replace(0);
                while written != 0 {
                    let offset = word_index * 64 + written.trailing_zeros() as usize;
                    written &= written - 1;
                    let index = chunk_index * SLOTS_PER_CHUNK + offset;
                    let row = PaintNode::of(
                        &chunk.slots[offset],
                        style_records.get(index).map_or(0, Cell::get),
                        style_nodes.get(index).and_then(Cell::get),
                    );
                    self.nodes.set(index, row).expect("the column holds every chunk");
                }
            }
        }
        self.nodes.publish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_write_that_changes_a_node_marks_it_and_one_that_does_not_leaves_it_unmarked() {
        let mut chunk = Chunk::new();
        let mut writes = ShapeWrites::default();
        writes.add_chunk(0);
        assert!(chunk.is_marked(0), "a new chunk's nodes are all marked");
        chunk.clear_marks();

        let shape = chunk.write_shape(0, 3, &writes);
        shape.set_kind(NodeKind::Unset);
        shape.set_parent(NodeSlotId::INVALID);
        shape.set_flags(0);
        assert!(!chunk.is_marked(3));
        assert_eq!(writes.all(), 0);

        chunk.write_shape(0, 3, &writes).set_kind(NodeKind::BlockContainer);
        assert!(chunk.is_marked(3));
        assert!(!chunk.is_marked(2) && !chunk.is_marked(4));
        assert_eq!(writes.all(), 1);

        chunk.write_shape(0, 70, &writes).set_dom_paint_facts(1);
        assert!(chunk.is_marked(70));
        assert!(!chunk.is_marked(71));

        *chunk.slot_mut(0, 100, &writes).slot_generation.get_mut() = 1;
        assert!(chunk.is_marked(100));
        assert_eq!(chunk.slot(3).kind.get(), NodeKind::BlockContainer);
    }

    #[test]
    fn only_a_write_of_what_a_row_is_counts_as_an_identity_write() {
        let chunk = Chunk::new();
        let mut writes = ShapeWrites::default();
        writes.add_chunk(0);
        let shape = chunk.write_shape(0, 3, &writes);
        shape.set_parent(NodeSlotId::new(1, 1));
        shape.set_style(StylePayloadsRef::new(std::ptr::NonNull::dangling().as_ptr()));
        shape.set_flags(NodeFlag::HasStyle as u32);
        shape.mark();
        assert_eq!((writes.all(), writes.identity()), (4, 0));

        shape.set_flags(NodeFlag::HasStyle as u32 | NodeFlag::IsBody as u32);
        shape.set_kind(NodeKind::BlockContainer);
        shape.set_generated_for(1);
        shape.mark_identity();
        assert_eq!((writes.all(), writes.identity()), (8, 4));
    }

    #[test]
    fn a_publication_visits_only_the_chunks_holding_a_marked_node() {
        let chunk = Chunk::new();
        let mut writes = ShapeWrites::default();
        for chunk_index in [0, 1, 70] {
            writes.add_chunk(chunk_index);
        }
        assert_eq!(writes.take_written_chunks().collect::<Vec<_>>(), [0, 1, 70]);
        assert_eq!(writes.take_written_chunks().count(), 0);
        chunk.write_shape(70, 7, &writes).set_kind(NodeKind::BlockContainer);
        chunk.write_shape(1, 8, &writes).set_kind(NodeKind::BlockContainer);
        assert_eq!(writes.take_written_chunks().collect::<Vec<_>>(), [1, 70]);
    }

    #[test]
    fn a_publication_copies_only_the_chunks_of_the_nodes_written_since_the_last() {
        let mut arena = crate::layout::LayoutNodeArena::new();
        let nodes: Vec<NodeSlotId> = (0..3 * PUBLISHED_ROWS_PER_CHUNK)
            .map(|_| arena.allocate_for_test().slot)
            .collect();
        let first = arena.publish_paint_tree();
        let written = nodes[PUBLISHED_ROWS_PER_CHUNK + 1];
        arena.write_shape(written).set_kind(NodeKind::Box);
        // A write that leaves a node as it was publishes nothing new.
        arena.write_shape(nodes[0]).set_kind(NodeKind::Unset);
        let second = arena.publish_paint_tree();

        let row = |snapshot: &ColumnSnapshot<PaintNode, PUBLISHED_ROWS_PER_CHUNK>, id: NodeSlotId| {
            std::ptr::from_ref(snapshot.get(id.slot_index() as usize).unwrap())
        };
        assert_eq!(row(&first, nodes[0]), row(&second, nodes[0]));
        assert_eq!(
            row(&first, nodes[2 * PUBLISHED_ROWS_PER_CHUNK]),
            row(&second, nodes[2 * PUBLISHED_ROWS_PER_CHUNK])
        );
        assert_ne!(row(&first, written), row(&second, written));
        assert_eq!(first.get(written.slot_index() as usize).unwrap().kind, NodeKind::Unset);
        assert_eq!(second.get(written.slot_index() as usize).unwrap().kind, NodeKind::Box);
    }
}
