/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

/// The complete identity of a memoizable run: the layout input plus the
/// pre-run root input cells the dispatch seam captures anyway, so every
/// value a parent hands a spawned run that the run reads is part of the
/// key. Viewport changes reach a run through that input or through the
/// normal style/layout epoch invalidation for viewport-dependent computed
/// values.
#[derive(Clone, Copy, PartialEq)]
pub(super) struct FcRunCacheKey {
    fc_type: formatting_context::FormattingContextType,
    layout_mode: LayoutMode,
    input: LayoutInput,
    root_cells: used_values::RootInputCells,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(super) struct RunInputsTheStoredRunNeverObserved {
    pub(super) percentage_block_basis_and_block_size_definiteness: bool,
    pub(super) definite_available_inline_sizes_at_or_above: Option<CssPixels>,
    pub(super) percentage_inline_basis: bool,
    pub(super) own_block_size: formatting_context::OwnBlockSizeObservation,
}

impl RunInputsTheStoredRunNeverObserved {
    fn proven_by_stored_run(outputs: &formatting_context::RunOutputs) -> Self {
        Self {
            percentage_block_basis_and_block_size_definiteness: !outputs.result.depends_on_percentage_block_size,
            definite_available_inline_sizes_at_or_above: outputs
                .atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above,
            percentage_inline_basis: outputs
                .atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above
                .is_some()
                || outputs.result.percentage_inline_basis_observation
                    == formatting_context::PercentageInlineBasisObservation::Unobserved,
            own_block_size: outputs.result.own_block_size_observation,
        }
    }

    fn accepted_when_reusing_stale_line_data_after_structural_inline_damage(
        outputs: &formatting_context::RunOutputs,
    ) -> Self {
        Self {
            definite_available_inline_sizes_at_or_above: None,
            percentage_inline_basis: false,
            own_block_size: formatting_context::OwnBlockSizeObservation::Observed,
            ..Self::proven_by_stored_run(outputs)
        }
    }

    fn relaxes_any_input(self) -> bool {
        let Self {
            percentage_block_basis_and_block_size_definiteness,
            definite_available_inline_sizes_at_or_above,
            percentage_inline_basis,
            own_block_size,
        } = self;
        percentage_block_basis_and_block_size_definiteness
            || definite_available_inline_sizes_at_or_above.is_some()
            || percentage_inline_basis
            || own_block_size != formatting_context::OwnBlockSizeObservation::Observed
    }

    fn available_block_size_definiteness(self) -> bool {
        self.percentage_block_basis_and_block_size_definiteness
            || self.own_block_size != formatting_context::OwnBlockSizeObservation::Observed
    }
}

fn root_input_cells_match_ignoring_unobserved_own_block_size_and_block_margins(
    stored: used_values::RootInputCells,
    probe: used_values::RootInputCells,
    own_block_size: formatting_context::OwnBlockSizeObservation,
) -> bool {
    match own_block_size {
        formatting_context::OwnBlockSizeObservation::Observed => stored == probe,
        formatting_context::OwnBlockSizeObservation::Unobserved => {
            let without_own_block_size_and_block_margins = |cells| used_values::RootInputCells {
                content_block_size: CssPixels::default(),
                has_definite_block_size: false,
                margin_top: CssPixels::default(),
                margin_bottom: CssPixels::default(),
                ..cells
            };
            without_own_block_size_and_block_margins(stored) == without_own_block_size_and_block_margins(probe)
        }
    }
}

impl FcRunCacheKey {
    pub(super) fn new(
        fc_type: formatting_context::FormattingContextType,
        layout_mode: LayoutMode,
        mut input: LayoutInput,
        root_cells: used_values::RootInputCells,
    ) -> Self {
        if !formatting_context::run_reads_content_block_size_adopted_after_it(fc_type, &input) {
            input.sizing.adopt_automatic_content_block_size = false;
            input.sizing.content_block_size_after_inside_layout = None;
        }
        Self {
            fc_type,
            layout_mode,
            input,
            root_cells,
        }
    }

    pub(super) fn fc_type(&self) -> formatting_context::FormattingContextType {
        self.fc_type
    }

    pub(super) fn has_no_table_cell_intrinsic_block_padding(&self) -> bool {
        self.input.sizing.table_cell_intrinsic_block_padding.is_none()
    }

    /// The key with the intrinsic block padding a table hands its cell taken out of the root's block paddings.
    fn without_table_cell_intrinsic_block_padding(&self) -> Self {
        let mut key = *self;
        if let Some((padding_top, padding_bottom)) = key.input.sizing.table_cell_intrinsic_block_padding {
            key.root_cells.padding_top -= padding_top;
            key.root_cells.padding_bottom -= padding_bottom;
            key.input.sizing.table_cell_intrinsic_block_padding = Some((CssPixels::default(), CssPixels::default()));
        }
        key
    }

    fn matches(&self, probe: &Self, unobserved: RunInputsTheStoredRunNeverObserved) -> bool {
        if self == probe {
            return true;
        }
        if !unobserved.relaxes_any_input() {
            return false;
        }
        self.fc_type == probe.fc_type
            && self.layout_mode == probe.layout_mode
            && root_input_cells_match_ignoring_unobserved_own_block_size_and_block_margins(
                self.root_cells,
                probe.root_cells,
                unobserved.own_block_size,
            )
            && layout_inputs_match_ignoring_unobserved(&self.input, &probe.input, unobserved)
    }
}

fn available_block_sizes_match(a: AvailableSize, b: AvailableSize, block_size_definiteness_unobserved: bool) -> bool {
    let is_definite_or_indefinite =
        |size: AvailableSize| matches!(size, AvailableSize::Definite(_) | AvailableSize::Indefinite);
    a == b || (block_size_definiteness_unobserved && is_definite_or_indefinite(a) && is_definite_or_indefinite(b))
}

fn layout_inputs_match_ignoring_unobserved(
    a: &LayoutInput,
    b: &LayoutInput,
    unobserved: RunInputsTheStoredRunNeverObserved,
) -> bool {
    let LayoutInput {
        available_space,
        containing_block_constraints,
        content_box_position_in_bfc_root,
        sizing,
        participation,
    } = *a;
    let ContainingBlockConstraints {
        percentage_basis_inline_size,
        percentage_basis_block_size,
        quirks_mode_percentage_basis_block_size,
    } = containing_block_constraints;
    if participation != b.participation || content_box_position_in_bfc_root != b.content_box_position_in_bfc_root {
        return false;
    }
    let available_inline_sizes_match = available_space.inline_size == b.available_space.inline_size
        || match (
            unobserved.definite_available_inline_sizes_at_or_above,
            available_space.inline_size,
            b.available_space.inline_size,
        ) {
            (Some(threshold), AvailableSize::Definite(_), AvailableSize::Definite(probe)) => probe >= threshold,
            _ => false,
        };
    let inline_bases_match = percentage_basis_inline_size
        == b.containing_block_constraints.percentage_basis_inline_size
        || unobserved.percentage_inline_basis;
    let block_bases_match = (percentage_basis_block_size == b.containing_block_constraints.percentage_basis_block_size
        && quirks_mode_percentage_basis_block_size
            == b.containing_block_constraints.quirks_mode_percentage_basis_block_size)
        || unobserved.percentage_block_basis_and_block_size_definiteness;
    available_inline_sizes_match
        && available_block_sizes_match(
            available_space.block_size,
            b.available_space.block_size,
            unobserved.available_block_size_definiteness(),
        )
        && inline_bases_match
        && block_bases_match
        && sizing_directives_match_ignoring_unobserved(
            &sizing,
            &b.sizing,
            unobserved.available_block_size_definiteness(),
        )
}

fn sizing_directives_match_ignoring_unobserved(
    a: &RootSizingDirectives,
    b: &RootSizingDirectives,
    block_size_definiteness_unobserved: bool,
) -> bool {
    let RootSizingDirectives {
        forced_content_inline_size,
        forced_content_block_size,
        forced_min_border_box_block_size,
        block_parent_resolved_content_inline_size,
        table_cell_intrinsic_block_padding,
        table_box_content_block_offset_in_wrapper,
        adopt_automatic_content_block_size,
        finds_automatic_line_clamp_lines,
        content_block_size_after_inside_layout,
        flex_self_block_size_resolution_space,
        float_avoidance_inline_size,
        outer_float_intrusion_before_list_item_children,
        treat_block_axis_percentage_insets_as_auto_beyond_root,
        math_stretch_size_constraint,
        maximum_inline_size_resolved_by_caller,
    } = *a;
    forced_content_inline_size == b.forced_content_inline_size
        && forced_content_block_size == b.forced_content_block_size
        && forced_min_border_box_block_size == b.forced_min_border_box_block_size
        && block_parent_resolved_content_inline_size == b.block_parent_resolved_content_inline_size
        && table_cell_intrinsic_block_padding == b.table_cell_intrinsic_block_padding
        && table_box_content_block_offset_in_wrapper == b.table_box_content_block_offset_in_wrapper
        && adopt_automatic_content_block_size == b.adopt_automatic_content_block_size
        && finds_automatic_line_clamp_lines == b.finds_automatic_line_clamp_lines
        && content_block_size_after_inside_layout == b.content_block_size_after_inside_layout
        && float_avoidance_inline_size == b.float_avoidance_inline_size
        && outer_float_intrusion_before_list_item_children == b.outer_float_intrusion_before_list_item_children
        && treat_block_axis_percentage_insets_as_auto_beyond_root
            == b.treat_block_axis_percentage_insets_as_auto_beyond_root
        && math_stretch_size_constraint == b.math_stretch_size_constraint
        && maximum_inline_size_resolved_by_caller == b.maximum_inline_size_resolved_by_caller
        && match (
            flex_self_block_size_resolution_space,
            b.flex_self_block_size_resolution_space,
        ) {
            (None, None) => true,
            (Some(a_space), Some(b_space)) => {
                a_space.inline_size == b_space.inline_size
                    && available_block_sizes_match(
                        a_space.block_size,
                        b_space.block_size,
                        block_size_definiteness_unobserved,
                    )
            }
            _ => false,
        }
}

/// What must still be true for a stored entry to be replayed: the slot
/// holds the same box (generation), nothing in its subtree was invalidated
/// (the fragment cache epoch, whose bump walk has no propagation boundary).
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) struct FcRunCacheValidity {
    pub(crate) slot_generation: u8,
    pub(crate) fragment_cache_epoch: u32,
}

/// The outputs of a completed run, which stand for any run with a matching key, whether or not the stored run's own
/// result was committed. Its fragments are those it produced; where they are what the box committed, a hit keeps the
/// committed subtree instead of replaying them.
pub(super) struct FcRunCacheEntry {
    key: FcRunCacheKey,
    validity: FcRunCacheValidity,
    pub(super) outputs: formatting_context::RunOutputs,
    /// Whether the run is a table cell's that did not observe the intrinsic block padding its table hands it (see
    /// run_observes_root_block_padding), so that it stands for runs with any such padding.
    table_cell_intrinsic_block_padding_unobserved: bool,
    root_content_block_size_adopted_after_run: Option<CssPixels>,
    /// How many layouts the arena had committed when the run was stored. The subtrees the run took from the cache as
    /// they were committed are only known to be so while no layout was committed since.
    layout_commits_when_stored: u64,
    /// The committed subtree token of the box when its committed fragments were found to be the stored ones, after
    /// which the entry keeps no descendant fragments: they stay the stored ones while the token does.
    committed_subtree_token_when_descendants_dropped: Option<u32>,
    last_used_at_layout_commit: std::sync::atomic::AtomicU64,
    structural_epoch_bumps_when_stored: u32,
}

/// A stored run a probe may take in place of running, and whether it keeps the box's committed subtree.
pub(super) struct FcRunCacheHit {
    pub(super) entry: std::sync::Arc<FcRunCacheEntry>,
    pub(super) reuses_committed_subtree: bool,
}

impl FcRunCacheEntry {
    /// Whether the committed fragment of `box_` and every fragment committed in its subtree are the ones the stored run
    /// produced: the root's fragment shows the stored line data at the stored content size, or at any block size where
    /// the stored run never observed its own, and each fragment the run placed in its own scope is the box's committed
    /// fragment, placed as the run placed it. A fragment that was committed holds the fragments committed below it.
    pub(super) fn describes_committed_subtree(&self, arena: &LayoutNodeArena, box_: Node) -> bool {
        let Some(root) = self.outputs.root.as_ref() else {
            return false;
        };
        if !root.propagated_pending_abspos.is_empty() {
            return false;
        }
        if !arena.paintable_row_is_populated(box_) {
            return false;
        }
        let (content_inline_size, content_block_size) = self.root_content_size_as_its_parent_holds_it();
        let root_fragment_is_the_stored_one = arena
            .with_current_committed_fragment(box_, |fragment| {
                same_allocation(
                    fragment.line_data.as_ref(),
                    self.outputs.root_outcome.line_data.as_ref(),
                ) && fragment.content_inline_size == content_inline_size
                    && (fragment.content_block_size == content_block_size
                        || self.outputs.result.own_block_size_observation
                            == formatting_context::OwnBlockSizeObservation::Unobserved)
            })
            .unwrap_or(false);
        root_fragment_is_the_stored_one
            && match self.committed_subtree_token_when_descendants_dropped {
                Some(token) => arena.committed_subtree_token(box_) == token,
                None => links_are_committed(arena, &root.scoped_descendants),
            }
    }

    fn root_content_size_as_its_parent_holds_it(&self) -> (CssPixels, CssPixels) {
        match &self.outputs.root_outcome.cells {
            formatting_context::RootCellsOutcome::AllCells(cells) => {
                (cells.inputs.content_inline_size, cells.inputs.content_block_size)
            }
            formatting_context::RootCellsOutcome::OutputCellsOnly(_) => (
                self.key.root_cells.content_inline_size,
                self.root_content_block_size_adopted_after_run
                    .unwrap_or(self.key.root_cells.content_block_size),
            ),
        }
    }

    /// Whether the stored fragments can be replayed. Those of the subtrees the stored run took from the cache are the
    /// committed ones, so such a run is replayed only before anything else is committed.
    fn can_be_replayed(&self, callbacks: &LayoutPass<'_>) -> bool {
        self.committed_subtree_token_when_descendants_dropped.is_none()
            && self.outputs.root.as_ref().is_none_or(|root| {
                root.reused_subtree_roots.is_empty()
                    || callbacks.arena().layout_commit_count() == self.layout_commits_when_stored
            })
    }

    /// This entry without its descendant fragments, which are the box's committed ones while its committed subtree
    /// token is `token`.
    fn without_descendant_fragments(&self, token: u32) -> Self {
        Self {
            key: self.key,
            validity: self.validity,
            outputs: outputs_without_descendant_fragments(&self.outputs),
            table_cell_intrinsic_block_padding_unobserved: self.table_cell_intrinsic_block_padding_unobserved,
            root_content_block_size_adopted_after_run: self.root_content_block_size_adopted_after_run,
            layout_commits_when_stored: self.layout_commits_when_stored,
            committed_subtree_token_when_descendants_dropped: Some(token),
            last_used_at_layout_commit: std::sync::atomic::AtomicU64::new(self.last_used_at_layout_commit()),
            structural_epoch_bumps_when_stored: self.structural_epoch_bumps_when_stored,
        }
    }

    pub(super) fn outputs_without_fragments(&self) -> formatting_context::RunOutputs {
        formatting_context::RunOutputs {
            result: self.outputs.result,
            root: None,
            root_outcome: self.outputs.root_outcome.clone(),
            atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above: self
                .outputs
                .atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above,
            host_findings: self.outputs.host_findings.clone(),
        }
    }

    fn last_used_at_layout_commit(&self) -> u64 {
        self.last_used_at_layout_commit
            .load(std::sync::atomic::Ordering::Relaxed)
    }

    fn note_used_at_layout_commit(&self, layout_commit_count: u64) {
        self.last_used_at_layout_commit
            .store(layout_commit_count, std::sync::atomic::Ordering::Relaxed);
    }

    fn holds_uncommitted_fragments(&self) -> bool {
        self.outputs.root.is_some() && self.committed_subtree_token_when_descendants_dropped.is_none()
    }

    fn may_have_been_committed_by_the_last_layout(&self, layout_commit_count: u64) -> bool {
        self.holds_uncommitted_fragments() && self.last_used_at_layout_commit() + 1 >= layout_commit_count
    }

    fn went_unused_too_long(&self, layout_commit_count: u64) -> bool {
        self.holds_uncommitted_fragments()
            && layout_commit_count.saturating_sub(self.last_used_at_layout_commit())
                > LAYOUT_COMMITS_AN_UNCOMMITTED_ENTRY_SURVIVES_UNUSED
    }

    fn answers_probe_for(&self, key: &FcRunCacheKey) -> bool {
        let unobserved = RunInputsTheStoredRunNeverObserved::proven_by_stored_run(&self.outputs);
        self.key.matches(key, unobserved)
            || (self.table_cell_intrinsic_block_padding_unobserved
                && self
                    .key
                    .without_table_cell_intrinsic_block_padding()
                    .matches(&key.without_table_cell_intrinsic_block_padding(), unobserved))
    }

    /// The outputs a hit absorbs: the stored ones, or only what escapes the stored run where the hit keeps the
    /// committed subtree.
    pub(super) fn outputs_to_replay(&self, reuses_committed_subtree: bool) -> formatting_context::RunOutputs {
        if reuses_committed_subtree {
            outputs_without_descendant_fragments(&self.outputs)
        } else {
            self.outputs.clone()
        }
    }
}

fn same_allocation<T>(left: Option<&std::sync::Arc<T>>, right: Option<&std::sync::Arc<T>>) -> bool {
    match (left, right) {
        (None, None) => true,
        (Some(left), Some(right)) => std::sync::Arc::ptr_eq(left, right),
        _ => false,
    }
}

/// Whether each of `links` is the committed fragment of its box, or the later committed fragment of a box with
/// several, placed as the link places it.
fn links_are_committed(arena: &LayoutNodeArena, links: &[fragment_tree::FragmentLink]) -> bool {
    let mut earlier_fragments_of_box: HashMap<Node, usize> = HashMap::default();
    links.iter().all(|link| {
        let node = link.fragment.node;
        let fragment_index = earlier_fragments_of_box.entry(node).or_default();
        let index = *fragment_index;
        *fragment_index += 1;
        if index == 0 {
            return arena.holds_current_committed_fragment_link(node, link);
        }
        arena.paintable_row_is_populated(node)
            && arena
                .committed_side_data(node)
                .box_fragments()
                .get(index - 1)
                .is_some_and(|committed| link.places_same_fragment_identically_to(&committed.link))
    })
}

fn outputs_without_descendant_fragments(outputs: &formatting_context::RunOutputs) -> formatting_context::RunOutputs {
    let root = outputs.root.as_ref().map(|root| fragment_tree::UnplacedRootFragment {
        node: root.node,
        // Commit stops at the reused root, so descendant fragments and nested reuse markers never
        // enter its scopes. Only payloads that escape the run still have to reach the parent.
        scoped_descendants: Vec::new(),
        reused_subtree_roots: HashSet::default(),
        propagated_pending_abspos: root.propagated_pending_abspos.clone(),
        propagated_anchor_candidates: root.propagated_anchor_candidates.clone(),
        propagated_inline_containing_block_rects: root.propagated_inline_containing_block_rects.clone(),
        propagated_abspos_containing_block_info: root.propagated_abspos_containing_block_info.clone(),
    });
    formatting_context::RunOutputs {
        result: outputs.result,
        root,
        root_outcome: outputs.root_outcome.clone(),
        atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above: outputs
            .atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above,
        host_findings: outputs.host_findings.clone(),
    }
}

#[derive(Clone, Copy, Default)]
struct InlineLayoutDamage {
    generation: u8,
    structural_epoch_bumps: u32,
}

/// How many entries a box keeps beside its latest one and the one whose fragments it committed, the newest first.
const EARLIER_ENTRIES_PER_BOX: usize = 3;

const LAYOUT_COMMITS_AN_UNCOMMITTED_ENTRY_SURVIVES_UNUSED: u64 = 2;

#[derive(Clone, Default)]
struct FcRunCacheSlot {
    latest: Option<std::sync::Arc<FcRunCacheEntry>>,
    committed: Option<std::sync::Arc<FcRunCacheEntry>>,
}

type EarlierEntriesOfBox = smallvec::SmallVec<[std::sync::Arc<FcRunCacheEntry>; EARLIER_ENTRIES_PER_BOX]>;

/// Per-document store of completed run results, surviving across layout passes on the node arena: per box the latest
/// entry, the one whose fragments the box committed where a later store moved it aside, and a few earlier ones.
#[derive(Clone, Default)]
pub(crate) struct FcRunCacheArenaStore {
    slots: RefCell<Vec<FcRunCacheSlot>>,
    earlier_entries: RefCell<HashMap<u32, EarlierEntriesOfBox>>,
    inline_layout_damage: RefCell<Vec<InlineLayoutDamage>>,
    pending_sweep_slots: RefCell<Vec<u32>>,
    enrolled_for_sweep: RefCell<Vec<bool>>,
}

impl FcRunCacheArenaStore {
    pub(super) fn enqueue_for_sweep(&self, slot: u32) {
        let mut enrolled = self.enrolled_for_sweep.borrow_mut();
        if enrolled.len() <= slot as usize {
            enrolled.resize(slot as usize + 1, false);
        }
        if !enrolled[slot as usize] {
            enrolled[slot as usize] = true;
            self.pending_sweep_slots.borrow_mut().push(slot);
        }
    }

    fn slot_holds_entries(&self, slot: u32) -> bool {
        self.slots
            .borrow()
            .get(slot as usize)
            .is_some_and(|entries| entries.latest.is_some() || entries.committed.is_some())
    }

    pub(crate) fn note_invalidated_entry(&self, node: Node) {
        if self.slot_holds_entries(node.slot_index()) {
            self.enqueue_for_sweep(node.slot_index());
        }
    }

    /// The first entry of `slot` that `answers`, trying the one whose fragments the box committed first, then the
    /// latest, then the earlier ones newest first. A matching entry stays stored and hands out a shared handle.
    fn first_entry_where(
        &self,
        slot: u32,
        mut answers: impl FnMut(&FcRunCacheEntry) -> bool,
    ) -> Option<std::sync::Arc<FcRunCacheEntry>> {
        {
            let slots = self.slots.borrow();
            let entries = slots.get(slot as usize)?;
            if let Some(entry) = [&entries.committed, &entries.latest]
                .into_iter()
                .flatten()
                .find(|entry| answers(entry))
            {
                return Some(entry.clone());
            }
        }
        self.earlier_entries
            .borrow()
            .get(&slot)?
            .iter()
            .find(|entry| answers(entry))
            .cloned()
    }

    pub(crate) fn remove_entry(&self, slot: u32) {
        if let Some(entries) = self.slots.borrow_mut().get_mut(slot as usize) {
            *entries = FcRunCacheSlot::default();
        }
        self.earlier_entries.borrow_mut().remove(&slot);
        self.forget_inline_layout_damage(slot);
    }

    pub(crate) fn note_inline_layout_damage(&self, box_: Node) {
        if !self.slot_holds_entries(box_.slot_index()) {
            return;
        }
        let mut damage = self.inline_layout_damage.borrow_mut();
        if damage.len() <= box_.slot_index() as usize {
            damage.resize(box_.slot_index() as usize + 1, InlineLayoutDamage::default());
        }
        let entry = &mut damage[box_.slot_index() as usize];
        if entry.generation != box_.generation() {
            *entry = InlineLayoutDamage {
                generation: box_.generation(),
                structural_epoch_bumps: 1,
            };
        } else {
            entry.structural_epoch_bumps = entry.structural_epoch_bumps.wrapping_add(1);
        }
    }

    /// How many structural changes the inline content of `box_` saw while it held entries, which an entry tells apart
    /// from those before its run by the count when it was stored.
    fn structural_epoch_bumps_of(&self, box_: Node) -> u32 {
        self.inline_layout_damage
            .borrow()
            .get(box_.slot_index() as usize)
            .filter(|damage| damage.generation == box_.generation())
            .map_or(0, |damage| damage.structural_epoch_bumps)
    }

    fn forget_inline_layout_damage(&self, slot: u32) {
        if let Some(damage) = self.inline_layout_damage.borrow_mut().get_mut(slot as usize) {
            *damage = InlineLayoutDamage::default();
        }
    }

    /// A stale entry survives until the sweep after the pass, allowing structural inline damage to use its line data
    /// during the fresh run.
    fn structurally_damaged_entry(
        &self,
        box_: Node,
        validity: FcRunCacheValidity,
        key: &FcRunCacheKey,
    ) -> Option<std::sync::Arc<FcRunCacheEntry>> {
        let structural_epoch_bumps = self.structural_epoch_bumps_of(box_);
        self.first_entry_where(box_.slot_index(), |entry| {
            let structural_epoch_bumps_since_stored =
                structural_epoch_bumps.wrapping_sub(entry.structural_epoch_bumps_when_stored);
            entry.outputs.root.is_some()
                && structural_epoch_bumps_since_stored != 0
                && entry.validity.slot_generation == validity.slot_generation
                && entry.key.matches(
                    key,
                    RunInputsTheStoredRunNeverObserved::accepted_when_reusing_stale_line_data_after_structural_inline_damage(
                        &entry.outputs,
                    ),
                )
                // Each child-list edit bumps once when topology changes and once
                // when the parent is marked for layout-tree-update layout.
                && validity
                    .fragment_cache_epoch
                    .wrapping_sub(entry.validity.fragment_cache_epoch)
                    == structural_epoch_bumps_since_stored
                        .checked_mul(2)
                        .expect("inline layout damage epoch delta overflowed")
        })
    }

    fn store(&self, box_: Node, mut entry: FcRunCacheEntry) {
        let slot = box_.slot_index();
        entry.structural_epoch_bumps_when_stored = self.structural_epoch_bumps_of(box_);
        let mut slots = self.slots.borrow_mut();
        if slots.len() <= slot as usize {
            slots.resize_with(slot as usize + 1, FcRunCacheSlot::default);
        }
        let entries = &mut slots[slot as usize];
        let mut earlier_entries = self.earlier_entries.borrow_mut();
        if let Some(earlier) = earlier_entries.get_mut(&slot) {
            earlier.retain(|earlier| earlier.key != entry.key);
            if earlier.is_empty() {
                earlier_entries.remove(&slot);
            }
        }
        let mut displaced = entries.committed.take_if(|committed| committed.key == entry.key);
        match entries.latest.take() {
            Some(latest) if latest.key == entry.key => displaced = Some(latest),
            Some(latest) if latest.committed_subtree_token_when_descendants_dropped.is_some() => {
                displaced = entries.committed.replace(latest).or(displaced);
            }
            Some(latest) => {
                let earlier = earlier_entries.entry(slot).or_default();
                if earlier.len() == EARLIER_ENTRIES_PER_BOX {
                    let oldest_result_only_or_oldest = earlier
                        .iter()
                        .rposition(|earlier| earlier.outputs.root.is_none())
                        .unwrap_or(EARLIER_ENTRIES_PER_BOX - 1);
                    displaced = Some(earlier.remove(oldest_result_only_or_oldest)).or(displaced);
                }
                earlier.insert(0, latest);
            }
            None => {}
        }
        entries.latest = Some(shared_entry_in_allocation_of(displaced, entry));
        drop(earlier_entries);
        drop(slots);
        // NB: Invalidation can occur between probing a run and storing its result.
        self.enqueue_for_sweep(slot);
    }

    /// Releases stale entries after their last chance to supply reusable inline line data. Drops the descendant
    /// fragments of the entry whose fragments the box committed, for which `committed_subtree_token` gives the token,
    /// and the entries holding fragments the box did not commit once they went unused for a while, except the latest
    /// entry of a box that committed none.
    pub(super) fn sweep_pending_entries(
        &self,
        mut keep: impl FnMut(u32, FcRunCacheValidity) -> bool,
        mut committed_subtree_token: impl FnMut(Node, &FcRunCacheEntry) -> Option<u32>,
        layout_commit_count: u64,
    ) {
        let mut slots = self.slots.borrow_mut();
        let mut earlier_entries = self.earlier_entries.borrow_mut();
        let mut enrolled = self.enrolled_for_sweep.borrow_mut();
        let mut pending_sweep_slots = self.pending_sweep_slots.borrow_mut();
        let mut slots_holding_entries_that_may_go_unused = Vec::new();
        for slot in pending_sweep_slots.drain(..) {
            enrolled[slot as usize] = false;
            let Some(entries) = slots.get_mut(slot as usize) else {
                continue;
            };
            let mut earlier = earlier_entries.remove(&slot).unwrap_or_default();
            entries.committed.take_if(|committed| !keep(slot, committed.validity));
            entries.latest.take_if(|latest| !keep(slot, latest.validity));
            earlier.retain(|earlier| keep(slot, earlier.validity));
            let mut token_of = |entry: &FcRunCacheEntry| {
                committed_subtree_token(Node::new(slot, entry.validity.slot_generation), entry)
            };
            if let Some(latest) = &mut entries.latest
                && latest.may_have_been_committed_by_the_last_layout(layout_commit_count)
                && let Some(token) = token_of(latest)
            {
                *latest = std::sync::Arc::new(latest.without_descendant_fragments(token));
                entries.committed = None;
            } else {
                entries.latest.take_if(|latest| {
                    latest.committed_subtree_token_when_descendants_dropped.is_some() && token_of(latest).is_none()
                });
                if let Some((index, token)) = earlier.iter().enumerate().find_map(|(index, earlier)| {
                    earlier
                        .may_have_been_committed_by_the_last_layout(layout_commit_count)
                        .then(|| token_of(earlier))
                        .flatten()
                        .map(|token| (index, token))
                }) {
                    entries.committed = Some(std::sync::Arc::new(
                        earlier.remove(index).without_descendant_fragments(token),
                    ));
                } else {
                    entries.committed.take_if(|committed| token_of(committed).is_none());
                }
            }
            let box_holds_an_entry_it_committed = entries.committed.is_some();
            earlier.retain(|earlier| !earlier.went_unused_too_long(layout_commit_count));
            if box_holds_an_entry_it_committed {
                entries
                    .latest
                    .take_if(|latest| latest.went_unused_too_long(layout_commit_count));
            }
            if entries.latest.is_none() && !earlier.is_empty() {
                entries.latest = Some(earlier.remove(0));
            }
            let may_go_unused = |entry: &std::sync::Arc<FcRunCacheEntry>| entry.holds_uncommitted_fragments();
            if earlier.iter().any(may_go_unused)
                || (entries.committed.is_some() && entries.latest.as_ref().is_some_and(may_go_unused))
            {
                slots_holding_entries_that_may_go_unused.push(slot);
            }
            if !earlier.is_empty() {
                earlier_entries.insert(slot, earlier);
            }
            if entries.latest.is_none() && entries.committed.is_none() {
                self.forget_inline_layout_damage(slot);
            }
        }
        for slot in slots_holding_entries_that_may_go_unused {
            enrolled[slot as usize] = true;
            pending_sweep_slots.push(slot);
        }
    }
}

fn shared_entry_in_allocation_of(
    displaced: Option<std::sync::Arc<FcRunCacheEntry>>,
    entry: FcRunCacheEntry,
) -> std::sync::Arc<FcRunCacheEntry> {
    let Some(mut displaced) = displaced else {
        return std::sync::Arc::new(entry);
    };
    match std::sync::Arc::get_mut(&mut displaced) {
        Some(unshared) => {
            *unshared = entry;
            displaced
        }
        None => std::sync::Arc::new(entry),
    }
}

pub(super) fn run_root_validity(callbacks: &LayoutPass<'_>, box_: Node) -> FcRunCacheValidity {
    let data = NodeFacts::new(callbacks, box_).data();
    FcRunCacheValidity {
        slot_generation: data.slot_generation.get(),
        fragment_cache_epoch: data.fragment_cache_epoch.get(),
    }
}

/// A run's cache interaction, decided at probe time and concluded after
/// the run executes. Uncacheable classes bypass entirely: subgridded grid
/// items copy the parent grid's mid-run track state, which the key cannot
/// see; anchor()-positioned roots keep their bypass while the resolved-inset
/// side effects are audited; pass entries, measurement roots that do not
/// keep their result and internal runs are not spawned child runs; devtools
/// collection emits per-run callbacks a replay would skip.
pub(super) enum FcRunCacheAttempt {
    Bypass,
    Store {
        /// Captured at probe time and reused at store time: an invalidation
        /// landing between probe and store makes the stored entry look stale
        /// on its next probe (a fail-safe miss) instead of being baked into
        /// a forever-valid entry.
        validity: FcRunCacheValidity,
        structurally_damaged_entry: Option<std::sync::Arc<FcRunCacheEntry>>,
    },
    /// A run that builds no fragments, whose result is kept.
    StoreResultOnly {
        validity: FcRunCacheValidity,
    },
}

impl FcRunCacheAttempt {
    pub(super) fn stores_outputs(&self) -> bool {
        !matches!(self, Self::Bypass)
    }

    pub(super) fn trace_action(&self) -> &'static str {
        match self {
            Self::Bypass => "RUN (cache=bypass)",
            Self::Store { .. } | Self::StoreResultOnly { .. } => "RUN (cache=miss)",
        }
    }

    /// Err carries the entry the caller must replay instead of running. A probe whose run builds fragments takes only
    /// an entry whose run built them; any other probe reads the result of any matching entry.
    pub(super) fn probe(
        box_: Node,
        parent_grid_is_present: bool,
        builds_fragments: bool,
        should_collect_devtools_layout_data: bool,
        measurement_root_keeps_its_result: bool,
        callbacks: &LayoutPass<'_>,
        key: &FcRunCacheKey,
    ) -> Result<Self, FcRunCacheHit> {
        if !can_describe_run(
            box_,
            parent_grid_is_present,
            should_collect_devtools_layout_data,
            measurement_root_keeps_its_result,
            callbacks,
            key,
        ) {
            return Ok(Self::Bypass);
        }
        let store = callbacks.arena().fc_run_cache_store();
        let validity = run_root_validity(callbacks, box_);
        if !builds_fragments {
            if let Some(entry) = store.first_entry_where(box_.slot_index(), |entry| {
                entry.validity == validity && entry.answers_probe_for(key)
            }) {
                entry.note_used_at_layout_commit(callbacks.arena().layout_commit_count());
                return Err(FcRunCacheHit {
                    entry,
                    reuses_committed_subtree: false,
                });
            }
            return Ok(Self::StoreResultOnly { validity });
        }
        let mut reuses_committed_subtree = false;
        if let Some(entry) = store.first_entry_where(box_.slot_index(), |entry| {
            if entry.validity != validity || entry.outputs.root.is_none() || !entry.answers_probe_for(key) {
                return false;
            }
            reuses_committed_subtree = entry.describes_committed_subtree(callbacks.arena(), box_);
            reuses_committed_subtree || entry.can_be_replayed(callbacks)
        }) {
            if !reuses_committed_subtree {
                // NB: Once the replayed fragments are committed, the entry can drop them.
                store.enqueue_for_sweep(box_.slot_index());
            }
            entry.note_used_at_layout_commit(callbacks.arena().layout_commit_count());
            return Err(FcRunCacheHit {
                entry,
                reuses_committed_subtree,
            });
        }
        let structurally_damaged_entry = store.structurally_damaged_entry(box_, validity, key);
        Ok(Self::Store {
            validity,
            structurally_damaged_entry,
        })
    }

    pub(super) fn previous_line_data(&self) -> Option<std::sync::Arc<inline_content::InlineContent>> {
        let Self::Store {
            structurally_damaged_entry: Some(entry),
            ..
        } = self
        else {
            return None;
        };
        entry.outputs.root_outcome.line_data.clone()
    }

    pub(super) fn conclude(
        self,
        callbacks: &LayoutPass<'_>,
        box_: Node,
        key: FcRunCacheKey,
        input: &LayoutInput,
        outputs: &formatting_context::RunOutputs,
    ) {
        let root_content_block_size_adopted_after_run = formatting_context::content_block_size_adopted_after_run(
            input,
            outputs.result.automatic_content_block_size,
        )
        .map(|block_size| clamp_to_max_dimension_value(block_size.max(CssPixels::default())));
        let store = callbacks.arena().fc_run_cache_store();
        let (validity, structurally_damaged_entry) = match self {
            Self::Bypass => return,
            Self::StoreResultOnly { validity } => {
                debug_assert!(outputs.root.is_none(), "a run that builds no fragments returned a root");
                store.store(
                    box_,
                    FcRunCacheEntry {
                        key,
                        validity,
                        outputs: outputs.clone(),
                        table_cell_intrinsic_block_padding_unobserved: false,
                        root_content_block_size_adopted_after_run,
                        layout_commits_when_stored: callbacks.arena().layout_commit_count(),
                        committed_subtree_token_when_descendants_dropped: None,
                        last_used_at_layout_commit: std::sync::atomic::AtomicU64::new(
                            callbacks.arena().layout_commit_count(),
                        ),
                        structural_epoch_bumps_when_stored: 0,
                    },
                );
                return;
            }
            Self::Store {
                validity,
                structurally_damaged_entry,
            } => (validity, structurally_damaged_entry),
        };
        // Release the probed entry so that store() can replace it in place.
        drop(structurally_damaged_entry);
        let Some(root) = &outputs.root else {
            return;
        };
        // A committed-subtree hit omits that subtree's descendant fragments. Do not embed such a
        // skeletal result in an entry that exports an out-of-flow descendant: replaying the parent
        // has to rebuild its paintable subtree so the freshly laid-out descendant can commit.
        if !root.propagated_pending_abspos.is_empty() && !root.reused_subtree_roots.is_empty() {
            return;
        }
        let entry = FcRunCacheEntry {
            key,
            validity,
            outputs: outputs.clone(),
            table_cell_intrinsic_block_padding_unobserved: key
                .input
                .sizing
                .table_cell_intrinsic_block_padding
                .is_some()
                && outputs.root_outcome.cells.parent_owns_root_input_cells()
                && !run_observes_root_block_padding(callbacks, box_, outputs),
            root_content_block_size_adopted_after_run,
            layout_commits_when_stored: callbacks.arena().layout_commit_count(),
            committed_subtree_token_when_descendants_dropped: None,
            last_used_at_layout_commit: std::sync::atomic::AtomicU64::new(callbacks.arena().layout_commit_count()),
            structural_epoch_bumps_when_stored: 0,
        };
        store.store(box_, entry);
    }
}

pub(super) fn can_describe_run(
    box_: Node,
    parent_grid_is_present: bool,
    should_collect_devtools_layout_data: bool,
    measurement_root_keeps_its_result: bool,
    callbacks: &LayoutPass<'_>,
    key: &FcRunCacheKey,
) -> bool {
    let fc_type = key.fc_type;
    let input = &key.input;
    if should_collect_devtools_layout_data
        || (input.participation == ParticipationInParentFormattingContext::Root && !measurement_root_keeps_its_result)
        || matches!(
            fc_type,
            formatting_context::FormattingContextType::InternalReplaced | formatting_context::FormattingContextType::InternalDummy
        )
        // The direct normal-layout path for an empty atomic block only sizes and snapshots its root.
        // Replaying a stored output costs more than rebuilding it and retains an entry needlessly.
        || (fc_type == formatting_context::FormattingContextType::Block
            && input.participation == ParticipationInParentFormattingContext::AtomicInline
            && callbacks.first_child(box_).is_invalid())
    {
        return false;
    }
    if fc_type == formatting_context::FormattingContextType::Grid
        && parent_grid_is_present
        && grid_formatting_context::grid_template_declares_a_subgrid_axis(callbacks, box_)
    {
        return false;
    }
    // Structural invariants behind this root-flag check, to re-verify if it is ever
    // narrowed: anchor() applies only to absolutely positioned boxes and every abspos
    // box is the root of its own spawned run, so the flag on the run root covers every
    // anchor consumer; anchor eligibility is resolved from used values of the same
    // pass under the anchor lookup's containing-block guard, never from a previous
    // pass; scroll compensation compares chains that converge at the run root; and
    // anchor-size() resolves to None today, so inset properties are the only anchor
    // dependency a run can have.
    !node_facts::has_flag(
        NodeFacts::new(callbacks, box_).data(),
        NodeFlag::InsetsUseAnchorFunctions,
    )
}

/// Whether a run's outputs may depend on its root's block paddings. They hold the content in content box coordinates,
/// so they stand for the same run at other block paddings unless the root is a scroll container, whose scrollable
/// overflow area is bounded by its padding box, places a rendered legend on its border box, or passes payloads on to
/// its ancestors (out-of-flow descendants, their containing block rects, anchors), which are placed against its
/// padding box. Outputs without a root fragment cannot show those payloads.
pub(super) fn run_observes_root_block_padding(
    callbacks: &LayoutPass<'_>,
    box_: Node,
    outputs: &formatting_context::RunOutputs,
) -> bool {
    let facts = NodeFacts::new(callbacks, box_);
    let places_rendered_legend_on_its_border_box = facts.is_fieldset_box() && !facts.rendered_legend().is_invalid();
    facts.is_scroll_container()
        || places_rendered_legend_on_its_border_box
        || outputs.root.as_ref().is_none_or(|root| !root.propagates_nothing())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key(available_inline: AvailableSize, percentage_basis_inline_size: Option<CssPixels>) -> FcRunCacheKey {
        FcRunCacheKey {
            fc_type: formatting_context::FormattingContextType::Block,
            layout_mode: LayoutMode::Normal,
            input: LayoutInput::new(
                AvailableSpace {
                    inline_size: available_inline,
                    block_size: AvailableSize::Indefinite,
                },
                ContainingBlockConstraints {
                    percentage_basis_inline_size,
                    ..ContainingBlockConstraints::default()
                },
                ParticipationInParentFormattingContext::AtomicInline,
            ),
            root_cells: used_values::RootInputCells::capture(&UsedValues::default()),
        }
    }

    fn px(value: i64) -> CssPixels {
        CssPixels::from_integer(value)
    }

    fn inline_inputs_unobserved_at_or_above(threshold: i64) -> RunInputsTheStoredRunNeverObserved {
        RunInputsTheStoredRunNeverObserved {
            definite_available_inline_sizes_at_or_above: Some(px(threshold)),
            percentage_inline_basis: true,
            ..RunInputsTheStoredRunNeverObserved::default()
        }
    }

    fn at(inline_size: i64) -> FcRunCacheKey {
        key(AvailableSize::definite(px(inline_size)), Some(px(inline_size)))
    }

    fn root_with_a_descendant() -> fragment_tree::UnplacedRootFragment {
        fragment_tree::UnplacedRootFragment {
            node: Node::new(0, 1),
            scoped_descendants: vec![fragment_tree::FragmentLink::for_test(Node::new(1, 1))],
            reused_subtree_roots: HashSet::default(),
            propagated_pending_abspos: Vec::new(),
            propagated_anchor_candidates: Vec::new(),
            propagated_inline_containing_block_rects: Vec::new(),
            propagated_abspos_containing_block_info: Vec::new(),
        }
    }

    fn entry(
        key: FcRunCacheKey,
        fragment_cache_epoch: u32,
        root: Option<fragment_tree::UnplacedRootFragment>,
        layout_commit_count: u64,
    ) -> FcRunCacheEntry {
        FcRunCacheEntry {
            key,
            validity: FcRunCacheValidity {
                slot_generation: 1,
                fragment_cache_epoch,
            },
            outputs: formatting_context::RunOutputs {
                result: formatting_context::ChildLayoutResult::default(),
                root,
                root_outcome: formatting_context::RunRootOutcome {
                    cells: formatting_context::RootCellsOutcome::AllCells(used_values::UsedValuesCellState::capture(
                        &UsedValues::default(),
                    )),
                    own_metrics_sealed: false,
                    line_data: None,
                    rare: None,
                },
                atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above: None,
                host_findings: None,
            },
            table_cell_intrinsic_block_padding_unobserved: false,
            root_content_block_size_adopted_after_run: None,
            layout_commits_when_stored: layout_commit_count,
            committed_subtree_token_when_descendants_dropped: None,
            last_used_at_layout_commit: std::sync::atomic::AtomicU64::new(layout_commit_count),
            structural_epoch_bumps_when_stored: 0,
        }
    }

    fn widths(inline_sizes: &[i64]) -> Vec<AvailableSize> {
        inline_sizes
            .iter()
            .map(|&inline_size| AvailableSize::definite(px(inline_size)))
            .collect()
    }

    fn widths_of_entries(store: &FcRunCacheArenaStore, slot: u32) -> Vec<AvailableSize> {
        let mut widths = Vec::new();
        store.first_entry_where(slot, |entry| {
            widths.push(entry.key.input.available_space.inline_size);
            false
        });
        widths
    }

    fn width_of_committed_entry(store: &FcRunCacheArenaStore, slot: u32) -> Option<AvailableSize> {
        store.slots.borrow()[slot as usize]
            .committed
            .as_ref()
            .map(|entry| entry.key.input.available_space.inline_size)
    }

    fn mark_entries_of_key(marked: FcRunCacheKey) -> impl FnMut(Node, &FcRunCacheEntry) -> Option<u32> {
        move |_, entry| (entry.key == marked).then_some(7)
    }

    #[test]
    fn sweep_checks_changed_entries_without_discarding_reusable_stale_data_early() {
        let store = FcRunCacheArenaStore::default();
        let entry = |epoch| entry(at(300), epoch, None, 0);
        store.store(Node::new(0, 1), entry(1));
        store.store(Node::new(1, 1), entry(1));
        store.sweep_pending_entries(|_, _| true, |_, _| None, 1);
        store.sweep_pending_entries(|_, _| panic!("unchanged entries must not be swept"), |_, _| None, 2);

        store.note_invalidated_entry(Node::new(0, 1));
        store.note_invalidated_entry(Node::new(0, 1));
        assert_eq!(widths_of_entries(&store, 0).len(), 1);
        let mut checked = 0;
        store.sweep_pending_entries(
            |slot, _| {
                assert_eq!(slot, 0);
                checked += 1;
                false
            },
            |_, _| None,
            3,
        );
        assert_eq!(checked, 1);
        assert!(widths_of_entries(&store, 0).is_empty());
        assert_eq!(widths_of_entries(&store, 1).len(), 1);

        store.note_invalidated_entry(Node::new(1, 1));
        store.store(Node::new(1, 1), entry(2));
        store.sweep_pending_entries(|_, validity| validity.fragment_cache_epoch == 2, |_, _| None, 4);
        assert_eq!(widths_of_entries(&store, 1).len(), 1);

        // A result stored after a mid-run invalidation still needs its validity checked.
        store.store(Node::new(0, 1), entry(1));
        store.sweep_pending_entries(|_, validity| validity.fragment_cache_epoch == 2, |_, _| None, 5);
        assert!(widths_of_entries(&store, 0).is_empty());
    }

    #[test]
    fn a_store_keeps_the_entry_whose_fragments_the_box_committed_beside_newer_ones() {
        let store = FcRunCacheArenaStore::default();
        let box_ = Node::new(0, 1);
        store.store(box_, entry(at(300), 1, Some(root_with_a_descendant()), 0));
        store.sweep_pending_entries(|_, _| true, mark_entries_of_key(at(300)), 1);
        store.store(box_, entry(at(320), 1, Some(root_with_a_descendant()), 1));
        store.store(box_, entry(at(340), 1, None, 1));
        assert_eq!(
            width_of_committed_entry(&store, 0),
            Some(AvailableSize::definite(px(300)))
        );
        assert_eq!(widths_of_entries(&store, 0), widths(&[300, 340, 320]));

        store.store(box_, entry(at(300), 1, Some(root_with_a_descendant()), 1));
        assert_eq!(width_of_committed_entry(&store, 0), None);
        assert_eq!(widths_of_entries(&store, 0), widths(&[300, 340, 320]));
    }

    #[test]
    fn the_sweep_marks_the_entry_whose_fragments_the_box_committed_and_drops_its_descendant_fragments() {
        let store = FcRunCacheArenaStore::default();
        let box_ = Node::new(0, 1);
        store.store(box_, entry(at(300), 1, Some(root_with_a_descendant()), 0));
        store.store(box_, entry(at(320), 1, Some(root_with_a_descendant()), 0));
        store.sweep_pending_entries(|_, _| true, mark_entries_of_key(at(300)), 1);
        let committed = store.slots.borrow()[0]
            .committed
            .clone()
            .expect("the earlier entry whose fragments the box committed moves beside the latest one");
        assert_eq!(
            committed.key.input.available_space.inline_size,
            AvailableSize::definite(px(300))
        );
        assert_eq!(committed.committed_subtree_token_when_descendants_dropped, Some(7));
        assert!(committed.outputs.root.as_ref().unwrap().scoped_descendants.is_empty());
        assert_eq!(widths_of_entries(&store, 0), widths(&[300, 320]));

        let both_are_committed =
            |_, entry: &FcRunCacheEntry| (entry.key == at(300) || entry.key == at(320)).then_some(7);
        store.sweep_pending_entries(|_, _| true, both_are_committed, 2);
        assert_eq!(
            width_of_committed_entry(&store, 0),
            Some(AvailableSize::definite(px(300))),
            "only an entry stored or used since the previous layout commit can have been committed by the last one"
        );
        store
            .first_entry_where(0, |entry| entry.key == at(320))
            .unwrap()
            .note_used_at_layout_commit(2);
        store.sweep_pending_entries(|_, _| true, mark_entries_of_key(at(320)), 3);
        assert_eq!(width_of_committed_entry(&store, 0), None);
        assert_eq!(widths_of_entries(&store, 0), widths(&[320]));
    }

    #[test]
    fn an_earlier_entry_committed_in_place_of_the_marked_latest_one_stays_beside_later_stores() {
        let store = FcRunCacheArenaStore::default();
        let box_ = Node::new(0, 1);
        store.store(box_, entry(at(300), 1, Some(root_with_a_descendant()), 0));
        store.store(box_, entry(at(320), 1, Some(root_with_a_descendant()), 0));
        store.sweep_pending_entries(|_, _| true, mark_entries_of_key(at(320)), 1);
        store
            .first_entry_where(0, |entry| entry.key == at(300))
            .unwrap()
            .note_used_at_layout_commit(1);
        store.sweep_pending_entries(|_, _| true, mark_entries_of_key(at(300)), 2);
        assert_eq!(
            width_of_committed_entry(&store, 0),
            Some(AvailableSize::definite(px(300)))
        );
        store.store(box_, entry(at(340), 1, None, 2));
        assert_eq!(
            width_of_committed_entry(&store, 0),
            Some(AvailableSize::definite(px(300))),
            "the entry marked before the box committed another one no longer describes the committed subtree"
        );
        assert_eq!(widths_of_entries(&store, 0), widths(&[300, 340]));
    }

    #[test]
    fn a_committed_entry_that_no_longer_describes_the_committed_subtree_goes() {
        let store = FcRunCacheArenaStore::default();
        let box_ = Node::new(0, 1);
        store.store(box_, entry(at(300), 1, Some(root_with_a_descendant()), 0));
        store.sweep_pending_entries(|_, _| true, mark_entries_of_key(at(300)), 1);
        store.store(box_, entry(at(320), 1, Some(root_with_a_descendant()), 1));
        assert_eq!(widths_of_entries(&store, 0), widths(&[300, 320]));
        store.sweep_pending_entries(|_, _| true, |_, _| None, 2);
        assert_eq!(widths_of_entries(&store, 0), widths(&[320]));
    }

    #[test]
    fn entries_holding_fragments_the_box_never_committed_go_after_two_unused_layout_commits_beside_one_it_did() {
        let store = FcRunCacheArenaStore::default();
        let box_ = Node::new(0, 1);
        store.store(box_, entry(at(280), 1, Some(root_with_a_descendant()), 0));
        store.sweep_pending_entries(|_, _| true, mark_entries_of_key(at(280)), 1);
        store.store(box_, entry(at(300), 1, None, 1));
        store.store(box_, entry(at(320), 1, Some(root_with_a_descendant()), 1));
        store.store(box_, entry(at(340), 1, Some(root_with_a_descendant()), 1));
        store.sweep_pending_entries(|_, _| true, mark_entries_of_key(at(280)), 2);
        store
            .first_entry_where(0, |entry| entry.key == at(320))
            .unwrap()
            .note_used_at_layout_commit(2);
        store.sweep_pending_entries(|_, _| true, mark_entries_of_key(at(280)), 3);
        store.sweep_pending_entries(|_, _| true, mark_entries_of_key(at(280)), 4);
        assert_eq!(widths_of_entries(&store, 0), widths(&[280, 320, 300]));
        store.sweep_pending_entries(|_, _| true, mark_entries_of_key(at(280)), 5);
        assert_eq!(widths_of_entries(&store, 0), widths(&[280, 300]));
        store.sweep_pending_entries(
            |_, _| panic!("a box holding no fragments it did not commit is not swept again"),
            |_, _| None,
            6,
        );
    }

    #[test]
    fn the_latest_entry_of_a_box_that_committed_none_of_its_entries_stays() {
        let store = FcRunCacheArenaStore::default();
        let box_ = Node::new(0, 1);
        store.store(box_, entry(at(300), 1, Some(root_with_a_descendant()), 0));
        for layout_commit_count in 1..5 {
            store.sweep_pending_entries(|_, _| true, |_, _| None, layout_commit_count);
        }
        assert_eq!(widths_of_entries(&store, 0), widths(&[300]));
    }

    #[test]
    fn earlier_entries_make_room_oldest_first_and_results_of_runs_without_fragments_first() {
        let store = FcRunCacheArenaStore::default();
        let box_ = Node::new(0, 1);
        store.store(box_, entry(at(100), 1, Some(root_with_a_descendant()), 0));
        store.store(box_, entry(at(200), 1, None, 0));
        store.store(box_, entry(at(300), 1, Some(root_with_a_descendant()), 0));
        store.store(box_, entry(at(400), 1, Some(root_with_a_descendant()), 0));
        assert_eq!(widths_of_entries(&store, 0), widths(&[400, 300, 200, 100]));
        store.store(box_, entry(at(500), 1, Some(root_with_a_descendant()), 0));
        assert_eq!(widths_of_entries(&store, 0), widths(&[500, 400, 300, 100]));
        store.store(box_, entry(at(600), 1, Some(root_with_a_descendant()), 0));
        assert_eq!(widths_of_entries(&store, 0), widths(&[600, 500, 400, 300]));
        store.store(box_, entry(at(400), 1, None, 0));
        assert_eq!(widths_of_entries(&store, 0), widths(&[400, 600, 500, 300]));
    }

    #[test]
    fn a_store_keeps_the_inline_damage_an_earlier_entry_saw() {
        let store = FcRunCacheArenaStore::default();
        let box_ = Node::new(0, 1);
        store.store(box_, entry(at(300), 1, Some(root_with_a_descendant()), 0));
        store.note_inline_layout_damage(box_);
        store.store(box_, entry(at(320), 3, None, 0));
        let damaged_validity = FcRunCacheValidity {
            slot_generation: 1,
            fragment_cache_epoch: 3,
        };
        let damaged = store
            .structurally_damaged_entry(box_, damaged_validity, &at(300))
            .expect("the entry stored before the damage supplies its line data");
        assert_eq!(
            damaged.key.input.available_space.inline_size,
            AvailableSize::definite(px(300))
        );
        store.note_inline_layout_damage(box_);
        assert!(
            store
                .structurally_damaged_entry(box_, damaged_validity, &at(300))
                .is_none()
        );
    }

    #[test]
    fn item_keys_leave_out_root_outputs_and_the_block_size_adopted_after_the_run_unless_the_run_reads_it() {
        let record = UsedValues::default();
        let item_input = LayoutInput::new(
            AvailableSpace {
                inline_size: AvailableSize::definite(px(300)),
                block_size: AvailableSize::Indefinite,
            },
            ContainingBlockConstraints::default(),
            ParticipationInParentFormattingContext::Item,
        );
        let key_of = |fc_type, input| {
            FcRunCacheKey::new(
                fc_type,
                LayoutMode::Normal,
                input,
                used_values::RootInputCells::capture(&record),
            )
        };
        let stored_block_key = key_of(formatting_context::FormattingContextType::Block, item_input);
        let stored_flex_key = key_of(formatting_context::FormattingContextType::Flex, item_input);

        record.has_first_baseline.set(true);
        record.first_baseline.set(px(12));
        record.has_line_clamp_point.set(true);
        record.clamped_content_is_scrollable_overflow.set(true);
        let mut adopting_input = item_input;
        adopting_input.sizing.adopt_automatic_content_block_size = true;
        adopting_input.sizing.content_block_size_after_inside_layout = Some(px(40));

        let exact = RunInputsTheStoredRunNeverObserved::default();
        assert!(stored_block_key.matches(
            &key_of(formatting_context::FormattingContextType::Block, adopting_input),
            exact
        ));
        assert!(!stored_flex_key.matches(
            &key_of(formatting_context::FormattingContextType::Flex, adopting_input),
            exact
        ));
    }

    #[test]
    fn identical_keys_match_without_relaxations() {
        let stored = key(AvailableSize::definite(px(300)), Some(px(300)));
        assert!(stored.matches(&stored, RunInputsTheStoredRunNeverObserved::default()));
    }

    #[test]
    fn a_different_available_inline_size_misses_without_the_inline_relaxation() {
        let stored = key(AvailableSize::definite(px(300)), Some(px(300)));
        let probe = key(AvailableSize::definite(px(320)), Some(px(320)));
        assert!(!stored.matches(&probe, RunInputsTheStoredRunNeverObserved::default()));
        assert!(!stored.matches(
            &probe,
            RunInputsTheStoredRunNeverObserved {
                percentage_block_basis_and_block_size_definiteness: true,
                ..RunInputsTheStoredRunNeverObserved::default()
            }
        ));
    }

    #[test]
    fn the_inline_guarantees_are_granted_separately() {
        let stored = key(AvailableSize::definite(px(300)), Some(px(300)));
        let only_available_inline_sizes_unobserved = RunInputsTheStoredRunNeverObserved {
            definite_available_inline_sizes_at_or_above: Some(px(0)),
            ..RunInputsTheStoredRunNeverObserved::default()
        };
        let only_inline_basis_unobserved = RunInputsTheStoredRunNeverObserved {
            percentage_inline_basis: true,
            ..RunInputsTheStoredRunNeverObserved::default()
        };
        let wider_with_same_basis = key(AvailableSize::definite(px(320)), Some(px(300)));
        assert!(stored.matches(&wider_with_same_basis, only_available_inline_sizes_unobserved));
        assert!(!stored.matches(&wider_with_same_basis, only_inline_basis_unobserved));
        let same_width_with_other_basis = key(AvailableSize::definite(px(300)), Some(px(320)));
        assert!(stored.matches(&same_width_with_other_basis, only_inline_basis_unobserved));
        assert!(!stored.matches(&same_width_with_other_basis, only_available_inline_sizes_unobserved));
    }

    #[test]
    fn the_inline_relaxation_accepts_available_sizes_at_or_above_the_threshold() {
        let stored = key(AvailableSize::definite(px(300)), Some(px(300)));
        assert!(stored.matches(
            &key(AvailableSize::definite(px(320)), Some(px(320))),
            inline_inputs_unobserved_at_or_above(100)
        ));
        assert!(stored.matches(
            &key(AvailableSize::definite(px(100)), Some(px(100))),
            inline_inputs_unobserved_at_or_above(100)
        ));
        assert!(!stored.matches(
            &key(AvailableSize::definite(px(99)), Some(px(99))),
            inline_inputs_unobserved_at_or_above(100)
        ));
    }

    #[test]
    fn the_inline_relaxation_needs_definite_available_sizes_on_both_sides() {
        let stored = key(AvailableSize::definite(px(300)), Some(px(300)));
        assert!(!stored.matches(
            &key(AvailableSize::MaxContent, None),
            inline_inputs_unobserved_at_or_above(0)
        ));
        let stored_intrinsic = key(AvailableSize::MaxContent, None);
        assert!(!stored_intrinsic.matches(
            &key(AvailableSize::definite(px(300)), Some(px(300))),
            inline_inputs_unobserved_at_or_above(0)
        ));
    }

    #[test]
    fn the_inline_relaxation_leaves_every_other_input_exact() {
        let stored = key(AvailableSize::definite(px(300)), Some(px(300)));
        let mutators: [fn(&mut FcRunCacheKey); 4] = [
            |probe| probe.input.containing_block_constraints.percentage_basis_block_size = Some(px(10)),
            |probe| probe.input.available_space.block_size = AvailableSize::definite(px(10)),
            |probe| probe.input.sizing.forced_min_border_box_block_size = Some(px(10)),
            |probe| probe.input.participation = ParticipationInParentFormattingContext::Float,
        ];
        for mutate in mutators {
            let mut probe = key(AvailableSize::definite(px(320)), Some(px(320)));
            mutate(&mut probe);
            assert!(!stored.matches(&probe, inline_inputs_unobserved_at_or_above(0)));
        }
    }

    fn item_key(content_block_size: i64, has_definite_block_size: bool) -> FcRunCacheKey {
        let record = UsedValues::default();
        record.content_block_size.set(px(content_block_size));
        record.has_definite_block_size.set(has_definite_block_size);
        record.has_definite_inline_size.set(true);
        record.content_inline_size.set(px(300));
        FcRunCacheKey::new(
            formatting_context::FormattingContextType::Block,
            LayoutMode::Normal,
            LayoutInput::new(
                AvailableSpace {
                    inline_size: AvailableSize::definite(px(300)),
                    block_size: if has_definite_block_size {
                        AvailableSize::definite(px(content_block_size))
                    } else {
                        AvailableSize::Indefinite
                    },
                },
                ContainingBlockConstraints::default(),
                ParticipationInParentFormattingContext::Item,
            ),
            used_values::RootInputCells::capture(&record),
        )
    }

    fn own_block_size(observation: formatting_context::OwnBlockSizeObservation) -> RunInputsTheStoredRunNeverObserved {
        RunInputsTheStoredRunNeverObserved {
            percentage_block_basis_and_block_size_definiteness: true,
            own_block_size: observation,
            ..RunInputsTheStoredRunNeverObserved::default()
        }
    }

    #[test]
    fn an_item_run_that_observed_its_own_block_size_matches_only_the_block_size_it_was_assigned() {
        let observed = own_block_size(formatting_context::OwnBlockSizeObservation::Observed);
        let stored = item_key(40, false);
        assert!(stored.matches(&item_key(40, false), observed));
        assert!(!stored.matches(&item_key(40, true), observed));
        assert!(!stored.matches(&item_key(60, false), observed));
        assert!(!stored.matches(&item_key(60, true), observed));
    }

    #[test]
    fn an_item_run_that_never_observed_its_own_block_size_matches_any_assigned_block_size_and_definiteness() {
        let unobserved = own_block_size(formatting_context::OwnBlockSizeObservation::Unobserved);
        for stored in [item_key(40, false), item_key(40, true)] {
            for probe in [
                item_key(40, false),
                item_key(40, true),
                item_key(60, true),
                item_key(0, false),
            ] {
                assert!(stored.matches(&probe, unobserved));
            }
        }
        let mut wider = item_key(60, true);
        wider.root_cells.content_inline_size = px(320);
        assert!(
            !item_key(40, false).matches(&wider, unobserved),
            "only the root's own block size is unobserved"
        );
        let mut button_aligned = item_key(60, true);
        button_aligned
            .root_cells
            .has_definite_block_size_only_for_button_content_alignment = true;
        assert!(!item_key(40, false).matches(&button_aligned, unobserved));
    }

    #[test]
    fn an_item_run_whose_root_never_observed_its_percentage_inline_basis_matches_at_another_basis() {
        let at_percentage_inline_basis = |basis: i64| {
            let mut key = item_key(40, false);
            key.input.containing_block_constraints.percentage_basis_inline_size = Some(px(basis));
            key
        };
        let mut outputs = entry(item_key(40, false), 1, None, 0).outputs;
        assert!(!at_percentage_inline_basis(400).matches(
            &at_percentage_inline_basis(300),
            RunInputsTheStoredRunNeverObserved::proven_by_stored_run(&outputs)
        ));
        outputs.result.percentage_inline_basis_observation =
            formatting_context::PercentageInlineBasisObservation::Unobserved;
        let unobserved = RunInputsTheStoredRunNeverObserved::proven_by_stored_run(&outputs);
        assert!(at_percentage_inline_basis(400).matches(&at_percentage_inline_basis(300), unobserved));
        let mut wider = at_percentage_inline_basis(300);
        wider.root_cells.content_inline_size = px(320);
        wider.input.available_space.inline_size = AvailableSize::definite(px(320));
        assert!(
            !at_percentage_inline_basis(400).matches(&wider, unobserved),
            "only the percentage inline basis is unobserved"
        );
        assert!(!at_percentage_inline_basis(400).matches(
            &at_percentage_inline_basis(300),
            RunInputsTheStoredRunNeverObserved::accepted_when_reusing_stale_line_data_after_structural_inline_damage(
                &outputs
            )
        ));
    }

    #[test]
    fn an_item_run_that_never_observed_its_own_block_size_matches_at_other_block_margins() {
        let at_margins = |margin_top: i64, margin_bottom: i64, margin_left: i64| {
            let mut key = item_key(40, true);
            key.root_cells.margin_top = px(margin_top);
            key.root_cells.margin_bottom = px(margin_bottom);
            key.root_cells.margin_left = px(margin_left);
            key
        };
        let stored = at_margins(0, 0, 0);
        let unobserved = own_block_size(formatting_context::OwnBlockSizeObservation::Unobserved);
        let observed = own_block_size(formatting_context::OwnBlockSizeObservation::Observed);
        assert!(stored.matches(&at_margins(30, -10, 0), unobserved));
        assert!(!stored.matches(&at_margins(30, -10, 0), observed));
        assert!(
            !stored.matches(&at_margins(0, 0, 10), unobserved),
            "only the root's block margins are unobserved"
        );
    }

    #[test]
    fn stale_line_data_after_structural_inline_damage_is_taken_only_at_the_same_own_block_size() {
        let mut outputs = entry(item_key(40, false), 1, None, 0).outputs;
        outputs.result.own_block_size_observation = formatting_context::OwnBlockSizeObservation::Unobserved;
        assert!(item_key(40, false).matches(
            &item_key(60, true),
            RunInputsTheStoredRunNeverObserved::proven_by_stored_run(&outputs)
        ));
        assert!(!item_key(40, false).matches(
            &item_key(60, true),
            RunInputsTheStoredRunNeverObserved::accepted_when_reusing_stale_line_data_after_structural_inline_damage(
                &outputs
            )
        ));
    }
}
