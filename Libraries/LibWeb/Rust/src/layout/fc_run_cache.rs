/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

/// The complete identity of a cacheable run: the layout input plus the
/// pre-run root input cells the dispatch seam captures anyway, so every
/// value a parent hands a spawned run that the run reads is part of the
/// key. Viewport changes reach a run through that input or through the
/// normal style/layout epoch invalidation for viewport-dependent computed
/// values.
#[derive(Clone, Copy, PartialEq)]
pub(super) struct FcRunCacheKey {
    fc_type: formatting_context::FormattingContextType,
    input: LayoutInput,
    root_cells: used_values::RootInputCells,
}

/// The inputs a run never observed, at which its stored outputs are reused as they are.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct RunInputsTheStoredRunNeverObserved {
    pub(super) percentage_block_basis_and_block_size_definiteness: bool,
    pub(super) definite_available_inline_sizes_at_or_above: Option<CssPixels>,
    pub(super) percentage_inline_basis: bool,
    pub(super) own_block_size: formatting_context::OwnBlockSizeObservation,
    pub(super) inline_space_around_root: bool,
    /// The block size an item's root adopts at the end of a run whose contents never read it.
    pub(super) content_block_size_adopted_after_run: bool,
}

impl RunInputsTheStoredRunNeverObserved {
    fn accepted_when_reusing_stale_line_data_after_structural_inline_damage(self) -> Self {
        Self {
            definite_available_inline_sizes_at_or_above: None,
            percentage_inline_basis: false,
            own_block_size: formatting_context::OwnBlockSizeObservation::Observed,
            inline_space_around_root: false,
            ..self
        }
    }

    fn relaxes_any_input(self) -> bool {
        let Self {
            percentage_block_basis_and_block_size_definiteness,
            definite_available_inline_sizes_at_or_above,
            percentage_inline_basis,
            own_block_size,
            inline_space_around_root,
            content_block_size_adopted_after_run,
        } = self;
        percentage_block_basis_and_block_size_definiteness
            || definite_available_inline_sizes_at_or_above.is_some()
            || percentage_inline_basis
            || own_block_size != formatting_context::OwnBlockSizeObservation::Observed
            || inline_space_around_root
            || content_block_size_adopted_after_run
    }

    fn available_block_size_definiteness(self) -> bool {
        self.percentage_block_basis_and_block_size_definiteness
            || self.own_block_size != formatting_context::OwnBlockSizeObservation::Observed
    }
}

fn root_input_cells_match_ignoring_unobserved(
    stored: used_values::RootInputCells,
    probe: used_values::RootInputCells,
    own_block_size: formatting_context::OwnBlockSizeObservation,
    inline_space_around_root_unobserved: bool,
) -> bool {
    let without_unobserved_inline_space = |cells: used_values::RootInputCells| {
        if !inline_space_around_root_unobserved {
            return cells;
        }
        used_values::RootInputCells {
            content_inline_size: CssPixels::default(),
            margin_left: CssPixels::default(),
            margin_right: CssPixels::default(),
            ..cells
        }
    };
    let stored = without_unobserved_inline_space(stored);
    let probe = without_unobserved_inline_space(probe);
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
        mut input: LayoutInput,
        root_cells: used_values::RootInputCells,
    ) -> Self {
        // NB: The parent block formatting context sizes a float against its containing block position before the
        //     run and places it after. The run lays the float out from its own origin.
        if let ParticipationInParentFormattingContext::Float {
            containing_block_position_in_bfc_root,
        } = &mut input.participation
        {
            *containing_block_position_in_bfc_root = FfiCssPixelPoint::default();
        }
        if let ParticipationInParentFormattingContext::AbsolutelyPositioned(inputs) = &mut input.participation {
            *inputs = inputs.without_static_position_in_axes_placed_by_own_insets();
        }
        Self {
            fc_type,
            input,
            root_cells,
        }
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
            && root_input_cells_match_ignoring_unobserved(
                self.root_cells,
                probe.root_cells,
                unobserved.own_block_size,
                unobserved.inline_space_around_root,
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
        sizing,
        participation,
    } = *a;
    let ContainingBlockConstraints {
        percentage_basis_inline_size,
        percentage_basis_block_size,
        quirks_mode_percentage_basis_block_size,
    } = containing_block_constraints;
    if participation != b.participation {
        return false;
    }
    let available_inline_sizes_match = available_space.inline_size == b.available_space.inline_size
        || unobserved.inline_space_around_root
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
            unobserved.inline_space_around_root,
            unobserved.content_block_size_adopted_after_run,
        )
}

fn sizing_directives_match_ignoring_unobserved(
    a: &RootSizingDirectives,
    b: &RootSizingDirectives,
    block_size_definiteness_unobserved: bool,
    float_avoidance_inline_size_unobserved: bool,
    content_block_size_adopted_after_run_unobserved: bool,
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
        root_is_measured_for_its_content_block_size,
        root_keeps_its_committed_geometry,
    } = *a;
    forced_content_inline_size == b.forced_content_inline_size
        && forced_content_block_size == b.forced_content_block_size
        && forced_min_border_box_block_size == b.forced_min_border_box_block_size
        && block_parent_resolved_content_inline_size == b.block_parent_resolved_content_inline_size
        && table_cell_intrinsic_block_padding == b.table_cell_intrinsic_block_padding
        && table_box_content_block_offset_in_wrapper == b.table_box_content_block_offset_in_wrapper
        && ((adopt_automatic_content_block_size == b.adopt_automatic_content_block_size
            && content_block_size_after_inside_layout == b.content_block_size_after_inside_layout)
            || content_block_size_adopted_after_run_unobserved)
        && finds_automatic_line_clamp_lines == b.finds_automatic_line_clamp_lines
        && (float_avoidance_inline_size == b.float_avoidance_inline_size || float_avoidance_inline_size_unobserved)
        && outer_float_intrusion_before_list_item_children == b.outer_float_intrusion_before_list_item_children
        && treat_block_axis_percentage_insets_as_auto_beyond_root
            == b.treat_block_axis_percentage_insets_as_auto_beyond_root
        && math_stretch_size_constraint == b.math_stretch_size_constraint
        && maximum_inline_size_resolved_by_caller == b.maximum_inline_size_resolved_by_caller
        && root_is_measured_for_its_content_block_size == b.root_is_measured_for_its_content_block_size
        && root_keeps_its_committed_geometry == b.root_keeps_its_committed_geometry
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
    last_used_at_layout_commit: std::sync::atomic::AtomicU64,
    structural_epoch_bumps_when_stored: u32,
}

/// A stored run a probe may take in place of running.
pub(super) struct FcRunCacheHit {
    pub(super) entry: std::sync::Arc<FcRunCacheEntry>,
}

impl FcRunCacheEntry {
    /// Whether the committed fragment of `box_` holds the fragments the stored run produced: it shows the stored line data
    /// at the stored content size, or at any block size where the stored run never observed its own, around the very
    /// child fragments the run placed in its own scope, which are then what the box's subtree committed. Committed
    /// geometry is current only while nothing inside the box was invalidated since, which also moves the box's own epoch.
    pub(super) fn holds_committed_fragments(&self, arena: &LayoutNodeArena, box_: Node) -> bool {
        let Some(root) = self.outputs.root.as_ref() else {
            return false;
        };
        if !root.payloads_past_run.pending_abspos.is_empty() || !arena.paintable_row_is_populated(box_) {
            return false;
        }
        let (content_inline_size, content_block_size) = self.root_content_size_as_its_parent_holds_it();
        arena
            .with_current_committed_fragment(box_, |committed| {
                if std::sync::Arc::ptr_eq(committed, &root.fragment) {
                    return true;
                }
                committed.content_inline_size == content_inline_size
                    && (committed.content_block_size == content_block_size
                        || self.outputs.inputs_never_observed.own_block_size
                            == formatting_context::OwnBlockSizeObservation::Unobserved)
                    && same_allocation(
                        committed.line_data.as_ref(),
                        self.outputs.root_outcome.line_data.as_ref(),
                    )
                    && committed.holds_same_contents_as(&root.fragment)
            })
            .unwrap_or(false)
    }

    fn root_content_size_as_its_parent_holds_it(&self) -> (CssPixels, CssPixels) {
        match &self.outputs.root_outcome.input_cells_left_by_run {
            Some(cells) => (cells.content_inline_size, cells.content_block_size),
            None => (
                self.key.root_cells.content_inline_size,
                formatting_context::content_block_size_adopted_after_run(
                    &self.key.input,
                    self.outputs.result.automatic_content_block_size,
                )
                .map_or(self.key.root_cells.content_block_size, |block_size| {
                    clamp_to_max_dimension_value(block_size.max(CssPixels::default()))
                }),
            ),
        }
    }

    pub(super) fn outputs_without_fragments(&self) -> formatting_context::RunOutputs {
        formatting_context::RunOutputs {
            result: self.outputs.result,
            root: None,
            root_outcome: self.outputs.root_outcome.clone(),
            inputs_never_observed: self.outputs.inputs_never_observed,
            host_findings: self.outputs.host_findings.clone(),
            laid_out_abspos_contained_by_root: self.outputs.laid_out_abspos_contained_by_root,
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

    fn holds_fragments(&self) -> bool {
        self.outputs.root.is_some()
    }

    fn was_used_by_the_last_layout(&self, layout_commit_count: u64) -> bool {
        self.last_used_at_layout_commit() + 1 >= layout_commit_count
    }

    /// Whether the entry holds fragments and went unused for longer than an entry whose fragments the box did not
    /// commit survives.
    fn holds_fragments_unused_too_long(&self, layout_commit_count: u64) -> bool {
        self.holds_fragments()
            && layout_commit_count.saturating_sub(self.last_used_at_layout_commit())
                > LAYOUT_COMMITS_AN_UNCOMMITTED_ENTRY_SURVIVES_UNUSED
    }

    fn answers_probe_for(&self, key: &FcRunCacheKey) -> bool {
        let unobserved = self.outputs.inputs_never_observed;
        self.key.matches(key, unobserved)
            || (self.table_cell_intrinsic_block_padding_unobserved
                && self
                    .key
                    .without_table_cell_intrinsic_block_padding()
                    .matches(&key.without_table_cell_intrinsic_block_padding(), unobserved))
    }

    /// The outputs a hit absorbs.
    pub(super) fn outputs_to_replay(&self) -> formatting_context::RunOutputs {
        self.outputs.clone()
    }
}

fn same_allocation<T>(left: Option<&std::sync::Arc<T>>, right: Option<&std::sync::Arc<T>>) -> bool {
    match (left, right) {
        (None, None) => true,
        (Some(left), Some(right)) => std::sync::Arc::ptr_eq(left, right),
        _ => false,
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

/// Per-document store of completed run results, surviving across layout passes in the layout stage's scratch: per box
/// the latest entry, the one whose fragments the box committed where a later store moved it aside, and a few earlier
/// ones.
#[derive(Clone, Default)]
pub(crate) struct FcRunCacheStore {
    slots: RefCell<Vec<FcRunCacheSlot>>,
    earlier_entries: RefCell<HashMap<u32, EarlierEntriesOfBox>>,
    inline_layout_damage: RefCell<Vec<InlineLayoutDamage>>,
    pending_sweep_slots: RefCell<Vec<u32>>,
    enrolled_for_sweep: RefCell<Vec<bool>>,
}

impl FcRunCacheStore {
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
                    entry
                        .outputs
                        .inputs_never_observed
                        .accepted_when_reusing_stale_line_data_after_structural_inline_damage(),
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

    /// Stores `entry` as the latest entry of `box_`, keeping the latest one beside it where `is_committed` finds the box
    /// committed its fragments.
    fn store(&self, box_: Node, mut entry: FcRunCacheEntry, is_committed: impl Fn(&FcRunCacheEntry) -> bool) {
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
            Some(latest) if is_committed(&latest) => {
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

    /// Releases stale entries after their last chance to supply reusable inline line data. Keeps the entry whose
    /// fragments the box committed, which `is_committed` finds, beside the latest one, and drops the entries holding
    /// fragments the box did not commit once they went unused for a while, except the latest entry of a box that
    /// committed none.
    pub(super) fn sweep_pending_entries(
        &self,
        mut keep: impl FnMut(u32, FcRunCacheValidity) -> bool,
        mut is_committed: impl FnMut(Node, &FcRunCacheEntry) -> bool,
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
            let mut is_committed =
                |entry: &FcRunCacheEntry| is_committed(Node::new(slot, entry.validity.slot_generation), entry);
            if entries.latest.as_deref().is_some_and(&mut is_committed) {
                entries.committed = None;
            } else if let Some(index) = earlier.iter().position(|earlier| {
                // NB: An earlier entry was not committed when a later store moved it aside, so only a replay since can
                //     have committed its fragments.
                earlier.was_used_by_the_last_layout(layout_commit_count) && is_committed(earlier)
            }) {
                entries.committed = Some(earlier.remove(index));
            } else {
                entries.committed.take_if(|committed| !is_committed(committed));
            }
            earlier.retain(|earlier| !earlier.holds_fragments_unused_too_long(layout_commit_count));
            if entries.committed.is_some() {
                entries
                    .latest
                    .take_if(|latest| latest.holds_fragments_unused_too_long(layout_commit_count));
            }
            if entries.latest.is_none() && !earlier.is_empty() {
                entries.latest = Some(earlier.remove(0));
            }
            if earlier.iter().any(|earlier| earlier.holds_fragments())
                || (entries.committed.is_some()
                    && entries.latest.as_ref().is_some_and(|latest| latest.holds_fragments()))
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
        fragment_cache_epoch: data.fragment_cache_epoch,
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
        let store = callbacks.fc_run_cache_store();
        let validity = run_root_validity(callbacks, box_);
        if !builds_fragments {
            if let Some(entry) = store.first_entry_where(box_.slot_index(), |entry| {
                entry.validity == validity && entry.answers_probe_for(key)
            }) {
                entry.note_used_at_layout_commit(callbacks.arena().layout_commit_count());
                return Err(FcRunCacheHit { entry });
            }
            return Ok(Self::StoreResultOnly { validity });
        }
        if let Some(entry) = store.first_entry_where(box_.slot_index(), |entry| {
            entry.validity == validity && entry.outputs.root.is_some() && entry.answers_probe_for(key)
        }) {
            // NB: Once the replayed fragments are committed, the sweep keeps the entry beside later ones.
            store.enqueue_for_sweep(box_.slot_index());
            entry.note_used_at_layout_commit(callbacks.arena().layout_commit_count());
            return Err(FcRunCacheHit { entry });
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
        outputs: &formatting_context::RunOutputs,
    ) {
        let store = callbacks.fc_run_cache_store();
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
                        last_used_at_layout_commit: std::sync::atomic::AtomicU64::new(
                            callbacks.arena().layout_commit_count(),
                        ),
                        structural_epoch_bumps_when_stored: 0,
                    },
                    |entry| entry.holds_committed_fragments(callbacks.arena(), box_),
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
        if outputs.root.is_none() {
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
                && outputs.root_outcome.parent_owns_root_input_cells()
                && !run_observes_root_block_padding(callbacks, box_, outputs),
            last_used_at_layout_commit: std::sync::atomic::AtomicU64::new(callbacks.arena().layout_commit_count()),
            structural_epoch_bumps_when_stored: 0,
        };
        store.store(box_, entry, |entry| {
            entry.holds_committed_fragments(callbacks.arena(), box_)
        });
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
        || fc_type == formatting_context::FormattingContextType::InternalReplaced
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
/// overflow area is bounded by its padding box, places a rendered legend on its border box, lays out absolutely
/// positioned boxes against its padding box, or passes payloads on to its ancestors (out-of-flow descendants, their
/// containing block rects, anchors), which are placed against its padding box. Outputs without a root fragment cannot
/// show those payloads.
pub(super) fn run_observes_root_block_padding(
    callbacks: &LayoutPass<'_>,
    box_: Node,
    outputs: &formatting_context::RunOutputs,
) -> bool {
    let facts = NodeFacts::new(callbacks, box_);
    let places_rendered_legend_on_its_border_box = facts.is_fieldset_box() && !facts.rendered_legend().is_invalid();
    facts.is_scroll_container()
        || places_rendered_legend_on_its_border_box
        || outputs.laid_out_abspos_contained_by_root
        || outputs
            .root
            .as_ref()
            .is_none_or(|root| !root.payloads_past_run.is_empty())
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
            root_cells: used_values::RootInputCells::default(),
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

    fn root_with_a_descendant() -> fragment_tree::RunRootFragment {
        let mut root = fragment_tree::FragmentLink::for_test(Node::new(0, 1)).fragment;
        std::sync::Arc::get_mut(&mut root)
            .expect("a fresh fragment is not shared")
            .children = vec![fragment_tree::FragmentLink::for_test(Node::new(1, 1))];
        fragment_tree::RunRootFragment::propagating_nothing(root)
    }

    fn entry(
        key: FcRunCacheKey,
        fragment_cache_epoch: u32,
        root: Option<fragment_tree::RunRootFragment>,
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
                    input_cells_left_by_run: Some(used_values::RootInputCells::default()),
                    line_data: None,
                },
                // NB: As a run whose result depends on no percentage block size proves.
                inputs_never_observed: RunInputsTheStoredRunNeverObserved {
                    percentage_block_basis_and_block_size_definiteness: true,
                    ..RunInputsTheStoredRunNeverObserved::default()
                },
                host_findings: None,
                laid_out_abspos_contained_by_root: false,
            },
            table_cell_intrinsic_block_padding_unobserved: false,
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

    fn widths_of_entries(store: &FcRunCacheStore, slot: u32) -> Vec<AvailableSize> {
        let mut widths = Vec::new();
        store.first_entry_where(slot, |entry| {
            widths.push(entry.key.input.available_space.inline_size);
            false
        });
        widths
    }

    fn width_of_committed_entry(store: &FcRunCacheStore, slot: u32) -> Option<AvailableSize> {
        store.slots.borrow()[slot as usize]
            .committed
            .as_ref()
            .map(|entry| entry.key.input.available_space.inline_size)
    }

    /// The committedness the arena would report while the box's committed fragments are those of the entry of `marked`.
    fn mark_entries_of_key(marked: FcRunCacheKey) -> impl FnMut(Node, &FcRunCacheEntry) -> bool {
        move |_, entry| entry.key == marked
    }

    fn entries_of_key_are_committed(marked: FcRunCacheKey) -> impl Fn(&FcRunCacheEntry) -> bool {
        move |entry| entry.key == marked
    }

    fn nothing_is_committed(_: &FcRunCacheEntry) -> bool {
        false
    }

    #[test]
    fn sweep_checks_changed_entries_without_discarding_reusable_stale_data_early() {
        let store = FcRunCacheStore::default();
        let entry = |epoch| entry(at(300), epoch, None, 0);
        store.store(Node::new(0, 1), entry(1), nothing_is_committed);
        store.store(Node::new(1, 1), entry(1), nothing_is_committed);
        store.sweep_pending_entries(|_, _| true, |_, _| false, 1);
        store.sweep_pending_entries(|_, _| panic!("unchanged entries must not be swept"), |_, _| false, 2);

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
            |_, _| false,
            3,
        );
        assert_eq!(checked, 1);
        assert!(widths_of_entries(&store, 0).is_empty());
        assert_eq!(widths_of_entries(&store, 1).len(), 1);

        store.note_invalidated_entry(Node::new(1, 1));
        store.store(Node::new(1, 1), entry(2), nothing_is_committed);
        store.sweep_pending_entries(|_, validity| validity.fragment_cache_epoch == 2, |_, _| false, 4);
        assert_eq!(widths_of_entries(&store, 1).len(), 1);

        // A result stored after a mid-run invalidation still needs its validity checked.
        store.store(Node::new(0, 1), entry(1), nothing_is_committed);
        store.sweep_pending_entries(|_, validity| validity.fragment_cache_epoch == 2, |_, _| false, 5);
        assert!(widths_of_entries(&store, 0).is_empty());
    }

    #[test]
    fn a_store_keeps_the_entry_whose_fragments_the_box_committed_beside_newer_ones() {
        let store = FcRunCacheStore::default();
        let box_ = Node::new(0, 1);
        store.store(
            box_,
            entry(at(300), 1, Some(root_with_a_descendant()), 0),
            nothing_is_committed,
        );
        store.sweep_pending_entries(|_, _| true, mark_entries_of_key(at(300)), 1);
        store.store(
            box_,
            entry(at(320), 1, Some(root_with_a_descendant()), 1),
            entries_of_key_are_committed(at(300)),
        );
        store.store(box_, entry(at(340), 1, None, 1), entries_of_key_are_committed(at(300)));
        assert_eq!(
            width_of_committed_entry(&store, 0),
            Some(AvailableSize::definite(px(300)))
        );
        assert_eq!(widths_of_entries(&store, 0), widths(&[300, 340, 320]));

        store.store(
            box_,
            entry(at(300), 1, Some(root_with_a_descendant()), 1),
            entries_of_key_are_committed(at(300)),
        );
        assert_eq!(width_of_committed_entry(&store, 0), None);
        assert_eq!(widths_of_entries(&store, 0), widths(&[300, 340, 320]));
    }

    #[test]
    fn the_sweep_keeps_the_entry_whose_fragments_the_box_committed_and_its_fragments() {
        let store = FcRunCacheStore::default();
        let box_ = Node::new(0, 1);
        store.store(
            box_,
            entry(at(300), 1, Some(root_with_a_descendant()), 0),
            nothing_is_committed,
        );
        store.store(
            box_,
            entry(at(320), 1, Some(root_with_a_descendant()), 0),
            nothing_is_committed,
        );
        store.sweep_pending_entries(|_, _| true, mark_entries_of_key(at(300)), 1);
        let committed = store.slots.borrow()[0]
            .committed
            .clone()
            .expect("the earlier entry whose fragments the box committed moves beside the latest one");
        assert_eq!(
            committed.key.input.available_space.inline_size,
            AvailableSize::definite(px(300))
        );
        assert_eq!(committed.outputs.root.as_ref().unwrap().fragment.children.len(), 1);
        assert_eq!(widths_of_entries(&store, 0), widths(&[300, 320]));

        store.enqueue_for_sweep(0);
        store.sweep_pending_entries(|_, _| true, mark_entries_of_key(at(300)), 2);
        assert_eq!(
            width_of_committed_entry(&store, 0),
            Some(AvailableSize::definite(px(300))),
            "the entry stays beside the latest one while the box keeps its fragments committed"
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
    fn an_earlier_entry_committed_in_place_of_the_latest_one_stays_beside_later_stores() {
        let store = FcRunCacheStore::default();
        let box_ = Node::new(0, 1);
        store.store(
            box_,
            entry(at(300), 1, Some(root_with_a_descendant()), 0),
            nothing_is_committed,
        );
        store.store(
            box_,
            entry(at(320), 1, Some(root_with_a_descendant()), 0),
            nothing_is_committed,
        );
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
        store.store(box_, entry(at(340), 1, None, 2), entries_of_key_are_committed(at(300)));
        assert_eq!(
            width_of_committed_entry(&store, 0),
            Some(AvailableSize::definite(px(300)))
        );
        assert_eq!(
            widths_of_entries(&store, 0),
            widths(&[300, 340, 320]),
            "the entry whose fragments the box committed before stays to be replayed"
        );
    }

    #[test]
    fn a_committed_entry_that_no_longer_describes_the_committed_subtree_goes() {
        let store = FcRunCacheStore::default();
        let box_ = Node::new(0, 1);
        store.store(
            box_,
            entry(at(300), 1, Some(root_with_a_descendant()), 0),
            nothing_is_committed,
        );
        store.sweep_pending_entries(|_, _| true, mark_entries_of_key(at(300)), 1);
        store.store(
            box_,
            entry(at(320), 1, Some(root_with_a_descendant()), 1),
            entries_of_key_are_committed(at(300)),
        );
        assert_eq!(widths_of_entries(&store, 0), widths(&[300, 320]));
        store.sweep_pending_entries(|_, _| true, |_, _| false, 2);
        assert_eq!(widths_of_entries(&store, 0), widths(&[320]));
    }

    #[test]
    fn entries_holding_fragments_the_box_never_committed_go_after_two_unused_layout_commits_beside_one_it_did() {
        let store = FcRunCacheStore::default();
        let box_ = Node::new(0, 1);
        store.store(
            box_,
            entry(at(280), 1, Some(root_with_a_descendant()), 0),
            nothing_is_committed,
        );
        store.sweep_pending_entries(|_, _| true, mark_entries_of_key(at(280)), 1);
        store.store(box_, entry(at(300), 1, None, 1), entries_of_key_are_committed(at(280)));
        store.store(
            box_,
            entry(at(320), 1, Some(root_with_a_descendant()), 1),
            entries_of_key_are_committed(at(280)),
        );
        store.store(
            box_,
            entry(at(340), 1, Some(root_with_a_descendant()), 1),
            entries_of_key_are_committed(at(280)),
        );
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
            |_, _| false,
            6,
        );
    }

    #[test]
    fn the_latest_entry_of_a_box_that_committed_none_of_its_entries_stays() {
        let store = FcRunCacheStore::default();
        let box_ = Node::new(0, 1);
        store.store(
            box_,
            entry(at(300), 1, Some(root_with_a_descendant()), 0),
            nothing_is_committed,
        );
        for layout_commit_count in 1..5 {
            store.sweep_pending_entries(|_, _| true, |_, _| false, layout_commit_count);
        }
        assert_eq!(widths_of_entries(&store, 0), widths(&[300]));
    }

    #[test]
    fn earlier_entries_make_room_oldest_first_and_results_of_runs_without_fragments_first() {
        let store = FcRunCacheStore::default();
        let box_ = Node::new(0, 1);
        store.store(
            box_,
            entry(at(100), 1, Some(root_with_a_descendant()), 0),
            nothing_is_committed,
        );
        store.store(box_, entry(at(200), 1, None, 0), nothing_is_committed);
        store.store(
            box_,
            entry(at(300), 1, Some(root_with_a_descendant()), 0),
            nothing_is_committed,
        );
        store.store(
            box_,
            entry(at(400), 1, Some(root_with_a_descendant()), 0),
            nothing_is_committed,
        );
        assert_eq!(widths_of_entries(&store, 0), widths(&[400, 300, 200, 100]));
        store.store(
            box_,
            entry(at(500), 1, Some(root_with_a_descendant()), 0),
            nothing_is_committed,
        );
        assert_eq!(widths_of_entries(&store, 0), widths(&[500, 400, 300, 100]));
        store.store(
            box_,
            entry(at(600), 1, Some(root_with_a_descendant()), 0),
            nothing_is_committed,
        );
        assert_eq!(widths_of_entries(&store, 0), widths(&[600, 500, 400, 300]));
        store.store(box_, entry(at(400), 1, None, 0), nothing_is_committed);
        assert_eq!(widths_of_entries(&store, 0), widths(&[400, 600, 500, 300]));
    }

    #[test]
    fn queued_memo_invalidations_count_structural_changes_only_while_the_box_holds_entries() {
        use crate::layout::in_flow_block_cache::InFlowBlockCacheStore;
        use crate::layout::memo_invalidations::MemoInvalidations;
        let store = FcRunCacheStore::default();
        let in_flow_block_cache = InFlowBlockCacheStore::default();
        let text_memos = crate::layout::rendered_text::TextMemos::default();
        let prepared_inline_content = crate::layout::prepared_inline_content::PreparedInlineContentMemos::default();
        let intrinsic_size_caches = crate::layout::layout_node_arena::IntrinsicSizeCaches::default();
        let node = Node::new(0, 1);
        let mut notes = MemoInvalidations::default();
        notes.note_structural_inline_change(node);
        notes.apply_to(
            &store,
            &in_flow_block_cache,
            &text_memos,
            &prepared_inline_content,
            &intrinsic_size_caches,
        );
        assert_eq!(store.structural_epoch_bumps_of(node), 0);

        store.store(
            node,
            entry(at(100), 1, Some(root_with_a_descendant()), 0),
            nothing_is_committed,
        );
        let mut notes = MemoInvalidations::default();
        notes.note_structural_inline_change(node);
        notes.note_fragment_cache_epoch_bumped(node);
        notes.note_structural_inline_change(node);
        notes.apply_to(
            &store,
            &in_flow_block_cache,
            &text_memos,
            &prepared_inline_content,
            &intrinsic_size_caches,
        );
        assert_eq!(store.structural_epoch_bumps_of(node), 2);

        let reused = Node::new(0, 2);
        let mut notes = MemoInvalidations::default();
        notes.note_structural_inline_change(node);
        notes.note_slot_freed(node.slot_index());
        notes.note_structural_inline_change(reused);
        notes.apply_to(
            &store,
            &in_flow_block_cache,
            &text_memos,
            &prepared_inline_content,
            &intrinsic_size_caches,
        );
        assert!(!store.slot_holds_entries(0));
        assert_eq!(store.structural_epoch_bumps_of(reused), 0);
    }

    #[test]
    fn a_store_keeps_the_inline_damage_an_earlier_entry_saw() {
        let store = FcRunCacheStore::default();
        let box_ = Node::new(0, 1);
        store.store(
            box_,
            entry(at(300), 1, Some(root_with_a_descendant()), 0),
            nothing_is_committed,
        );
        store.note_inline_layout_damage(box_);
        store.store(box_, entry(at(320), 3, None, 0), nothing_is_committed);
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
    fn an_item_run_that_never_read_the_block_size_its_root_adopts_matches_at_another() {
        let root_cells = used_values::RootInputCells::default();
        let item_input = LayoutInput::new(
            AvailableSpace {
                inline_size: AvailableSize::definite(px(300)),
                block_size: AvailableSize::Indefinite,
            },
            ContainingBlockConstraints::default(),
            ParticipationInParentFormattingContext::Item,
        );
        let key_of = |input| FcRunCacheKey::new(formatting_context::FormattingContextType::Block, input, root_cells);
        let stored = key_of(item_input);

        let mut adopting_input = item_input;
        adopting_input.sizing.adopt_automatic_content_block_size = true;
        adopting_input.sizing.content_block_size_after_inside_layout = Some(px(40));
        let probe = key_of(adopting_input);

        assert!(!stored.matches(&probe, RunInputsTheStoredRunNeverObserved::default()));
        assert!(stored.matches(
            &probe,
            RunInputsTheStoredRunNeverObserved {
                content_block_size_adopted_after_run: true,
                ..RunInputsTheStoredRunNeverObserved::default()
            }
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
            |probe| {
                probe.input.participation = ParticipationInParentFormattingContext::Float {
                    containing_block_position_in_bfc_root: FfiCssPixelPoint::default(),
                }
            },
        ];
        for mutate in mutators {
            let mut probe = key(AvailableSize::definite(px(320)), Some(px(320)));
            mutate(&mut probe);
            assert!(!stored.matches(&probe, inline_inputs_unobserved_at_or_above(0)));
        }
    }

    fn item_key(content_block_size: i64, has_definite_block_size: bool) -> FcRunCacheKey {
        let root_cells = used_values::RootInputCells {
            content_inline_size: px(300),
            content_block_size: px(content_block_size),
            has_definite_inline_size: true,
            has_definite_block_size,
            ..used_values::RootInputCells::default()
        };
        FcRunCacheKey::new(
            formatting_context::FormattingContextType::Block,
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
            root_cells,
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
        let mut unobserved = entry(item_key(40, false), 1, None, 0).outputs.inputs_never_observed;
        assert!(!at_percentage_inline_basis(400).matches(&at_percentage_inline_basis(300), unobserved));
        unobserved.percentage_inline_basis = true;
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
            unobserved.accepted_when_reusing_stale_line_data_after_structural_inline_damage()
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
        let mut unobserved = entry(item_key(40, false), 1, None, 0).outputs.inputs_never_observed;
        unobserved.own_block_size = formatting_context::OwnBlockSizeObservation::Unobserved;
        assert!(item_key(40, false).matches(&item_key(60, true), unobserved));
        assert!(!item_key(40, false).matches(
            &item_key(60, true),
            unobserved.accepted_when_reusing_stale_line_data_after_structural_inline_damage()
        ));
    }
}
