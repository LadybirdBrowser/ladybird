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
//! The fields a [`PaintNode`] copies are [`ShapeCell`]s. They read like plain fields, but only a
//! [`ShapeWriter`] or `&mut` writes one, and only a [`Chunk`] hands out either, marking the node in
//! the chunk when a write changes it. A write the next publication would miss does not compile.

use super::layout_node_arena::SLOTS_PER_CHUNK;
use super::node_data::{NodeData, NodeFlag, NodeKind, NodeSlotId, PaintNode, StylePayloadsRef};
use crate::cow_column::{ColumnSnapshot, CowColumn};
use crate::css::style::tree::StyleNodeID;
use std::ops::Deref;

/// A field of a node the paint side reads. It reads like a plain field, and is written through a
/// [`ShapeWriter`] or through `&mut`, both of which mark the node in its chunk.
#[repr(transparent)]
#[derive(Clone)]
pub(crate) struct ShapeCell<T>(T);

impl<T: Copy> ShapeCell<T> {
    pub(crate) const fn new(value: T) -> Self {
        Self(value)
    }

    #[inline]
    pub(crate) fn get(&self) -> T {
        self.0
    }

    #[inline]
    pub(crate) fn get_mut(&mut self) -> &mut T {
        &mut self.0
    }

    #[inline]
    fn set(&mut self, value: T) {
        self.0 = value;
    }
}

/// How far the arena's nodes have been written, which tells a reader of the rows published before that they moved on.
#[derive(Clone, Default)]
pub(crate) struct ShapeWrites {
    all: u64,
    identity: u64,
    style: u64,
    /// One bit per chunk holding a node marked since the last publication, so that publishing visits those alone.
    written_chunks: Vec<u64>,
}

impl ShapeWrites {
    /// Every write that changed a node's shape, or the style record or node kept beside it.
    pub(crate) fn all(&self) -> u64 {
        self.all
    }

    /// The writes among them that changed what a row is: its slot's generation, its kind, what it is generated for,
    /// its [`NodeFlag::IDENTITY`] flags, and the node whose style it carries. Installing a style changes none of it.
    pub(crate) fn identity(&self) -> u64 {
        self.identity
    }

    /// The writes among them that gave a row a style record, but for a row they made, which changes what the row is.
    pub(crate) fn style(&self) -> u64 {
        self.style
    }

    fn note(&mut self) {
        self.all += 1;
    }

    /// Notes a write that gave a row a style record.
    pub(crate) fn note_style(&mut self) {
        self.note();
        self.style += 1;
    }

    /// Notes a write that changed what a row is.
    pub(crate) fn note_identity(&mut self) {
        self.note();
        self.identity += 1;
    }

    /// Makes room for the chunk at `chunk_index`, whose nodes are all marked as it is made.
    pub(crate) fn add_chunk(&mut self, chunk_index: usize) {
        self.written_chunks.resize(chunk_index / 64 + 1, 0);
        self.note_chunk_written(chunk_index);
    }

    fn note_chunk_written(&mut self, chunk_index: usize) {
        self.written_chunks[chunk_index / 64] |= 1 << (chunk_index % 64);
    }

    /// The chunks holding a node marked since the last publication, which it forgets.
    fn take_written_chunks(&mut self) -> impl Iterator<Item = usize> + '_ {
        self.written_chunks
            .iter_mut()
            .enumerate()
            .flat_map(|(word_index, word)| {
                let mut chunks = std::mem::take(word);
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
    data: &'a mut NodeData,
    written_rows: &'a mut u64,
    row_bit: u64,
    chunk_index: usize,
    writes: &'a mut ShapeWrites,
}

impl Deref for ShapeWriter<'_> {
    type Target = NodeData;

    fn deref(&self) -> &NodeData {
        self.data
    }
}

impl ShapeWriter<'_> {
    /// Writes the field `field` selects, marking the node only for a value the field does not already hold.
    #[inline]
    fn write<T: Copy + PartialEq>(&mut self, field: fn(&mut NodeData) -> &mut ShapeCell<T>, value: T) {
        if field(self.data).get() != value {
            self.mark();
            field(self.data).set(value);
        }
    }

    /// Like [`Self::write`], for a field that says what the row is.
    #[inline]
    fn write_identity<T: Copy + PartialEq>(&mut self, field: fn(&mut NodeData) -> &mut ShapeCell<T>, value: T) {
        if field(self.data).get() != value {
            self.mark_identity();
            field(self.data).set(value);
        }
    }

    /// Marks the node for the next publication, for a write to what the arena keeps beside it.
    pub(crate) fn mark(&mut self) {
        self.mark_row();
        self.writes.note();
    }

    /// Like [`Self::mark`], for a write that changes what the row is.
    pub(crate) fn mark_identity(&mut self) {
        self.mark_row();
        self.writes.note_identity();
    }

    fn mark_row(&mut self) {
        *self.written_rows |= self.row_bit;
        self.writes.note_chunk_written(self.chunk_index);
    }

    pub(crate) fn set_parent(&mut self, parent: NodeSlotId) {
        self.write(|data| &mut data.parent, parent);
    }

    pub(crate) fn set_first_child(&mut self, child: NodeSlotId) {
        self.write(|data| &mut data.first_child, child);
    }

    pub(crate) fn set_last_child(&mut self, child: NodeSlotId) {
        self.write(|data| &mut data.last_child, child);
    }

    pub(crate) fn set_previous_sibling(&mut self, sibling: NodeSlotId) {
        self.write(|data| &mut data.previous_sibling, sibling);
    }

    pub(crate) fn set_next_sibling(&mut self, sibling: NodeSlotId) {
        self.write(|data| &mut data.next_sibling, sibling);
    }

    pub(crate) fn set_kind(&mut self, kind: NodeKind) {
        self.write_identity(|data| &mut data.kind, kind);
    }

    pub(crate) fn set_generated_for(&mut self, generated_for: u8) {
        self.write_identity(|data| &mut data.generated_for, generated_for);
    }

    pub(crate) fn set_flags(&mut self, flags: u32) {
        if (self.data.flags.get() ^ flags) & NodeFlag::IDENTITY != 0 {
            self.write_identity(|data| &mut data.flags, flags);
        } else {
            self.write(|data| &mut data.flags, flags);
        }
    }

    pub(crate) fn set_dom_paint_facts(&mut self, facts: u8) {
        self.write(|data| &mut data.dom_paint_facts, facts);
    }

    pub(crate) fn set_compositor_animation_frame_kinds(&mut self, kinds: u8) {
        self.write(|data| &mut data.compositor_animation_frame_kinds, kinds);
    }

    pub(crate) fn set_style(&mut self, style: StylePayloadsRef) {
        self.write(|data| &mut data.style, style);
    }
}

/// A run of the arena's nodes. It is the only way to reach a node's data, so it knows which of its
/// nodes may have changed since the paint side last looked.
// NodeData is sized to fit cache lines evenly; the aligned chunk keeps every densely-strided slot
// line-aligned, and per-slot bookkeeping lives in a parallel array so it stays that way.
#[repr(align(64))]
#[derive(Clone)]
pub(crate) struct Chunk {
    slots: [NodeData; SLOTS_PER_CHUNK],
    /// One bit per node whose shape may have been written since the paint side last looked.
    written_rows: [u64; SLOTS_PER_CHUNK / 64],
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
            (&raw mut (*chunk.as_mut_ptr()).written_rows).write([u64::MAX; SLOTS_PER_CHUNK / 64]);
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

    /// The facts of the node at `offset` that no publication carries, to write without marking the node.
    #[inline]
    pub(crate) fn unpublished_facts_mut(&mut self, offset: usize) -> super::node_data::UnpublishedNodeFactsMut<'_> {
        self.slots[offset].unpublished_facts_mut()
    }

    /// The node at `offset` of this chunk, the arena's chunk at `chunk_index`, marked for the next publication.
    #[inline]
    pub(crate) fn slot_mut(&mut self, chunk_index: usize, offset: usize, writes: &mut ShapeWrites) -> &mut NodeData {
        self.written_rows[offset / 64] |= 1 << (offset % 64);
        writes.note_chunk_written(chunk_index);
        &mut self.slots[offset]
    }

    #[inline]
    pub(crate) fn write_shape<'a>(
        &'a mut self,
        chunk_index: usize,
        offset: usize,
        writes: &'a mut ShapeWrites,
    ) -> ShapeWriter<'a> {
        ShapeWriter {
            data: &mut self.slots[offset],
            written_rows: &mut self.written_rows[offset / 64],
            row_bit: 1 << (offset % 64),
            chunk_index,
            writes,
        }
    }

    /// Whether the node at `offset` may have been written since the marks were last taken.
    #[cfg(test)]
    pub(crate) fn is_marked(&self, offset: usize) -> bool {
        self.written_rows[offset / 64] & (1 << (offset % 64)) != 0
    }

    /// Clears every mark.
    #[cfg(test)]
    pub(crate) fn clear_marks(&mut self) {
        self.written_rows = [0; SLOTS_PER_CHUNK / 64];
    }
}

/// How many rows a chunk of the published column holds. A node written after a publication copies
/// the chunk the publication shares, while the nodes a layout writes lie scattered over the arena,
/// so a chunk holds far fewer rows than one of the arena's own.
pub(crate) const PUBLISHED_ROWS_PER_CHUNK: usize = 32;

/// The arena's column of what the paint side reads of every node, which it publishes from.
#[derive(Clone, Default)]
pub(crate) struct TreeShape {
    nodes: CowColumn<PaintNode, PUBLISHED_ROWS_PER_CHUNK>,
}

impl TreeShape {
    /// Brings the rows of every node written since the last publication up to date and publishes
    /// the column. A row whose node did not change is not written, so a chunk an earlier
    /// publication shares is copied only for a change.
    pub(crate) fn publish(
        &mut self,
        chunks: &mut [Box<Chunk>],
        style_records: &[u64],
        style_nodes: &[Option<StyleNodeID>],
        style_is_derived: impl Fn(usize) -> bool,
        writes: &mut ShapeWrites,
    ) -> ColumnSnapshot<PaintNode, PUBLISHED_ROWS_PER_CHUNK> {
        self.nodes.grow_to(chunks.len() * SLOTS_PER_CHUNK);
        for chunk_index in writes.take_written_chunks() {
            let chunk = &mut chunks[chunk_index];
            for word_index in 0..chunk.written_rows.len() {
                let mut written = std::mem::take(&mut chunk.written_rows[word_index]);
                while written != 0 {
                    let offset = word_index * 64 + written.trailing_zeros() as usize;
                    written &= written - 1;
                    let index = chunk_index * SLOTS_PER_CHUNK + offset;
                    let row = PaintNode::of(
                        &chunk.slots[offset],
                        style_records.get(index).copied().unwrap_or(0),
                        style_nodes.get(index).copied().flatten(),
                        style_is_derived(index),
                    );
                    self.nodes.set(index, row).expect("the column holds every chunk");
                }
            }
        }
        self.nodes.publish()
    }
}

const _: () = {
    const fn assert_sync<T: Sync>() {}
    assert_sync::<NodeData>();
    assert_sync::<Chunk>();
    assert_sync::<ShapeWrites>();
    assert_sync::<TreeShape>();
};

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

        let mut shape = chunk.write_shape(0, 3, &mut writes);
        shape.set_kind(NodeKind::Unset);
        shape.set_parent(NodeSlotId::INVALID);
        shape.set_flags(0);
        assert!(!chunk.is_marked(3));
        assert_eq!(writes.all(), 0);

        chunk.write_shape(0, 3, &mut writes).set_kind(NodeKind::BlockContainer);
        assert!(chunk.is_marked(3));
        assert!(!chunk.is_marked(2) && !chunk.is_marked(4));
        assert_eq!(writes.all(), 1);

        chunk.write_shape(0, 70, &mut writes).set_dom_paint_facts(1);
        assert!(chunk.is_marked(70));
        assert!(!chunk.is_marked(71));

        *chunk.slot_mut(0, 100, &mut writes).slot_generation.get_mut() = 1;
        assert!(chunk.is_marked(100));
        assert_eq!(chunk.slot(3).kind.get(), NodeKind::BlockContainer);
    }

    #[test]
    fn only_a_write_of_what_a_row_is_counts_as_an_identity_write() {
        let mut chunk = Chunk::new();
        let mut writes = ShapeWrites::default();
        writes.add_chunk(0);
        let mut shape = chunk.write_shape(0, 3, &mut writes);
        shape.set_parent(NodeSlotId::new(1, 1));
        shape.set_style(StylePayloadsRef::new(std::ptr::NonNull::dangling().as_ptr()));
        shape.set_flags(NodeFlag::HasStyle as u32);
        shape.mark();
        assert_eq!((writes.all(), writes.identity()), (4, 0));

        let mut shape = chunk.write_shape(0, 3, &mut writes);
        shape.set_flags(NodeFlag::HasStyle as u32 | NodeFlag::IsBody as u32);
        shape.set_kind(NodeKind::BlockContainer);
        shape.set_generated_for(1);
        shape.mark_identity();
        assert_eq!((writes.all(), writes.identity()), (8, 4));
    }

    #[test]
    fn a_publication_visits_only_the_chunks_holding_a_marked_node() {
        let mut chunk = Chunk::new();
        let mut writes = ShapeWrites::default();
        for chunk_index in [0, 1, 70] {
            writes.add_chunk(chunk_index);
        }
        assert_eq!(writes.take_written_chunks().collect::<Vec<_>>(), [0, 1, 70]);
        assert_eq!(writes.take_written_chunks().count(), 0);
        chunk.write_shape(70, 7, &mut writes).set_kind(NodeKind::BlockContainer);
        chunk.write_shape(1, 8, &mut writes).set_kind(NodeKind::BlockContainer);
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
