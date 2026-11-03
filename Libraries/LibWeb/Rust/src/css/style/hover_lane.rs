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
    FfiDocumentStyleComputationInputs, FfiStyleDelta, FfiStyleInvalidationField, element_adjustment_fact,
};
use super::fast_hash::FastMap as HashMap;
use super::input_types::{STYLE_REACTION_RECOMPUTE_STYLE, StateFact};
use super::layout_style::DerivedStyleRecord;
use super::tree::StyleNodeID;
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::transition::HoverTransitions;
use smallvec::SmallVec;

/// The elements that match `:hover` while one element is hovered: the element and its eligible flat-tree ancestors, the
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

/// The cursor facts of the presented frame, including elements that have no layout box, read from the generations of
/// rows and installed records it shows: publishing them costs reference counts however many elements there are.
pub(crate) struct HoverCursorFacts {
    projection: super::pipeline::projection::ProjectionGeneration,
    installed: super::computed::InstalledStyles,
    editable: std::sync::Arc<super::fast_hash::FastSet<StyleNodeID>>,
}

impl HoverCursorFacts {
    pub(super) fn editable(&self) -> std::sync::Arc<super::fast_hash::FastSet<StyleNodeID>> {
        self.editable.clone()
    }

    pub(crate) fn is_editable(&self, element: StyleNodeID) -> bool {
        self.editable.contains(&element)
    }

    pub(crate) fn text_may_be_selected(&self, mut element: StyleNodeID) -> Option<bool> {
        use crate::css::css_enums::user_select;
        loop {
            if self.is_editable(element) {
                return None;
            }
            let installed = self.installed.element(element)?;
            let value = ComputedValuesView::of(&installed.record.payloads)
                .misc_interaction()
                .user_select;
            if value != user_select::AUTO {
                return Some(value != user_select::NONE);
            }
            match self.projection.element_inheritance_parent(element) {
                Some(parent) if parent.element_index().is_some() => element = parent,
                _ => return Some(true),
            }
        }
    }
}

impl StyleEngine {
    /// Whether `record` names an anchor the host registers, or is gone.
    fn record_names_anchors(&self, record: u64) -> bool {
        self.style_record_payloads(record).is_none_or(|payloads| {
            !ComputedValuesView::of(payloads)
                .anchor()
                .anchor_names
                .as_slice()
                .is_empty()
        })
    }

    /// The cursor facts of the elements as they stand, published again only where what they read moved since they were
    /// published last.
    pub(crate) fn hover_cursor_facts(&mut self) -> std::sync::Arc<HoverCursorFacts> {
        if let Some(facts) = self.memoized_hover_cursor_facts() {
            return facts;
        }
        let facts = std::sync::Arc::new(HoverCursorFacts {
            projection: self.retained.projection.generation(),
            installed: self.computed_group_sets.publish_installed_styles(),
            editable: self.hover_editable_nodes(),
        });
        self.memoize_hover_cursor_facts(facts.clone());
        facts
    }

    /// Whether a hover needs the style engine to restyle an element or reject a disabled control.
    pub(crate) fn hover_needs_style_state(&mut self, root: StyleNodeID) -> bool {
        self.hover_seal_needs_style_state(root)
    }

    /// The element a mouse move aimed at `node`, an element, is dispatched to, as the host dispatches it: none where
    /// the node is gone, or where the move would be aimed at a disabled form control or anything under one, which the
    /// host dispatches no event to.
    pub(crate) fn element_for_hover_dispatch(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        if node.element_index().is_none() || !self.projection.is_live(node) {
            return None;
        }
        let disabled =
            std::iter::successors(Some(node), |&ancestor| self.projection.parent(ancestor)).any(|ancestor| {
                self.projection
                    .written_facts(ancestor)
                    .is_some_and(|facts| facts.states().contains(StateFact::Disabled))
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
        let tree = &self.projection;
        let mut element = element;
        loop {
            if self.hover_target_is_editable(element) {
                return None;
            }
            let record = self.computed_group_sets.assigned_style_record(element)?;
            let view = self.computed_group_sets.style_record_view(record.raw())?;
            let computed = crate::css::computed_value_views::ComputedValuesView::of(view.payloads)
                .misc_interaction()
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

    /// The eligible hover chain, validated as a whole before any hover facts are published. A missing or
    /// excluded ancestor invalidates the path; the document and the first top layer element are valid boundaries.
    fn hover_chain(&self, hovered: Option<StyleNodeID>) -> HoverChain {
        let tree = self.tree();
        let Some(mut node) = hovered else {
            return HoverChain::new();
        };
        let mut chain = HoverChain::new();
        // https://drafts.csswg.org/selectors-4/#useraction-pseudos
        loop {
            // NB: An element whose links were not written stands as the tree settled it. The relation-only Document
            //     identity has no relations in the projection, so the tree answers for it.
            let relations = match self.retained.projection.relations_were_written(node) {
                true => self.retained.projection.written_relations(node),
                false => tree.is_live(node).then(|| self.retained.settled_tree_relations(node)),
            };
            let Some(relations) = relations else {
                return HoverChain::new();
            };
            if tree.is_relation_only(node) {
                return if relations.parent.is_none() && relations.tree_scope == super::tree::TreeScopeID::DOCUMENT {
                    chain
                } else {
                    HoverChain::new()
                };
            }
            let adjustment_facts = self.projection.adjustment_facts(node);
            // NB: The document element has no style-tree parent. The Document identity roots only the DOM child sequence.
            let is_document_element = relations.parent.is_none()
                && relations.tree_scope == super::tree::TreeScopeID::DOCUMENT
                && adjustment_facts & element_adjustment_fact::IS_DOCUMENT_ELEMENT != 0;
            if node.element_index().is_some()
                && self.retained.projection.host_of(node).is_none()
                && (relations.parent.is_some() || is_document_element)
            {
                chain.push(node);
                if is_document_element || adjustment_facts & element_adjustment_fact::RENDERED_IN_TOP_LAYER != 0 {
                    return chain;
                }
            }
            if let Some(slot) = relations.assigned_slot {
                node = slot;
                continue;
            }
            let Some(parent) = relations.parent else {
                return HoverChain::new();
            };
            if let Some(host) = self.retained.projection.host_of(parent) {
                node = host;
            } else if self.retained.projection.shadow_root_of(parent).is_some()
                || !self.retained.projection.assigned_nodes_of(parent).is_empty()
            {
                return HoverChain::new();
            } else {
                node = parent;
            }
        }
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
            self.properties_inherited_by_record(record, properties)
                .is_none_or(|inherited| !inherited.is_empty())
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

    /// Those of `properties` that a box with the style record `record` takes from its parent rather than declares, or
    /// none where the engine holds no such record.
    pub(crate) fn properties_inherited_by_record(&self, record: u64, properties: &[u16]) -> Option<SmallVec<[u16; 8]>> {
        let longhands = self.computed_group_sets.style_record_view(record)?.longhands();
        Some(
            properties
                .iter()
                .copied()
                .filter(|&property| longhands.is_inherited(property))
                .collect(),
        )
    }

    /// Whether the style record `record` composes the values of the animations of the element or pseudo-element that
    /// holds it over its base record.
    pub(crate) fn record_composes_animations(&self, record: u64) -> bool {
        super::computed::FinalStyleRecordID::from_raw(record)
            .is_some_and(super::computed::FinalStyleRecordID::is_animation_overlay)
    }

    /// The live element last requested by the host, retained while its eligible hover chain is empty.
    pub(crate) fn hover_target(&self) -> Option<StyleNodeID> {
        self.retained.hover.target
    }

    /// Moves the hover to `target`, or off the document for none: the one way hover facts move. The elements that held
    /// the hover fact and stop, and those that take it, are the ones whose facts change.
    pub(crate) fn set_hover(&mut self, target: Option<StyleNodeID>) {
        // Keep the requested identity while its chain is temporarily excluded. Tree transactions and top-layer
        // changes replay it; an explicit move off the document or identity retirement clears it.
        let target = target.filter(|&node| {
            let tree = self.tree();
            node.element_index().is_some()
                && !tree.is_relation_only(node)
                && self.retained.projection.host_of(node).is_none()
                && (tree.is_live(node) || self.node_arrival_is_pending(node))
        });
        let chain = self.hover_chain(target);
        self.retained.projection.place_hover(&chain);
        self.retained.hover = Hover { target };
    }
}

/// The element the hover targets, as [`StyleEngine::set_hover`] last placed it. The chain of elements that hold the hover
/// state for it is the projection's hover overlay.
#[derive(Clone, Default)]
pub(crate) struct Hover {
    target: Option<StyleNodeID>,
}

impl Hover {
    /// Lets go of the element `node` names, whose identity retires.
    pub(super) fn retire(&mut self, node: StyleNodeID) {
        if self.target == Some(node) {
            self.target = None;
        }
    }
}

/// A row of a hover's style update the render owner installed in the engine and in the boxes, which the host
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

/// The rows of a wave of a hover's style update, element rows each followed by the rows of its pseudo-elements,
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
                if row.pseudo_kind != u8::MAX {
                    continue;
                }
                return Some((row, pseudo_rows));
            }
            None
        })
    }
}

/// The pseudo-elements that style parts of the boxes of any element their rules match, as bits of a pseudo-style mask.
/// Rules for `::before` and `::after` generate boxes only where they give content, which the boxes answer, and every
/// element matches rules for `::marker` and `::backdrop`, which generate boxes only for a list item and a top layer
/// element.
const BOX_PSEUDO_ELEMENTS: [u8; 2] = {
    use super::publication::pseudo_kind::{FIRST_LETTER, FIRST_LINE};
    [FIRST_LETTER, FIRST_LINE]
};

/// Whether `row` keeps its element, whose record the host composes with its animations, on the record it holds, and
/// owes it no animation plan or transition step: the composition the element shows stands, and the host has nothing to
/// install for the row.
fn hover_row_keeps_a_composed_record(row: &FfiStyleDelta) -> bool {
    row.composed_by_the_host
        && !row.owes_an_animation_plan
        && !row.owes_a_transition_step
        && row.pseudo_kind == u8::MAX
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
    /// Whether a style update the render owner takes for a hover takes no input but the hover's: the host left
    /// nothing pending and no transaction of its own unended.
    pub(crate) fn takes_hover_transactions(&self) -> bool {
        !self.has_pending_transaction() && !self.has_deferred_element_style_inputs()
    }

    /// Takes the next wave of the style update of a hover, under `root`, with the document computation inputs the
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
        let output = unsafe { super::bridge::run_style_update(self, root, inputs) };
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
            output.reclaimed_style_atoms().is_empty() && rows.iter().all(|row| self.hover_row_refusal(row).is_none());
        let mut refusal = None;
        // A scroll container that snaps snaps again as the content the host lays out moves, which the render owner
        // leaves to the host.
        let resnaps = FfiStyleInvalidationField::ResnapScrollContainer as u32;
        if scroll_snaps && rows.iter().any(|row| row.record_damage & resnaps != 0) {
            installable = false;
            refusal = Some("a move a snapping scroll container snaps to");
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
        // A child that inherits a group its parent's transitions animate explicitly reads the composition, whether an
        // earlier wave started them or this one, which styles the children of an element with it under the engine.
        if installable
            && rows.iter().any(|row| {
                StyleNodeID::from_raw(row.style_node)
                    .and_then(|node| self.projection.inheritance_parent(node))
                    .and_then(|parent| {
                        composed_beside.get(&parent).copied().or_else(|| {
                            composed
                                .iter()
                                .find(|&&(node, _)| node == parent)
                                .map(|&(_, groups)| groups)
                        })
                    })
                    .is_some_and(|groups| row.explicitly_inherited_groups & groups != 0)
            })
        {
            installable = false;
            refusal = Some("a child that inherits what its parent's transitions animate");
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
        // row, which moves nothing its children inherit, as no transition animates an inherited property.
        for (node, groups) in composed {
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
            if row.record_damage & engine_computed != 0 || row.old_style_record == 0 || row.new_style_record == 0 {
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

    /// Whether `row`, of a hover's transaction, moves its element to the record the host holds for it, or to the one
    /// the composition the host holds is laid over.
    pub(crate) fn hover_row_returns_to_held_record(&self, row: &FfiStyleDelta) -> bool {
        if row.pseudo_kind != u8::MAX || row.new_style_record == 0 {
            return false;
        }
        let Some(held) = StyleNodeID::from_raw(row.style_node).and_then(|node| self.retained.held_style_record(node))
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
                || BOX_PSEUDO_ELEMENTS
                    .iter()
                    .any(|&kind| self.computed_group_sets.pseudo_style_record(node, kind).is_some())
                || self.record_displays_a_list_item(row.new_style_record)
                || self.projection.adjustment_facts(node) & element_adjustment_fact::RENDERED_IN_TOP_LAYER != 0)
        {
            return Some("a transition step of an element with pseudo-elements");
        }
        let engine_damage = row.record_damage & FfiStyleInvalidationField::EngineComputed as u32 != 0;
        if row.pseudo_kind != u8::MAX {
            if hover_row_generated_for(row).is_none() {
                return Some("a pseudo-element other than ::before, ::after and ::marker");
            }
            return (!engine_damage).then_some("a pseudo-element the host settles");
        }
        // A first record the engine computed for an element the host holds no style for, below one it did not display,
        // is the record the boxes the hover reveals are built from.
        let reveals_boxes =
            row.old_style_record == 0 && row.new_style_record != 0 && !self.retained.held_style_records.contains(node);
        if !reveals_boxes {
            if !engine_damage {
                return Some("damage the host compares");
            }
            if row.old_style_record == 0 || row.new_style_record == 0 {
                return Some("a first style");
            }
        }
        // The host keeps the root's font metrics beside its record, which only it moves.
        if self.projection.adjustment_facts(node) & element_adjustment_fact::IS_DOCUMENT_ELEMENT != 0 {
            return Some("the document element");
        }
        if (row.old_style_record != 0 && self.record_names_anchors(row.old_style_record))
            || self.record_names_anchors(row.new_style_record)
        {
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
        let view = |record: u64| self.style_record_payloads(record).map(ComputedValuesView::of);
        let paints_a_server = |values: ComputedValuesView<'_>| {
            values.inherited_svg().fill.kind == SVG_PAINT_URL || values.inherited_svg().stroke.kind == SVG_PAINT_URL
        };
        let Some(new) = view(row.new_style_record) else {
            return true;
        };
        // A first style, below an element the host did not display, publishes what the new record names alone.
        if row.old_style_record == 0 {
            use crate::css::computed_values::GroupPayloadRef;
            use crate::css::table_group_builder::group_index;
            // SAFETY: The mask payload is the record's, which stays live while the row is read.
            let masks = unsafe { GroupPayloadRef::new(group_index::MASK, std::ptr::from_ref(new.mask()).cast()) }
                .is_none_or(|mask| !mask.holds_the_defaults());
            return paints_a_server(new) || masks;
        }
        let Some(old) = view(row.old_style_record) else {
            return true;
        };
        paints_a_server(old) || paints_a_server(new) || !std::ptr::eq(old.mask(), new.mask())
    }

    /// Whether `row` moves its element to a record that holds other images than the one it held, which the host loads
    /// for the box. A box keeps the images of a move between records that hold the same ones.
    fn hover_row_moves_images(&self, row: &FfiStyleDelta) -> bool {
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
        // SAFETY: Published style-group payloads stay live while their records do.
        unsafe { crate::css::computed_values::style_group_payloads_move_image_values(&old.pointers(), &new.pointers()) }
    }

    /// Whether `record` makes its element a list item, which generates a marker.
    fn record_displays_a_list_item(&self, record: u64) -> bool {
        use crate::css::computed_value_views::ComputedValuesView;
        self.style_record_payloads(record)
            .is_none_or(|payloads| ComputedValuesView::of(payloads).display().is_list_item())
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
            for child in self.projection.flat_tree_children(parent) {
                let Some(view) = self
                    .computed_group_sets
                    .assigned_style_record(child)
                    .and_then(|record| self.style_record_view(record.raw()))
                else {
                    continue;
                };
                let longhands = view.longhands();
                let inherits: SmallVec<[u16; 2]> = inherited
                    .iter()
                    .copied()
                    .filter(|&property| longhands.is_inherited(property))
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
        std::iter::successors(self.projection.inheritance_parent(node), |&ancestor| {
            self.projection.inheritance_parent(ancestor)
        })
        .any(|ancestor| self.element_adjustment_facts(ancestor) & element_adjustment_fact::HAS_ANIMATIONS != 0)
    }

    /// Installs the rows of `wave`, which the render owner applies to the boxes, on the engine's side as the host's
    /// application of them does: what each element owes is answered, the engine's record becomes its own, and what the
    /// element's move derives for its children joins the next wave. Answers the installs for the host, whose records
    /// stay pinned until it has installed them.
    pub(crate) fn install_hover_wave(&mut self, wave: &HoverWave) -> Vec<HoverInstall> {
        let mut installs = Vec::new();
        for (row, pseudo_rows) in wave.element_rows() {
            let node = StyleNodeID::from_raw(row.style_node).expect("an installable row names a style node");
            if hover_row_keeps_a_composed_record(&row) && pseudo_rows.is_empty() {
                // As the host installs such a row: the record answers what the element owes. The composition stands as
                // the host made it.
                self.absorb_element_style_input(node, row.reaction, row.inherited_style_groups);
                self.consume_element_style_input(node);
                continue;
            }
            let (mut reaction, mut inherited_style_groups) = (row.reaction, row.inherited_style_groups);
            // A reaction a row before this one derived for the element joins the element's own, as it covers it.
            let absorbed = self.absorb_element_style_input(node, reaction, inherited_style_groups);
            if absorbed != 0 {
                reaction = absorbed as u8;
                inherited_style_groups = (absorbed >> 8) as u8;
            }
            self.consume_element_style_input(node);
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
        let _ = self.discard_style_update_outputs();
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

    /// Ends the style update of a hover once its waves are installed, as the host ends one.
    pub(crate) fn end_hover_transaction(&mut self) {
        // NB: The engine is a lane's fork, whose released identities no host mints again.
        let _ = self.discard_style_update_outputs();
    }

    /// Lets go of the records of `install`, which the host installed, or which a later install replaced.
    pub(crate) fn release_hover_install(&mut self, install: &HoverInstall) {
        for record in install.records() {
            self.unpin_style_record(record);
        }
    }
}

/// Takes the element the style node `target` names, or none for 0, as the element the hover of `host`'s document
/// targets, as the events of a mouse move hover it, and moves the engine's hover to it behind what the host wrote
/// before, where it moves. Answers whether it does.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_request_hover(host: &crate::render_state::DocumentHost, target: u32) -> bool {
    let target = StyleNodeID::from_raw(target);
    let moves = host.request_hover(target);
    if moves {
        // NB: The hover's chain reads the tree as the host wrote it.
        host.publish_element_writes();
        host.queue_change(crate::render_state::ArenaChange::Engine(
            super::engine_calls::EngineWrite::MoveHover(target),
        ));
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
