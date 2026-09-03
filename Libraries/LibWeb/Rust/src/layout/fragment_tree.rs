/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

pub(crate) struct Fragment {
    pub(crate) identity: u64,
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
    pub(crate) uses_collapsing_borders_model: bool,
    pub(crate) collapsed_table_borders: Option<std::rc::Rc<table_formatting_context::OwnedCollapsedTableBorders>>,
    pub(crate) line_data: Option<std::rc::Rc<used_values::LineData>>,
    pub(crate) grid_layout_data: Option<std::rc::Rc<grid_formatting_context::GridLayoutData>>,
    pub(crate) flex_layout_data: Option<std::rc::Rc<formatting_context::FlexLayoutData>>,
    pub(crate) used_grid_tracks: Option<std::rc::Rc<grid_formatting_context::OwnedUsedGridTracks>>,
    pub(crate) svg_viewport_transform: Option<svg_formatting_context::FfiAffineTransform>,
    pub(crate) svg_viewport_size: Option<FfiCssPixelSize>,
    pub(crate) svg_view_box: Option<svg_formatting_context::FfiSvgViewBox>,
    pub(crate) svg_viewport_percentage_basis: CssPixels,
    pub(crate) computed_svg_path: Option<std::rc::Rc<libgfx_rust::path::OwnedPath>>,
    pub(crate) has_line_clamp_point: bool,
    pub(crate) is_invisible_for_line_clamp: bool,
    pub(crate) children: Vec<FragmentLink>,
}

#[derive(Clone)]
pub(crate) struct FragmentLink {
    pub(crate) fragment: std::rc::Rc<Fragment>,
    pub(crate) committed_offset: FfiCssPixelPoint,
    pub(crate) inset_left: CssPixels,
    pub(crate) inset_right: CssPixels,
    pub(crate) inset_top: CssPixels,
    pub(crate) inset_bottom: CssPixels,
    pub(crate) containing_line_box_index: Option<usize>,
    pub(crate) abspos_layout_inputs: Option<abspos_inputs::AbsposLayoutInputs>,
}

fn same_allocation<T>(left: Option<&std::rc::Rc<T>>, right: Option<&std::rc::Rc<T>>) -> bool {
    match (left, right) {
        (None, None) => true,
        (Some(left), Some(right)) => std::rc::Rc::ptr_eq(left, right),
        _ => false,
    }
}

impl Fragment {
    fn builds_identically_to(&self, previous: &Fragment) -> bool {
        self.node == previous.node
            && self.content_inline_size == previous.content_inline_size
            && self.content_block_size == previous.content_block_size
            && self.margin_left == previous.margin_left
            && self.margin_right == previous.margin_right
            && self.margin_top == previous.margin_top
            && self.margin_bottom == previous.margin_bottom
            && self.border_left == previous.border_left
            && self.border_right == previous.border_right
            && self.border_top == previous.border_top
            && self.border_bottom == previous.border_bottom
            && self.padding_left == previous.padding_left
            && self.padding_right == previous.padding_right
            && self.padding_top == previous.padding_top
            && self.padding_bottom == previous.padding_bottom
            && self.uses_collapsing_borders_model == previous.uses_collapsing_borders_model
            && same_allocation(
                self.collapsed_table_borders.as_ref(),
                previous.collapsed_table_borders.as_ref(),
            )
            && same_allocation(self.line_data.as_ref(), previous.line_data.as_ref())
            && same_allocation(self.grid_layout_data.as_ref(), previous.grid_layout_data.as_ref())
            && same_allocation(self.flex_layout_data.as_ref(), previous.flex_layout_data.as_ref())
            && same_allocation(self.used_grid_tracks.as_ref(), previous.used_grid_tracks.as_ref())
            && self.svg_viewport_transform == previous.svg_viewport_transform
            && self.svg_viewport_size == previous.svg_viewport_size
            && self.svg_view_box == previous.svg_view_box
            && self.svg_viewport_percentage_basis == previous.svg_viewport_percentage_basis
            && same_allocation(self.computed_svg_path.as_ref(), previous.computed_svg_path.as_ref())
            && self.has_line_clamp_point == previous.has_line_clamp_point
            && self.is_invisible_for_line_clamp == previous.is_invisible_for_line_clamp
            && self.children.len() == previous.children.len()
            && self
                .children
                .iter()
                .zip(&previous.children)
                .all(|(link, previous_link)| link.places_same_fragment_identically_to(previous_link))
    }
}

impl FragmentLink {
    fn places_same_fragment_identically_to(&self, previous: &FragmentLink) -> bool {
        std::rc::Rc::ptr_eq(&self.fragment, &previous.fragment)
            && self.committed_offset == previous.committed_offset
            && self.inset_left == previous.inset_left
            && self.inset_right == previous.inset_right
            && self.inset_top == previous.inset_top
            && self.inset_bottom == previous.inset_bottom
            && self.containing_line_box_index == previous.containing_line_box_index
            && self.abspos_layout_inputs == previous.abspos_layout_inputs
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct AnchorCandidate {
    pub(crate) node: crate::layout::node_data::NodeSlotId,
    pub(crate) border_box_rect: formatting_context::PhysicalRect,
    pub(crate) coordinate_space_box: crate::layout::node_data::NodeSlotId,
}

/// The padding-box rect of an inline box that acts as an abspos containing
/// block, spanning its first and last content lines.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct InlineContainingBlockRect {
    pub(crate) inline_box: crate::layout::node_data::NodeSlotId,
    pub(crate) rect: formatting_context::PhysicalRect,
    pub(crate) coordinate_space_box: crate::layout::node_data::NodeSlotId,
}

/// Containing block info a formatting context computed for a pending abspos
/// child it does not itself register (a grid supplying its grid area to deep
/// descendants). The rect is relative to the containing block's own content
/// origin, so the contribution never rebases; it travels as-is to whichever
/// run drains the child and is joined by child-box identity.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct AbsposContainingBlockInfoContribution {
    pub(crate) child_box: crate::layout::node_data::NodeSlotId,
    pub(crate) info: abspos_inputs::AbsposContainingBlockInfo,
}

trait PropagatedPayload {
    fn coordinate_space_box(&self) -> crate::layout::node_data::NodeSlotId;
    fn set_coordinate_space_box(&mut self, node: crate::layout::node_data::NodeSlotId);
    fn translate_by(&mut self, offset: FfiCssPixelPoint);
}

impl PropagatedPayload for abspos_inputs::PendingAbsposChild {
    fn coordinate_space_box(&self) -> crate::layout::node_data::NodeSlotId {
        self.coordinate_space_box
    }
    fn set_coordinate_space_box(&mut self, node: crate::layout::node_data::NodeSlotId) {
        self.coordinate_space_box = node;
    }
    fn translate_by(&mut self, offset: FfiCssPixelPoint) {
        self.static_position_rect =
            formatting_context::translate_static_position_rect(self.static_position_rect, offset);
    }
}

impl PropagatedPayload for InlineContainingBlockRect {
    fn coordinate_space_box(&self) -> crate::layout::node_data::NodeSlotId {
        self.coordinate_space_box
    }
    fn set_coordinate_space_box(&mut self, node: crate::layout::node_data::NodeSlotId) {
        self.coordinate_space_box = node;
    }
    fn translate_by(&mut self, offset: FfiCssPixelPoint) {
        self.rect.x += offset.x;
        self.rect.y += offset.y;
    }
}

impl PropagatedPayload for AnchorCandidate {
    fn coordinate_space_box(&self) -> crate::layout::node_data::NodeSlotId {
        self.coordinate_space_box
    }
    fn set_coordinate_space_box(&mut self, node: crate::layout::node_data::NodeSlotId) {
        self.coordinate_space_box = node;
    }
    fn translate_by(&mut self, offset: FfiCssPixelPoint) {
        self.border_box_rect.x += offset.x;
        self.border_box_rect.y += offset.y;
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
    records: &RunRecords,
    callbacks: &FfiLayoutFcCallbacks,
) {
    while payload.coordinate_space_box() != run_root {
        let Some(used) = records
            .used_values_if_owned(payload.coordinate_space_box())
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

fn previously_committed_fragment_matching(
    callbacks: &FfiLayoutFcCallbacks,
    candidate: &Fragment,
) -> Option<std::rc::Rc<Fragment>> {
    if !callbacks.has_committed_fragment_link(candidate.node) {
        return None;
    }
    callbacks
        .arena()
        .with_committed_fragment_link_during_layout(candidate.node, |previous_link| {
            previous_link
                .filter(|previous_link| candidate.builds_identically_to(&previous_link.fragment))
                .map(|previous_link| previous_link.fragment.clone())
        })
}

fn snapshot_fragment(
    callbacks: &FfiLayoutFcCallbacks,
    node: crate::layout::node_data::NodeSlotId,
    children: Vec<FragmentLink>,
    used: &UsedValues,
) -> std::rc::Rc<Fragment> {
    static NEXT_IDENTITY: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
    let line_data = used.line_data.get().map(std::cell::RefCell::take);
    let rare_payloads = used.rare_data.get().map(|cell| {
        let mut rare = cell.borrow_mut();
        (
            rare.collapsed_table_borders.take(),
            rare.grid_layout_data.take(),
            rare.flex_layout_data.take(),
            rare.used_grid_tracks.take(),
            rare.svg_viewport_transform,
            rare.svg_viewport_size,
            rare.svg_view_box,
            rare.svg_viewport_percentage_basis,
            rare.computed_svg_path.take(),
        )
    });
    let (
        collapsed_table_borders,
        grid_layout_data,
        flex_layout_data,
        used_grid_tracks,
        svg_viewport_transform,
        svg_viewport_size,
        svg_view_box,
        svg_viewport_percentage_basis,
        computed_svg_path,
    ) = rare_payloads.unwrap_or_default();
    let mut fragment = Fragment {
        identity: 0,
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
        uses_collapsing_borders_model: used.uses_collapsing_borders_model.get(),
        collapsed_table_borders,
        line_data,
        grid_layout_data,
        flex_layout_data,
        used_grid_tracks,
        svg_viewport_transform,
        svg_viewport_size,
        svg_view_box,
        svg_viewport_percentage_basis,
        computed_svg_path,
        has_line_clamp_point: used.has_line_clamp_point.get(),
        is_invisible_for_line_clamp: used.is_invisible_for_line_clamp.get(),
        children,
    };
    if let Some(previous) = previously_committed_fragment_matching(callbacks, &fragment) {
        return previous;
    }
    fragment.identity = NEXT_IDENTITY.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    std::rc::Rc::new(fragment)
}

pub(crate) struct PlacementData {
    pub(crate) committed_offset: FfiCssPixelPoint,
    pub(crate) inset_left: CssPixels,
    pub(crate) inset_right: CssPixels,
    pub(crate) inset_top: CssPixels,
    pub(crate) inset_bottom: CssPixels,
    pub(crate) containing_line_box_index: Option<usize>,
    pub(crate) abspos_layout_inputs: Option<abspos_inputs::AbsposLayoutInputs>,
}

impl PlacementData {
    fn from_record(
        used: &UsedValues,
        containing_line_box_index: Option<usize>,
        committed_offset: FfiCssPixelPoint,
    ) -> Self {
        Self {
            committed_offset,
            inset_left: used.inset_left.get(),
            inset_right: used.inset_right.get(),
            inset_top: used.inset_top.get(),
            inset_bottom: used.inset_bottom.get(),
            containing_line_box_index,
            abspos_layout_inputs: used.rare_data.get().and_then(|cell| cell.borrow().abspos_layout_inputs),
        }
    }
}

fn link_fragment(fragment: std::rc::Rc<Fragment>, placement: PlacementData) -> FragmentLink {
    FragmentLink {
        fragment,
        committed_offset: placement.committed_offset,
        inset_left: placement.inset_left,
        inset_right: placement.inset_right,
        inset_top: placement.inset_top,
        inset_bottom: placement.inset_bottom,
        containing_line_box_index: placement.containing_line_box_index,
        abspos_layout_inputs: placement.abspos_layout_inputs,
    }
}

#[derive(Clone)]
pub(crate) struct UnplacedRootFragment {
    pub(crate) node: crate::layout::node_data::NodeSlotId,
    pub(crate) scoped_descendants: Vec<FragmentLink>,
    pub(crate) reused_subtree_roots: HashSet<u32>,
    pub(crate) propagated_pending_abspos: Vec<abspos_inputs::PendingAbsposChild>,
    pub(crate) propagated_anchor_candidates: Vec<AnchorCandidate>,
    pub(crate) propagated_inline_containing_block_rects: Vec<InlineContainingBlockRect>,
    pub(crate) propagated_abspos_containing_block_info: Vec<AbsposContainingBlockInfoContribution>,
}

pub(crate) struct CompletedPassFragments {
    pub(crate) roots: Vec<FragmentLink>,
    pub(crate) reused_subtree_roots: HashSet<u32>,
}

impl CompletedPassFragments {
    pub(crate) fn links_by_slot(&self) -> HashMap<u32, &FragmentLink> {
        fn insert_links<'tree>(links: &'tree [FragmentLink], links_by_slot: &mut HashMap<u32, &'tree FragmentLink>) {
            for link in links {
                let previous = links_by_slot.insert(link.fragment.node.slot_index(), link);
                assert!(
                    previous.is_none(),
                    "two fragments claim slot {}",
                    link.fragment.node.slot_index()
                );
                insert_links(&link.fragment.children, links_by_slot);
            }
        }
        let mut links_by_slot = HashMap::default();
        insert_links(&self.roots, &mut links_by_slot);
        links_by_slot
    }

    pub(crate) fn subtree_was_reused(&self, slot: u32) -> bool {
        self.reused_subtree_roots.contains(&slot)
    }
}

struct PendingFragment {
    node: crate::layout::node_data::NodeSlotId,
    children: Vec<FragmentLink>,
    pending_abspos: Vec<abspos_inputs::PendingAbsposChild>,
    anchor_candidates: Vec<AnchorCandidate>,
    inline_containing_block_rects: Vec<InlineContainingBlockRect>,
}

impl PendingFragment {
    fn new(node: crate::layout::node_data::NodeSlotId) -> Self {
        Self {
            node,
            children: Vec::new(),
            pending_abspos: Vec::new(),
            anchor_candidates: Vec::new(),
            inline_containing_block_rects: Vec::new(),
        }
    }
}

pub(crate) struct RunFragmentBuilder {
    root_node: crate::layout::node_data::NodeSlotId,
    root_containing_block_slot: Option<u32>,
    is_entry_accumulator: bool,
    inner: std::cell::RefCell<RunFragmentBuilderInner>,
}

#[derive(Default)]
struct RunFragmentBuilderInner {
    pending_fragments: HashMap<u32, PendingFragment>,
    #[cfg(debug_assertions)]
    placed_slots: HashSet<u32>,
    child_roots_awaiting_placement: HashMap<u32, UnplacedRootFragment>,
    pending_abspos_at_root: Vec<abspos_inputs::PendingAbsposChild>,
    anchor_candidates_at_root: Vec<AnchorCandidate>,
    inline_containing_block_rects_at_root: Vec<InlineContainingBlockRect>,
    abspos_containing_block_info_contributions: Vec<AbsposContainingBlockInfoContribution>,
    top_scope_links: Vec<FragmentLink>,
    reused_subtree_roots: HashSet<u32>,
}

impl RunFragmentBuilderInner {
    fn iter_pending_abspos(&self) -> impl Iterator<Item = &abspos_inputs::PendingAbsposChild> {
        self.pending_fragments
            .values()
            .flat_map(|pending_fragment| pending_fragment.pending_abspos.iter())
            .chain(self.pending_abspos_at_root.iter())
    }

    fn iter_anchor_candidates(&self) -> impl Iterator<Item = &AnchorCandidate> {
        self.pending_fragments
            .values()
            .flat_map(|pending_fragment| pending_fragment.anchor_candidates.iter())
            .chain(self.anchor_candidates_at_root.iter())
    }

    fn iter_inline_containing_block_rects(&self) -> impl Iterator<Item = &InlineContainingBlockRect> {
        self.pending_fragments
            .values()
            .flat_map(|pending_fragment| pending_fragment.inline_containing_block_rects.iter())
            .chain(self.inline_containing_block_rects_at_root.iter())
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
        entry: abspos_inputs::PendingAbsposChild,
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
                    let mut pending_fragment = PendingFragment::new(coordinate_space_box);
                    pending_fragment.pending_abspos.push(entry);
                    inner
                        .pending_fragments
                        .insert(coordinate_space_box.slot_index(), pending_fragment);
                }
            }
        }
    }

    pub(crate) fn register_inline_containing_block_rect(
        &self,
        inline_box: crate::layout::node_data::NodeSlotId,
        rect: formatting_context::PhysicalRect,
        coordinate_space_box: crate::layout::node_data::NodeSlotId,
    ) {
        let mut inner = self.inner.borrow_mut();
        #[cfg(debug_assertions)]
        assert!(
            !inner.placed_slots.contains(&coordinate_space_box.slot_index()),
            "an inline containing block rect named an already-placed coordinate-space box"
        );
        debug_assert!(
            !inner
                .iter_inline_containing_block_rects()
                .any(|payload| payload.inline_box == inline_box),
            "an inline box's containing block rect was registered twice"
        );
        let payload = InlineContainingBlockRect {
            inline_box,
            rect,
            coordinate_space_box,
        };
        if coordinate_space_box == self.root_node {
            inner.inline_containing_block_rects_at_root.push(payload);
            return;
        }
        match inner.pending_fragments.get_mut(&coordinate_space_box.slot_index()) {
            Some(pending_fragment) => pending_fragment.inline_containing_block_rects.push(payload),
            None => {
                if self.is_entry_accumulator {
                    inner.inline_containing_block_rects_at_root.push(payload);
                } else {
                    let mut pending_fragment = PendingFragment::new(coordinate_space_box);
                    pending_fragment.inline_containing_block_rects.push(payload);
                    inner
                        .pending_fragments
                        .insert(coordinate_space_box.slot_index(), pending_fragment);
                }
            }
        }
    }

    pub(crate) fn find_inline_containing_block_rect(
        &self,
        inline_box: crate::layout::node_data::NodeSlotId,
    ) -> Option<(formatting_context::PhysicalRect, crate::layout::node_data::NodeSlotId)> {
        self.inner
            .borrow()
            .iter_inline_containing_block_rects()
            .find(|payload| payload.inline_box == inline_box)
            .map(|payload| (payload.rect, payload.coordinate_space_box))
    }

    /// Lists pending abspos children whose containing block is the given box
    /// and whose registration did not already resolve containing block info.
    /// Only root-resting entries are considered: by the time a containing
    /// block asks (its run's tail), every interior box is placed, so entries
    /// it contains have propagated to the root list.
    pub(crate) fn pending_abspos_children_awaiting_containing_block_info(
        &self,
        containing_block: crate::layout::node_data::NodeSlotId,
        callbacks: &FfiLayoutFcCallbacks,
    ) -> Vec<crate::layout::node_data::NodeSlotId> {
        let inner = self.inner.borrow();
        let awaits_info = |entry: &abspos_inputs::PendingAbsposChild| {
            entry.containing_block_info_override.is_none()
                && callbacks.containing_block(entry.child_box) == containing_block
        };
        debug_assert!(
            !inner
                .pending_fragments
                .values()
                .flat_map(|pending_fragment| pending_fragment.pending_abspos.iter())
                .any(awaits_info),
            "a pending abspos child awaiting containing block info rests below the root list"
        );
        inner
            .pending_abspos_at_root
            .iter()
            .filter(|entry| awaits_info(entry))
            .map(|entry| entry.child_box)
            .collect()
    }

    pub(crate) fn register_abspos_containing_block_info(
        &self,
        child_box: crate::layout::node_data::NodeSlotId,
        info: abspos_inputs::AbsposContainingBlockInfo,
    ) {
        let mut inner = self.inner.borrow_mut();
        debug_assert!(
            !inner
                .abspos_containing_block_info_contributions
                .iter()
                .any(|contribution| contribution.child_box == child_box),
            "an abspos child's containing block info was contributed twice"
        );
        inner
            .abspos_containing_block_info_contributions
            .push(AbsposContainingBlockInfoContribution { child_box, info });
    }

    pub(crate) fn find_abspos_containing_block_info(
        &self,
        child_box: crate::layout::node_data::NodeSlotId,
    ) -> Option<abspos_inputs::AbsposContainingBlockInfo> {
        self.inner
            .borrow()
            .abspos_containing_block_info_contributions
            .iter()
            .find(|contribution| contribution.child_box == child_box)
            .map(|contribution| contribution.info)
    }

    pub(crate) fn any_pending_abspos_has_inline_containing_block(&self) -> bool {
        self.inner
            .borrow()
            .iter_pending_abspos()
            .any(|entry| !entry.inline_containing_block.is_invalid())
    }

    pub(crate) fn any_pending_abspos_names_inline_containing_block(
        &self,
        inline_box: crate::layout::node_data::NodeSlotId,
    ) -> bool {
        self.inner
            .borrow()
            .iter_pending_abspos()
            .any(|entry| entry.inline_containing_block == inline_box)
    }

    pub(crate) fn anchor_candidate_shells(&self, callbacks: &FfiLayoutFcCallbacks) -> Vec<*mut c_void> {
        self.inner
            .borrow()
            .iter_anchor_candidates()
            .map(|candidate| callbacks.shell(candidate.node))
            .collect()
    }

    pub(crate) fn find_anchor_candidate(
        &self,
        node: crate::layout::node_data::NodeSlotId,
    ) -> Option<(formatting_context::PhysicalRect, crate::layout::node_data::NodeSlotId)> {
        self.inner
            .borrow()
            .iter_anchor_candidates()
            .find(|candidate| candidate.node == node)
            .map(|candidate| (candidate.border_box_rect, candidate.coordinate_space_box))
    }

    pub(crate) fn take_drainable_abspos(
        &self,
        placed: crate::layout::node_data::NodeSlotId,
        records: &RunRecords,
        callbacks: &FfiLayoutFcCallbacks,
    ) -> Vec<abspos_inputs::PendingAbsposChild> {
        let mut inner = self.inner.borrow_mut();
        let containing_block_is_owned_and_placed = |entry: &abspos_inputs::PendingAbsposChild| {
            let containing_block = callbacks.containing_block(entry.child_box);
            !containing_block.is_invalid()
                && (self.is_entry_accumulator || containing_block != self.root_node)
                && records
                    .used_values_if_owned(containing_block)
                    .is_some_and(|containing_block_used| containing_block_used.has_content_offset.get())
        };
        let mut taken: Vec<abspos_inputs::PendingAbsposChild> = inner
            .pending_abspos_at_root
            .extract_if(.., |entry| containing_block_is_owned_and_placed(entry))
            .collect();
        if let Some(pending_fragment) = inner.pending_fragments.get_mut(&placed.slot_index()) {
            taken.extend(
                pending_fragment
                    .pending_abspos
                    .extract_if(.., |entry| containing_block_is_owned_and_placed(entry)),
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
        let mut inner = self.inner.borrow_mut();
        inner
            .reused_subtree_roots
            .extend(root.reused_subtree_roots.iter().copied());
        let previous = inner.child_roots_awaiting_placement.insert(slot, root);
        debug_assert!(
            previous.is_none(),
            "a child run's root was handed over twice before placement"
        );
    }

    pub(crate) fn note_reused_subtree_root(&self, node: crate::layout::node_data::NodeSlotId) {
        self.inner.borrow_mut().reused_subtree_roots.insert(node.slot_index());
    }

    pub(crate) fn clear_reused_subtree_root(&self, node: crate::layout::node_data::NodeSlotId) {
        self.inner.borrow_mut().reused_subtree_roots.remove(&node.slot_index());
    }

    pub(crate) fn discard_unplaced_subtree(&self, node: crate::layout::node_data::NodeSlotId) {
        let mut inner = self.inner.borrow_mut();
        let slot = node.slot_index();
        if let Some(root) = inner.child_roots_awaiting_placement.remove(&slot) {
            for reused_subtree_root in root.reused_subtree_roots {
                inner.reused_subtree_roots.remove(&reused_subtree_root);
            }
        }
        inner.reused_subtree_roots.remove(&slot);
        inner.pending_fragments.remove(&slot);
    }

    pub(crate) fn normalize_arrivals_for_placement(&self, node: crate::layout::node_data::NodeSlotId) {
        let mut inner = self.inner.borrow_mut();
        let slot = node.slot_index();
        let root = inner.child_roots_awaiting_placement.remove(&slot);
        let pending_fragment = inner
            .pending_fragments
            .entry(slot)
            .or_insert_with(|| PendingFragment::new(node));
        let Some(root) = root else {
            return;
        };
        debug_assert!(
            root.node == node,
            "a held unplaced root was keyed under a different box"
        );
        debug_assert!(
            pending_fragment.children.is_empty() || root.scoped_descendants.is_empty(),
            "a held unplaced root and an open pending fragment both carry children for slot {slot}"
        );
        pending_fragment.children.extend(root.scoped_descendants);
        pending_fragment.pending_abspos.extend(root.propagated_pending_abspos);
        pending_fragment
            .anchor_candidates
            .extend(root.propagated_anchor_candidates);
        pending_fragment
            .inline_containing_block_rects
            .extend(root.propagated_inline_containing_block_rects);
        inner
            .abspos_containing_block_info_contributions
            .extend(root.propagated_abspos_containing_block_info);
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn build_fragment_for_placed_box(
        &self,
        callbacks: &FfiLayoutFcCallbacks,
        node: crate::layout::node_data::NodeSlotId,
        containing_block: Option<crate::layout::node_data::NodeSlotId>,
        used: &UsedValues,
        containing_block_is_sealed: bool,
        containing_line_box_index: Option<usize>,
        committed_offset: FfiCssPixelPoint,
        own_anchor_candidate_border_box_rect: Option<formatting_context::PhysicalRect>,
    ) {
        let mut inner = self.inner.borrow_mut();
        let slot = node.slot_index();
        #[cfg(debug_assertions)]
        assert!(inner.placed_slots.insert(slot), "a box was placed twice in one run");
        debug_assert!(
            !inner.child_roots_awaiting_placement.contains_key(&slot),
            "a box was placed without normalizing its held unplaced root (slot {slot})"
        );
        let PendingFragment {
            node: _,
            children,
            pending_abspos: pending_abspos_from_placed_box,
            anchor_candidates: anchor_candidates_from_placed_box,
            inline_containing_block_rects: inline_containing_block_rects_from_placed_box,
        } = inner
            .pending_fragments
            .remove(&slot)
            .unwrap_or_else(|| PendingFragment::new(node));
        let link = link_fragment(
            snapshot_fragment(callbacks, node, children, used),
            PlacementData::from_record(used, containing_line_box_index, committed_offset),
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
            match inner
                .pending_fragments
                .get_mut(&entry.coordinate_space_box.slot_index())
            {
                Some(pending_fragment) => pending_fragment.pending_abspos.push(entry),
                None => inner.pending_abspos_at_root.push(entry),
            }
        }
        let own_candidate = own_anchor_candidate_border_box_rect.map(|border_box_rect| AnchorCandidate {
            node,
            border_box_rect,
            coordinate_space_box: containing_block.unwrap_or(node),
        });
        for mut candidate in anchor_candidates_from_placed_box.into_iter().chain(own_candidate) {
            propagate_payload_into_containing_block_space(&mut candidate, node, containing_block, content_offset);
            match inner
                .pending_fragments
                .get_mut(&candidate.coordinate_space_box.slot_index())
            {
                Some(pending_fragment) => pending_fragment.anchor_candidates.push(candidate),
                None => inner.anchor_candidates_at_root.push(candidate),
            }
        }
        for mut payload in inline_containing_block_rects_from_placed_box {
            propagate_payload_into_containing_block_space(&mut payload, node, containing_block, content_offset);
            match inner
                .pending_fragments
                .get_mut(&payload.coordinate_space_box.slot_index())
            {
                Some(pending_fragment) => pending_fragment.inline_containing_block_rects.push(payload),
                None => inner.inline_containing_block_rects_at_root.push(payload),
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
        if containing_block == self.root_node || Some(containing_block.slot_index()) == self.root_containing_block_slot
        {
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
        let mut pending_fragment = PendingFragment::new(containing_block);
        pending_fragment.children.push(link);
        inner
            .pending_fragments
            .insert(containing_block.slot_index(), pending_fragment);
    }

    pub(crate) fn take_unplaced_root(
        &self,
        records: &RunRecords,
        callbacks: &FfiLayoutFcCallbacks,
    ) -> UnplacedRootFragment {
        debug_assert!(
            !self.is_entry_accumulator,
            "an entry accumulator closes as a pass, not a run"
        );
        self.close(records, callbacks)
    }

    pub(crate) fn take_completed_pass(
        &self,
        records: &RunRecords,
        callbacks: &FfiLayoutFcCallbacks,
    ) -> CompletedPassFragments {
        debug_assert!(
            self.is_entry_accumulator,
            "an ordinary run closes as a singular unplaced root"
        );
        let root = self.close(records, callbacks);
        CompletedPassFragments {
            roots: root.scoped_descendants,
            reused_subtree_roots: root.reused_subtree_roots,
        }
    }

    fn close(&self, records: &RunRecords, callbacks: &FfiLayoutFcCallbacks) -> UnplacedRootFragment {
        let mut inner = self.inner.take();
        let mut propagated_pending_abspos = std::mem::take(&mut inner.pending_abspos_at_root);
        let mut propagated_anchor_candidates = std::mem::take(&mut inner.anchor_candidates_at_root);
        let mut propagated_inline_containing_block_rects =
            std::mem::take(&mut inner.inline_containing_block_rects_at_root);
        let propagated_abspos_containing_block_info =
            std::mem::take(&mut inner.abspos_containing_block_info_contributions);
        let pending_fragments = std::mem::take(&mut inner.pending_fragments);
        for (_, pending_fragment) in pending_fragments {
            propagated_pending_abspos.extend(pending_fragment.pending_abspos);
            propagated_anchor_candidates.extend(pending_fragment.anchor_candidates);
            propagated_inline_containing_block_rects.extend(pending_fragment.inline_containing_block_rects);
            if pending_fragment.children.is_empty() {
                continue;
            }
            let used = records.used_values(pending_fragment.node);
            inner.top_scope_links.push(link_fragment(
                snapshot_fragment(callbacks, pending_fragment.node, pending_fragment.children, &used),
                PlacementData::from_record(&used, None, used.content_offset.get()),
            ));
        }
        let child_roots_awaiting_placement = std::mem::take(&mut inner.child_roots_awaiting_placement);
        for (_, root) in child_roots_awaiting_placement {
            let used = records.used_values(root.node);
            inner.top_scope_links.push(link_fragment(
                snapshot_fragment(callbacks, root.node, root.scoped_descendants, &used),
                PlacementData::from_record(&used, None, used.content_offset.get()),
            ));
        }
        for entry in &mut propagated_pending_abspos {
            #[cfg(debug_assertions)]
            {
                let containing_block = callbacks.containing_block(entry.child_box);
                debug_assert!(
                    containing_block.is_invalid()
                        || containing_block == self.root_node
                        || records
                            .used_values_if_owned(containing_block)
                            .is_none_or(|containing_block_used| !containing_block_used.has_content_offset.get()),
                    "a drainable abspos registration survived its containing block's run (slot {})",
                    entry.child_box.slot_index()
                );
            }
            propagate_payload_toward_run_root_space(entry, self.root_node, records, callbacks);
        }
        for candidate in &mut propagated_anchor_candidates {
            propagate_payload_toward_run_root_space(candidate, self.root_node, records, callbacks);
        }
        for payload in &mut propagated_inline_containing_block_rects {
            propagate_payload_toward_run_root_space(payload, self.root_node, records, callbacks);
        }
        UnplacedRootFragment {
            node: self.root_node,
            scoped_descendants: inner.top_scope_links,
            reused_subtree_roots: inner.reused_subtree_roots,
            propagated_pending_abspos,
            propagated_anchor_candidates,
            propagated_inline_containing_block_rects,
            propagated_abspos_containing_block_info,
        }
    }
}
