/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The engine on demand: the style of an element, or of one of its pseudo-elements, that a read asks
//! for and no pass keeps current, such as that of an element below one that is not displayed, or of
//! a pseudo-element that generates no box. It is computed from the records the engine holds,
//! read-only: the elements it inherits through that hold no current record are computed privately,
//! into an overlay kept until the next record could move.

use super::super::bridge::FfiRecordDemand;
use super::super::container_queries::ContainerVerdict;
use super::super::publication::{DemandedEngineRecord, RecordDemand, RecordDemandAnswer};
use super::super::*;
use super::live::Live;
use super::records::{
    ChildImpact, MatchList, PseudoElementRecords, PseudoElementTarget, Record, RecordCells, Records, RetainedRecords,
    highlights_moved,
};
use super::rule_changes::RuleIndexChanges;
use super::rules::RuleIndex;
use super::traversal::{self, DemandedPseudoElement, PassContext, PseudoElementRead};
use crate::css::style::input_types::StyleEnvironment;
use std::sync::Arc;

/// The records computed on demand in one generation of the engine.
#[derive(Default)]
pub(super) struct DemandOverlay {
    generation: u64,
    /// The records of the passes, with the ones computed on demand over them, where any was.
    records: Option<RetainedRecords>,
    /// The elements whose records were computed on demand.
    computed: HashSet<StyleNodeID>,
}

/// What an element kept styled below one that is not displayed was computed from.
struct KeptComputation {
    /// The parts of the environment it read.
    environment_reads: StyleEnvironment,
    /// The entries of the rule index it matched.
    matches: Option<MatchList>,
    /// What it computed, which what inherits through it computed from.
    record: Arc<Record>,
    /// The records of its highlight pseudo-elements beside `record`, which those of what inherits
    /// through it computed from.
    highlights: PseudoElementRecords,
}

/// Which records of the elements that keep their style below an element that is not displayed
/// still stand. No pass styles these elements or notes what they read, so an installation computes
/// them on demand, and nothing but what is noted here can move how they compute.
#[derive(Default)]
pub(in super::super) struct KeptHiddenRecords {
    /// The elements computed for an installation.
    computed: HashMap<StyleNodeID, KeptComputation>,
    /// The elements an input or a hint reached since the last installation, or whose children a
    /// pass found to compute from something else: what inherits through one computes anew, unless
    /// it comes out as it was.
    moved: HashSet<StyleNodeID>,
    /// Those of `moved` where a hint reached the elements below too, however it comes out.
    descendants_moved: HashSet<StyleNodeID>,
}

impl KeptHiddenRecords {
    /// Every element computes anew, as for a change to the rules.
    pub(super) fn forget(&mut self) {
        self.computed.clear();
    }

    pub(super) fn note_environment_moved(&mut self, environment: StyleEnvironment) {
        if !environment.is_empty() {
            self.moved.extend(
                self.computed
                    .iter()
                    .filter(|(_, computation)| computation.environment_reads.intersects(environment))
                    .map(|(&node, _)| node),
            );
        }
    }

    /// Only the elements that matched an entry that left, or that match one that arrived, compute
    /// anew where nothing but the conditions of rules moved, as the same width flips the rules of
    /// a media query.
    pub(super) fn note_rule_index_update(
        &mut self,
        state: &RetainedState,
        index: &RuleIndex,
        records: &RetainedRecords,
        changes: &RuleIndexChanges,
    ) {
        if !changes.conditions_moved_alone {
            self.forget();
            return;
        }
        if self.computed.is_empty() {
            return;
        }
        let numbering = changes.numbering(index);
        let mut moves_element = changes.moves_element(state, index, records);
        self.computed.retain(|&node, computation| {
            computation.matches.as_ref().is_some_and(|matches| {
                // NB: An element is computed anew rather than taking a list numbered anew.
                !moves_element(node, matches)
                    && matches
                        .iter()
                        .all(|&entry| numbering.get(entry as usize).copied().flatten() == Some(entry))
            })
        });
    }

    pub(super) fn note_moved(&mut self, nodes: impl IntoIterator<Item = StyleNodeID>) {
        // NB: An element below no computation has nothing to compute anew.
        if !self.computed.is_empty() {
            self.moved.extend(nodes);
        }
    }

    pub(super) fn note_descendants_moved(&mut self, node: StyleNodeID) {
        self.moved.insert(node);
        self.descendants_moved.insert(node);
    }

    /// Elements that left the tree compute nothing anymore.
    pub(super) fn note_left(&mut self, nodes: impl IntoIterator<Item = StyleNodeID>) {
        for node in nodes {
            self.computed.remove(&node);
        }
    }

    /// Whether `node` is one this holds a computation of.
    pub(super) fn computed(&self, node: StyleNodeID) -> bool {
        self.computed.contains_key(&node)
    }

    pub(super) fn holds_computations(&self) -> bool {
        !self.computed.is_empty()
    }

    /// The elements with a computation, for a check of what they computed.
    pub(super) fn computed_nodes(&self) -> impl Iterator<Item = (StyleNodeID, &Arc<Record>)> {
        self.computed
            .iter()
            .map(|(&node, computation)| (node, &computation.record))
    }

    /// Whether what `node` computes from stands, as `standing` remembers of the elements it
    /// inherits through.
    pub(super) fn stands(
        &self,
        state: &RetainedState,
        records: &RetainedRecords,
        node: StyleNodeID,
        standing: &mut HashMap<StyleNodeID, bool>,
    ) -> bool {
        if !self.computed.contains_key(&node) {
            return false;
        }
        let mut walked: SmallVec<[StyleNodeID; 32]> = SmallVec::new();
        let mut current = Some(node);
        let mut stands = true;
        while let Some(element) = current {
            if let Some(&known) = standing.get(&element) {
                stands = known;
                break;
            }
            walked.push(element);
            // NB: The sampled values of animations move with no input.
            if self.moved.contains(&element)
                || state.projection.adjustment_facts(element) & bridge::element_adjustment_fact::HAS_ANIMATIONS != 0
                || !(self.computed.contains_key(&element) || records.get(element).is_some())
            {
                stands = false;
                break;
            }
            current = state.projection.inheritance_parent(element);
        }
        standing.extend(walked.into_iter().map(|element| (element, stands)));
        stands
    }
}

/// What a computation of an element's style for a clock frame found: its record, with those of the
/// pseudo-elements it generates a box for.
enum DerivedElement {
    Styled(Arc<Record>, PseudoElementRecords),
    WaitingOnFonts(Vec<font_resolution::FontRequest>),
}

/// What a demand for an element's style found.
enum DemandedElement {
    Styled(Arc<Record>),
    /// Its style waits on the fonts and random base values it asks for.
    Waiting(
        Vec<font_resolution::FontRequest>,
        Vec<super::super::random_bases::RandomBaseRequest>,
    ),
}

impl Live {
    /// What the computation kept of each element below one that is not displayed differs in from
    /// what computing it now finds, one line per element.
    pub(super) fn kept_hidden_differences(
        &self,
        state: &RetainedState,
        describe: &dyn Fn(StyleNodeID) -> String,
    ) -> Vec<String> {
        let mut differences = Vec::new();
        let mut standing = HashMap::default();
        for (node, kept) in self.kept_hidden_records.computed_nodes() {
            if !state.projection.is_live(node)
                || !self
                    .kept_hidden_records
                    .stands(state, &self.records, node, &mut standing)
            {
                continue;
            }
            let mut chain: SmallVec<[StyleNodeID; 32]> =
                std::iter::successors(Some(node), |&node| state.projection.inheritance_parent(node)).collect();
            chain.reverse();
            // NB: The records held below an element that is not displayed are what they were as it stopped being
            //     displayed, and nothing computes from them.
            let mut parent_is_displayed = true;
            let Some(first) = chain.iter().position(|&element| {
                let record = self.records.get(element).filter(|_| parent_is_displayed);
                parent_is_displayed = record.is_some_and(|record| record.is_displayed());
                record.is_none()
            }) else {
                continue;
            };
            let mut cells = RecordCells::default();
            cells.reserve(state.projection.element_slot_count());
            let records = Records::new(state, &cells, Some(&self.records));
            let context = PassContext::new(
                state,
                &self.index,
                &self.initial,
                &records,
                traversal::held_root_font(state, &records, chain[0]),
            );
            let statistics = traversal::style_chain(&context, &chain[first..]);
            if !statistics.font_requests.is_empty() || !statistics.random_requests.is_empty() {
                continue;
            }
            if let Some(fresh) = records.get_shared(node) {
                let moved = super::self_check::record_differences(kept, fresh);
                if !moved.is_empty() {
                    differences.push(format!(
                        "the kept computation of {}: {}",
                        describe(node),
                        moved.join(", ")
                    ));
                }
            }
        }
        differences
    }

    /// The records computed on demand, for the current generation.
    fn demand_overlay(&mut self) -> &mut DemandOverlay {
        if self.demanded.generation != self.generation || self.demanded.records.is_none() {
            *self.demanded = DemandOverlay {
                generation: self.generation,
                records: None,
                computed: HashSet::default(),
            };
        }
        &mut self.demanded
    }

    /// Whether the record of `node` is one computed on demand in the current generation.
    fn computed_on_demand(&self, node: StyleNodeID) -> bool {
        self.demanded.generation == self.generation && self.demanded.computed.contains(&node)
    }

    /// The records a read takes: those computed on demand in the current generation over the
    /// passes'.
    fn demand_records(&self) -> &RetainedRecords {
        match &self.demanded.records {
            Some(records) if self.demanded.generation == self.generation => records,
            _ => &self.records,
        }
    }

    /// The current record of `node`, where it has one: the one a pass computed where the elements
    /// it inherits through are displayed, or the one computed on demand.
    pub(super) fn demanded_record(&self, node: StyleNodeID) -> Option<&Arc<Record>> {
        match self.computed_on_demand(node) {
            true => self.demand_records().get(node),
            false => self.records.get(node),
        }
    }

    /// The records of the pseudo-elements of `node` that generate a box, by target, beside its
    /// current record.
    pub(super) fn demanded_pseudo_element_records(&self, node: StyleNodeID) -> &[(PseudoElementTarget, Arc<Record>)] {
        match self.computed_on_demand(node) {
            true => self.demand_records().pseudo_element_records(node),
            false => self.records.pseudo_element_records(node),
        }
    }

    /// The records of the highlight pseudo-elements of `node`, beside its current record.
    fn demanded_highlights(&self, node: StyleNodeID) -> PseudoElementRecords {
        self.demanded_pseudo_element_records(node)
            .iter()
            .filter(|(target, _)| super::super::publication::pseudo_kind::is_highlight(target.kind))
            .cloned()
            .collect()
    }

    /// What the container conditions of the current record of `node` read of its containers.
    pub(super) fn demanded_container_reads(&self, node: StyleNodeID) -> Option<&Arc<ContainerVerdict>> {
        match self.computed_on_demand(node) {
            true => self.demand_records().container_reads(node),
            false => self.records.container_reads(node),
        }
    }

    /// The elements `node` inherits through, the topmost first, and the index of the first of them
    /// whose record is not current. A pass keeps current the record of every element whose
    /// ancestors are displayed, and computes none for the descendants of one that is not. No pass
    /// samples the animations of an element that is not displayed, so the record of one whose
    /// animations a read sampled is not current either, as it could read the custom properties they
    /// animate.
    pub(super) fn demand_chain(
        &self,
        state: &RetainedState,
        node: StyleNodeID,
    ) -> (SmallVec<[StyleNodeID; 32]>, Option<usize>) {
        let mut chain: SmallVec<[StyleNodeID; 32]> =
            std::iter::successors(Some(node), |&node| state.projection.inheritance_parent(node)).collect();
        chain.reverse();
        let mut parent_kept_current = true;
        for (index, &element) in chain.iter().enumerate() {
            match self.records.get(element) {
                Some(record)
                    if parent_kept_current
                        && (record.is_displayed()
                            || state
                                .element_custom_property_data
                                .get(&element)
                                .is_none_or(|held| held.sampled_over.is_none())) =>
                {
                    parent_kept_current = record.is_displayed();
                }
                _ if self.computed_on_demand(element) => parent_kept_current = false,
                _ => return (chain, Some(index)),
            }
        }
        (chain, None)
    }

    /// Compute the record of `node` where it holds no current one, with those of the elements it
    /// inherits through that hold none, keeping them for the generation. Where `fresh` asks, the
    /// record of `node` itself is computed anew whatever it holds, and kept by no one, as for a
    /// read of a style its own declarations do not all take part in.
    fn demand_element(&mut self, state: &RetainedState, node: StyleNodeID, fresh: bool) -> DemandedElement {
        let (chain, first_stale) = self.demand_chain(state, node);
        let first_stale = match (first_stale, fresh) {
            (Some(first_stale), _) => first_stale,
            (None, true) => chain.len() - 1,
            (None, false) => {
                return match self.demanded_record(node) {
                    Some(record) => DemandedElement::Styled(record.clone()),
                    None => unreachable!("the style engine answers every row"),
                };
            }
        };
        let base = self.demand_records().clone();
        let mut cells = RecordCells::default();
        cells.reserve(state.projection.element_slot_count());
        let records = Records::new(state, &cells, Some(&base));
        let context = PassContext {
            cache_audit: self.cache_audit,
            // NB: What an element kept below one that is not displayed matched tells whether a change
            //     to the rules reaches it.
            keeps_matches: true,
            ..PassContext::new(
                state,
                &self.index,
                &self.initial,
                &records,
                traversal::held_root_font(state, &records, chain[0]),
            )
        };
        let statistics = traversal::style_chain(&context, &chain[first_stale..]);
        let demanded = records.get_shared(node).cloned();
        if !statistics.font_requests.is_empty() || !statistics.random_requests.is_empty() {
            return DemandedElement::Waiting(statistics.font_requests, statistics.random_requests);
        }
        let Some(demanded) = demanded else {
            unreachable!("the style engine answers every row");
        };
        // The record of a fresh read is the read's alone; the elements above it are kept.
        let kept: SmallVec<[StyleNodeID; 32]> = statistics
            .styled
            .iter()
            .copied()
            .filter(|&styled| !fresh || styled != node)
            .collect();
        let overlay = self.demand_overlay();
        let mut records = overlay.records.take().unwrap_or(base);
        records.absorb(&mut cells, &kept, None, &mut Default::default());
        overlay.records = Some(records);
        overlay.computed.extend(kept);
        DemandedElement::Styled(demanded)
    }

    /// Compute the record of `node` anew for a box a clock frame builds, against what the engine
    /// reads now, such as the sizes the frame laid its containers out at: over `parent`, the record
    /// the frame shows for the element it inherits from, where it shows one, and otherwise over the
    /// current records of the elements it inherits through, those holding none computed with it.
    /// The records are the frame's alone: nothing keeps them.
    fn derive_element(&self, state: &RetainedState, node: StyleNodeID, parent: Option<&Arc<Record>>) -> DerivedElement {
        let (chain, first_stale) = self.demand_chain(state, node);
        let first = match (parent, first_stale) {
            (None, Some(first_stale)) => first_stale,
            _ => chain.len() - 1,
        };
        let mut cells = RecordCells::default();
        cells.reserve(state.projection.element_slot_count());
        let records = Records::new(state, &cells, Some(self.demand_records()));
        if let Some(parent) = parent {
            let inheritance_parent = chain[chain.len() - 2];
            records.insert(inheritance_parent, parent.clone(), inheritance_parent, None);
        }
        let context = PassContext {
            cache_audit: self.cache_audit,
            ..PassContext::new(
                state,
                &self.index,
                &self.initial,
                &records,
                traversal::held_root_font(state, &records, chain[0]),
            )
        };
        let statistics = traversal::style_chain(&context, &chain[first..]);
        if !statistics.font_requests.is_empty() {
            return DerivedElement::WaitingOnFonts(statistics.font_requests);
        }
        match records.get_shared(node) {
            Some(record) => {
                let pseudo_element_records = records.pseudo_element_records(node);
                DerivedElement::Styled(
                    record.with_pseudo_element_styles(super::records::PseudoElementStyles::of(pseudo_element_records)),
                    pseudo_element_records.into(),
                )
            }
            None => unreachable!("the style engine answers every row"),
        }
    }

    /// Style the pseudo-element of `kind` of `node`, whose current record was demanded before, as
    /// `read` says.
    fn demand_pseudo_element(
        &self,
        state: &RetainedState,
        node: StyleNodeID,
        target: PseudoElementTarget,
        read: PseudoElementRead,
        inheritance_parent: Option<StyleNodeID>,
    ) -> DemandedPseudoElement {
        // https://drafts.csswg.org/css-pseudo-4/#highlight-cascade
        // A highlight pseudo-element inherits from the one of its kind of the nearest ancestor that
        // has one. The passes style those only while the host shows a highlight of the kind, and
        // then only for the elements it shows it in, so those of the ancestors are styled for the
        // read too.
        let styles_ancestor_highlights = super::super::publication::pseudo_kind::is_highlight(target.kind);
        let mut cells = RecordCells::default();
        if styles_ancestor_highlights {
            cells.reserve(state.projection.element_slot_count());
        }
        let records = Records::new(state, &cells, Some(self.demand_records()));
        let context = PassContext {
            cache_audit: self.cache_audit,
            ..PassContext::new(
                state,
                &self.index,
                &self.initial,
                &records,
                traversal::held_root_font(
                    state,
                    &records,
                    std::iter::successors(Some(node), |&node| state.projection.inheritance_parent(node))
                        .last()
                        .expect("an element is its own topmost ancestor"),
                ),
            )
        };
        if styles_ancestor_highlights {
            let ancestors: SmallVec<[StyleNodeID; 32]> =
                std::iter::successors(state.projection.inheritance_parent(node), |&node| {
                    state.projection.inheritance_parent(node)
                })
                .collect();
            for &ancestor in ancestors.iter().rev() {
                let mut held: PseudoElementRecords = records.pseudo_element_records(ancestor).into();
                held.retain(|(held_target, _)| *held_target != target);
                match traversal::style_pseudo_element_inheriting_from(
                    &context,
                    ancestor,
                    target,
                    PseudoElementRead::Styled,
                    None,
                ) {
                    DemandedPseudoElement::Styled(record) => held.push((target, record)),
                    DemandedPseudoElement::Unstyled => {}
                    demanded @ (DemandedPseudoElement::Waiting(..) | DemandedPseudoElement::Declined) => {
                        return demanded;
                    }
                }
                records.insert_pseudo_element_records(ancestor, held);
            }
        }
        traversal::style_pseudo_element_inheriting_from(&context, node, target, read, inheritance_parent)
    }
}

impl StyleEngine {
    /// Answer a read of `node`'s style, or of one of its pseudo-elements', for a document live style
    /// the engine owns, from the records it holds: see the module comment. The record it answers
    /// is the host's to read alone.
    pub(in super::super) fn answer_live_record_demand(
        &mut self,
        node: StyleNodeID,
        demand: RecordDemand,
    ) -> RecordDemandAnswer {
        assert!(
            self.retained.record_demand_reads_current_inputs(node, &self.host),
            "a read runs style updates until none is pending, so no input is pending as the engine answers it"
        );
        // Only a private read of an element leaves its inline style out: the record it answers is
        // no element's.
        let without_inline_style = matches!(
            demand,
            RecordDemand::Element(FfiRecordDemand::ElementReadWithoutInlineStyle)
        );
        let hidden_inline_declarations = match without_inline_style {
            true => self.retained.element_declarations.hide_inline_for_demand(node),
            false => None,
        };
        let record = self.demand_element(node, without_inline_style);
        if let Some(hidden) = hidden_inline_declarations {
            self.retained
                .element_declarations
                .restore_inline_after_demand(node, hidden);
        }
        match demand {
            RecordDemand::Element(_) => {
                let answered = self.install_demanded_element(node, &record);
                RecordDemandAnswer::Record { record: answered }
            }
            RecordDemand::PseudoElement(_, pseudo_element, highlight_name) => {
                let target = PseudoElementTarget {
                    kind: pseudo_element as u8,
                    highlight_name,
                };
                self.answer_pseudo_element_demand(node, &record, target, demand.is_read_only())
            }
        }
    }

    /// Compute on demand the records of the elements that keep their style below an element that
    /// is not displayed, the SVG elements content can reference and the elements with animations,
    /// and answer the elements whose records the installation installs: each one with the elements
    /// it inherits through below that one. The passes style the others.
    pub(in super::super) fn demand_kept_hidden(&mut self) -> Vec<StyleNodeID> {
        let Some(live) = self.retained.live_style.as_mut() else {
            return Vec::new();
        };
        let mut kept = std::mem::take(&mut live.kept_hidden);
        kept.sort_unstable();
        kept.dedup();
        live.note_hints_moved_kept_hidden_records();
        let mut installed = self.settle_moved_kept_hidden();
        let mut standing = HashMap::default();
        for node in kept {
            if !self.retained.projection.is_live(node) || !self.retained.keeps_style_in_display_none_subtree(node) {
                continue;
            }
            let mut live = self.retained.live_style.take().expect("the engine owns the document");
            live.kept_hidden.push(node);
            let stands = live
                .kept_hidden_records
                .stands(&self.retained, &live.records, node, &mut standing);
            self.retained.live_style = Some(live);
            if !stands {
                installed.extend(self.demand_kept_chain(node));
            }
        }
        if let Some(live) = self.retained.live_style.as_mut() {
            live.kept_hidden_records.moved.clear();
            live.kept_hidden_records.descendants_moved.clear();
        }
        installed.sort_unstable();
        installed.dedup();
        installed
    }

    /// Compute the elements an input reached anew, and let those whose children compute from what
    /// they did stand for what inherits through them, as a viewport change reaches an element that
    /// reads the viewport for a property its children do not inherit. Answers the elements to
    /// install.
    fn settle_moved_kept_hidden(&mut self) -> Vec<StyleNodeID> {
        let live = self.retained.live_style.as_ref().expect("the engine owns the document");
        let kept = &live.kept_hidden_records;
        // NB: The chain of one element can compute another of them, so what each held is taken first.
        let mut reached: Vec<(StyleNodeID, Arc<Record>, PseudoElementRecords)> = kept
            .moved
            .iter()
            .filter(|node| !kept.descendants_moved.contains(node) && self.retained.projection.is_live(**node))
            .filter_map(|&node| {
                let computation = kept.computed.get(&node)?;
                Some((node, computation.record.clone(), computation.highlights.clone()))
            })
            .collect();
        reached.sort_unstable_by_key(|&(node, ..)| node);
        let mut installed = Vec::new();
        for (node, held, held_highlights) in reached {
            let mut demanded = self.demand_kept_chain(node);
            let state = &self.retained;
            let live = state.live_style.as_ref().expect("the engine owns the document");
            let kept = &live.kept_hidden_records;
            // NB: An element a pass styles now holds its record, not one computed on demand.
            let current = live.demanded_record(node).filter(|_| kept.computed(node)).cloned();
            let highlights = live.demanded_highlights(node);
            let holds_the_same = current.as_ref().is_some_and(|record| record.holds_the_same_as(&held));
            let children_stand = current.as_ref().is_some_and(|record| {
                record.pseudo_element_styles() == held.pseudo_element_styles()
                    && !highlights_moved(&highlights, &held_highlights)
                    && match state.child_impact_of_records(node, record, &held) {
                        ChildImpact::None => true,
                        ChildImpact::ResetReaders => state.projection.style_children(node).all(|child| {
                            let record = kept.computed.get(&child).map(|computation| &computation.record);
                            record
                                .or_else(|| live.records.get(child))
                                .is_none_or(|record| !state.reads_parent_reset_values(child, record))
                        }),
                        _ => false,
                    }
            });
            let live = self.retained.live_style.as_mut().expect("the engine owns the document");
            if let (Some(record), Some(computation)) = (current, live.kept_hidden_records.computed.get_mut(&node)) {
                computation.record = record;
                computation.highlights = highlights;
            }
            if children_stand {
                live.kept_hidden_records.moved.remove(&node);
            }
            if holds_the_same {
                demanded.retain(|&element| element != node);
            }
            installed.append(&mut demanded);
        }
        installed
    }

    /// Compute the elements of the chain of `node` that hold no current record, and note what they
    /// computed from. Answers them.
    fn demand_kept_chain(&mut self, node: StyleNodeID) -> Vec<StyleNodeID> {
        let live = self.retained.live_style.as_ref().expect("the engine owns the document");
        let (chain, first_stale) = live.demand_chain(&self.retained, node);
        let Some(first_stale) = first_stale else {
            return Vec::new();
        };
        self.demand_element(node, false);
        let live = self.retained.live_style.as_mut().expect("the engine owns the document");
        for &element in &chain[first_stale..] {
            let computation = live.demanded_record(element).and_then(|record| {
                // NB: The sizes of containers and the places of elements among their siblings move with no
                //     input an element holding no record is told of.
                let reads_container = live.demand_records().container_reads(element).is_some();
                (!reads_container && !record.rare().reads.reads_sibling_position).then(|| KeptComputation {
                    environment_reads: record.rare().reads.environment_reads,
                    matches: live.demand_records().matches_of(element).cloned(),
                    record: record.clone(),
                    highlights: live.demanded_highlights(element),
                })
            });
            match computation {
                Some(computation) => live.kept_hidden_records.computed.insert(element, computation),
                None => live.kept_hidden_records.computed.remove(&element),
            };
        }
        chain[first_stale..].to_vec()
    }

    /// The installation of the current record of `node`, computed on demand where it holds none, for a read of its
    /// style, which installs it as a style update does. No transaction lies behind it, and the render side leaves the
    /// element's boxes to the host. None for a document with no live style.
    pub(in super::super) fn demand_installation(
        &mut self,
        node: StyleNodeID,
    ) -> Option<super::install::StyleInstallation> {
        // NB: A document that is never styled, as the one holding a template's contents, has no live style.
        self.retained.live_style.as_ref()?;
        assert!(
            self.retained.record_demand_reads_current_inputs(node, &self.host),
            "a read runs style updates until none is pending, so no input is pending as the engine answers it"
        );
        let demanded = self.demand_element(node, false);
        let projection = &self.retained.projection;
        let root = std::iter::successors(Some(node), |&node| projection.inheritance_parent(node))
            .last()
            .expect("an element is its own chain's start");
        let mut live = self.retained.live_style.take().expect("the engine owns the document");
        if demanded.facts().in_display_none_subtree() {
            live.installed_as_undisplayed.insert(node);
        }
        // NB: The host takes a custom property environment over the one the element it inherits from holds, and the
        //     elements above it nothing read since they moved hold the one they held then.
        let sets = &self.retained.computed_group_sets;
        let stale_above: SmallVec<[StyleNodeID; 8]> =
            std::iter::successors(projection.inheritance_parent(node), |&parent| {
                projection.inheritance_parent(parent)
            })
            .filter(|&above| {
                live.computed_on_demand(above)
                    && live.demanded_record(above).is_some_and(|demanded| {
                        sets.assigned_style_record(above)
                            .and_then(|record| sets.installed_base_record(record))
                            .is_none_or(|installed| !Arc::ptr_eq(installed, demanded))
                    })
            })
            .collect();
        let mut scratch = live.begin_installation();
        let mut installation = super::install::StyleInstallation::default();
        self.retained.installation_rows(
            root,
            &live,
            stale_above.into_iter().chain([node]),
            &[],
            &[],
            &mut scratch,
            &mut installation,
        );
        live.end_installation(scratch);
        self.retained.live_style = Some(live);
        installation.leave_boxes_to_the_host();
        installation.note_held_by_the_host(&mut self.retained);
        self.consume_element_style_input(node);
        Some(installation)
    }

    /// The current record of `node`, computed on demand where it holds none.
    fn demand_element(&mut self, node: StyleNodeID, fresh: bool) -> Arc<Record> {
        let demanded = self.until_resolved(node, |engine| {
            let mut live = engine.retained.live_style.take().expect("the engine owns the document");
            let demanded = live.demand_element(&engine.retained, node, fresh);
            engine.retained.live_style = Some(live);
            match demanded {
                DemandedElement::Styled(record) => Ok(record),
                DemandedElement::Waiting(fonts, random_bases) => Err(super::Waits { fonts, random_bases }),
            }
        });
        match demanded {
            Some(record) => record,
            None => unreachable!("the style engine answers every row"),
        }
    }

    /// The custom property environment of the record `record` of `node`, resolved over those of the
    /// elements it inherits through: the ones they were installed with, or, for those whose
    /// records were computed on demand, the ones those resolve to.
    fn demanded_environment(
        &mut self,
        live: &Live,
        node: StyleNodeID,
        record: &Record,
        minted: &mut super::install_scratch::MintedEnvironments,
    ) -> u64 {
        let parent = self.retained.projection.inheritance_parent(node);
        let parent_record = parent.and_then(|parent| live.demanded_record(parent)).cloned();
        let parent_environment = match (parent, &parent_record) {
            (Some(parent), Some(parent_record)) => match live.computed_on_demand(parent) {
                false => self
                    .retained
                    .computed_group_sets
                    .assigned_style_record(parent)
                    .and_then(|assigned| {
                        self.retained
                            .computed_group_sets
                            .style_record_custom_property_environment(assigned.raw())
                    })
                    .unwrap_or(0),
                true => self.demanded_environment(live, parent, parent_record, minted),
            },
            _ => 0,
        };
        self.retained
            .record_environment(node, record, parent_record.as_deref(), parent_environment, minted)
    }

    /// The record the host reads for `node`, its current `record`, with those of the pseudo-elements it generates a box
    /// for.
    fn install_demanded_element(&mut self, node: StyleNodeID, record: &Arc<Record>) -> DemandedEngineRecord {
        let live = self.retained.live_style.take().expect("the engine owns the document");
        let mut minted = HashMap::default();
        let environment = self.demanded_environment(&live, node, record, &mut minted);
        let pseudo_element_records: PseudoElementRecords = match live.demanded_record(node) {
            Some(current) if Arc::ptr_eq(current, record) => live.demanded_pseudo_element_records(node).into(),
            _ => PseudoElementRecords::new(),
        };
        self.retained.live_style = Some(live);
        self.install_demanded_records(node, record, &pseudo_element_records, environment, &mut minted)
    }

    /// Install `record`, the record of `node` resolving its custom properties to `environment`, and
    /// `pseudo_element_records` beside it, for the host to read.
    fn install_demanded_records(
        &mut self,
        node: StyleNodeID,
        record: &Arc<Record>,
        pseudo_element_records: &PseudoElementRecords,
        environment: u64,
        minted: &mut super::install_scratch::MintedEnvironments,
    ) -> DemandedEngineRecord {
        #[expect(
            clippy::mutable_key_type,
            reason = "a key hashes and compares only the address it holds"
        )]
        let mut installed = HashMap::default();
        let record = &self
            .retained
            .record_in_environment(record, environment)
            .with_pseudo_element_styles(super::records::PseudoElementStyles::of(pseudo_element_records));
        let style_record = self.retained.install_record(record, &mut installed);
        self.retained
            .hold_demand_record(computed::ComputedStyleTarget::new(node, u8::MAX), style_record);
        let mut answered = DemandedEngineRecord {
            style_record: style_record.raw(),
            ..DemandedEngineRecord::default()
        };
        for (target, pseudo_record) in pseudo_element_records
            .iter()
            .filter(|(target, _)| !target.is_custom_highlight())
        {
            let kind = target.kind;
            let pseudo_environment =
                self.retained
                    .pseudo_element_environment(node, pseudo_record, record, environment, minted);
            let pseudo_record = &self.retained.record_in_environment(pseudo_record, pseudo_environment);
            let pseudo_style_record = self.retained.install_record(pseudo_record, &mut installed);
            self.retained
                .hold_demand_record(computed::ComputedStyleTarget::new(node, kind), pseudo_style_record);
            answered.pseudo_records_present |= 1 << kind;
            answered.pseudo_records[usize::from(kind)] = pseudo_style_record.raw();
        }
        answered
    }

    /// The record a clock frame shows for `node` in a box it builds, computed anew against what the
    /// engine reads now, over the record the frame shows for the element it inherits from where it
    /// shows one (see [`Live::derive_element`]), with those of the pseudo-elements it generates a box
    /// for. The host installs none of them: they are held for the frame to read.
    pub(in super::super) fn derive_for_clock(&mut self, node: StyleNodeID) -> DemandedEngineRecord {
        if self.retained.live_style.is_none() || !self.retained.record_demand_reads_current_inputs(node, &self.host) {
            unreachable!("the style engine answers every row");
        }
        let shown_parent = self
            .retained
            .projection
            .inheritance_parent(node)
            .and_then(|parent| self.tick_shown.element(parent));
        let parent = match shown_parent {
            Some(shown) => Some(
                self.retained
                    .computed_group_sets
                    .installed_record(shown)
                    .cloned()
                    .expect("the style engine answers every row"),
            ),
            None => None,
        };
        let derived = self.until_resolved(node, |engine| {
            let live = engine.retained.live_style.take().expect("the engine owns the document");
            let derived = live.derive_element(&engine.retained, node, parent.as_ref());
            engine.retained.live_style = Some(live);
            match derived {
                DerivedElement::WaitingOnFonts(fonts) => Err(super::Waits {
                    fonts,
                    random_bases: Vec::new(),
                }),
                derived => Ok(derived),
            }
        });
        let Some(DerivedElement::Styled(record, pseudo_element_records)) = derived else {
            unreachable!("the style engine answers every row");
        };
        let mut minted = HashMap::default();
        let environment = match shown_parent {
            Some(shown) => {
                let parent_environment = self
                    .retained
                    .computed_group_sets
                    .style_record_custom_property_environment(shown.raw())
                    .unwrap_or(0);
                self.retained
                    .record_environment(node, &record, parent.as_deref(), parent_environment, &mut minted)
            }
            None => {
                let live = self.retained.live_style.take().expect("the engine owns the document");
                let environment = self.demanded_environment(&live, node, &record, &mut minted);
                self.retained.live_style = Some(live);
                environment
            }
        };
        self.install_demanded_records(node, &record, &pseudo_element_records, environment, &mut minted)
    }

    /// Answer a read of the pseudo-element `target` of `node`, whose current record is `element`:
    /// the record of a pseudo-element of a kind the passes style that generates a box, and for any other
    /// read the style the kind computes to from the element's rules for it, whether or not it
    /// generates a box. A `read_only` read styles any kind; another one a kind no rule styles not,
    /// which is absent.
    fn answer_pseudo_element_demand(
        &mut self,
        node: StyleNodeID,
        element: &Arc<Record>,
        target: PseudoElementTarget,
        read_only: bool,
    ) -> RecordDemandAnswer {
        let live = self.retained.live_style.take().expect("the engine owns the document");
        // The passes keep the highlight pseudo-elements current only where the host shows them.
        let generated = (super::PSEUDO_ELEMENT_KINDS.contains(&target.kind)
            && !super::HIGHLIGHT_KINDS.contains(&target.kind))
        .then(|| {
            live.demanded_pseudo_element_records(node)
                .iter()
                .find(|(held, _)| *held == target)
                .map(|(_, record)| record.clone())
        })
        .flatten();
        let mut minted = HashMap::default();
        let element_environment = self.demanded_environment(&live, node, element, &mut minted);
        self.retained.live_style = Some(live);
        let demanded = match generated {
            Some(record) => DemandedPseudoElement::Styled(record),
            None => {
                let read = match read_only {
                    true => PseudoElementRead::Any,
                    false => PseudoElementRead::Styled,
                };
                self.demand_pseudo_element(node, target, read, None)
            }
        };
        let record = match demanded {
            DemandedPseudoElement::Styled(record) => record,
            DemandedPseudoElement::Unstyled => {
                return RecordDemandAnswer::Absent {
                    custom_property_environment: 0,
                };
            }
            DemandedPseudoElement::Waiting(..) | DemandedPseudoElement::Declined => {
                unreachable!("the style engine answers every row");
            }
        };
        let pseudo_environment =
            self.retained
                .pseudo_element_environment(node, &record, element, element_environment, &mut minted);
        let record = self.retained.record_in_environment(&record, pseudo_environment);
        let style_record = self.retained.install_record(&record, &mut HashMap::default());
        self.retained
            .hold_demand_record(computed::ComputedStyleTarget::new(node, target.kind), style_record);
        RecordDemandAnswer::Record {
            record: DemandedEngineRecord {
                style_record: style_record.raw(),
                ..DemandedEngineRecord::default()
            },
        }
    }

    /// The record of the `::selection` of `element`, or zero where no rule styles it, for the fork of a clock lane, whose
    /// drag reaches an element the host shows no selection in. Answers none where what it takes to style it is the
    /// host's to resolve, as a font is.
    pub(crate) fn demand_selection_record(&mut self, element: StyleNodeID) -> Option<u64> {
        let mut live = self.retained.live_style.take()?;
        let record = self.selection_record_with(&mut live, element);
        self.retained.live_style = Some(live);
        record
    }

    fn selection_record_with(&mut self, live: &mut Live, element: StyleNodeID) -> Option<u64> {
        let kind = super::super::publication::pseudo_kind::SELECTION;
        let DemandedElement::Styled(element_record) = live.demand_element(&self.retained, element, false) else {
            return None;
        };
        let record = match live.demand_pseudo_element(
            &self.retained,
            element,
            PseudoElementTarget::of_kind(kind),
            PseudoElementRead::Styled,
            None,
        ) {
            DemandedPseudoElement::Styled(record) => record,
            DemandedPseudoElement::Unstyled => return Some(0),
            DemandedPseudoElement::Waiting(..) | DemandedPseudoElement::Declined => return None,
        };
        let mut minted = HashMap::default();
        let element_environment = self.demanded_environment(live, element, &element_record, &mut minted);
        let environment = self.retained.pseudo_element_environment(
            element,
            &record,
            &element_record,
            element_environment,
            &mut minted,
        );
        let record = self.retained.record_in_environment(&record, environment);
        let style_record = self.retained.install_record(&record, &mut HashMap::default());
        self.retained
            .hold_demand_record(computed::ComputedStyleTarget::new(element, kind), style_record);
        Some(style_record.raw())
    }

    /// Style the pseudo-element `target` of `node` as [`Live::demand_pseudo_element`] does,
    /// resolving the fonts and random base values it asks for first.
    fn demand_pseudo_element(
        &mut self,
        node: StyleNodeID,
        target: PseudoElementTarget,
        read: PseudoElementRead,
        inheritance_parent: Option<StyleNodeID>,
    ) -> DemandedPseudoElement {
        self.until_resolved(node, |engine| {
            let live = engine.retained.live_style.take().expect("the engine owns the document");
            let demanded = live.demand_pseudo_element(&engine.retained, node, target, read, inheritance_parent);
            engine.retained.live_style = Some(live);
            match demanded {
                DemandedPseudoElement::Waiting(fonts, random_bases) => Err(super::Waits { fonts, random_bases }),
                demanded => Ok(demanded),
            }
        })
        .unwrap_or(DemandedPseudoElement::Declined)
    }

    /// The record of the `::first-letter` of `node` whose letter is in `text`, computed against
    /// the element containing the letter, where that is not `node`.
    pub(crate) fn first_letter_record_inheriting_from_text_parent(
        &mut self,
        node: StyleNodeID,
        text: StyleNodeID,
    ) -> Option<u64> {
        use super::super::publication::pseudo_kind;
        let parent = self.retained.first_letter_inheritance_parent(node, text)?;
        self.retained.live_style.as_ref()?;
        let kind = pseudo_kind::FIRST_LETTER;
        let DemandedPseudoElement::Styled(record) = self.demand_pseudo_element(
            node,
            PseudoElementTarget::of_kind(kind),
            PseudoElementRead::Styled,
            Some(parent),
        ) else {
            return None;
        };
        let parent_record = self.retained.computed_group_sets.assigned_style_record(parent)?;
        let parent_record = self
            .retained
            .computed_group_sets
            .installed_base_record(parent_record)?
            .clone();
        let parent_environment = self
            .retained
            .computed_group_sets
            .custom_property_environment_identity(parent)
            .unwrap_or(0);
        let mut minted = HashMap::default();
        let pseudo_environment =
            self.retained
                .pseudo_element_environment(parent, &record, &parent_record, parent_environment, &mut minted);
        let record = self.retained.record_in_environment(&record, pseudo_environment);
        let style_record = self.retained.install_record(&record, &mut HashMap::default());
        self.retained
            .hold_demand_record(computed::ComputedStyleTarget::new(node, kind), style_record);
        Some(style_record.raw())
    }
}

impl RetainedState {
    // https://drafts.csswg.org/css-pseudo-4/#first-letter-styling
    // NB: A `::first-letter` inherits from the element containing the letter, which is the
    //     originating element's descendant when the letter is inside one.
    pub(in crate::css::style) fn first_letter_inheritance_parent(
        &self,
        node: StyleNodeID,
        text: StyleNodeID,
    ) -> Option<StyleNodeID> {
        let parent = match self.assigned_slot_of(text) {
            Some(slot) => slot,
            None => {
                let parent = self.text_parent(text)?;
                self.host_of(parent).unwrap_or(parent)
            }
        };
        (parent != node).then_some(parent)
    }

    /// The installed record and custom-property environment of the element the `::first-letter`
    /// of `node` whose letter is in `text` inherits from, where that is not `node`.
    pub(crate) fn first_letter_inheritance_parent_style(
        &self,
        node: StyleNodeID,
        text: StyleNodeID,
    ) -> Option<(u64, u64)> {
        let parent = self.first_letter_inheritance_parent(node, text)?;
        let sets = &self.computed_group_sets;
        Some((
            sets.assigned_style_record(parent)?.raw(),
            sets.custom_property_environment_identity(parent).unwrap_or(0),
        ))
    }
}
