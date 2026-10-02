/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a document publishes for the display list recording to read.
//!
//! [`PublishedRows`] is one generation of a document's paintable rows and of the columns read
//! beside them. It is immutable: every column in it is a [`ColumnSnapshot`] the document shares
//! with it, so the document writes its live columns (copying a chunk a publication still shares)
//! while it is read.

use crate::cow_column::ColumnSnapshot;
use crate::layout::fragment_tree::FragmentLink;
use crate::layout::node_data::NodeSlotId;
use crate::painting::paintable_data::{CommittedSideData, PaintableData};
use crate::painting::paintable_rows::{CommittedFragmentLinkSlot, PAINTABLE_SLOTS_PER_CHUNK};

/// One published generation of a document's paintable rows and of the columns read beside them.
pub(crate) struct PublishedRows {
    rows: ColumnSnapshot<PaintableData, PAINTABLE_SLOTS_PER_CHUNK>,
    fragment_links: ColumnSnapshot<CommittedFragmentLinkSlot, PAINTABLE_SLOTS_PER_CHUNK>,
    side_data: ColumnSnapshot<CommittedSideData, PAINTABLE_SLOTS_PER_CHUNK>,
}

impl PublishedRows {
    pub(crate) fn new(
        rows: ColumnSnapshot<PaintableData, PAINTABLE_SLOTS_PER_CHUNK>,
        fragment_links: ColumnSnapshot<CommittedFragmentLinkSlot, PAINTABLE_SLOTS_PER_CHUNK>,
        side_data: ColumnSnapshot<CommittedSideData, PAINTABLE_SLOTS_PER_CHUNK>,
    ) -> Self {
        Self {
            rows,
            fragment_links,
            side_data,
        }
    }

    pub(crate) fn paintable_data(&self, id: NodeSlotId) -> &PaintableData {
        assert!(!id.is_invalid(), "invalid paintable arena slot ID");
        let data = self
            .rows
            .get(id.slot_index() as usize)
            .expect("invalid paintable arena slot ID");
        assert_eq!(
            data.slot_generation,
            id.generation(),
            "paintable arena read a stale or unused slot"
        );
        data
    }

    pub(crate) fn paintable_row_is_populated(&self, id: NodeSlotId) -> bool {
        if id.is_invalid() {
            return false;
        }
        self.rows
            .get(id.slot_index() as usize)
            .is_some_and(|data| data.slot_generation != 0 && data.slot_generation == id.generation())
    }

    pub(crate) fn with_committed_fragment_link<R>(
        &self,
        id: NodeSlotId,
        read: impl FnOnce(Option<&FragmentLink>) -> R,
    ) -> R {
        debug_assert!(self.paintable_row_is_populated(id));
        read(
            self.fragment_links
                .get(id.slot_index() as usize)
                .and_then(CommittedFragmentLinkSlot::link),
        )
    }

    pub(crate) fn committed_side_data(&self, id: NodeSlotId) -> &CommittedSideData {
        debug_assert!(self.paintable_row_is_populated(id));
        self.side_data
            .get(id.slot_index() as usize)
            .expect("a populated row has published side data")
    }
}

// The rows are read on whichever thread paints them while the document writes its live columns:
// they hold no cell, no raw pointer and no borrow of the document.
const _: () = {
    const fn assert_published<T: Send + Sync + 'static>() {}
    assert_published::<PublishedRows>();
};

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::css_pixels::CssPixels;
    use crate::layout::LayoutNodeArena;
    use crate::layout::fragment_tree;
    use crate::painting::paint_read::{GeometryRead, PaintSource};
    use crate::painting::record::recorder_state::AbsoluteRectMemo;
    use std::cell::RefCell;

    #[test]
    fn published_rows_keep_their_generation_after_a_later_write() {
        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate_for_test().slot;
        arena.populate_paintable_row(node);
        let mut link = fragment_tree::FragmentLink::for_test(node);
        link.inset_left = CssPixels::from_integer(10);
        arena.set_committed_fragment_link(arena.data(node), link.clone(), None);
        arena.paintable_rows_mut().paintable_data_mut(node).offset.x = CssPixels::from_integer(10).into();
        arena.committed_side_data_mut(node).piece_indices = Some([0].into());

        let published = arena.publish_rows();
        link.inset_left = CssPixels::from_integer(20);
        arena.set_committed_fragment_link(arena.data(node), link, None);
        arena.paintable_rows_mut().paintable_data_mut(node).offset.x = CssPixels::from_integer(20).into();
        arena.committed_side_data_mut(node).piece_indices = None;

        assert_eq!(
            published.paintable_data(node).offset.x,
            CssPixels::from_integer(10).into()
        );
        assert_eq!(
            published.with_committed_fragment_link(node, |link| link.map(|link| link.inset_left)),
            Some(CssPixels::from_integer(10))
        );
        assert_eq!(published.committed_side_data(node).piece_indices(), [0]);
        assert_eq!(
            arena.live_paintable_data(node).offset.x,
            CssPixels::from_integer(20).into()
        );
        assert_eq!(
            arena.with_committed_fragment_link(node, |link| link.map(|link| link.inset_left)),
            Some(CssPixels::from_integer(20))
        );
        assert!(arena.committed_side_data(node).piece_indices().is_empty());
    }

    #[test]
    fn published_rows_answer_every_row_read_as_the_arena_does() {
        let mut arena = LayoutNodeArena::new();
        let mut slots = Vec::new();
        for index in 0..PAINTABLE_SLOTS_PER_CHUNK + 3 {
            let node = arena.allocate_for_test().slot;
            slots.push(node);
            if index % 3 == 0 {
                continue;
            }
            arena.populate_paintable_row(node);
            arena.paintable_rows_mut().paintable_data_mut(node).offset.x = CssPixels::from_integer(index as i64).into();
            if index % 2 == 0 {
                let mut link = fragment_tree::FragmentLink::for_test(node);
                link.inset_top = CssPixels::from_integer(index as i64);
                arena.set_committed_fragment_link(arena.data(node), link, None);
                arena.committed_side_data_mut(node).overflow_valid_across_recommits = true;
            }
        }

        let published = arena.publish_rows();
        let absolute_rects = RefCell::new(AbsoluteRectMemo::default());
        let source = PaintSource::new(&arena, &published, &absolute_rects);
        for node in slots {
            assert_eq!(
                source.paintable_row_is_populated(node),
                arena.paintable_row_is_populated(node)
            );
            if !arena.paintable_row_is_populated(node) {
                continue;
            }
            assert_eq!(source.paintable_data(node), arena.live_paintable_data(node));
            assert_eq!(
                source.with_committed_fragment_link(node, |link| link.map(|link| link.inset_top)),
                arena.with_committed_fragment_link(node, |link| link.map(|link| link.inset_top))
            );
            assert!(*source.committed_side_data(node) == *arena.committed_side_data(node));
        }
    }
}
