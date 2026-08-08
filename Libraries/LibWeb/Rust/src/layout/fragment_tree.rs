/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub(crate) struct Fragment {
    #[allow(dead_code)]
    pub(crate) node: crate::layout::node_data::NodeSlotId,
    #[allow(dead_code)]
    pub(crate) children: Vec<FragmentLink>,
}

pub(crate) struct FragmentLink {
    #[allow(dead_code)]
    pub(crate) fragment: Box<Fragment>,
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
        containing_block_is_sealed: bool,
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
        let link = FragmentLink {
            fragment: Box::new(Fragment { node, children }),
        };
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

    pub(crate) fn take_unplaced_root(&self) -> UnplacedRootFragment {
        debug_assert!(!self.is_entry_accumulator, "an entry accumulator closes as a pass, not a run");
        UnplacedRootFragment {
            node: self.root_node,
            scoped_descendants: self.close(),
        }
    }

    pub(crate) fn take_completed_pass(&self) -> CompletedPassFragments {
        debug_assert!(self.is_entry_accumulator, "an ordinary run closes as a singular unplaced root");
        CompletedPassFragments { roots: self.close() }
    }

    fn close(&self) -> Vec<FragmentLink> {
        let mut inner = self.inner.take();
        let pending_fragments = std::mem::take(&mut inner.pending_fragments);
        for (_, pending_fragment) in pending_fragments {
            if pending_fragment.children.is_empty() {
                continue;
            }
            inner.top_scope_links.push(FragmentLink {
                fragment: Box::new(Fragment {
                    node: pending_fragment.node,
                    children: pending_fragment.children,
                }),
            });
        }
        let child_roots_awaiting_placement = std::mem::take(&mut inner.child_roots_awaiting_placement);
        for (_, root) in child_roots_awaiting_placement {
            inner.top_scope_links.push(FragmentLink {
                fragment: Box::new(Fragment {
                    node: root.node,
                    children: root.scoped_descendants,
                }),
            });
        }
        inner.top_scope_links
    }
}
