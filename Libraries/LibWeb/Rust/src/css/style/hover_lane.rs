/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The engine's side of a hover that the render owner moves while the host runs a task (see
//! `render_state::clock::hover`): which element a move hovers, the chain of elements that match `:hover` with it, and
//! the state facts a move between two chains writes, as the host's handling of the move would write them.

use super::StyleEngine;
use super::bridge::{
    FfiDocumentStyleComputationInputs, FfiStyleDelta, FfiStyleDeltaGap, FfiStyleInvalidationField,
    element_adjustment_fact, style_reaction_applied_fact,
};
use super::fast_hash::FastMap as HashMap;
use super::layout_style::DerivedStyleRecord;
use super::transaction::{
    STYLE_REACTION_INHERITED_CUSTOM_PROPERTIES, STYLE_REACTION_PSEUDO_INPUTS_MAY_HAVE_CHANGED,
    STYLE_REACTION_RECOMPUTE_STYLE, StateFact,
};
use super::tree::StyleNodeID;
use crate::css::transition::HoverTransitions;
use smallvec::SmallVec;

/// The elements that match `:hover` while one element is hovered: the element and its shadow-including ancestors, the
/// hovered element first.
pub(crate) type HoverChain = SmallVec<[StyleNodeID; 16]>;

/// How a hover's move of an element's record builds its boxes again, which a build of the boxes around it does by
/// itself.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum HoverBoxRebuild {
    /// The element gains a box, and every box below it, among the children of `parent`'s.
    GainsABox { parent: StyleNodeID },
    /// The element's box goes from among the children of `parent`'s.
    LosesItsBox { parent: StyleNodeID },
    /// The element's boxes, and every box below them, move with its record.
    BoxesMove,
    /// The element's box moves its place among the children of `parent`'s, as from inline-level to block-level, into or
    /// out of the flow, or into or out of a float: they are built again with it.
    PlaceMoves { parent: StyleNodeID },
}

impl StyleEngine {
    /// The element a mouse move aimed at `node`, an element, is dispatched to, as the host dispatches it: none where
    /// the node is gone, or where the move would be aimed at a disabled form control or anything under one, which the
    /// host dispatches no event to.
    pub(crate) fn element_for_hover_dispatch(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        if node.element_index().is_none() || !self.tree().is_live(node) {
            return None;
        }
        let disabled = std::iter::successors(Some(node), |&ancestor| self.tree().parent(ancestor)).any(|ancestor| {
            self.retained
                .facts
                .states_of_node(ancestor)
                .contains(StateFact::Disabled)
        });
        (!disabled).then_some(node)
    }

    /// Whether `element` is editable or an editing host.
    pub(crate) fn hover_target_is_editable(&self, element: StyleNodeID) -> bool {
        use crate::layout::node_data::DomPaintFact;
        self.tree().dom_paint_facts(element) & DomPaintFact::EditableOrEditingHost as u8 != 0
    }

    /// Whether text in `element` may be selected, as `DOM::Node::user_select_used_value` decides it: by the used
    /// `user-select` of the element, which is that of the nearest element at or above it whose computed value is not
    /// `auto`, where `all` and `none` pass down to the elements below, and `contain` and `text` leave them `text`, as
    /// does no such element. None where only the host knows: below an editable element or an editing host, which may
    /// be selected however its style says.
    pub(crate) fn hover_text_may_be_selected(&self, element: StyleNodeID) -> Option<bool> {
        use crate::css::css_enums::user_select;
        let tree = self.tree();
        let mut element = element;
        loop {
            if self.hover_target_is_editable(element) {
                return None;
            }
            let record = self.computed_group_sets.assigned_style_record(element)?;
            let view = self.computed_group_sets.style_record_view(record.raw())?;
            let computed = crate::css::computed_value_views::ComputedValuesView::new(
                crate::css::host_shared::SharedPayload::as_pointer_slice(view.payloads),
            )
            .misc_reset()
            .user_select;
            if computed != user_select::AUTO {
                return Some(computed != user_select::NONE);
            }
            match tree.inheritance_parent(element) {
                Some(parent) if parent.element_index().is_some() => element = parent,
                _ => return Some(true),
            }
        }
    }

    /// The parent of `node`, or the host of a shadow root, as the tree changes recorded so far leave it, or none where
    /// they leave `node` out of the tree.
    fn recorded_parent_or_shadow_host(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        let tree = self.tree();
        let settled = tree.is_live(node).then(|| self.retained.settled_tree_relations(node));
        let relations = self.host.tree_staging.current_row(node, settled)?;
        relations.parent.or_else(|| tree.host_of(node))
    }

    /// The elements that match `:hover` while `hovered` is hovered: the element and its shadow-including ancestor
    /// elements, short of the document at the root of the tree, as the tree changes recorded so far leave it.
    fn hover_chain(&self, hovered: Option<StyleNodeID>) -> HoverChain {
        let tree = self.tree();
        std::iter::successors(hovered, |&node| self.recorded_parent_or_shadow_host(node))
            .filter(|&node| {
                node.element_index().is_some()
                    && tree.host_of(node).is_none()
                    && self.recorded_parent_or_shadow_host(node).is_some()
            })
            .collect()
    }

    /// How the move of the element `row` restyles builds the element's boxes again, where `boxed` says whether it has a
    /// box now, or none where the host places its boxes otherwise.
    pub(crate) fn hover_box_rebuild(&self, row: &FfiStyleDelta, boxed: bool) -> Option<HoverBoxRebuild> {
        if row.pseudo_kind != u8::MAX {
            return None;
        }
        let node = StyleNodeID::from_raw(row.style_node)?;
        let record = super::computed::FinalStyleRecordID::from_raw(row.new_style_record)?;
        let parent = self.box_rebuild_parent(node)?;
        match (boxed, self.record_generates_a_box(record)) {
            (false, true) => Some(HoverBoxRebuild::GainsABox { parent }),
            (true, false) => Some(HoverBoxRebuild::LosesItsBox { parent }),
            (true, true) => {
                let old_record = super::computed::FinalStyleRecordID::from_raw(row.old_style_record);
                match old_record.is_some_and(|old| !self.box_keeps_its_place(old, record)) {
                    true => Some(HoverBoxRebuild::PlaceMoves { parent }),
                    false => Some(HoverBoxRebuild::BoxesMove),
                }
            }
            (false, false) => None,
        }
    }

    /// Whether the `::before`, `::after` or `::marker` of the element `node` names, as its record before `rows` and in
    /// them, takes any of `properties` from the element, which the element's transitions animate: the pseudo-element
    /// shows what the host composes then.
    pub(crate) fn box_pseudo_elements_inherit_any_of(
        &self,
        node: StyleNodeID,
        rows: &[FfiStyleDelta],
        properties: &[u16],
    ) -> bool {
        use super::publication::pseudo_kind::{AFTER, BEFORE, MARKER};
        let inherits = |record: u64| {
            let Some(table) = self
                .computed_group_sets
                .style_record_view(record)
                .and_then(|view| unsafe { view.longhand_table.as_ref() })
            else {
                return true;
            };
            properties.iter().any(|&property| table.is_inherited(property))
        };
        let held = [BEFORE, AFTER, MARKER]
            .into_iter()
            .filter_map(|kind| self.computed_group_sets.pseudo_style_record(node, kind))
            .map(super::computed::FinalStyleRecordID::raw);
        let moved = rows
            .iter()
            .filter(|row| row.style_node == node.raw() && [BEFORE, AFTER, MARKER].contains(&row.pseudo_kind))
            .map(|row| row.new_style_record)
            .filter(|&record| record != 0);
        held.chain(moved).any(inherits)
    }

    /// The element the hover targets, as [`Self::set_hover`] last placed it.
    pub(crate) fn hover_target(&self) -> Option<StyleNodeID> {
        self.retained.hover.target
    }

    /// Moves the hover to `target`, or off the document for none: the one way hover facts move. The elements that held
    /// the hover fact and stop, and those that take it, are the ones whose facts change.
    pub(crate) fn set_hover(&mut self, target: Option<StyleNodeID>) {
        let target = target.filter(|&node| self.recorded_parent_or_shadow_host(node).is_some());
        let new_chain = self.hover_chain(target);
        let old_chain = std::mem::take(&mut self.retained.hover.chain);
        for &node in old_chain.iter().filter(|node| !new_chain.contains(node)) {
            let arriving = self.node_arrival_is_pending(node);
            self.record_batched_state(node, StateFact::Hover, false, arriving);
        }
        for &node in new_chain.iter().filter(|node| !old_chain.contains(node)) {
            let arriving = self.node_arrival_is_pending(node);
            self.record_batched_state(node, StateFact::Hover, true, arriving);
        }
        self.settle_batched_inputs();
        self.retained.hover = Hover {
            target,
            chain: new_chain,
        };
    }
}

/// Where the hover is: the element it targets, and the elements that hold the hover fact for it, as
/// [`StyleEngine::set_hover`] last placed them.
#[derive(Clone, Default)]
pub(crate) struct Hover {
    target: Option<StyleNodeID>,
    chain: HoverChain,
}

impl Hover {
    /// Lets go of the element `node` names, whose identity retires: an element that leaves the tree takes its hover
    /// fact along.
    pub(super) fn retire(&mut self, node: StyleNodeID) {
        if self.target == Some(node) {
            self.target = None;
        }
        self.chain.retain(|held| *held != node);
    }
}

/// A row of a hover's style transaction the render owner installed in the engine and in the boxes, which the host
/// installs on its element in turn: the element's row, and the rows of its pseudo-elements beside it. The records the
/// rows move to stay pinned until [`StyleEngine::release_hover_install`].
#[derive(Clone)]
pub(crate) struct HoverInstall {
    pub(crate) element: FfiStyleDelta,
    pub(crate) pseudo_elements: SmallVec<[FfiStyleDelta; 2]>,
}

impl HoverInstall {
    /// The records the host installs: those the rows move their elements to, or the one an element whose row moves only
    /// its environment holds, which the engine moved as it moved the environment.
    fn records(&self) -> impl Iterator<Item = u64> + '_ {
        let element = match self.element.new_style_record {
            0 => self.element.old_style_record,
            record => record,
        };
        std::iter::once(element)
            .chain(self.pseudo_elements.iter().map(|row| row.new_style_record))
            .filter(|&record| record != 0)
    }
}

/// The rows of a wave of a hover's style transaction, element rows each followed by the rows of its pseudo-elements,
/// as the transaction answered them.
pub(crate) struct HoverWave {
    pub(crate) rows: Vec<FfiStyleDelta>,
    /// Whether the engine settled every row in a way the render owner can install as the host would.
    pub(crate) installable: bool,
    /// The transitions the rows start, each with its sample as it starts, pinned for the element's box.
    pub(crate) transitions: Vec<(HoverTransitions, DerivedStyleRecord)>,
    /// Why the render owner leaves the wave to the host where no row of it says, if it does.
    pub(crate) refusal: Option<&'static str>,
}

impl HoverWave {
    /// Each element row of the wave with the rows of its pseudo-elements, but those of elements the engine answered as
    /// hidden, which need no style until a read or a reveal asks.
    pub(crate) fn element_rows(&self) -> impl Iterator<Item = (FfiStyleDelta, &[FfiStyleDelta])> + '_ {
        let rows = &self.rows;
        let mut index = 0;
        std::iter::from_fn(move || {
            while index < rows.len() {
                let row = rows[index];
                let pseudo_elements = rows[index + 1..]
                    .iter()
                    .take_while(|pseudo| pseudo.style_node == row.style_node && pseudo.pseudo_kind != u8::MAX)
                    .count();
                let pseudo_rows = &rows[index + 1..index + 1 + pseudo_elements];
                index += 1 + pseudo_elements;
                if row.pseudo_kind != u8::MAX || row.gap == FfiStyleDeltaGap::Hidden {
                    continue;
                }
                return Some((row, pseudo_rows));
            }
            None
        })
    }
}

/// The reactions of a row that moves only its element's inherited custom-property environment, which the host answers
/// by computing nothing for an element whose style, its pseudo-elements' included, reads none of it.
const ENVIRONMENT_ONLY_REACTIONS: u8 =
    STYLE_REACTION_INHERITED_CUSTOM_PROPERTIES | STYLE_REACTION_PSEUDO_INPUTS_MAY_HAVE_CHANGED;

/// The pseudo-elements that style parts of the boxes of any element their rules match, as bits of a pseudo-style mask.
/// Rules for `::before` and `::after` generate boxes only where they give content, which the boxes answer, and every
/// element matches rules for `::marker` and `::backdrop`, which generate boxes only for a list item and a top layer
/// element.
const BOX_PSEUDO_ELEMENTS: u64 = {
    use super::publication::pseudo_kind::{FIRST_LETTER, FIRST_LINE};
    (1 << FIRST_LETTER) | (1 << FIRST_LINE)
};

/// Whether `row` keeps its element, whose record the host composes with its animations, on the record it holds, and
/// owes it no animation plan or transition step: the composition the element shows stands, and the host has nothing to
/// install for the row.
fn hover_row_keeps_a_composed_record(row: &FfiStyleDelta) -> bool {
    row.composed_by_the_host
        && !row.owes_an_animation_plan
        && !row.owes_a_transition_step
        && row.pseudo_kind == u8::MAX
        && row.gap == FfiStyleDeltaGap::Computed
        && row.old_style_record != 0
        && row.old_style_record == row.new_style_record
}

/// The descendants of an element that inherit what its animations or transitions animate, each with the properties it
/// inherits.
pub(crate) type InheritingDescendants = Vec<(StyleNodeID, SmallVec<[u16; 2]>)>;

/// How many descendants of an element whose animations or transitions animate an inherited property the render owner
/// restyles with them at each tick, beyond which it leaves them to the host.
pub(crate) const INHERITING_DESCENDANTS_LIMIT: usize = 64;

/// The pseudo-element a row of a hover's transaction styles, as a box's `generated_for`: 0 for the element's own row,
/// or none for a pseudo-element whose boxes the render owner does not style.
pub(crate) fn hover_row_generated_for(row: &FfiStyleDelta) -> Option<u8> {
    use super::publication::pseudo_kind::{AFTER, BEFORE, MARKER};
    use crate::layout::node_data::{GENERATED_FOR_AFTER, GENERATED_FOR_BEFORE, GENERATED_FOR_MARKER};
    match row.pseudo_kind {
        u8::MAX => Some(0),
        BEFORE => Some(GENERATED_FOR_BEFORE),
        AFTER => Some(GENERATED_FOR_AFTER),
        MARKER => Some(GENERATED_FOR_MARKER),
        _ => None,
    }
}

impl StyleEngine {
    /// Whether a style transaction the render owner takes for a hover takes no input but the hover's: the host left
    /// nothing pending and no transaction of its own unended.
    pub(crate) fn takes_hover_transactions(&self) -> bool {
        !self.has_pending_transaction()
            && !self.has_deferred_element_style_inputs()
            && self.retained.engine_computed_records_pending.is_empty()
    }

    /// Takes the next wave of the style transaction of a hover, under `root`, with the document computation inputs the
    /// host sealed last, and answers its rows, and whether the render owner can install all of them as the host would.
    /// The wave leaves the atoms it would sweep to a later transaction, which the host takes.
    ///
    /// # Safety
    ///
    /// The buffers `inputs` names must be live for this call.
    ///
    /// `composed_beside` holds the elements of the hover's earlier waves whose transitions the render owner composes,
    /// each with the non-inherited style groups they animate, which the wave adds its own to. `scroll_snaps` says whether
    /// a scroll container of the document may snap, `at` is the time of the hover in the document timeline's
    /// milliseconds, `reference_box` answers the transform reference box of an element's box, which the transitions
    /// of a transform interpolate against, and `lane_transitions` the transitions a lane's hover started on an element,
    /// which a step decides over in place of those the host runs on it.
    #[allow(clippy::too_many_arguments)]
    pub(crate) unsafe fn take_hover_wave<'lane>(
        &mut self,
        root: StyleNodeID,
        inputs: FfiDocumentStyleComputationInputs,
        composed_beside: &mut HashMap<StyleNodeID, u32>,
        scroll_snaps: bool,
        at: f64,
        reference_box: impl Fn(StyleNodeID) -> Option<crate::css::css_pixels::CssPixelRect>,
        lane_transitions: impl Fn(StyleNodeID) -> Option<crate::css::transition::LaneTransitions<'lane>>,
    ) -> HoverWave {
        self.defer_atom_sweep(true);
        // SAFETY: Guaranteed by the caller.
        let output = unsafe { super::bridge::take_style_transaction(self, root, inputs, None) };
        self.defer_atom_sweep(false);
        let mut rows = output.answers().to_vec();
        self.answer_hover_row_damage(&mut rows);
        // The host runs the transition step of an element that runs transitions as it installs a record that moves it,
        // whatever the row owes: a step toward a style that declares none still ends them. So does the render owner,
        // over the host's transitions or those a lane's hover started.
        for row in &mut rows {
            if row.pseudo_kind == u8::MAX
                && !row.owes_a_transition_step
                && !row.owes_an_animation_plan
                && row.old_style_record != 0
                && row.new_style_record != 0
                && row.old_style_record != row.new_style_record
                && StyleNodeID::from_raw(row.style_node).is_some_and(|node| {
                    self.element_adjustment_facts(node) & element_adjustment_fact::HAS_ANIMATIONS != 0
                        || lane_transitions(node).is_some()
                })
            {
                row.owes_a_transition_step = true;
            }
        }
        let mut installable =
            output.reclaimed_style_atoms().is_empty() && rows.iter().all(|row| self.hover_row_is_installable(row));
        let mut refusal = None;
        // A scroll container that snaps snaps again as the content the host lays out moves, which the render owner
        // leaves to the host.
        let resnaps = FfiStyleInvalidationField::ResnapScrollContainer as u32;
        if scroll_snaps && rows.iter().any(|row| row.record_damage & resnaps != 0) {
            installable = false;
            refusal = Some("a move a snapping scroll container snaps to");
        }
        // A child that inherits a group its parent's transitions animate explicitly reads the composition.
        if rows.iter().any(|row| {
            StyleNodeID::from_raw(row.style_node)
                .and_then(|node| self.tree.inheritance_parent(node))
                .and_then(|parent| composed_beside.get(&parent))
                .is_some_and(|&groups| row.explicitly_inherited_groups & groups != 0)
        }) {
            installable = false;
            refusal = Some("a child that inherits what its parent's transitions animate");
        }
        // A row that owes its element the transition step starts the transitions the step decides, which the render
        // owner samples itself where nothing but the engine decides them.
        let mut transitions = Vec::new();
        let mut composed = Vec::new();
        for row in rows.iter().filter(|row| row.owes_a_transition_step) {
            if !installable {
                break;
            }
            let node = StyleNodeID::from_raw(row.style_node);
            let reference_box = node.and_then(&reference_box);
            let lane = node.and_then(&lane_transitions);
            let started = HoverTransitions::of_row(self, row, reference_box, at, lane).and_then(|transitions| {
                let Some(mut transitions) = transitions else {
                    return Ok(None);
                };
                // The descendants that inherit what the transitions animate show it as they run.
                let inherited = transitions.inherited_properties();
                if !inherited.is_empty() {
                    (transitions.inheriting, transitions.inheriting_covers_subtree) = self
                        .inheriting_descendants(transitions.node, &inherited, INHERITING_DESCENDANTS_LIMIT)
                        .ok_or("more descendants inherit a transition than the render owner restyles")?;
                }
                let (sample, _) = transitions
                    .sample(self, 0.0, reference_box, scroll_snaps)
                    .map_err(|_| "a transition sample the host composes")?;
                Ok(Some((transitions, sample)))
            });
            match started {
                Ok(Some(started)) => {
                    if let Some(groups) = started.0.non_inherited_style_groups() {
                        composed.push((started.0.node, groups));
                    }
                    transitions.push(started);
                }
                Ok(None) => {
                    if let Some(node) = StyleNodeID::from_raw(row.style_node) {
                        composed.push((node, 0));
                    }
                }
                Err(transitions_refusal) => {
                    installable = false;
                    refusal = Some(transitions_refusal);
                }
            }
        }
        let mut wave = HoverWave {
            rows,
            installable,
            transitions,
            refusal,
        };
        if !wave.installable {
            self.release_hover_transition_samples(&mut wave);
            return wave;
        }
        // The host composes the record of an element whose transitions the render owner composes as it installs the
        // row, which moves nothing its children inherit, as no transition animates an inherited property: the next
        // wave settles them over the record the element holds.
        for (node, groups) in composed {
            self.settle_children_beside_composition_of(node);
            composed_beside.insert(node, groups);
        }
        wave
    }

    /// Lets go of the samples of the transitions of `wave`, which no box shows.
    pub(crate) fn release_hover_transition_samples(&mut self, wave: &mut HoverWave) {
        for (_, sample) in wave.transitions.drain(..) {
            self.unpin_layout_style_record(sample.record);
        }
    }

    /// Answers what the moves of the rows of a hover's wave that the transaction left the host to compare damage, as
    /// the engine answers it for a host that asks: the render owner reads the damage to install the rows.
    fn answer_hover_row_damage(&mut self, rows: &mut [FfiStyleDelta]) {
        let engine_computed = FfiStyleInvalidationField::EngineComputed as u32;
        let mut element_record = 0;
        for row in rows.iter_mut() {
            let Some(node) = StyleNodeID::from_raw(row.style_node) else {
                continue;
            };
            if row.pseudo_kind == u8::MAX {
                element_record = row.new_style_record;
            }
            if row.record_damage & engine_computed != 0
                || row.old_style_record == 0
                || row.new_style_record == 0
                || !matches!(row.gap, FfiStyleDeltaGap::Computed | FfiStyleDeltaGap::None)
            {
                continue;
            }
            let damage = if row.pseudo_kind == u8::MAX {
                self.retained
                    .element_record_damage(node, false, row.old_style_record, row.new_style_record)
            } else {
                self.retained.pseudo_element_record_damage(
                    node,
                    row.pseudo_kind,
                    row.old_style_record,
                    row.new_style_record,
                    element_record,
                    false,
                )
            };
            row.record_damage = damage | engine_computed;
        }
    }

    /// Whether the render owner can install `row`, of a hover's transaction, as the host would: a record the engine
    /// settled itself, which owes the host no animation plan or composition, and no transition step of a
    /// pseudo-element.
    fn hover_row_is_installable(&self, row: &FfiStyleDelta) -> bool {
        self.hover_row_refusal(row).is_none()
    }

    /// Whether `row`, of a hover's transaction, moves its element to the record the host holds for it, or to the one
    /// the composition the host holds is laid over.
    pub(crate) fn hover_row_returns_to_held_record(&self, row: &FfiStyleDelta) -> bool {
        if row.pseudo_kind != u8::MAX || row.new_style_record == 0 {
            return false;
        }
        let Some(held) =
            StyleNodeID::from_raw(row.style_node).and_then(|node| self.retained.held_style_records.get(&node).copied())
        else {
            return false;
        };
        held == row.new_style_record
            || super::computed::FinalStyleRecordID::from_raw(held)
                .and_then(|held| self.retained.computed_group_sets.underlying_style_record(held))
                .is_some_and(|underlying| underlying.raw() == row.new_style_record)
    }

    /// Why the render owner cannot install `row`, of a hover's transaction, as the host would, or none where it can.
    pub(crate) fn hover_row_refusal(&self, row: &FfiStyleDelta) -> Option<&'static str> {
        if hover_row_keeps_a_composed_record(row) {
            return None;
        }
        // The host composes the record of a row that owes the transition step, which the render owner samples itself:
        // only one that owes no animation plan, whose element's pseudo-elements, which the host settles over the
        // composition, generate no boxes. The boxes answer for `::before`, `::after` and `::marker`.
        if row.owes_an_animation_plan || (row.composed_by_the_host && !row.owes_a_transition_step) {
            return Some("animations");
        }
        let Some(node) = StyleNodeID::from_raw(row.style_node) else {
            return Some("no style node");
        };
        if row.owes_a_transition_step
            && (row.pseudo_kind != u8::MAX
                || self.published_pseudo_style_mask(node) & BOX_PSEUDO_ELEMENTS != 0
                || self.record_displays_a_list_item(row.new_style_record)
                || self.computed_group_sets.adjustment_facts(node) & element_adjustment_fact::RENDERED_IN_TOP_LAYER
                    != 0)
        {
            return Some("a transition step of an element with pseudo-elements");
        }
        let engine_damage = row.record_damage & FfiStyleInvalidationField::EngineComputed as u32 != 0;
        if row.pseudo_kind != u8::MAX {
            if hover_row_generated_for(row).is_none() {
                return Some("a pseudo-element other than ::before, ::after and ::marker");
            }
            return (row.gap != FfiStyleDeltaGap::Computed || !engine_damage)
                .then_some("a pseudo-element the host settles");
        }
        if self.hover_row_moves_only_an_environment(row) {
            return None;
        }
        match row.gap {
            FfiStyleDeltaGap::Hidden => return None,
            FfiStyleDeltaGap::Materialize => return Some("a record the host materializes"),
            FfiStyleDeltaGap::Computed | FfiStyleDeltaGap::None => {
                if !engine_damage {
                    return Some("damage the host compares");
                }
                if row.old_style_record == 0 || row.new_style_record == 0 {
                    return Some("a first style");
                }
            }
        }
        // The host keeps the root's font metrics beside its record, which only it moves.
        if self.computed_group_sets.adjustment_facts(node) & element_adjustment_fact::IS_DOCUMENT_ELEMENT != 0 {
            return Some("the document element");
        }
        if self.record_names_anchors(row.old_style_record) || self.record_names_anchors(row.new_style_record) {
            return Some("anchors the host registers");
        }
        if self.hover_row_moves_images(row) {
            return Some("images the host loads");
        }
        if self.hover_row_moves_svg_references(row) {
            return Some("SVG references the host publishes");
        }
        None
    }

    /// Whether `row` may move what the host publishes of the SVG resources its element's style names: its mask, clip
    /// path, fill and stroke. Only a paint server or a mask or clip path names one.
    fn hover_row_moves_svg_references(&self, row: &FfiStyleDelta) -> bool {
        use crate::css::computed_value_types::SVG_PAINT_URL;
        use crate::css::computed_value_views::ComputedValuesView;
        use crate::css::host_shared::SharedPayload;
        let view = |record: u64| {
            self.style_record_payloads(record)
                .map(|payloads| ComputedValuesView::new(SharedPayload::as_pointer_slice(payloads)))
        };
        let (Some(old), Some(new)) = (view(row.old_style_record), view(row.new_style_record)) else {
            return true;
        };
        let paints_a_server = |values: ComputedValuesView<'_>| {
            values.inherited_svg().fill.kind == SVG_PAINT_URL || values.inherited_svg().stroke.kind == SVG_PAINT_URL
        };
        paints_a_server(old) || paints_a_server(new) || !std::ptr::eq(old.mask(), new.mask())
    }

    /// Whether `row` moves its element to a record that holds other images than the one it held, which the host loads
    /// for the box. A box keeps the images of a move between records that hold the same ones.
    fn hover_row_moves_images(&self, row: &FfiStyleDelta) -> bool {
        use crate::css::host_shared::SharedPayload;
        if !self.style_record_holds_image_values(row.old_style_record)
            && !self.style_record_holds_image_values(row.new_style_record)
        {
            return false;
        }
        let (Some(old), Some(new)) = (
            self.style_record_payloads(row.old_style_record),
            self.style_record_payloads(row.new_style_record),
        ) else {
            return true;
        };
        crate::css::computed_values::style_group_payloads_move_image_values(
            SharedPayload::as_pointer_slice(old),
            SharedPayload::as_pointer_slice(new),
        )
    }

    /// Whether `row` owes its element no more than a moved inherited custom-property environment, which its style reads
    /// none of: the engine moved the element's environment, and the record over it, as its parent's moved. The host
    /// installs nothing for such a row, and only carries the reaction on to the children. The engine asks the host to
    /// compute the style again of an element it leaves such a reaction to that reads the environment.
    pub(crate) fn hover_row_moves_only_an_environment(&self, row: &FfiStyleDelta) -> bool {
        row.gap == FfiStyleDeltaGap::Materialize
            && row.pseudo_kind == u8::MAX
            && row.reaction & STYLE_REACTION_INHERITED_CUSTOM_PROPERTIES != 0
            && row.reaction & !ENVIRONMENT_ONLY_REACTIONS == 0
            && row.old_style_record != 0
            && StyleNodeID::from_raw(row.style_node).is_some_and(|node| {
                self.deferred_element_style_reaction(node)
                    .is_none_or(|pending| pending & !ENVIRONMENT_ONLY_REACTIONS == 0)
            })
    }

    /// Whether `record` makes its element a list item, which generates a marker.
    fn record_displays_a_list_item(&self, record: u64) -> bool {
        use crate::css::computed_value_views::ComputedValuesView;
        use crate::css::host_shared::SharedPayload;
        self.style_record_payloads(record).is_none_or(|payloads| {
            ComputedValuesView::new(SharedPayload::as_pointer_slice(payloads))
                .display()
                .is_list_item()
        })
    }

    /// The flat-tree element descendants of the element `node` names that inherit one of `properties` from it, each with
    /// those it inherits, through descendants that inherit them, and whether every styled element in its subtree does;
    /// none where more than `limit` do.
    pub(crate) fn inheriting_descendants(
        &self,
        node: StyleNodeID,
        properties: &[u16],
        limit: usize,
    ) -> Option<(InheritingDescendants, bool)> {
        let mut inheriting = Vec::new();
        let mut covers_subtree = true;
        let mut parents: Vec<(StyleNodeID, SmallVec<[u16; 2]>)> = vec![(node, properties.iter().copied().collect())];
        while let Some((parent, inherited)) = parents.pop() {
            let children: SmallVec<[StyleNodeID; 8]> = self.tree.flat_tree_children(parent).collect();
            for child in children {
                let Some(view) = self
                    .computed_group_sets
                    .assigned_style_record(child)
                    .and_then(|record| self.style_record_view(record.raw()))
                else {
                    continue;
                };
                // SAFETY: A live record's table lives as long as the record.
                let Some(table) = (unsafe { view.longhand_table.as_ref() }) else {
                    continue;
                };
                let inherits: SmallVec<[u16; 2]> = inherited
                    .iter()
                    .copied()
                    .filter(|&property| table.is_inherited(property))
                    .collect();
                if inherits.is_empty() {
                    covers_subtree = false;
                    continue;
                }
                if inheriting.len() == limit {
                    return None;
                }
                inheriting.push((child, inherits.clone()));
                parents.push((child, inherits));
            }
        }
        Some((inheriting, covers_subtree))
    }

    /// Whether an element the element `node` inherits from runs animations, which move what it inherits.
    pub(crate) fn inheritance_ancestors_animate(&self, node: StyleNodeID) -> bool {
        std::iter::successors(self.tree.inheritance_parent(node), |&ancestor| {
            self.tree.inheritance_parent(ancestor)
        })
        .any(|ancestor| self.element_adjustment_facts(ancestor) & element_adjustment_fact::HAS_ANIMATIONS != 0)
    }

    /// Whether `row` moves its element's custom-property environment, which the elements below it inherit.
    fn hover_row_moves_custom_properties(&self, row: &FfiStyleDelta) -> bool {
        let sets = &self.computed_group_sets;
        sets.style_record_custom_property_environment(row.old_style_record)
            != sets.style_record_custom_property_environment(row.new_style_record)
    }

    /// Installs the rows of `wave`, which the render owner applies to the boxes, on the engine's side as the host's
    /// application of them does: what each element owes is answered, the engine's record becomes its own, and what the
    /// element's move derives for its children joins the next wave. Answers the installs for the host, whose records
    /// stay pinned until it has installed them.
    pub(crate) fn install_hover_wave(&mut self, wave: &HoverWave) -> Vec<HoverInstall> {
        let mut installs = Vec::new();
        for (row, pseudo_rows) in wave.element_rows() {
            let node = StyleNodeID::from_raw(row.style_node).expect("an installable row names a style node");
            if self.hover_row_moves_only_an_environment(&row) {
                // As the host applies such a row: the reaction it owes joins the element's own, and the children take
                // it on from an element whose style did not move.
                let absorbed = self.absorb_element_style_input(node, row.reaction, row.inherited_style_groups, true);
                let (reaction, inherited_style_groups) = match absorbed {
                    0 => (row.reaction, row.inherited_style_groups),
                    absorbed => (absorbed as u8, (absorbed >> 8) as u8),
                };
                // A reaction an earlier row derived for the element that asks for more is the host's to answer.
                if reaction & !ENVIRONMENT_ONLY_REACTIONS != 0 {
                    self.record_derived_element_style_input(
                        node,
                        reaction | STYLE_REACTION_RECOMPUTE_STYLE,
                        inherited_style_groups,
                    );
                    continue;
                }
                let (_, facts) =
                    self.engine_row_child_reaction_facts(node, row.old_style_record, row.old_style_record, []);
                self.note_style_reaction_applied(
                    node,
                    reaction,
                    0,
                    facts | style_reaction_applied_fact::INVALIDATION_IS_NONE,
                );
                // The host's element holds the environment, and the record, the engine held before it moved them, which
                // the host takes in its turn, as its own move of the environment hands them down.
                let install = HoverInstall {
                    element: FfiStyleDelta { reaction, ..row },
                    pseudo_elements: SmallVec::new(),
                };
                for record in install.records() {
                    self.pin_style_record(record);
                }
                installs.push(install);
                continue;
            }
            if hover_row_keeps_a_composed_record(&row) && pseudo_rows.is_empty() {
                // As the host installs such a row: the record answers what the element owes, and the children take the
                // reaction on from an element whose style did not move. The composition stands as the host made it.
                let absorbed = self.absorb_element_style_input(node, row.reaction, row.inherited_style_groups, false);
                let reaction = if absorbed != 0 { absorbed as u8 } else { row.reaction };
                self.consume_element_style_input(node);
                self.retained.acknowledge_engine_computed_record(node);
                let (_, facts) =
                    self.engine_row_child_reaction_facts(node, row.old_style_record, row.old_style_record, []);
                self.note_style_reaction_applied(
                    node,
                    reaction,
                    0,
                    facts | style_reaction_applied_fact::INVALIDATION_IS_NONE,
                );
                continue;
            }
            let (mut reaction, mut inherited_style_groups) = (row.reaction, row.inherited_style_groups);
            // A reaction a row before this one derived for the element joins the element's own, as it covers it.
            let absorbed = self.absorb_element_style_input(node, reaction, inherited_style_groups, false);
            if absorbed != 0 {
                reaction = absorbed as u8;
                inherited_style_groups = (absorbed >> 8) as u8;
            }
            self.consume_element_style_input(node);
            if row.gap == FfiStyleDeltaGap::Computed {
                self.retained.acknowledge_engine_computed_record(node);
            }
            let damages =
                std::iter::once(row.record_damage).chain(pseudo_rows.iter().map(|pseudo| pseudo.record_damage));
            let (inherited_style_groups_changed, mut facts) =
                self.engine_row_child_reaction_facts(node, row.old_style_record, row.new_style_record, damages);
            // The host moves the environments below an element whose own moved as it installs the row. Here, the
            // elements below compute again over the environment the engine now assigns their parent, in the next wave.
            if self.hover_row_moves_custom_properties(&row) {
                facts |= style_reaction_applied_fact::DID_CHANGE_CUSTOM_PROPERTIES;
            }
            self.note_style_reaction_applied(node, reaction, inherited_style_groups_changed, facts);
            let install = HoverInstall {
                element: FfiStyleDelta {
                    reaction,
                    inherited_style_groups,
                    ..row
                },
                pseudo_elements: pseudo_rows.iter().copied().collect(),
            };
            for record in install.records() {
                self.pin_style_record(record);
            }
            installs.push(install);
        }
        installs
    }

    /// Leaves the rows of `wave`, which the render owner cannot install, to the host: the transaction's outputs are
    /// discarded, and each element of the wave owes the host the reaction its row answered, which the host's next style
    /// update takes.
    pub(crate) fn abandon_hover_wave(&mut self, wave: &mut HoverWave) {
        self.release_hover_transition_samples(wave);
        // NB: The engine is a lane's fork, whose released identities no host mints again.
        let _ = self.discard_style_transaction_outputs();
        for (row, _) in wave.element_rows() {
            if let Some(node) = StyleNodeID::from_raw(row.style_node) {
                self.record_derived_element_style_input(
                    node,
                    row.reaction | STYLE_REACTION_RECOMPUTE_STYLE,
                    row.inherited_style_groups,
                );
            }
        }
    }

    /// Ends the style transaction of a hover once its waves are installed, as the host ends one.
    pub(crate) fn end_hover_transaction(&mut self) {
        // NB: The engine is a lane's fork, whose released identities no host mints again.
        let _ = self.discard_style_transaction_outputs();
    }

    /// Lets go of the records of `install`, which the host installed, or which a later install replaced.
    pub(crate) fn release_hover_install(&mut self, install: &HoverInstall) {
        for record in install.records() {
            self.unpin_style_record(record);
        }
    }
}

/// Takes the element the style node `target` names, or none for 0, as the element the hover of `host`'s document
/// targets, as the events of a mouse move hover it, and stages the move of the engine's hover to it for the input the
/// host submits next, where it moves. Answers whether it does.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_request_hover(host: &crate::render_state::DocumentHost, target: u32) -> bool {
    let target = StyleNodeID::from_raw(target);
    let moves = host.request_hover(target);
    if moves {
        host.engine_memo().staged_input.borrow_mut().stage_hover(target);
    }
    moves
}

/// Hands `adopt` the transitions the hover of the clock lane of `host`'s document that follows the presented frame
/// leaves each element running that the host has yet to run as the lane does: the element's style node, the host's
/// transitions the lane's steps ended as they ran, those the steps saw at all, and the transitions.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread. `adopt` must not keep the pointers it gets beyond
/// the call, or the values they point at without retaining them.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_adopt_lane_transitions(
    host: &crate::render_state::DocumentHost,
    context: *mut std::ffi::c_void,
    adopt: unsafe extern "C" fn(
        context: *mut std::ffi::c_void,
        node: u32,
        cancelled: *const u64,
        cancelled_count: usize,
        seen: *const u64,
        seen_count: usize,
        transitions: *const crate::css::transition::FfiLaneTransition,
        transition_count: usize,
    ),
) {
    for transitions in host.take_lane_transitions() {
        let lane_transitions: Vec<_> = transitions.lane_transitions().collect();
        let host_seen = &transitions.host_seen;
        // SAFETY: Guaranteed by the caller; the element's transitions keep the values alive across the call.
        unsafe {
            adopt(
                context,
                transitions.node.raw(),
                host_seen.cancelled.as_ptr(),
                host_seen.cancelled.len(),
                host_seen.seen.as_ptr(),
                host_seen.seen.len(),
                lane_transitions.as_ptr(),
                lane_transitions.len(),
            );
        }
    }
}
