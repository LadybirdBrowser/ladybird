/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[allow(dead_code)]
pub(crate) struct Fragment {
    pub(crate) node: crate::layout::node_data::NodeSlotId,
    pub(crate) content_inline_size: CssPixels,
    pub(crate) content_block_size: CssPixels,
    pub(crate) margin_left: CssPixels,
    pub(crate) margin_right: CssPixels,
    pub(crate) margin_top: CssPixels,
    pub(crate) margin_bottom: CssPixels,
    pub(crate) border_left: CssPixels,
    pub(crate) border_right: CssPixels,
    pub(crate) border_top: CssPixels,
    pub(crate) border_bottom: CssPixels,
    pub(crate) padding_left: CssPixels,
    pub(crate) padding_right: CssPixels,
    pub(crate) padding_top: CssPixels,
    pub(crate) padding_bottom: CssPixels,
    pub(crate) children: Vec<FragmentLink>,
}

#[allow(dead_code)]
pub(crate) struct FragmentLink {
    pub(crate) fragment: Box<Fragment>,
    pub(crate) committed_offset: FfiCssPixelPoint,
    pub(crate) inset_left: CssPixels,
    pub(crate) inset_right: CssPixels,
    pub(crate) inset_top: CssPixels,
    pub(crate) inset_bottom: CssPixels,
    pub(crate) containing_line_box_index: Option<usize>,
}

fn snapshot_fragment(
    node: crate::layout::node_data::NodeSlotId,
    children: Vec<FragmentLink>,
    used: &UsedValues,
) -> Box<Fragment> {
    Box::new(Fragment {
        node,
        content_inline_size: used.content_inline_size.get(),
        content_block_size: used.content_block_size.get(),
        margin_left: used.margin_left.get(),
        margin_right: used.margin_right.get(),
        margin_top: used.margin_top.get(),
        margin_bottom: used.margin_bottom.get(),
        border_left: used.border_left.get(),
        border_right: used.border_right.get(),
        border_top: used.border_top.get(),
        border_bottom: used.border_bottom.get(),
        padding_left: used.padding_left.get(),
        padding_right: used.padding_right.get(),
        padding_top: used.padding_top.get(),
        padding_bottom: used.padding_bottom.get(),
        children,
    })
}

pub(crate) struct PlacementData {
    pub(crate) committed_offset: FfiCssPixelPoint,
    pub(crate) inset_left: CssPixels,
    pub(crate) inset_right: CssPixels,
    pub(crate) inset_top: CssPixels,
    pub(crate) inset_bottom: CssPixels,
    pub(crate) containing_line_box_index: Option<usize>,
}

impl PlacementData {
    fn from_record(used: &UsedValues, containing_line_box_index: Option<usize>) -> Self {
        Self {
            committed_offset: crate::layout::point_add(used.content_offset.get(), used.committed_offset_delta.get()),
            inset_left: used.inset_left.get(),
            inset_right: used.inset_right.get(),
            inset_top: used.inset_top.get(),
            inset_bottom: used.inset_bottom.get(),
            containing_line_box_index,
        }
    }
}

fn link_fragment(fragment: Box<Fragment>, placement: PlacementData) -> FragmentLink {
    FragmentLink {
        fragment,
        committed_offset: placement.committed_offset,
        inset_left: placement.inset_left,
        inset_right: placement.inset_right,
        inset_top: placement.inset_top,
        inset_bottom: placement.inset_bottom,
        containing_line_box_index: placement.containing_line_box_index,
    }
}

pub(crate) struct UnplacedRootFragment {
    pub(crate) node: crate::layout::node_data::NodeSlotId,
    pub(crate) scoped_descendants: Vec<FragmentLink>,
}

pub(crate) struct CompletedPassFragments {
    pub(crate) roots: Vec<FragmentLink>,
}

struct PendingFragment {
    node: crate::layout::node_data::NodeSlotId,
    children: Vec<FragmentLink>,
}

pub(crate) struct RunFragmentBuilder {
    root_node: crate::layout::node_data::NodeSlotId,
    root_containing_block_slot: Option<u32>,
    is_entry_accumulator: bool,
    inner: std::cell::RefCell<RunFragmentBuilderInner>,
}

#[derive(Default)]
struct RunFragmentBuilderInner {
    pending_fragments: std::collections::HashMap<u32, PendingFragment>,
    #[cfg(debug_assertions)]
    placed_slots: std::collections::HashSet<u32>,
    child_roots_awaiting_placement: std::collections::HashMap<u32, UnplacedRootFragment>,
    top_scope_links: Vec<FragmentLink>,
}

impl RunFragmentBuilder {
    pub(crate) fn new(
        root_node: crate::layout::node_data::NodeSlotId,
        root_containing_block: Option<crate::layout::node_data::NodeSlotId>,
    ) -> Self {
        Self {
            root_node,
            root_containing_block_slot: root_containing_block.map(|node| node.slot_index()),
            is_entry_accumulator: false,
            inner: std::cell::RefCell::new(RunFragmentBuilderInner::default()),
        }
    }

    pub(crate) fn new_entry_accumulator(root_node: crate::layout::node_data::NodeSlotId) -> Self {
        Self {
            root_node,
            root_containing_block_slot: None,
            is_entry_accumulator: true,
            inner: std::cell::RefCell::new(RunFragmentBuilderInner::default()),
        }
    }

    pub(crate) fn hold_unplaced_root(&self, root: UnplacedRootFragment) {
        let slot = root.node.slot_index();
        let previous = self.inner.borrow_mut().child_roots_awaiting_placement.insert(slot, root);
        debug_assert!(
            previous.is_none(),
            "a child run's root was handed over twice before placement"
        );
    }

    pub(crate) fn normalize_arrivals_for_placement(&self, node: crate::layout::node_data::NodeSlotId) {
        let mut inner = self.inner.borrow_mut();
        let slot = node.slot_index();
        let scoped_descendants = match inner.child_roots_awaiting_placement.remove(&slot) {
            Some(root) => {
                debug_assert!(root.node == node, "a held unplaced root was keyed under a different box");
                root.scoped_descendants
            }
            None => Vec::new(),
        };
        let pending_fragment = inner.pending_fragments.entry(slot).or_insert_with(|| PendingFragment {
            node,
            children: Vec::new(),
        });
        debug_assert!(
            pending_fragment.children.is_empty() || scoped_descendants.is_empty(),
            "a held unplaced root and an open pending fragment both carry children for slot {slot}"
        );
        pending_fragment.children.extend(scoped_descendants);
    }

    pub(crate) fn build_fragment_for_placed_box(
        &self,
        node: crate::layout::node_data::NodeSlotId,
        containing_block: Option<crate::layout::node_data::NodeSlotId>,
        used: &UsedValues,
        containing_block_is_sealed: bool,
        containing_line_box_index: Option<usize>,
    ) {
        let mut inner = self.inner.borrow_mut();
        let slot = node.slot_index();
        #[cfg(debug_assertions)]
        assert!(inner.placed_slots.insert(slot), "a box was placed twice in one run");
        debug_assert!(
            !inner.child_roots_awaiting_placement.contains_key(&slot),
            "a box was placed without normalizing its held unplaced root (slot {slot})"
        );
        let children = match inner.pending_fragments.remove(&slot) {
            Some(pending_fragment) => pending_fragment.children,
            None => Vec::new(),
        };
        let link = link_fragment(
            snapshot_fragment(node, children, used),
            PlacementData::from_record(used, containing_line_box_index),
        );
        self.attach(&mut inner, link, containing_block, containing_block_is_sealed);
    }

    fn attach(
        &self,
        inner: &mut RunFragmentBuilderInner,
        link: FragmentLink,
        containing_block: Option<crate::layout::node_data::NodeSlotId>,
        containing_block_is_sealed: bool,
    ) {
        let Some(containing_block) = containing_block else {
            inner.top_scope_links.push(link);
            return;
        };
        if containing_block == self.root_node || Some(containing_block.slot_index()) == self.root_containing_block_slot {
            inner.top_scope_links.push(link);
            return;
        }
        if let Some(pending_fragment) = inner.pending_fragments.get_mut(&containing_block.slot_index()) {
            pending_fragment.children.push(link);
            return;
        }
        if self.is_entry_accumulator || containing_block_is_sealed {
            inner.top_scope_links.push(link);
            return;
        }
        inner.pending_fragments.insert(
            containing_block.slot_index(),
            PendingFragment {
                node: containing_block,
                children: vec![link],
            },
        );
    }

    pub(crate) fn take_unplaced_root(&self, state: &LayoutState) -> UnplacedRootFragment {
        debug_assert!(!self.is_entry_accumulator, "an entry accumulator closes as a pass, not a run");
        UnplacedRootFragment {
            node: self.root_node,
            scoped_descendants: self.close(state),
        }
    }

    pub(crate) fn take_completed_pass(&self, state: &LayoutState) -> CompletedPassFragments {
        debug_assert!(self.is_entry_accumulator, "an ordinary run closes as a singular unplaced root");
        CompletedPassFragments { roots: self.close(state) }
    }

    fn close(&self, state: &LayoutState) -> Vec<FragmentLink> {
        let mut inner = self.inner.take();
        let pending_fragments = std::mem::take(&mut inner.pending_fragments);
        for (slot, pending_fragment) in pending_fragments {
            if pending_fragment.children.is_empty() {
                continue;
            }
            let used = state
                .used_values_by_slot(slot)
                .expect("a pending fragment's containing block has no record");
            inner.top_scope_links.push(link_fragment(
                snapshot_fragment(pending_fragment.node, pending_fragment.children, used),
                PlacementData::from_record(used, None),
            ));
        }
        let child_roots_awaiting_placement = std::mem::take(&mut inner.child_roots_awaiting_placement);
        for (slot, root) in child_roots_awaiting_placement {
            let used = state
                .used_values_by_slot(slot)
                .expect("a held child run's root has no record");
            inner.top_scope_links.push(link_fragment(
                snapshot_fragment(root.node, root.scoped_descendants, used),
                PlacementData::from_record(used, None),
            ));
        }
        inner.top_scope_links
    }
}
