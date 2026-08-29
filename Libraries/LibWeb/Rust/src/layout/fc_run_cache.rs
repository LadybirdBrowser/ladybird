/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum FcRunCacheMode {
    Disabled,
    Enabled,
    /// Hits do not replay: the real layout runs and the entry is verified
    /// against it, panicking on any divergence.
    Shadow,
}

pub(super) fn fc_run_cache_mode_from_environment() -> FcRunCacheMode {
    static MODE: std::sync::OnceLock<FcRunCacheMode> = std::sync::OnceLock::new();
    *MODE.get_or_init(|| match std::env::var("LADYBIRD_FC_RUN_CACHE").as_deref() {
        Ok("0") => FcRunCacheMode::Disabled,
        Ok("1") => FcRunCacheMode::Enabled,
        Ok("shadow") => FcRunCacheMode::Shadow,
        Ok(unknown) => {
            eprintln!(
                "Unknown LADYBIRD_FC_RUN_CACHE value {unknown:?} (expected 0, 1, or shadow); disabling the run cache"
            );
            FcRunCacheMode::Disabled
        }
        Err(_) => FcRunCacheMode::Enabled,
    })
}

/// The complete identity of a memoizable run: the layout input plus the
/// pre-run root record state the dispatch seam captures anyway, so every
/// value a parent hands a spawned run is part of the key. Viewport changes
/// reach a run through that input or through the normal style/layout epoch
/// invalidation for viewport-dependent computed values.
#[derive(Clone, Copy, PartialEq)]
pub(super) struct FcRunCacheKey {
    fc_type: formatting_context::FfiFormattingContextType,
    input: LayoutInput,
    root_cells: used_values::UsedValuesCellState,
}

impl FcRunCacheKey {
    fn matches(&self, probe: &Self, entry_depends_on_percentage_block_size: bool) -> bool {
        if self == probe {
            return true;
        }
        if entry_depends_on_percentage_block_size {
            return false;
        }
        self.fc_type == probe.fc_type
            && self.root_cells == probe.root_cells
            && layout_inputs_match_ignoring_percentage_block_size(&self.input, &probe.input)
    }
}

fn available_block_sizes_are_interchangeable(a: AvailableSize, b: AvailableSize) -> bool {
    let is_definite_or_indefinite =
        |size: AvailableSize| matches!(size, AvailableSize::Definite(_) | AvailableSize::Indefinite);
    a == b || (is_definite_or_indefinite(a) && is_definite_or_indefinite(b))
}

fn layout_inputs_match_ignoring_percentage_block_size(a: &LayoutInput, b: &LayoutInput) -> bool {
    let LayoutInput {
        available_space,
        containing_block_constraints,
        content_box_position_in_bfc_root,
        sizing,
        participation,
    } = *a;
    let ContainingBlockConstraints {
        percentage_basis_inline_size,
        percentage_basis_block_size: _,
        quirks_mode_percentage_basis_block_size: _,
    } = containing_block_constraints;
    available_space.inline_size == b.available_space.inline_size
        && available_block_sizes_are_interchangeable(available_space.block_size, b.available_space.block_size)
        && percentage_basis_inline_size == b.containing_block_constraints.percentage_basis_inline_size
        && content_box_position_in_bfc_root == b.content_box_position_in_bfc_root
        && participation == b.participation
        && sizing_directives_match_ignoring_percentage_block_size(&sizing, &b.sizing)
}

fn sizing_directives_match_ignoring_percentage_block_size(a: &RootSizingDirectives, b: &RootSizingDirectives) -> bool {
    let RootSizingDirectives {
        forced_content_inline_size,
        forced_content_block_size,
        forced_min_border_box_block_size,
        block_parent_resolved_content_inline_size,
        table_cell_intrinsic_block_padding,
        table_box_content_block_offset_in_wrapper,
        adopt_automatic_content_block_size,
        flex_self_block_size_resolution_space,
        float_avoidance_inline_size,
        outer_float_intrusion_before_list_item_children,
        treat_block_axis_percentage_insets_as_auto_beyond_root,
    } = *a;
    forced_content_inline_size == b.forced_content_inline_size
        && forced_content_block_size == b.forced_content_block_size
        && forced_min_border_box_block_size == b.forced_min_border_box_block_size
        && block_parent_resolved_content_inline_size == b.block_parent_resolved_content_inline_size
        && table_cell_intrinsic_block_padding == b.table_cell_intrinsic_block_padding
        && table_box_content_block_offset_in_wrapper == b.table_box_content_block_offset_in_wrapper
        && adopt_automatic_content_block_size == b.adopt_automatic_content_block_size
        && float_avoidance_inline_size == b.float_avoidance_inline_size
        && outer_float_intrusion_before_list_item_children == b.outer_float_intrusion_before_list_item_children
        && treat_block_axis_percentage_insets_as_auto_beyond_root
            == b.treat_block_axis_percentage_insets_as_auto_beyond_root
        && match (
            flex_self_block_size_resolution_space,
            b.flex_self_block_size_resolution_space,
        ) {
            (None, None) => true,
            (Some(a_space), Some(b_space)) => {
                a_space.inline_size == b_space.inline_size
                    && available_block_sizes_are_interchangeable(a_space.block_size, b_space.block_size)
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

pub(super) struct FcRunCacheEntry {
    key: FcRunCacheKey,
    validity: FcRunCacheValidity,
    pub(super) outputs: formatting_context::RunOutputs,
    /// Keeps every font referenced by cached line data alive: glyph runs
    /// borrow raw font pointers, and a paint-only style change can drop
    /// the owning computed values without touching any layout epoch.
    ///
    /// Cached line data also holds raw text_utf16 pointers into arena text
    /// slots, deliberately unretained: replay never re-runs line building,
    /// commit emits offsets rather than the pointers, and every text change
    /// bumps epochs before the next probe, so nothing on the replay path
    /// dereferences them. Any future consumer of cached fragment text must
    /// snapshot the text alongside the fonts first.
    retained_fonts: Vec<libgfx_rust::font::RetainedFont>,
}

impl FcRunCacheEntry {
    pub(super) fn can_reuse_committed_subtree(&self) -> bool {
        self.outputs
            .root
            .as_ref()
            .is_none_or(|root| root.propagated_pending_abspos.is_empty())
    }

    pub(super) fn outputs_for_reused_subtree(&self) -> formatting_context::RunOutputs {
        debug_assert!(self.can_reuse_committed_subtree());
        let root = self
            .outputs
            .root
            .as_ref()
            .map(|root| fragment_tree::UnplacedRootFragment {
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
            result: self.outputs.result,
            root,
            root_outcome: self.outputs.root_outcome.clone(),
        }
    }
}

#[derive(Clone, Copy, Default)]
struct InlineLayoutDamage {
    generation: u8,
    structural_epoch_bumps: u32,
}

/// Per-document store of completed run results, one entry per slot,
/// surviving across layout passes on the node arena.
#[derive(Default)]
pub(crate) struct FcRunCacheArenaStore {
    hit_count: Cell<u64>,
    entries: RefCell<Vec<Option<std::rc::Rc<FcRunCacheEntry>>>>,
    inline_layout_damage: RefCell<Vec<InlineLayoutDamage>>,
}

impl FcRunCacheArenaStore {
    pub(crate) fn hit_count(&self) -> u64 {
        self.hit_count.get()
    }

    pub(crate) fn remove_entry(&self, slot: u32) {
        if let Some(entry) = self.entries.borrow_mut().get_mut(slot as usize) {
            *entry = None;
        }
        if let Some(damage) = self.inline_layout_damage.borrow_mut().get_mut(slot as usize) {
            *damage = InlineLayoutDamage::default();
        }
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

    fn take_inline_layout_damage(&self, box_: Node) -> u32 {
        let mut damage = self.inline_layout_damage.borrow_mut();
        let Some(entry) = damage.get_mut(box_.slot_index() as usize) else {
            return 0;
        };
        let result = if entry.generation == box_.generation() {
            entry.structural_epoch_bumps
        } else {
            0
        };
        *entry = InlineLayoutDamage::default();
        result
    }

    /// A matching entry stays stored and hands out a shared handle. A stale
    /// entry survives until the fresh run replaces it, allowing structural
    /// inline damage to use its line data during that run.
    fn matching(
        &self,
        slot: u32,
        validity: FcRunCacheValidity,
        key: &FcRunCacheKey,
    ) -> Option<std::rc::Rc<FcRunCacheEntry>> {
        let entries = self.entries.borrow();
        let stored = entries.get(slot as usize)?;
        let entry = stored.as_ref()?;
        if entry.validity != validity {
            return None;
        }
        if !entry
            .key
            .matches(key, entry.outputs.result.depends_on_percentage_block_size)
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
    ) -> Option<std::rc::Rc<FcRunCacheEntry>> {
        if structural_epoch_bumps == 0 {
            return None;
        }
        let entries = self.entries.borrow();
        let entry = entries.get(slot as usize)?.as_ref()?;
        if entry.validity.slot_generation != validity.slot_generation
            || !entry.key.matches(key, entry.outputs.result.depends_on_percentage_block_size)
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

    fn store(&self, slot: u32, entry: std::rc::Rc<FcRunCacheEntry>) {
        let mut entries = self.entries.borrow_mut();
        if entries.len() <= slot as usize {
            entries.resize_with(slot as usize + 1, || None);
        }
        entries[slot as usize] = Some(entry);
    }

    /// Drops every entry the keep predicate rejects, releasing its tree and
    /// fonts. The end-of-pass sweep uses this so entries invalidated while
    /// their box never probes again do not accumulate for the document's
    /// lifetime.
    pub(crate) fn retain_entries(&self, mut keep: impl FnMut(u32, FcRunCacheValidity) -> bool) {
        let mut entries = self.entries.borrow_mut();
        for (slot, stored) in entries.iter_mut().enumerate() {
            if let Some(entry) = stored
                && !keep(slot as u32, entry.validity)
            {
                *stored = None;
            }
        }
    }
}

fn collect_line_data_fonts(line_data: &used_values::LineData, fonts: &mut Vec<*const c_void>) {
    for line in &line_data.line_boxes {
        for fragment in &line.fragments {
            if let Some(glyphs) = &fragment.glyphs
                && !glyphs.font.is_null()
            {
                fonts.push(glyphs.font);
            }
        }
    }
}

fn collect_fragment_tree_fonts(links: &[FragmentLink], fonts: &mut Vec<*const c_void>) {
    for link in links {
        if let Some(line_data) = &link.fragment.line_data {
            collect_line_data_fonts(line_data, fonts);
        }
        collect_fragment_tree_fonts(&link.fragment.children, fonts);
    }
}

fn run_root_validity(callbacks: &FfiLayoutFcCallbacks, box_: Node) -> FcRunCacheValidity {
    let data = NodeFacts::new(callbacks, box_).data();
    FcRunCacheValidity {
        slot_generation: data.slot_generation,
        fragment_cache_epoch: data.fragment_cache_epoch,
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
        key: Box<FcRunCacheKey>,
        /// Captured at probe time and reused at store time: an invalidation
        /// landing between probe and store makes the stored entry look stale
        /// on its next probe (a fail-safe miss) instead of being baked into
        /// a forever-valid entry.
        validity: FcRunCacheValidity,
        shadow_entry: Option<std::rc::Rc<FcRunCacheEntry>>,
        structurally_damaged_entry: Option<std::rc::Rc<FcRunCacheEntry>>,
    },
}

impl FcRunCacheAttempt {
    /// Err carries the entry the caller must replay instead of running.
    #[expect(clippy::too_many_arguments)]
    pub(super) fn probe(
        purpose: formatting_context::LayoutPurpose,
        box_: Node,
        parent_grid_is_present: bool,
        fc_type: formatting_context::FfiFormattingContextType,
        layout_mode: LayoutMode,
        should_collect_devtools_layout_data: bool,
        callbacks: &FfiLayoutFcCallbacks,
        input: &LayoutInput,
        root_cells: &used_values::UsedValuesCellState,
    ) -> Result<Self, std::rc::Rc<FcRunCacheEntry>> {
        let mode = fc_run_cache_mode_from_environment();
        // A run this cache cannot describe still commits its subtree, so a stored entry would go on
        // describing paintables that run has replaced. Measurement runs commit nothing and leave it alone.
        let run_supersedes_stored_entry = layout_mode == LayoutMode::Normal && !purpose.is_measurement();
        let drop_superseded_entry = || {
            if run_supersedes_stored_entry {
                callbacks.arena().fc_run_cache_store().remove_entry(box_.slot_index());
            }
        };
        if mode == FcRunCacheMode::Disabled
            || layout_mode != LayoutMode::Normal
            || purpose.is_measurement()
            || should_collect_devtools_layout_data
            || input.participation == ParticipationInParentFormattingContext::Root
            || matches!(
                fc_type,
                formatting_context::FfiFormattingContextType::InternalReplaced | formatting_context::FfiFormattingContextType::InternalDummy
            )
            // The direct normal-layout path for an empty atomic block only sizes and snapshots its root.
            // Replaying a stored output costs more than rebuilding it and retains an entry needlessly.
            || (fc_type == formatting_context::FfiFormattingContextType::Block
                && input.participation == ParticipationInParentFormattingContext::AtomicInline
                && callbacks.first_child(box_).is_invalid())
        {
            drop_superseded_entry();
            return Ok(Self::Bypass);
        }
        if fc_type == formatting_context::FfiFormattingContextType::Grid
            && parent_grid_is_present
            && grid_formatting_context::grid_template_declares_a_subgrid_axis(callbacks, box_)
        {
            drop_superseded_entry();
            return Ok(Self::Bypass);
        }
        // Structural invariants behind this root-flag check, to re-verify if it is ever
        // narrowed: anchor() applies only to absolutely positioned boxes and every abspos
        // box is the root of its own spawned run, so the flag on the run root covers every
        // anchor consumer; anchor eligibility is resolved from used values of the same
        // pass under the C++ anchor_lookup containing-block guard, never from a previous
        // pass; scroll compensation compares chains that converge at the run root and
        // registers scroll-shift side effects a replay would skip; and anchor-size()
        // resolves to None today, so inset properties are the only anchor dependency a
        // run can have.
        if node_facts::has_flag(
            NodeFacts::new(callbacks, box_).data(),
            NodeFlag::InsetsUseAnchorFunctions,
        ) {
            drop_superseded_entry();
            return Ok(Self::Bypass);
        }
        let key = Box::new(FcRunCacheKey {
            fc_type,
            input: *input,
            root_cells: *root_cells,
        });
        let store = callbacks.arena().fc_run_cache_store();
        let validity = run_root_validity(callbacks, box_);
        let structural_epoch_bumps = store.take_inline_layout_damage(box_);
        match store.matching(box_.slot_index(), validity, &key) {
            Some(entry) if mode == FcRunCacheMode::Shadow => {
                // A shadow match is the same event a replay would be, so the
                // hit counter reports it: the hit-count tests hold under the
                // oracle as well.
                store.hit_count.set(store.hit_count.get() + 1);
                Ok(Self::Store {
                    key,
                    validity,
                    shadow_entry: Some(entry),
                    structurally_damaged_entry: None,
                })
            }
            Some(entry) => {
                store.hit_count.set(store.hit_count.get() + 1);
                Err(entry)
            }
            None => {
                let structurally_damaged_entry =
                    store.structurally_damaged_entry(box_.slot_index(), validity, &key, structural_epoch_bumps);
                Ok(Self::Store {
                    key,
                    validity,
                    shadow_entry: None,
                    structurally_damaged_entry,
                })
            }
        }
    }

    pub(super) fn previous_line_data(&self) -> Option<std::rc::Rc<used_values::LineData>> {
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
        callbacks: &FfiLayoutFcCallbacks,
        box_: Node,
        outputs: &formatting_context::RunOutputs,
    ) {
        let Self::Store {
            key,
            validity,
            shadow_entry,
            structurally_damaged_entry: _,
        } = self
        else {
            return;
        };
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
        let mut fonts = Vec::new();
        if let Some(line_data) = &outputs.root_outcome.line_data {
            collect_line_data_fonts(line_data, &mut fonts);
        }
        collect_fragment_tree_fonts(&root.scoped_descendants, &mut fonts);
        fonts.sort_unstable();
        fonts.dedup();
        let entry = FcRunCacheEntry {
            key: *key,
            validity,
            outputs: outputs.clone(),
            retained_fonts: fonts
                .into_iter()
                // SAFETY: every collected font pointer is live during the
                // pass that produced the line data now being cached.
                .map(|font| unsafe { libgfx_rust::font::RetainedFont::retain(font) })
                .collect(),
        };
        if let Some(cached) = shadow_entry {
            verify_cached_entry_against_fresh_run(box_.slot_index(), &cached, &entry);
        }
        store.store(box_.slot_index(), std::rc::Rc::new(entry));
    }
}

fn verify_cached_entry_against_fresh_run(root_slot: u32, cached: &FcRunCacheEntry, fresh: &FcRunCacheEntry) {
    assert!(
        cached.outputs.result.depends_on_percentage_block_size == fresh.outputs.result.depends_on_percentage_block_size,
        "run cache shadow: percentage block-size dependency diverged for slot {root_slot}"
    );
    assert!(
        cached.outputs.result == fresh.outputs.result,
        "run cache shadow: child layout result diverged for slot {root_slot}"
    );
    assert!(
        cached.outputs.root_outcome.cells == fresh.outputs.root_outcome.cells,
        "run cache shadow: body-end root record diverged for slot {root_slot}\ncached: {:?}\nfresh: {:?}",
        cached.outputs.root_outcome.cells,
        fresh.outputs.root_outcome.cells,
    );
    assert!(
        cached.outputs.root_outcome.own_metrics_sealed == fresh.outputs.root_outcome.own_metrics_sealed,
        "run cache shadow: root seal state diverged for slot {root_slot}"
    );
    let cached_fonts: Vec<_> = cached.retained_fonts.iter().map(|font| font.as_raw()).collect();
    let fresh_fonts: Vec<_> = fresh.retained_fonts.iter().map(|font| font.as_raw()).collect();
    assert!(
        cached_fonts == fresh_fonts,
        "run cache shadow: referenced fonts diverged for slot {root_slot}"
    );
    assert_line_data_matches(
        root_slot,
        cached.outputs.root_outcome.line_data.as_deref(),
        fresh.outputs.root_outcome.line_data.as_deref(),
    );
    assert_rare_data_matches(
        root_slot,
        cached.outputs.root_outcome.rare.as_ref(),
        fresh.outputs.root_outcome.rare.as_ref(),
    );
    assert_unplaced_roots_match(root_slot, cached.outputs.root.as_ref(), fresh.outputs.root.as_ref());
}

/// The comparable view of the payloads both `UsedValuesRareData` and
/// `Fragment` carry under identical field names, so the oracle's two
/// comparison sites cannot drift apart when a payload is added. Shared
/// payloads compare by content through the Rc (paths through their
/// process-unique identity first).
macro_rules! shadow_comparable_rare_payloads {
    ($carrier:expr) => {
        (
            $carrier.svg_viewport_transform,
            $carrier.svg_viewport_size,
            $carrier.svg_view_box,
            $carrier.svg_viewport_percentage_basis,
            &$carrier.computed_svg_path,
            &$carrier.grid_layout_data,
            &$carrier.flex_layout_data,
            &$carrier.used_grid_tracks,
            &$carrier.collapsed_table_borders,
        )
    };
}

fn line_data_matches(cached: Option<&used_values::LineData>, fresh: Option<&used_values::LineData>) -> bool {
    cached == fresh
}

fn assert_line_data_matches(
    root_slot: u32,
    cached: Option<&used_values::LineData>,
    fresh: Option<&used_values::LineData>,
) {
    assert!(
        line_data_matches(cached, fresh),
        "run cache shadow: root line data diverged for slot {root_slot}"
    );
}

fn assert_rare_data_matches(
    root_slot: u32,
    cached: Option<&used_values::UsedValuesRareData>,
    fresh: Option<&used_values::UsedValuesRareData>,
) {
    assert!(
        cached.is_some() == fresh.is_some(),
        "run cache shadow: root rare data presence diverged for slot {root_slot}"
    );
    let (Some(cached), Some(fresh)) = (cached, fresh) else {
        return;
    };
    assert!(
        shadow_comparable_rare_payloads!(cached) == shadow_comparable_rare_payloads!(fresh)
            && cached.abspos_layout_inputs == fresh.abspos_layout_inputs,
        "run cache shadow: root rare data diverged for slot {root_slot}"
    );
}

fn sorted_by_key<T: Clone, K: Ord>(items: &[T], key: impl Fn(&T) -> K) -> Vec<T> {
    let mut sorted = items.to_vec();
    sorted.sort_by_key(|item| key(item));
    sorted
}

fn assert_unplaced_roots_match(
    root_slot: u32,
    cached: Option<&fragment_tree::UnplacedRootFragment>,
    fresh: Option<&fragment_tree::UnplacedRootFragment>,
) {
    assert!(
        cached.is_some() == fresh.is_some(),
        "run cache shadow: unplaced root presence diverged for slot {root_slot}"
    );
    let (Some(cached), Some(fresh)) = (cached, fresh) else {
        return;
    };
    assert!(
        cached.node == fresh.node,
        "run cache shadow: unplaced root node diverged for slot {root_slot}"
    );

    // Escape lists are collected partly from hash-map sweeps, whose order
    // is not deterministic between runs; consumption sorts registrations
    // into tree order, so the oracle compares them order-insensitively.
    let pending_order = |child: &abspos_inputs::PendingAbsposChild| {
        (child.child_box.slot_index(), child.coordinate_space_box.slot_index())
    };
    let cached_pending = sorted_by_key(&cached.propagated_pending_abspos, pending_order);
    let fresh_pending = sorted_by_key(&fresh.propagated_pending_abspos, pending_order);
    assert!(
        cached_pending == fresh_pending,
        "run cache shadow: propagated abspos children diverged for slot {root_slot}\ncached: {cached_pending:#?}\nfresh: {fresh_pending:#?}"
    );
    let candidate_order = |candidate: &fragment_tree::AnchorCandidate| {
        (candidate.node.slot_index(), candidate.coordinate_space_box.slot_index())
    };
    let cached_candidates = sorted_by_key(&cached.propagated_anchor_candidates, candidate_order);
    let fresh_candidates = sorted_by_key(&fresh.propagated_anchor_candidates, candidate_order);
    assert!(
        cached_candidates == fresh_candidates,
        "run cache shadow: propagated anchor candidates diverged for slot {root_slot}"
    );
    let inline_rect_order = |rect: &fragment_tree::InlineContainingBlockRect| {
        (rect.inline_box.slot_index(), rect.coordinate_space_box.slot_index())
    };
    let cached_inline_rects = sorted_by_key(&cached.propagated_inline_containing_block_rects, inline_rect_order);
    let fresh_inline_rects = sorted_by_key(&fresh.propagated_inline_containing_block_rects, inline_rect_order);
    assert!(
        cached_inline_rects == fresh_inline_rects,
        "run cache shadow: propagated inline containing block rects diverged for slot {root_slot}"
    );
    let contribution_order =
        |contribution: &fragment_tree::AbsposContainingBlockInfoContribution| contribution.child_box.slot_index();
    let cached_contributions = sorted_by_key(&cached.propagated_abspos_containing_block_info, contribution_order);
    let fresh_contributions = sorted_by_key(&fresh.propagated_abspos_containing_block_info, contribution_order);
    assert!(
        cached_contributions == fresh_contributions,
        "run cache shadow: propagated containing block info diverged for slot {root_slot}"
    );

    assert_link_lists_match(root_slot, &cached.scoped_descendants, &fresh.scoped_descendants);
}

fn assert_link_lists_match(root_slot: u32, cached: &[FragmentLink], fresh: &[FragmentLink]) {
    assert!(
        cached.len() == fresh.len(),
        "run cache shadow: fragment child count diverged for slot {root_slot}"
    );
    for (cached_link, fresh_link) in cached.iter().zip(fresh) {
        assert_links_match(root_slot, cached_link, fresh_link);
    }
}

fn assert_links_match(root_slot: u32, cached: &FragmentLink, fresh: &FragmentLink) {
    let mut diverged = Vec::new();
    if cached.committed_offset != fresh.committed_offset {
        diverged.push("committed_offset");
    }
    if (
        cached.inset_left,
        cached.inset_right,
        cached.inset_top,
        cached.inset_bottom,
    ) != (fresh.inset_left, fresh.inset_right, fresh.inset_top, fresh.inset_bottom)
    {
        diverged.push("insets");
    }
    if cached.containing_line_box_index != fresh.containing_line_box_index {
        diverged.push("containing_line_box_index");
    }
    if cached.abspos_layout_inputs != fresh.abspos_layout_inputs {
        diverged.push("abspos_layout_inputs");
    }
    collect_diverged_fragment_fields(&cached.fragment, &fresh.fragment, &mut diverged);
    assert!(
        diverged.is_empty(),
        "run cache shadow: fragment for slot {} diverged under run root slot {root_slot}: {}",
        fresh.fragment.node.slot_index(),
        diverged.join(", ")
    );
    assert_link_lists_match(root_slot, &cached.fragment.children, &fresh.fragment.children);
}

fn collect_diverged_fragment_fields(
    cached: &fragment_tree::Fragment,
    fresh: &fragment_tree::Fragment,
    diverged: &mut Vec<&'static str>,
) {
    if cached.node != fresh.node {
        diverged.push("node");
    }
    if (cached.content_inline_size, cached.content_block_size) != (fresh.content_inline_size, fresh.content_block_size)
    {
        diverged.push("content_size");
    }
    if (
        cached.margin_left,
        cached.margin_right,
        cached.margin_top,
        cached.margin_bottom,
    ) != (
        fresh.margin_left,
        fresh.margin_right,
        fresh.margin_top,
        fresh.margin_bottom,
    ) {
        diverged.push("margins");
    }
    if (
        cached.border_left,
        cached.border_right,
        cached.border_top,
        cached.border_bottom,
    ) != (
        fresh.border_left,
        fresh.border_right,
        fresh.border_top,
        fresh.border_bottom,
    ) {
        diverged.push("borders");
    }
    if (
        cached.padding_left,
        cached.padding_right,
        cached.padding_top,
        cached.padding_bottom,
    ) != (
        fresh.padding_left,
        fresh.padding_right,
        fresh.padding_top,
        fresh.padding_bottom,
    ) {
        diverged.push("paddings");
    }
    if shadow_comparable_rare_payloads!(cached) != shadow_comparable_rare_payloads!(fresh) {
        diverged.push("rare payloads");
    }
    if !line_data_matches(cached.line_data.as_deref(), fresh.line_data.as_deref()) {
        diverged.push("line_data");
    }
}
