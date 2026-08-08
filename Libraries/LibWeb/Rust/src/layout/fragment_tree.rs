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

pub(crate) fn translate_pending_abspos_payloads(entry: &mut PendingAbsposChild, offset: FfiCssPixelPoint) {
    entry.static_position_rect = crate::layout::translate_static_position_rect(entry.static_position_rect, offset);
    if let Some(rect) = &mut entry.inline_containing_block_rect {
        rect.x += offset.x;
        rect.y += offset.y;
    }
}

trait PropagatedPayload {
    fn coordinate_space_box(&self) -> crate::layout::node_data::NodeSlotId;
    fn set_coordinate_space_box(&mut self, node: crate::layout::node_data::NodeSlotId);
    fn translate_by(&mut self, offset: FfiCssPixelPoint);
}

impl PropagatedPayload for PendingAbsposChild {
    fn coordinate_space_box(&self) -> crate::layout::node_data::NodeSlotId {
        self.coordinate_space_box
    }
    fn set_coordinate_space_box(&mut self, node: crate::layout::node_data::NodeSlotId) {
        self.coordinate_space_box = node;
    }
    fn translate_by(&mut self, offset: FfiCssPixelPoint) {
        translate_pending_abspos_payloads(self, offset);
    }
}

fn propagate_payload_into_containing_block_space<Payload: PropagatedPayload>(
    payload: &mut Payload,
    placed_box: crate::layout::node_data::NodeSlotId,
    containing_block: Option<crate::layout::node_data::NodeSlotId>,
    placed_box_content_offset: FfiCssPixelPoint,
) {
    if payload.coordinate_space_box() == placed_box
        && let Some(containing_block) = containing_block
    {
        payload.translate_by(placed_box_content_offset);
        payload.set_coordinate_space_box(containing_block);
    }
}

fn propagate_payload_toward_run_root_space<Payload: PropagatedPayload>(
    payload: &mut Payload,
    run_root: crate::layout::node_data::NodeSlotId,
    state: &LayoutState,
    callbacks: &FfiLayoutFcCallbacks,
) {
    while payload.coordinate_space_box() != run_root {
        let Some(used) = state
            .used_values_by_slot(payload.coordinate_space_box().slot_index())
            .filter(|used| used.has_content_offset.get())
        else {
            break;
        };
        payload.translate_by(used.content_offset.get());
        let containing_block = callbacks.containing_block(payload.coordinate_space_box());
        if containing_block.is_invalid() {
            break;
        }
        payload.set_coordinate_space_box(containing_block);
    }
}

pub(crate) struct UnplacedRootFragment {
    pub(crate) node: crate::layout::node_data::NodeSlotId,
    pub(crate) scoped_descendants: Vec<FragmentLink>,
    pub(crate) propagated_pending_abspos: Vec<PendingAbsposChild>,
}

pub(crate) struct CompletedPassFragments {
    pub(crate) roots: Vec<FragmentLink>,
}

struct PendingFragment {
    node: crate::layout::node_data::NodeSlotId,
    children: Vec<FragmentLink>,
    pending_abspos: Vec<PendingAbsposChild>,
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
    placed_slots: std::collections::HashSet<u32>,
    child_roots_awaiting_placement: std::collections::HashMap<u32, UnplacedRootFragment>,
    pending_abspos_at_root: Vec<PendingAbsposChild>,
    top_scope_links: Vec<FragmentLink>,
}

impl RunFragmentBuilderInner {
    fn iter_pending_abspos(&self) -> impl Iterator<Item = &PendingAbsposChild> {
        self.pending_fragments
            .values()
            .flat_map(|pending_fragment| pending_fragment.pending_abspos.iter())
            .chain(self.pending_abspos_at_root.iter())
    }

    fn iter_pending_abspos_mut(&mut self) -> impl Iterator<Item = &mut PendingAbsposChild> {
        self.pending_fragments
            .values_mut()
            .flat_map(|pending_fragment| pending_fragment.pending_abspos.iter_mut())
            .chain(self.pending_abspos_at_root.iter_mut())
    }
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

    pub(crate) fn root_node(&self) -> crate::layout::node_data::NodeSlotId {
        self.root_node
    }

    pub(crate) fn register_pending_abspos(
        &self,
        coordinate_space_box: crate::layout::node_data::NodeSlotId,
        entry: PendingAbsposChild,
    ) {
        let mut inner = self.inner.borrow_mut();
        #[cfg(debug_assertions)]
        assert!(
            !inner.placed_slots.contains(&coordinate_space_box.slot_index()),
            "an abspos registration named an already-placed coordinate-space box"
        );
        if coordinate_space_box == self.root_node {
            inner.pending_abspos_at_root.push(entry);
            return;
        }
        match inner.pending_fragments.get_mut(&coordinate_space_box.slot_index()) {
            Some(pending_fragment) => pending_fragment.pending_abspos.push(entry),
            None => {
                if self.is_entry_accumulator {
                    inner.pending_abspos_at_root.push(entry);
                } else {
                    inner.pending_fragments.insert(
                        coordinate_space_box.slot_index(),
                        PendingFragment {
                            node: coordinate_space_box,
                            children: Vec::new(),
                            pending_abspos: vec![entry],
                        },
                    );
                }
            }
        }
    }

    pub(crate) fn override_containing_block_info_for_pending_abspos_of_containing_block(
        &self,
        containing_block: crate::layout::node_data::NodeSlotId,
        callbacks: &FfiLayoutFcCallbacks,
        containing_block_info_for_child: impl Fn(crate::layout::node_data::NodeSlotId) -> AbsposContainingBlockInfo,
    ) {
        for entry in &mut self.inner.borrow_mut().pending_abspos_at_root {
            if callbacks.containing_block(entry.child_box) == containing_block {
                entry.containing_block_info_override = Some(containing_block_info_for_child(entry.child_box));
            }
        }
    }

    pub(crate) fn any_pending_abspos_has_inline_containing_block(&self) -> bool {
        self.inner
            .borrow()
            .iter_pending_abspos()
            .any(|entry| !entry.inline_containing_block.is_invalid())
    }

    pub(crate) fn any_pending_abspos_names_inline_containing_block(&self, inline_box: crate::layout::node_data::NodeSlotId) -> bool {
        self.inner
            .borrow()
            .iter_pending_abspos()
            .any(|entry| entry.inline_containing_block == inline_box)
    }

    pub(crate) fn set_inline_containing_block_rect_on_pending_abspos(
        &self,
        inline_box: crate::layout::node_data::NodeSlotId,
        rect: PhysicalRect,
        expected_space: crate::layout::node_data::NodeSlotId,
    ) {
        for entry in self.inner.borrow_mut().iter_pending_abspos_mut() {
            if entry.inline_containing_block == inline_box {
                debug_assert!(
                    entry.coordinate_space_box == expected_space,
                    "an inline containing block rect met an entry in a different space"
                );
                entry.inline_containing_block_rect = Some(rect);
            }
        }
    }

    pub(crate) fn take_drainable_abspos(
        &self,
        placed: crate::layout::node_data::NodeSlotId,
        callbacks: &FfiLayoutFcCallbacks,
    ) -> Vec<PendingAbsposChild> {
        let mut inner = self.inner.borrow_mut();
        let inner = &mut *inner;
        let containing_block_is_placed = |entry: &PendingAbsposChild| {
            let containing_block = callbacks.containing_block(entry.child_box);
            !containing_block.is_invalid()
                && (self.is_entry_accumulator || containing_block != self.root_node)
                && (containing_block == placed || inner.placed_slots.contains(&containing_block.slot_index()))
        };
        let mut taken: Vec<PendingAbsposChild> = inner
            .pending_abspos_at_root
            .extract_if(.., |entry| containing_block_is_placed(entry))
            .collect();
        if let Some(pending_fragment) = inner.pending_fragments.get_mut(&placed.slot_index()) {
            taken.extend(
                pending_fragment
                    .pending_abspos
                    .extract_if(.., |entry| containing_block_is_placed(entry)),
            );
        }
        taken.sort_by(|left, right| {
            if left.child_box == right.child_box {
                std::cmp::Ordering::Equal
            } else if callbacks.is_before(left.child_box, right.child_box) {
                std::cmp::Ordering::Less
            } else {
                std::cmp::Ordering::Greater
            }
        });
        taken
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
        let (scoped_descendants, propagated_pending_abspos) =
            match inner.child_roots_awaiting_placement.remove(&slot) {
                Some(root) => {
                    debug_assert!(root.node == node, "a held unplaced root was keyed under a different box");
                    (root.scoped_descendants, root.propagated_pending_abspos)
                }
                None => (Vec::new(), Vec::new()),
            };
        let pending_fragment = inner.pending_fragments.entry(slot).or_insert_with(|| PendingFragment {
            node,
            children: Vec::new(),
            pending_abspos: Vec::new(),
        });
        debug_assert!(
            pending_fragment.children.is_empty() || scoped_descendants.is_empty(),
            "a held unplaced root and an open pending fragment both carry children for slot {slot}"
        );
        pending_fragment.children.extend(scoped_descendants);
        pending_fragment.pending_abspos.extend(propagated_pending_abspos);
    }

    pub(crate) fn build_fragment_for_placed_box(
        &self,
        callbacks: &FfiLayoutFcCallbacks,
        node: crate::layout::node_data::NodeSlotId,
        containing_block: Option<crate::layout::node_data::NodeSlotId>,
        used: &UsedValues,
        containing_block_is_sealed: bool,
        containing_line_box_index: Option<usize>,
    ) {
        let mut inner = self.inner.borrow_mut();
        let slot = node.slot_index();
        assert!(inner.placed_slots.insert(slot), "a box was placed twice in one run");
        debug_assert!(
            !inner.child_roots_awaiting_placement.contains_key(&slot),
            "a box was placed without normalizing its held unplaced root (slot {slot})"
        );
        let (children, pending_abspos_from_placed_box) = match inner.pending_fragments.remove(&slot) {
            Some(pending_fragment) => (pending_fragment.children, pending_fragment.pending_abspos),
            None => (Vec::new(), Vec::new()),
        };
        let link = link_fragment(
            snapshot_fragment(node, children, used),
            PlacementData::from_record(used, containing_line_box_index),
        );
        self.attach(&mut inner, link, containing_block, containing_block_is_sealed);
        let content_offset = used.content_offset.get();
        for mut entry in pending_abspos_from_placed_box {
            debug_assert!(
                callbacks.containing_block(entry.child_box) != node,
                "an abspos registration outlived its containing block's placement drain (slot {})",
                entry.child_box.slot_index()
            );
            propagate_payload_into_containing_block_space(&mut entry, node, containing_block, content_offset);
            match inner.pending_fragments.get_mut(&entry.coordinate_space_box.slot_index()) {
                Some(pending_fragment) => pending_fragment.pending_abspos.push(entry),
                None => inner.pending_abspos_at_root.push(entry),
            }
        }
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
                pending_abspos: Vec::new(),
            },
        );
    }

    pub(crate) fn take_unplaced_root(&self, state: &LayoutState, callbacks: &FfiLayoutFcCallbacks) -> UnplacedRootFragment {
        debug_assert!(!self.is_entry_accumulator, "an entry accumulator closes as a pass, not a run");
        let (scoped_descendants, propagated_pending_abspos) = self.close(state, callbacks);
        UnplacedRootFragment {
            node: self.root_node,
            scoped_descendants,
            propagated_pending_abspos,
        }
    }

    pub(crate) fn take_completed_pass(&self, state: &LayoutState, callbacks: &FfiLayoutFcCallbacks) -> CompletedPassFragments {
        debug_assert!(self.is_entry_accumulator, "an ordinary run closes as a singular unplaced root");
        let (roots, _dropped_pending_abspos) = self.close(state, callbacks);
        CompletedPassFragments { roots }
    }

    fn close(
        &self,
        state: &LayoutState,
        callbacks: &FfiLayoutFcCallbacks,
    ) -> (Vec<FragmentLink>, Vec<PendingAbsposChild>) {
        let mut inner = self.inner.take();
        let mut propagated_pending_abspos = std::mem::take(&mut inner.pending_abspos_at_root);
        let pending_fragments = std::mem::take(&mut inner.pending_fragments);
        for (slot, pending_fragment) in pending_fragments {
            propagated_pending_abspos.extend(pending_fragment.pending_abspos);
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
            propagated_pending_abspos.extend(root.propagated_pending_abspos);
            inner.top_scope_links.push(link_fragment(
                snapshot_fragment(root.node, root.scoped_descendants, used),
                PlacementData::from_record(used, None),
            ));
        }
        for entry in &mut propagated_pending_abspos {
            #[cfg(debug_assertions)]
            {
                let containing_block = callbacks.containing_block(entry.child_box);
                debug_assert!(
                    containing_block.is_invalid()
                        || containing_block == self.root_node
                        || !inner.placed_slots.contains(&containing_block.slot_index()),
                    "a drainable abspos registration survived its containing block's run (slot {})",
                    entry.child_box.slot_index()
                );
            }
            propagate_payload_toward_run_root_space(entry, self.root_node, state, callbacks);
        }
        (inner.top_scope_links, propagated_pending_abspos)
    }
}