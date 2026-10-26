/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

/// The complete identity of a memoizable run: the layout input plus the
/// pre-run root record state the dispatch seam captures anyway, so every
/// value a parent hands a spawned run is part of the key. Viewport changes
/// reach a run through that input or through the normal style/layout epoch
/// invalidation for viewport-dependent computed values.
#[derive(Clone, Copy, PartialEq)]
pub(super) struct FcRunCacheKey {
    fc_type: formatting_context::FormattingContextType,
    input: LayoutInput,
    root_cells: used_values::UsedValuesCellState,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(super) struct RunInputsTheStoredRunNeverObserved {
    pub(super) percentage_block_basis_and_block_size_definiteness: bool,
    pub(super) definite_available_inline_sizes_at_or_above: Option<CssPixels>,
    pub(super) percentage_inline_basis: bool,
}

impl RunInputsTheStoredRunNeverObserved {
    fn proven_by_stored_run(outputs: &formatting_context::RunOutputs) -> Self {
        Self {
            percentage_block_basis_and_block_size_definiteness: !outputs.result.depends_on_percentage_block_size,
            definite_available_inline_sizes_at_or_above: outputs
                .atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above,
            percentage_inline_basis: outputs
                .atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above
                .is_some(),
        }
    }

    fn accepted_when_reusing_stale_line_data_after_structural_inline_damage(
        outputs: &formatting_context::RunOutputs,
    ) -> Self {
        Self {
            definite_available_inline_sizes_at_or_above: None,
            percentage_inline_basis: false,
            ..Self::proven_by_stored_run(outputs)
        }
    }

    fn relaxes_any_input(self) -> bool {
        let Self {
            percentage_block_basis_and_block_size_definiteness,
            definite_available_inline_sizes_at_or_above,
            percentage_inline_basis,
        } = self;
        percentage_block_basis_and_block_size_definiteness
            || definite_available_inline_sizes_at_or_above.is_some()
            || percentage_inline_basis
    }
}

impl FcRunCacheKey {
    pub(super) fn new(
        fc_type: formatting_context::FormattingContextType,
        input: LayoutInput,
        root_cells: used_values::UsedValuesCellState,
    ) -> Self {
        Self {
            fc_type,
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
            && self.root_cells == probe.root_cells
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
            unobserved.percentage_block_basis_and_block_size_definiteness,
        )
        && inline_bases_match
        && block_bases_match
        && sizing_directives_match_ignoring_unobserved(
            &sizing,
            &b.sizing,
            unobserved.percentage_block_basis_and_block_size_definiteness,
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
        content_block_size_after_inside_layout,
        flex_self_block_size_resolution_space,
        float_avoidance_inline_size,
        outer_float_intrusion_before_list_item_children,
        treat_block_axis_percentage_insets_as_auto_beyond_root,
        math_stretch_size_constraint,
    } = *a;
    forced_content_inline_size == b.forced_content_inline_size
        && forced_content_block_size == b.forced_content_block_size
        && forced_min_border_box_block_size == b.forced_min_border_box_block_size
        && block_parent_resolved_content_inline_size == b.block_parent_resolved_content_inline_size
        && table_cell_intrinsic_block_padding == b.table_cell_intrinsic_block_padding
        && table_box_content_block_offset_in_wrapper == b.table_box_content_block_offset_in_wrapper
        && adopt_automatic_content_block_size == b.adopt_automatic_content_block_size
        && content_block_size_after_inside_layout == b.content_block_size_after_inside_layout
        && float_avoidance_inline_size == b.float_avoidance_inline_size
        && outer_float_intrusion_before_list_item_children == b.outer_float_intrusion_before_list_item_children
        && treat_block_axis_percentage_insets_as_auto_beyond_root
            == b.treat_block_axis_percentage_insets_as_auto_beyond_root
        && math_stretch_size_constraint == b.math_stretch_size_constraint
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
    /// Whether the run is a table cell's that cannot observe the intrinsic block padding its table hands it (see
    /// table_cell_contents_never_observe_intrinsic_block_padding), so that it stands for runs with any such padding.
    table_cell_intrinsic_block_padding_unobserved: bool,
    /// How many layouts the arena had committed when the run was stored. The subtrees the run took from the cache as
    /// they were committed are only known to be so while no layout was committed since.
    layout_commits_when_stored: u64,
    /// The committed subtree token of the box when its committed fragments were found to be the stored ones, after
    /// which the entry keeps no descendant fragments: they stay the stored ones while the token does.
    committed_subtree_token_when_descendants_dropped: Option<u32>,
}

/// A stored run a probe may take in place of running, and whether it keeps the box's committed subtree.
pub(super) struct FcRunCacheHit {
    pub(super) entry: std::sync::Arc<FcRunCacheEntry>,
    pub(super) reuses_committed_subtree: bool,
}

impl FcRunCacheEntry {
    /// Whether the committed fragment of `box_` and every fragment committed in its subtree are the ones the stored run
    /// produced: the root's fragment shows the stored line data at the stored content size, and each fragment the run
    /// placed in its own scope is the box's committed fragment, placed as the run placed it. A fragment that was
    /// committed holds the fragments committed below it.
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
        let cells = &self.outputs.root_outcome.cells;
        let root_fragment_is_the_stored_one = arena
            .with_current_committed_fragment(box_, |fragment| {
                same_allocation(
                    fragment.line_data.as_ref(),
                    self.outputs.root_outcome.line_data.as_ref(),
                ) && fragment.content_inline_size == cells.content_inline_size
                    && fragment.content_block_size == cells.content_block_size
            })
            .unwrap_or(false);
        root_fragment_is_the_stored_one
            && match self.committed_subtree_token_when_descendants_dropped {
                Some(token) => arena.committed_subtree_token(box_) == token,
                None => links_are_committed(arena, &root.scoped_descendants),
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
            layout_commits_when_stored: self.layout_commits_when_stored,
            committed_subtree_token_when_descendants_dropped: Some(token),
        }
    }

    /// The outputs a hit for `probe` absorbs: the stored ones, or only what escapes the stored run where the hit keeps
    /// the committed subtree, with the root's block paddings moved by however much the intrinsic block padding `probe`
    /// carries differs from the stored run's.
    pub(super) fn outputs_to_replay(
        &self,
        probe: &FcRunCacheKey,
        reuses_committed_subtree: bool,
    ) -> formatting_context::RunOutputs {
        let mut outputs = if reuses_committed_subtree {
            outputs_without_descendant_fragments(&self.outputs)
        } else {
            self.outputs.clone()
        };
        self.move_root_block_paddings_to(probe, &mut outputs);
        outputs
    }

    fn move_root_block_paddings_to(&self, probe: &FcRunCacheKey, outputs: &mut formatting_context::RunOutputs) {
        let cells = &mut outputs.root_outcome.cells;
        cells.padding_top += probe.root_cells.padding_top - self.key.root_cells.padding_top;
        cells.padding_bottom += probe.root_cells.padding_bottom - self.key.root_cells.padding_bottom;
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

/// Per-document store of completed run results, one entry per slot,
/// surviving across layout passes on the node arena.
#[derive(Clone, Default)]
pub(crate) struct FcRunCacheArenaStore {
    entries: RefCell<Vec<Option<std::sync::Arc<FcRunCacheEntry>>>>,
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

    pub(crate) fn note_invalidated_entry(&self, node: Node) {
        if self
            .entries
            .borrow()
            .get(node.slot_index() as usize)
            .is_some_and(Option::is_some)
        {
            self.enqueue_for_sweep(node.slot_index());
        }
    }

    pub(crate) fn remove_entry(&self, slot: u32) {
        if let Some(entry) = self.entries.borrow_mut().get_mut(slot as usize) {
            *entry = None;
        }
        self.forget_inline_layout_damage(slot);
    }

    pub(crate) fn note_inline_layout_damage(&self, box_: Node) {
        if self
            .entries
            .borrow()
            .get(box_.slot_index() as usize)
            .is_none_or(Option::is_none)
        {
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
            entry.structural_epoch_bumps = entry
                .structural_epoch_bumps
                .checked_add(1)
                .expect("inline layout damage counter overflowed");
        }
    }

    /// How many structural changes the inline content of `box_` saw since its entry was stored.
    fn inline_layout_damage_since_stored(&self, box_: Node) -> u32 {
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

    /// A matching entry stays stored and hands out a shared handle. A stale
    /// entry survives until the fresh run replaces it, allowing structural
    /// inline damage to use its line data during that run.
    fn matching(
        &self,
        slot: u32,
        validity: FcRunCacheValidity,
        key: &FcRunCacheKey,
    ) -> Option<std::sync::Arc<FcRunCacheEntry>> {
        let entries = self.entries.borrow();
        let stored = entries.get(slot as usize)?;
        let entry = stored.as_ref()?;
        if entry.validity != validity {
            return None;
        }
        let unobserved = RunInputsTheStoredRunNeverObserved::proven_by_stored_run(&entry.outputs);
        if !entry.key.matches(key, unobserved)
            && !(entry.table_cell_intrinsic_block_padding_unobserved
                && entry
                    .key
                    .without_table_cell_intrinsic_block_padding()
                    .matches(&key.without_table_cell_intrinsic_block_padding(), unobserved))
        {
            return None;
        }
        Some(entry.clone())
    }

    fn structurally_damaged_entry(
        &self,
        slot: u32,
        validity: FcRunCacheValidity,
        key: &FcRunCacheKey,
        structural_epoch_bumps: u32,
    ) -> Option<std::sync::Arc<FcRunCacheEntry>> {
        if structural_epoch_bumps == 0 {
            return None;
        }
        let entries = self.entries.borrow();
        let entry = entries.get(slot as usize)?.as_ref()?;
        if entry.validity.slot_generation != validity.slot_generation
            || !entry.key.matches(
                key,
                RunInputsTheStoredRunNeverObserved::accepted_when_reusing_stale_line_data_after_structural_inline_damage(
                    &entry.outputs,
                ),
            )
            // Each child-list edit bumps once when topology changes and once
            // when the parent is marked for layout-tree-update layout.
            || validity
                .fragment_cache_epoch
                .wrapping_sub(entry.validity.fragment_cache_epoch)
                != structural_epoch_bumps
                    .checked_mul(2)
                    .expect("inline layout damage epoch delta overflowed")
        {
            return None;
        }
        Some(entry.clone())
    }

    fn store(&self, slot: u32, entry: FcRunCacheEntry) {
        let mut entries = self.entries.borrow_mut();
        if entries.len() <= slot as usize {
            entries.resize_with(slot as usize + 1, || None);
        }
        match entries[slot as usize].as_mut().and_then(std::sync::Arc::get_mut) {
            Some(stored) => *stored = entry,
            None => entries[slot as usize] = Some(std::sync::Arc::new(entry)),
        }
        drop(entries);
        // The damage counts the structural changes since the stored entry's run.
        self.forget_inline_layout_damage(slot);
        // NB: Invalidation can occur between probing a run and storing its result.
        self.enqueue_for_sweep(slot);
    }

    /// Releases stale entries after their last chance to supply reusable inline line data, and drops the descendant
    /// fragments of entries whose fragments the box committed, for which `committed_subtree_token` gives the token.
    pub(super) fn sweep_pending_entries(
        &self,
        mut keep: impl FnMut(u32, FcRunCacheValidity) -> bool,
        mut committed_subtree_token: impl FnMut(Node, &FcRunCacheEntry) -> Option<u32>,
    ) {
        let mut entries = self.entries.borrow_mut();
        let mut enrolled = self.enrolled_for_sweep.borrow_mut();
        for slot in self.pending_sweep_slots.borrow_mut().drain(..) {
            enrolled[slot as usize] = false;
            let Some(stored) = entries.get_mut(slot as usize) else {
                continue;
            };
            let Some(entry) = stored else {
                continue;
            };
            if !keep(slot, entry.validity) {
                *stored = None;
                self.forget_inline_layout_damage(slot);
                continue;
            }
            if entry.committed_subtree_token_when_descendants_dropped.is_some() {
                continue;
            }
            let box_ = Node::new(slot, entry.validity.slot_generation);
            if let Some(token) = committed_subtree_token(box_, entry) {
                *entry = std::sync::Arc::new(entry.without_descendant_fragments(token));
            }
        }
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
/// the run executes. Uncacheable classes bypass entirely: measurement and
/// intrinsic-sizing runs produce no fragments; subgridded grid items copy
/// the parent grid's mid-run track state, which the key cannot see;
/// anchor()-positioned roots keep their bypass while the resolved-inset
/// side effects are audited; pass entries and internal runs are not
/// spawned child runs; devtools collection emits per-run callbacks a
/// replay would skip.
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
}

impl FcRunCacheAttempt {
    pub(super) fn trace_action(&self) -> &'static str {
        match self {
            Self::Bypass => "RUN (cache=bypass)",
            Self::Store { .. } => "RUN (cache=miss)",
        }
    }

    /// Err carries the entry the caller must replay instead of running.
    pub(super) fn probe(
        purpose: formatting_context::LayoutPurpose,
        box_: Node,
        parent_grid_is_present: bool,
        layout_mode: LayoutMode,
        should_collect_devtools_layout_data: bool,
        callbacks: &LayoutPass<'_>,
        key: &FcRunCacheKey,
    ) -> Result<Self, FcRunCacheHit> {
        // A run this cache cannot describe still commits its subtree, so a stored entry would go on
        // describing paintables that run has replaced. Measurement runs commit nothing and leave it alone.
        let run_supersedes_stored_entry = layout_mode == LayoutMode::Normal && !purpose.is_measurement();
        if purpose.is_measurement()
            || !can_describe_run(
                box_,
                parent_grid_is_present,
                layout_mode,
                should_collect_devtools_layout_data,
                callbacks,
                key,
            )
        {
            if run_supersedes_stored_entry {
                callbacks.arena().fc_run_cache_store().remove_entry(box_.slot_index());
            }
            return Ok(Self::Bypass);
        }
        let store = callbacks.arena().fc_run_cache_store();
        let validity = run_root_validity(callbacks, box_);
        let structural_epoch_bumps = store.inline_layout_damage_since_stored(box_);
        if let Some(entry) = store.matching(box_.slot_index(), validity, key) {
            let reuses_committed_subtree = entry.describes_committed_subtree(callbacks.arena(), box_);
            if !reuses_committed_subtree && entry.can_be_replayed(callbacks) {
                // NB: Once the replayed fragments are committed, the entry can drop them.
                store.enqueue_for_sweep(box_.slot_index());
            }
            if reuses_committed_subtree || entry.can_be_replayed(callbacks) {
                return Err(FcRunCacheHit {
                    entry,
                    reuses_committed_subtree,
                });
            }
        }
        let structurally_damaged_entry =
            store.structurally_damaged_entry(box_.slot_index(), validity, key, structural_epoch_bumps);
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
        outputs: &formatting_context::RunOutputs,
    ) {
        let Self::Store {
            validity,
            structurally_damaged_entry,
        } = self
        else {
            return;
        };
        // Release the probed entry so that store() can replace it in place.
        drop(structurally_damaged_entry);
        // Every path out of here belongs to a run that has committed its subtree, so an entry left
        // behind without being replaced would describe paintables that no longer exist.
        let store = callbacks.arena().fc_run_cache_store();
        let Some(root) = &outputs.root else {
            store.remove_entry(box_.slot_index());
            return;
        };
        // A committed-subtree hit omits that subtree's descendant fragments. Do not embed such a
        // skeletal result in an entry that exports an out-of-flow descendant: replaying the parent
        // has to rebuild its paintable subtree so the freshly laid-out descendant can commit.
        if !root.propagated_pending_abspos.is_empty() && !root.reused_subtree_roots.is_empty() {
            store.remove_entry(box_.slot_index());
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
                && table_cell_contents_never_observe_intrinsic_block_padding(callbacks, box_),
            layout_commits_when_stored: callbacks.arena().layout_commit_count(),
            committed_subtree_token_when_descendants_dropped: None,
        };
        store.store(box_.slot_index(), entry);
    }
}

pub(super) fn can_describe_run(
    box_: Node,
    parent_grid_is_present: bool,
    layout_mode: LayoutMode,
    should_collect_devtools_layout_data: bool,
    callbacks: &LayoutPass<'_>,
    key: &FcRunCacheKey,
) -> bool {
    layout_mode == LayoutMode::Normal
        && can_describe_run_in_any_layout_mode(
            box_,
            parent_grid_is_present,
            should_collect_devtools_layout_data,
            callbacks,
            key,
        )
}

pub(super) fn can_describe_run_in_any_layout_mode(
    box_: Node,
    parent_grid_is_present: bool,
    should_collect_devtools_layout_data: bool,
    callbacks: &LayoutPass<'_>,
    key: &FcRunCacheKey,
) -> bool {
    let fc_type = key.fc_type;
    let input = &key.input;
    if should_collect_devtools_layout_data
        || input.participation == ParticipationInParentFormattingContext::Root
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

/// Whether a table cell's run cannot observe the intrinsic block padding its table hands it (the vertical-alignment
/// stretch): the padding only moves the cell's content box, and content that is only text lays out relative to that.
/// Nothing in it resolves against the padding box, as out-of-flow descendants would, and it holds no scroll container.
pub(super) fn table_cell_contents_never_observe_intrinsic_block_padding(
    callbacks: &LayoutPass<'_>,
    box_: Node,
) -> bool {
    let facts = NodeFacts::new(callbacks, box_);
    if !facts.is_table_cell() || facts.is_scroll_container() {
        return false;
    }
    for child in callbacks.children(box_) {
        let child_facts = NodeFacts::new(callbacks, child);
        if !child_facts.is_text_node() && !child_facts.is_break_node() {
            return false;
        }
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key(available_inline: AvailableSize, percentage_basis_inline_size: Option<CssPixels>) -> FcRunCacheKey {
        FcRunCacheKey {
            fc_type: formatting_context::FormattingContextType::Block,
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
            root_cells: used_values::UsedValuesCellState::capture(&UsedValues::default()),
        }
    }

    fn px(value: i64) -> CssPixels {
        CssPixels::from_integer(value)
    }

    fn inline_inputs_unobserved_at_or_above(threshold: i64) -> RunInputsTheStoredRunNeverObserved {
        RunInputsTheStoredRunNeverObserved {
            percentage_block_basis_and_block_size_definiteness: false,
            definite_available_inline_sizes_at_or_above: Some(px(threshold)),
            percentage_inline_basis: true,
        }
    }

    #[test]
    fn sweep_checks_changed_entries_without_discarding_reusable_stale_data_early() {
        let store = FcRunCacheArenaStore::default();
        let entry = |epoch| FcRunCacheEntry {
            key: key(AvailableSize::definite(px(300)), Some(px(300))),
            validity: FcRunCacheValidity {
                slot_generation: 1,
                fragment_cache_epoch: epoch,
            },
            outputs: formatting_context::RunOutputs {
                result: formatting_context::ChildLayoutResult::default(),
                root: None,
                root_outcome: formatting_context::RunRootOutcome {
                    cells: used_values::UsedValuesCellState::capture(&UsedValues::default()),
                    own_metrics_sealed: false,
                    line_data: None,
                    rare: None,
                },
                atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above: None,
                host_findings: None,
            },
            table_cell_intrinsic_block_padding_unobserved: false,
            layout_commits_when_stored: 0,
            committed_subtree_token_when_descendants_dropped: None,
        };
        store.store(0, entry(1));
        store.store(1, entry(1));
        store.sweep_pending_entries(|_, _| true, |_, _| None);
        store.sweep_pending_entries(|_, _| panic!("unchanged entries must not be swept"), |_, _| None);

        store.note_invalidated_entry(Node::new(0, 1));
        store.note_invalidated_entry(Node::new(0, 1));
        assert!(store.entries.borrow()[0].is_some());
        let mut checked = 0;
        store.sweep_pending_entries(
            |slot, _| {
                assert_eq!(slot, 0);
                checked += 1;
                false
            },
            |_, _| None,
        );
        assert_eq!(checked, 1);
        assert!(store.entries.borrow()[0].is_none());
        assert!(store.entries.borrow()[1].is_some());

        store.note_invalidated_entry(Node::new(1, 1));
        store.store(1, entry(2));
        store.sweep_pending_entries(|_, validity| validity.fragment_cache_epoch == 2, |_, _| None);
        assert!(store.entries.borrow()[1].is_some());

        // A result stored after a mid-run invalidation still needs its validity checked.
        store.store(0, entry(1));
        store.sweep_pending_entries(|_, validity| validity.fragment_cache_epoch == 2, |_, _| None);
        assert!(store.entries.borrow()[0].is_none());
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
                definite_available_inline_sizes_at_or_above: None,
                percentage_inline_basis: false,
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
            |probe| probe.input.sizing.adopt_automatic_content_block_size = true,
            |probe| probe.input.participation = ParticipationInParentFormattingContext::Float,
        ];
        for mutate in mutators {
            let mut probe = key(AvailableSize::definite(px(320)), Some(px(320)));
            mutate(&mut probe);
            assert!(!stored.matches(&probe, inline_inputs_unobserved_at_or_above(0)));
        }
    }
}
