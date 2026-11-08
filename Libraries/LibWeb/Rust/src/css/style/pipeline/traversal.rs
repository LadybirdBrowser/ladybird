/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The style traversal: every element of the flat tree, shadow trees included, is matched,
//! cascaded and computed once the parent it inherits from is, on a pool of threads.
//!
//! The calling thread styles the document breadth-first from its root. Once a level of the tree is
//! wide enough, it keeps a queue of elements for itself and hands the rest out in work units, which
//! the threads of the pool style the same way; a small or narrow document never leaves the calling
//! thread.
//! Every thread keeps its own matcher, cascade scratch and ancestor filter, which it reconciles with
//! the ancestor chain of each element's parent in its own tree as it moves between parents.

use super::super::container_queries::{ContainerRecords, ContainerVerdict};
use super::super::custom_property_cascade::{
    LonghandCustomPropertyReads, PreparedCustomFunctions, value_reads_substitution_subject,
};
use super::super::random_bases::RandomBaseRequest;
use super::super::*;
use super::bloom::AncestorFilter;
use super::cascade::{Cascade, CascadeCounters};
use super::compute::{GroupSeeds, RootFontInputs};
use super::custom_properties::{DrivenWithRegisteredProperties, SubstitutingSubject, drive_with_registered_properties};
use super::drive::DriveRecords;
use super::matched_declarations::{
    self, Declared, MatchedConditions, MatchedContext, MatchedDeclarationsCache, MatchedKey, OwnDeclarationLookups,
};
use super::matching::{ElementMatch, MatchCounters, Matcher};
use super::records::{ChildImpact, RareStyleFacts, Record, Records};
use super::rule_changes::all_lose_every_longhand;
use super::rules::{ContainerGateID, RuleIndex};
use super::sharing::SharingCache;
#[cfg(test)]
use super::write_once::WriteOnce;
use crate::css::computed_values::{GroupSeed, GroupValueFacts, LonghandID};
use crate::css::style_compute::RootFontReads;
use smallvec::SmallVec;
use std::borrow::Cow;
use std::collections::VecDeque;
use std::sync::Mutex;

mod cache_audit;

#[cfg(test)]
pub(super) use cache_audit::CacheAudit;
pub(super) use cache_audit::mode_from_environment as cache_audit_mode_from_environment;

/// How many elements a task hands to another at once.
const WORK_UNIT: usize = 16;

/// How many elements the calling thread keeps queued for itself before it hands work to the pool.
const CALLING_THREAD_QUEUE_SIZE: usize = 32;

/// How many elements a pass finds to style before the pool's threads join in. A pass styling fewer
/// runs on the calling thread alone: starting the pool's threads costs more than they would save.
const PARALLEL_PASS_SIZE: usize = 64;

/// The computed initial values every element's style starts from.
pub(in crate::css::style) struct InitialStyle {
    /// Whether each initial value is important, inherited and evaluated, and what the box
    /// adjustments and the font computation settled.
    pub(super) flags: crate::css::computed_longhand_table::LonghandTableFlags,
    /// What the initial values depend on, as `DEPENDS_ON_VIEWPORT_METRICS` and the like.
    pub(super) dependency_flags: u8,
    /// The initial groups, holding the initial values, which every element not changing a group's
    /// values from them shares.
    pub(super) payloads: crate::css::computed_values::StyleGroups,
    /// The groups the documents of the process share, which `payloads` holds all but the font
    /// group of.
    _shared: std::sync::Arc<super::super::process_style::SharedInitialGroups>,
    pub(super) color: u32,
    /// What the initial groups are built from beside their values.
    pub(super) value_facts: GroupValueFacts,
    /// The font the initial values resolved.
    pub(super) font: InitialFont,
}

/// The host font objects the document's initial font resolved to, by address.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(in crate::css::style) struct InitialFont {
    pub(in crate::css::style) first_available_font: usize,
    pub(in crate::css::style) font_cascade_list: usize,
}

impl InitialStyle {
    /// The initial style of what a drive `computed`, whose values the groups `payloads` hold.
    pub(super) fn new(
        computed: super::compute::ComputedInitialStyle,
        payloads: crate::css::computed_values::StyleGroups,
        shared: std::sync::Arc<super::super::process_style::SharedInitialGroups>,
    ) -> Self {
        Self {
            flags: computed.flags,
            dependency_flags: computed.dependency_flags,
            payloads,
            _shared: shared,
            color: computed.color,
            value_facts: computed.value_facts,
            font: computed.font,
        }
    }

    /// The initial value of `longhand`.
    pub(super) fn longhand(&self, longhand: crate::css::computed_values::LonghandID) -> &RetainedStyleValueData {
        self.payloads.value(longhand.property())
    }

    /// The initial groups, as the values of an element start from them.
    pub(super) fn group_seed(&self) -> GroupSeed<'_> {
        GroupSeed::new(&self.payloads, &self.value_facts)
    }

    /// What the group values of an element whose parent's record is `parent` start from.
    pub(super) fn group_seeds<'a>(&'a self, parent: Option<&'a std::sync::Arc<Record>>) -> GroupSeeds<'a> {
        GroupSeeds {
            seeds: [
                parent.map_or_else(|| self.group_seed(), |record| record.group_seed()),
                self.group_seed(),
            ],
            parent,
        }
    }

    /// Whether `other` holds the initial values and groups this does.
    pub(super) fn holds_the_same_values_as(&self, other: &InitialStyle) -> bool {
        self.value_facts.publication_equals(&other.value_facts)
            && self.payloads.inherited_values() == other.payloads.inherited_values()
            && self
                .payloads
                .iter()
                .zip(other.payloads.iter())
                .enumerate()
                .all(|(group, (mine, theirs))| {
                    // SAFETY: Both styles hold a reference to each of their groups.
                    unsafe { crate::css::computed_values::groups_hold_the_same(group, mine.as_ptr(), theirs.as_ptr()) }
                })
    }
}

/// How an element's style came about.
#[derive(Clone, Copy)]
pub(super) enum Outcome {
    Computed {
        directly: bool,
    },
    /// The element took the record of an element it agrees with.
    Shared,
    /// The element took the record of an element whose cascade picked the same declarations.
    Reused,
    /// The element's cascade did not finish.
    CascadeDeclined,
    /// The element's computation did not finish.
    ComputeDeclined,
}

/// What the traversal produced for one element.
#[cfg(test)]
pub(super) struct ElementOutput {
    /// The element's matches, kept to check them against the reference matcher, or none when its
    /// facts could not be matched.
    pub(super) matches: Option<Vec<ElementMatch>>,
    pub(super) outcome: Outcome,
}

/// The hint of an element: what a pass restyles of it and below it.
pub(super) type HintBits = u16;

/// The element itself is restyled.
pub(super) const RESTYLE_SELF: HintBits = 1 << 0;
/// Every descendant of the element is restyled.
pub(super) const RESTYLE_DESCENDANTS: HintBits = 1 << 1;
/// Some descendant of the element is restyled.
pub(super) const DIRTY_DESCENDANTS: HintBits = 1 << 2;
/// The element is matched against the whole rule index, not only the entries it is restyled for.
pub(super) const MATCH_SELF: HintBits = 1 << 3;
/// Every descendant of the element is.
pub(super) const MATCH_DESCENDANTS: HintBits = 1 << 4;
/// The element's parent was restyled to something its children compute from, which its traversal
/// passes it, or the composition of the parent's animations moved, which the hint notes.
pub(super) const PARENT_CHANGED: HintBits = 1 << 5;
/// The element's parent was restyled, to what its children compute from or not.
const PARENT_RESTYLED: HintBits = 1 << 6;
/// An ancestor of the element describes another container than it did, which the elements below
/// it that read their containers are restyled for.
pub(super) const CONTAINERS_MOVED: HintBits = 1 << 7;
/// The element is restyled for its pseudo-elements, which inherit the composition of its
/// animations that moved, as under a parent that changed. Where nothing else restyles it, the
/// host owes it no other composition.
pub(super) const RESTYLE_PSEUDO_ELEMENTS: HintBits = 1 << 8;
/// The custom properties the element's parent hands down moved, and nothing else it computes from:
/// it takes them without computing again where it substitutes none that moved.
const PARENT_CUSTOM_PROPERTIES_MOVED: HintBits = 1 << 9;
/// The font of the document element above the element moved, which the elements below it that
/// resolve lengths against what moved of the root element's font are restyled for.
const ROOT_FONT_MOVED: HintBits = 1 << 10;
/// Something the element computes from its matches moved, and nothing its matches follow, such as
/// its font environment: it is computed again from the matches it holds, where nothing else asks to
/// rematch it.
pub(super) const RECOMPUTE_SELF: HintBits = 1 << 11;
/// The bits restyling the element itself.
pub(super) const RESTYLES_SELF: HintBits = RESTYLE_SELF | RESTYLE_PSEUDO_ELEMENTS;
/// The bits a pass styles the element or its descendants for, which it walks down to the element
/// for. A pass leaves the last two to children of an element whose record moved beneath the
/// composition of its animations, which the next one takes alone.
pub(super) const STYLES_SOMETHING: HintBits =
    RESTYLES_SELF | RESTYLE_DESCENDANTS | CONTAINERS_MOVED | PARENT_CUSTOM_PROPERTIES_MOVED;

/// The rule index entries elements are restyled for, which may match them now where they did
/// not or the other way around: an element restyled for some entries alone keeps its other
/// matches.
pub(super) struct Reasons<'a> {
    /// The elements with the entries each is restyled for, sorted.
    pub(super) own: &'a [(StyleNodeID, u32)],
    /// The same for the entries the descendants of an element are restyled for.
    pub(super) descendants: &'a [(StyleNodeID, u32)],
}

impl Reasons<'_> {
    /// The entries of `node` in sorted pairs, in index order.
    fn of(pairs: &[(StyleNodeID, u32)], node: StyleNodeID) -> Option<super::records::MatchList> {
        let start = pairs.partition_point(|&(held, _)| held < node);
        let end = start + pairs[start..].partition_point(|&(held, _)| held == node);
        (start != end).then(|| std::sync::Arc::new(pairs[start..end].iter().map(|&(_, entry)| entry).collect()))
    }
}

/// The union of two sorted entry lists.
fn union(
    a: Option<super::records::MatchList>,
    b: Option<&super::records::MatchList>,
) -> Option<super::records::MatchList> {
    match (a, b) {
        (None, b) => b.cloned(),
        (a, None) => a,
        (Some(a), Some(b)) => {
            let mut merged: Vec<u32> = a.iter().chain(b.iter()).copied().collect();
            merged.sort_unstable();
            merged.dedup();
            Some(std::sync::Arc::new(merged.into_boxed_slice()))
        }
    }
}

pub(super) struct PassContext<'a> {
    pub(super) state: &'a RetainedState,
    pub(super) index: &'a RuleIndex,
    pub(super) initial: &'a InitialStyle,
    pub(super) records: &'a Records<'a>,
    /// What the pass produced for each element, kept for a test to check.
    #[cfg(test)]
    pub(super) outputs: Option<&'a [WriteOnce<ElementOutput>]>,
    /// What to restyle, by element slot, or none to style every element.
    pub(super) hints: Option<&'a super::live::HintCells>,
    pub(super) reasons: Option<&'a Reasons<'a>>,
    /// Whether the records keep what their elements matched.
    pub(super) keeps_matches: bool,
    /// Whether the pass styles what waits on a font or a random base value in a round of its own
    /// once those resolve: it suspends such an element with what its ancestors ask of it, and
    /// holds back the records of the pseudo-elements of an element until none of them waits.
    pub(super) suspends: bool,
    /// The group caches of the threads, by thread, which a pass keeps for the next where it
    /// keeps its caches: an element restyled in a later pass finds the groups the elements styled
    /// before it built.
    pub(super) group_caches: Option<&'a super::per_thread::PerThread<super::compute::GroupCache>>,
    /// The matched declarations cache a pass keeps for the next, where it keeps its caches.
    pub(super) kept_matched_declarations: Option<&'a MatchedDeclarationsCache>,
    /// The cache of a pass that keeps none for the next.
    pub(super) own_matched_declarations: MatchedDeclarationsCache,
    /// Whether an element or pseudo-element whose key another's computation waited with waits for
    /// the next round without one of its own. Elements with the same key can still ask for other
    /// fonts or random base values, so the last rounds of a pass, which have to answer for all
    /// that are left, do not.
    pub(super) waits_once: bool,
    /// Whether the pass reuses what it computed for one element for another: shares records
    /// between elements, finds them in the matched declarations cache, and keeps the caches of its
    /// computations across elements. A reference pass checking the others does none of that.
    pub(super) uses_caches: bool,
    /// What the audit of what the pass reuses asks for, as `LIBWEB_STYLE_AUDIT_CACHES` asks.
    pub(super) cache_audit: super::self_check::Mode,
    /// The font inputs of the document element's record from before the pass, which elements
    /// resolve root-relative lengths against unless the pass styles the document element.
    pub(super) held_root_font: Option<RootFontInputs>,
    /// The font inputs of the document element where the pass styled it, which every element
    /// after it resolves root-relative lengths against.
    pub(super) root_font: std::sync::OnceLock<RootFontInputs>,
    /// What of those moved from the ones the document element had before, which restyles every
    /// element under it that reads it.
    pub(super) root_font_moved: std::sync::OnceLock<RootFontReads>,
    /// The body's color computed independently of the order in which the workers reach tables.
    pub(super) quirks_body_style: std::sync::OnceLock<QuirksBodyStyle>,
}

pub(super) struct QuirksBodyStyle {
    color: Option<RetainedStyleValueData>,
    fonts: Vec<font_resolution::FontRequest>,
    random_bases: Vec<RandomBaseRequest>,
}

impl<'a> PassContext<'a> {
    /// A pass over every element, whose caches work across its elements and which keeps nothing beyond its records.
    pub(super) fn new(
        state: &'a RetainedState,
        index: &'a RuleIndex,
        initial: &'a InitialStyle,
        records: &'a Records<'a>,
        held_root_font: Option<RootFontInputs>,
    ) -> Self {
        Self {
            state,
            index,
            initial,
            records,
            #[cfg(test)]
            outputs: None,
            hints: None,
            reasons: None,
            keeps_matches: false,
            suspends: false,
            group_caches: None,
            kept_matched_declarations: None,
            own_matched_declarations: MatchedDeclarationsCache::default(),
            waits_once: false,
            uses_caches: true,
            cache_audit: super::self_check::Mode::Off,
            held_root_font,
            root_font: Default::default(),
            root_font_moved: Default::default(),
            quirks_body_style: Default::default(),
        }
    }
}

impl PassContext<'_> {
    // https://quirks.spec.whatwg.org/#the-tables-inherit-color-from-body-quirk
    // A body after the table can still be awaiting computation on another worker. Compute its inheritance chain
    // privately, as an on-demand read does, and keep the result for this round. Font and random-value requests
    // suspend the table until the next round, which computes the dependency against the resolved inputs.
    fn quirks_table_color(&self, node: StyleNodeID) -> &QuirksBodyStyle {
        self.quirks_body_style.get_or_init(|| {
            let Some(body) = self.state.quirks_table_body(node) else {
                return QuirksBodyStyle {
                    color: Some(
                        crate::css::style_compute::initial_value_handle(
                            crate::css::property_metadata::property_id::COLOR,
                        )
                        .clone(),
                    ),
                    fonts: Vec::new(),
                    random_bases: Vec::new(),
                };
            };
            if self.records.computed(body)
                && let Some(view) = self.records.assigned_record_view(body)
            {
                return QuirksBodyStyle {
                    color: Some(
                        view.longhands()
                            .effective_value(view.animated_overlay, crate::css::property_metadata::property_id::COLOR)
                            .clone(),
                    ),
                    fonts: Vec::new(),
                    random_bases: Vec::new(),
                };
            }
            let mut chain: SmallVec<[StyleNodeID; 32]> =
                std::iter::successors(Some(body), |&node| self.state.projection.inheritance_parent(node)).collect();
            chain.reverse();
            let mut cells = super::records::RecordCells::default();
            cells.reserve(self.state.projection.element_slot_count());
            let records =
                Records::new(self.state, &cells, None).composing_over_equal_records(Record::holds_the_same_style_as);
            let context = PassContext {
                uses_caches: false,
                ..PassContext::new(self.state, self.index, self.initial, &records, self.root_font())
            };
            let statistics = style_chain(&context, &chain);
            let color = records
                .assigned_record_view(body)
                .filter(|_| records.computed(body))
                .map(|view| {
                    view.longhands()
                        .effective_value(view.animated_overlay, crate::css::property_metadata::property_id::COLOR)
                        .clone()
                });
            QuirksBodyStyle {
                color,
                fonts: statistics.font_requests,
                random_bases: statistics.random_requests,
            }
        })
    }

    /// The matched declarations cache of the pass.
    fn matched_declarations(&self) -> &MatchedDeclarationsCache {
        self.kept_matched_declarations.unwrap_or(&self.own_matched_declarations)
    }
}

pub(super) fn is_document_element(state: &RetainedState, node: StyleNodeID) -> bool {
    state.projection.adjustment_facts(node) & bridge::element_adjustment_fact::IS_DOCUMENT_ELEMENT != 0
}

// https://drafts.csswg.org/css-values-4/#rem
// Equal to the computed value of the em unit on the root element.
// NB: The computed value holds what the root element's animations apply, which the composition of
//     them over its record has where the host composed one.
fn root_font_of(records: &Records<'_>, node: StyleNodeID, record: &Record) -> RootFontInputs {
    match records.composition(node) {
        Some(view) => RootFontInputs::from_payloads(view.payloads, view.dependency_flags),
        None => RootFontInputs::from_payloads(
            &record.payloads,
            super::super::computed::record_dependency_flags(&record.facts(), &record.payloads),
        ),
    }
}

/// The document element, where `root` is it or its parent.
pub(super) fn document_element_at(state: &RetainedState, root: StyleNodeID) -> Option<StyleNodeID> {
    match is_document_element(state, root) {
        true => Some(root),
        false => std::iter::successors(state.projection.first_element_child(root), |&child| {
            state.projection.next_element_sibling(child)
        })
        .find(|&child| is_document_element(state, child)),
    }
}

/// The font inputs of the record the document element under `root` holds in `records`, if any:
/// those the last pass that styled it resolved against, or those its record holds.
pub(super) fn held_root_font(
    state: &RetainedState,
    records: &Records<'_>,
    root: StyleNodeID,
) -> Option<RootFontInputs> {
    let document_element = document_element_at(state, root)?;
    let record = records.get(document_element)?;
    Some(
        records
            .kept_root_font(document_element)
            .unwrap_or_else(|| root_font_of(records, document_element, record)),
    )
}

impl PassContext<'_> {
    #[cfg(test)]
    fn set_output(&self, node: StyleNodeID, output: impl FnOnce() -> ElementOutput) {
        if let Some(outputs) = self.outputs {
            let _ = outputs[node.element_slot()].set(output());
        }
    }

    fn root_font(&self) -> Option<RootFontInputs> {
        self.root_font.get().copied().or(self.held_root_font)
    }

    fn root_font_moved(&self) -> RootFontReads {
        self.root_font_moved.get().copied().unwrap_or_default()
    }

    /// Whether `node` resolves lengths against what moved of the document element's font.
    fn reads_root_font_move(&self, node: StyleNodeID) -> bool {
        let moved = self.root_font_moved();
        moved != RootFontReads::None && self.records.reads_root_font(node).reads_move(moved)
    }

    /// Note the font inputs of the record the pass gave the document element `node`.
    fn note_root_font(&self, node: StyleNodeID) {
        let Some(record) = self.records.get(node) else {
            return;
        };
        let root_font = root_font_of(self.records, node, record);
        let previous = self
            .held_root_font
            .unwrap_or_else(|| RootFontInputs::from_document(&self.state.document_style_computation_inputs));
        if self.root_font.set(root_font).is_ok() {
            let _ = self.root_font_moved.set(root_font.moved_from(previous));
        }
    }

    fn hint(&self, node: StyleNodeID) -> HintBits {
        self.hints.map_or(
            RESTYLE_SELF | RESTYLE_DESCENDANTS | MATCH_SELF | MATCH_DESCENDANTS,
            |hints| hints.get(node.element_slot()).map_or(0, |cell| cell.bits),
        )
    }
}

#[derive(Default)]
pub(super) struct Statistics {
    pub(super) match_counters: MatchCounters,
    pub(super) cascade_counters: CascadeCounters,
    /// The fonts a drive waited on, to resolve before styling again.
    pub(super) font_requests: Vec<font_resolution::FontRequest>,
    /// The random caching keys a computation needed the base values of, to draw before styling
    /// again.
    pub(super) random_requests: Vec<RandomBaseRequest>,
    /// The elements whose styles waited on those, which the pass styled nothing below.
    pub(super) waiting: Vec<StyleNodeID>,
    /// The elements of `waiting` the pass matched to the entries they held matched before, which
    /// the next pass computes again from those without matching them.
    pub(super) waiting_with_kept_matches: Vec<StyleNodeID>,
    /// The elements the pass styled but for some of their pseudo-elements, whose styles waited on
    /// those.
    pub(super) waiting_pseudo_elements: Vec<StyleNodeID>,
    /// What the pass suspended of those, where it suspends, for its next round to resume.
    pub(super) suspended: Vec<Suspended>,
    /// The elements the pass styled.
    pub(super) styled: Vec<StyleNodeID>,
    /// The elements that took the custom properties their parents hand down now without being
    /// styled, keeping their style otherwise.
    pub(super) moved_custom_properties: Vec<StyleNodeID>,
    /// How many of those it styled differently than before, in what their children compute from.
    pub(super) changed_elements: u64,
    /// Those elements.
    pub(super) children_moved: Vec<StyleNodeID>,
    /// The children of elements whose records moved beneath the composition of their animations,
    /// with the hints the pass kept for them, which the next pass restyles once the host composed
    /// the animations over the new records.
    pub(super) deferred: Vec<(StyleNodeID, HintBits)>,
    /// The elements the pass restyled for their pseudo-elements alone.
    pub(super) restyled_for_pseudo_elements: Vec<StyleNodeID>,
    /// How many elements took the record of another.
    pub(super) outcomes: super::counters::OutcomeCounts,
    /// What `LIBWEB_STYLE_AUDIT_CACHES` found of what the pass reused, where it is set.
    pub(super) cache_audit: cache_audit::CacheAudit,
}

/// What a pass suspended on fonts and random base values, for its next round to style once they
/// resolve.
pub(super) enum Suspended {
    /// An element whose style waited, with what its ancestors ask of it: the round styles it and
    /// the elements under it as if it had not waited.
    Element(Queued),
    /// The pseudo-elements of an element the pass styled, some of which waited.
    PseudoElements(SuspendedPseudoElements),
}

/// The pseudo-elements of an element a pass styled but for some of them, which waited.
pub(super) struct SuspendedPseudoElements {
    node: StyleNodeID,
    /// The matches of the element, and those whose container conditions held, which the
    /// pseudo-elements are styled from.
    matches: Vec<ElementMatch>,
    applied: Vec<ElementMatch>,
    gated: bool,
    /// The records of those that did not wait, which the element keeps where no round follows.
    records: super::records::PseudoElementRecords,
}

impl Suspended {
    fn node(&self) -> StyleNodeID {
        match self {
            Self::Element(queued) => queued.node,
            Self::PseudoElements(pseudo_elements) => pseudo_elements.node,
        }
    }
}

/// One thread's working state.
/// How many longhand table flags a thread keeps for the records it makes next to share.
const TABLE_FLAGS_KEPT: usize = 4;

/// How many of the rare facts of the records it made last a thread keeps to share with the next.
const RARE_FACTS_KEPT: usize = 8;

struct Scratch<'a> {
    matcher: Matcher<'a>,
    cascade: Cascade,
    filter: AncestorFilter,
    /// The elements whose keys the filter holds, root first, with those keys.
    ancestors: Vec<(StyleNodeID, SmallVec<[u32; 8]>)>,
    matches: Vec<ElementMatch>,
    /// What the entries an element was rematched for answer now, and whether one answers other
    /// than it did before.
    reason_answers: Vec<bool>,
    reason_flipped: bool,
    /// The matches of the element styled last whose container conditions hold, which its cascade
    /// reads in place of `matches` where `gated`.
    applied: Vec<ElementMatch>,
    /// Whether the element styled last matched a rule applying under container conditions.
    gated: bool,
    /// What the container conditions of the element styled last read of its containers.
    container_reads: Option<ContainerVerdict>,
    /// What the thread measured and gathered, which the pass adds up.
    statistics: Statistics,
    substitutions: std::cell::RefCell<super::custom_properties::SubstitutionCache>,
    sharing: SharingCache,
    moved_names: super::custom_property_moves::MovedCustomProperties,
    /// The hashes of the keys of the elements and pseudo-elements whose computations waited on
    /// fonts or random base values, where the pass lets another with the same key, which mostly
    /// asks for the same ones, wait for its next round without computing what the first one threw
    /// away.
    waiting_keys: SmallVec<[u64; 8]>,
    /// What decides which of the elements with declarations of their own the cache is worth for.
    own_declaration_lookups: OwnDeclarationLookups,
    /// The longhand table flags of the records the thread made last, most recent first, which the
    /// next record holding the same flags shares.
    table_flags: SmallVec<[std::sync::Arc<crate::css::computed_longhand_table::LonghandTableFlags>; TABLE_FLAGS_KEPT]>,
    /// The rare sets of the records the thread made last, most recent first, whose rare facts
    /// the next record recording the same shares, and whose set it shares where it holds the same
    /// rare groups.
    rare_facts: SmallVec<[crate::css::computed_values::RareSetRef; RARE_FACTS_KEPT]>,
    /// The record of the element the thread styled last, whose group sets the next record holding
    /// the same groups there shares, as the siblings of a list styled alike do.
    last_record: Option<std::sync::Arc<Record>>,
    /// The match lists of the elements the thread styled last, most recent first.
    kept_match_lists: SmallVec<[super::records::MatchList; RARE_FACTS_KEPT]>,
    groups: super::compute::GroupCache,
    absolutized: crate::css::style_compute::AbsolutizedValues,
    custom_property_stores: super::custom_properties::StoreCache,
    revalidation_matches: Vec<ElementMatch>,
    /// Whether the shadow host the scratch styled last moved the matches of its pseudo-elements
    /// that elements of its shadow tree stand for, which are restyled with it then.
    backing_elements_moved: bool,
    /// What `LIBWEB_STYLE_AUDIT_CACHES` found of what the pass reused, where it is set.
    cache_audit: Option<Box<cache_audit::CacheAudit>>,
    /// The pseudo-elements of the element styled last whose records came from the
    /// matched declarations cache, kept for the audit alone.
    pseudo_element_cache_hits: SmallVec<[super::PseudoElementTarget; 2]>,
}

impl<'a> Scratch<'a> {
    fn new(context: &PassContext<'a>) -> Self {
        let state = context.state;
        Self {
            matcher: Matcher::new(context.index, state),
            cascade: Cascade::new(),
            filter: AncestorFilter::new(),
            ancestors: Vec::new(),
            matches: Vec::new(),
            reason_answers: Vec::new(),
            reason_flipped: false,
            applied: Vec::new(),
            gated: false,
            container_reads: None,
            statistics: Statistics::default(),
            substitutions: Default::default(),
            sharing: SharingCache::default(),
            moved_names: Default::default(),
            waiting_keys: SmallVec::new(),
            own_declaration_lookups: OwnDeclarationLookups::default(),
            table_flags: SmallVec::new(),
            rare_facts: SmallVec::new(),
            last_record: None,
            kept_match_lists: SmallVec::new(),
            groups: Default::default(),
            absolutized: Default::default(),
            custom_property_stores: Default::default(),
            revalidation_matches: Vec::new(),
            backing_elements_moved: false,
            cache_audit: cache_audit::CacheAudit::for_pass(context),
            pseudo_element_cache_hits: SmallVec::new(),
        }
    }

    /// `flags`, shared with the record the thread made last holding the same flags, if any.
    fn shared_table_flags(
        &mut self,
        flags: crate::css::computed_longhand_table::LonghandTableFlags,
    ) -> std::sync::Arc<crate::css::computed_longhand_table::LonghandTableFlags> {
        let kept = &mut self.table_flags;
        if let Some(index) = kept.iter().position(|held| held.same_as(&flags)) {
            kept[..=index].rotate_right(1);
            return kept[0].clone();
        }
        let flags = std::sync::Arc::new(flags);
        if kept.len() == TABLE_FLAGS_KEPT {
            kept.pop();
        }
        kept.insert(0, flags.clone());
        flags
    }

    /// `rare`, shared with the record the thread made last recording the same, if any: the
    /// siblings of a list mostly record alike.
    fn interned_rare(
        kept: &mut SmallVec<[crate::css::computed_values::RareSetRef; RARE_FACTS_KEPT]>,
        rare: RareStyleFacts,
        groups: crate::css::computed_values::StyleGroups,
        custom_winners: &[(
            super::rule_store::CustomDeclaration,
            crate::css::style_value::RetainedStyleValueData,
        )],
    ) -> crate::css::computed_values::StyleGroups {
        let declarations_of = |winners: &[_]| (!winners.is_empty()).then(|| winners.into());
        if let Some(index) = kept.iter().position(|held| held.facts().same_as(&rare)) {
            kept[..=index].rotate_right(1);
            // NB: The written values are what a sample resolves, so holding the same ones is what makes
            //     two elements' declarations the same.
            let custom_declarations = match kept[0].custom_declarations() {
                Some(held)
                    if held.len() == custom_winners.len()
                        && held
                            .iter()
                            .zip(custom_winners)
                            .all(|(held, winner)| held.0 == winner.0 && held.1.pointer() == winner.1.pointer()) =>
                {
                    Some(held.clone())
                }
                _ => declarations_of(custom_winners),
            };
            let groups = groups.with_rare_facts(kept[0].facts().clone(), custom_declarations, &[Some(&kept[0])]);
            if !groups.rare_set().ptr_eq(&kept[0]) {
                kept[0] = groups.rare_set().clone();
            }
            return groups;
        }
        let groups = groups.with_rare_facts(std::sync::Arc::new(rare), declarations_of(custom_winners), &[]);
        if kept.len() == RARE_FACTS_KEPT {
            kept.pop();
        }
        kept.insert(0, groups.rare_set().clone());
        groups
    }

    /// The entries the element just matched, shared with the element the thread styled last that
    /// matched the same ones, if any: the siblings of a list mostly match alike.
    fn interned_matches(&mut self) -> super::records::MatchList {
        let matched = || self.matches.iter().map(|matched| matched.entry);
        let kept = &mut self.kept_match_lists;
        if let Some(index) = kept.iter().position(|held| held.iter().copied().eq(matched())) {
            kept[..=index].rotate_right(1);
            return kept[0].clone();
        }
        let matches: super::records::MatchList = std::sync::Arc::new(matched().collect());
        if kept.len() == RARE_FACTS_KEPT {
            kept.pop();
        }
        kept.insert(0, matches.clone());
        matches
    }

    /// Forget what the computations of the elements styled before cached, for the next to compute
    /// from nothing they left.
    fn forget_caches(&mut self) {
        *self.substitutions.get_mut() = Default::default();
        self.groups = Default::default();
        self.absolutized = Default::default();
        self.custom_property_stores = Default::default();
    }

    /// Make the filter hold exactly the keys of `parent` and its ancestors.
    fn enter_parent(&mut self, context: &PassContext<'_>, parent: Option<StyleNodeID>) {
        let held = self.ancestors.last().map(|(node, _)| *node);
        if held == parent {
            return;
        }
        let tree = &context.state.projection;
        // A walk down a tree holds everything above the new parent already.
        if let Some(node) = parent
            && tree.parent(node) == held
        {
            self.hold_ancestor(context, node);
            return;
        }
        let mut chain: SmallVec<[StyleNodeID; 32]> = SmallVec::new();
        let mut ancestor = parent;
        while let Some(node) = ancestor {
            chain.push(node);
            ancestor = tree.parent(node);
        }
        chain.reverse();
        let common = self
            .ancestors
            .iter()
            .zip(&chain)
            .take_while(|((held, _), wanted)| held == *wanted)
            .count();
        while self.ancestors.len() > common {
            let (_, keys) = self.ancestors.pop().expect("an ancestor was held");
            for key in keys {
                self.filter.remove(key);
            }
        }
        for &node in &chain[common..] {
            self.hold_ancestor(context, node);
        }
    }

    /// Put the keys of `node`, a child of the last ancestor held, into the filter.
    fn hold_ancestor(&mut self, context: &PassContext<'_>, node: StyleNodeID) {
        let keys = context
            .state
            .projection
            .row_of(node)
            .map_or_else(SmallVec::new, super::ancestor_keys_of);
        for &key in &keys {
            self.filter.insert(key);
        }
        self.ancestors.push((node, keys));
    }

    /// Test `node` against the entries of `reasons` alone, into `reason_answers`, and note in
    /// `reason_flipped` whether one answers other than before, or answer that it has to be matched
    /// against the whole index.
    fn test_reasons(&mut self, context: &PassContext<'_>, node: StyleNodeID, reasons: &[u32]) -> bool {
        let Some(before) = context.records.matches_of(node) else {
            return false;
        };
        let scoped = |entry: &u32| context.index.entries.view(*entry as usize).scoped();
        // The entries of other trees that reach an element are matched from those trees.
        if (context.index.has_scoped_entries() && (before.iter().any(scoped) || reasons.iter().any(scoped)))
            || self.matcher.is_reached_from_other_trees(node)
        {
            return false;
        }
        self.reason_answers.clear();
        self.reason_flipped = false;
        for &reason in reasons {
            let Ok(now) = self.matcher.matches_entry(node, reason) else {
                return false;
            };
            self.reason_flipped |= now != before.binary_search(&reason).is_ok();
            self.reason_answers.push(now);
        }
        true
    }

    /// Whether every entry of `reasons` that answered other than before for `node` loses each longhand
    /// it declares to the matches that stay, in the answers `test_reasons` left and the matches
    /// `merge_reason_answers` made of them.
    fn flipped_reasons_lose(&self, context: &PassContext<'_>, node: StyleNodeID, reasons: &[u32]) -> bool {
        let Some(before) = context.records.matches_of(node) else {
            return false;
        };
        let stayed: SmallVec<[u32; 16]> = self
            .matches
            .iter()
            .map(|matched| matched.entry)
            .filter(|entry| before.binary_search(entry).is_ok())
            .collect();
        let flipped = reasons
            .iter()
            .zip(&self.reason_answers)
            .filter(|&(reason, &holds)| holds != before.binary_search(reason).is_ok())
            .map(|(&reason, _)| reason);
        all_lose_every_longhand(context.state, context.index, &[], flipped, &stayed)
    }

    /// The matches of `node` from the answers `test_reasons` left: those of `reasons` that hold,
    /// and its other matches from before.
    fn merge_reason_answers(&mut self, context: &PassContext<'_>, node: StyleNodeID, reasons: &[u32]) {
        self.matches.clear();
        // Both lists are in index order, as the matches have to be.
        let mut kept = context
            .records
            .matches_of(node)
            .map_or(&[][..], |before| &before[..])
            .iter()
            .copied()
            .peekable();
        for (&reason, &holds) in reasons.iter().zip(&self.reason_answers) {
            while let Some(entry) = kept.next_if(|&entry| entry < reason) {
                self.matches.push(self.matcher.element_match(node, entry));
            }
            kept.next_if_eq(&reason);
            if holds {
                self.matches.push(self.matcher.element_match(node, reason));
            }
        }
        for entry in kept {
            self.matches.push(self.matcher.element_match(node, entry));
        }
    }

    /// Match, cascade and compute `node`, against the whole index where `reasons` is none and only
    /// the entries of it otherwise, and answer whether its children are styled too. `moved` holds
    /// the bits of its hint naming what moved of what it computes from besides its matches.
    fn style_element(
        &mut self,
        context: &PassContext<'_>,
        node: StyleNodeID,
        reasons: Option<&[u32]>,
        moved: HintBits,
    ) -> bool {
        let state = context.state;
        if !context.uses_caches {
            self.forget_caches();
        }
        self.statistics.styled.push(node);
        self.backing_elements_moved = false;
        self.gated = false;
        self.container_reads = None;
        self.pseudo_element_cache_hits.clear();
        // The elements of a shadow tree standing for its host's pseudo-elements are styled from
        // the host's matches for them, which no invalidation of theirs follows.
        let matched_before = state
            .projection
            .shadow_root_of(node)
            .map(|_| context.records.matches_of(node).cloned());
        let facts = state.projection.facts(node);
        let rematched = context.keeps_matches
            && match (facts, reasons) {
                (Some(_), Some(reasons)) => self.test_reasons(context, node, reasons),
                _ => false,
            };
        // What an element matches, its parent and its containers decide its style. Its matches
        // are the ones it had where no entry it was rematched for answers otherwise.
        let only_matches_moved = rematched
            && !state.is_quirks_table(node)
            && moved & (PARENT_CHANGED | CONTAINERS_MOVED | RECOMPUTE_SELF) == 0
            && (moved & PARENT_RESTYLED == 0
                || state
                    .projection
                    .inheritance_parent(node)
                    .is_some_and(|parent| context.records.child_impact(parent) == ChildImpact::None))
            && !context.reads_root_font_move(node);
        if only_matches_moved && !self.reason_flipped && context.records.keep_previous(node) {
            return Self::children_are_styled(context, node);
        }
        if let (true, Some(reasons)) = (rematched, reasons) {
            self.merge_reason_answers(context, node, reasons);
            // NB: Nor does its style move where the entries answering otherwise decide nothing.
            if only_matches_moved && self.flipped_reasons_lose(context, node, reasons) {
                let matches = self.interned_matches();
                if context.records.keep_previous_matching(node, Some(matches)) {
                    return Self::children_are_styled(context, node);
                }
            }
        }
        // An element standing for its shadow host's pseudo-element is styled from the host's
        // rules, which no other element's style tells.
        let backed = state.backed_host_pseudo_element(node);
        let key = facts
            .filter(|_| backed.is_none() && context.uses_caches)
            .and_then(|facts| super::sharing::key_of(state, context.index, context.records, node, facts));
        // The ancestor filter holds the element's ancestors in its own tree, which the rules of
        // that tree test, and not the flat tree ancestors it inherits from. An element keeping
        // its matches tests rules only to share a style.
        let parent = state.projection.parent(node);
        if !rematched || key.is_some() {
            self.enter_parent(context, parent);
            debug_assert!(
                self.ancestors
                    .iter()
                    .rev()
                    .map(|(ancestor, _)| *ancestor)
                    .eq(state.projection.ancestors(node))
            );
        }
        // An element matching what it matched before shares the style of another as one matched
        // anew does: the revalidation answers and what the key holds decide its matches alike.
        if let (Some(facts), Some(key)) = (facts, &key) {
            let (matcher, filter, revalidation_matches) =
                (&mut self.matcher, &self.filter, &mut self.revalidation_matches);
            let shared = self.sharing.find(&context.state.projection, key, facts, || {
                revalidation_matches.clear();
                matcher
                    .match_revalidation_entries(node, facts, filter, revalidation_matches)
                    .ok()
                    .map(|()| super::sharing::revalidation_of(context.index, revalidation_matches))
            });
            if let Some((record, identity)) = shared {
                let matches = context.records.matches_of(identity).cloned();
                if let Some(before) = &matched_before {
                    self.backing_elements_moved = Self::moves_backing_elements(
                        context,
                        node,
                        before.as_ref().map(|before| &before[..]),
                        matches.as_deref().map(|matches| matches.iter().copied()),
                    );
                }
                context.records.insert(node, record, identity, matches);
                // Elements sharing a style match the same rules, their pseudo-elements' too.
                let pseudo_element_records = context.records.pseudo_element_records(identity).into();
                context
                    .records
                    .insert_pseudo_element_records(node, pseudo_element_records);
                self.statistics.outcomes.note(&Outcome::Shared);
                self.audit_caches(context, node, cache_audit::Reuse::SharedStyle);
                #[cfg(test)]
                context.set_output(node, || ElementOutput {
                    matches: None,
                    outcome: Outcome::Shared,
                });
                return Self::children_are_styled(context, node);
            }
        }
        let matched = rematched || {
            self.matches.clear();
            facts.is_some_and(|facts| {
                self.statistics.outcomes.note_matched();
                self.matcher
                    .match_element(node, facts, &self.filter, &mut self.matches)
                    .is_ok()
            })
        };
        if matched {
            self.apply_container_gates(context, node, false);
        }
        // An element that matched what one styled before did, with what that element's computation
        // read holding still, takes its record without a cascade.
        let matched_key = match matched {
            true => self.matched_key(context, node, backed),
            false => None,
        };
        let found = matched_key
            .as_ref()
            .and_then(|key| self.find_matched_declarations(context, node, key));
        let matched_hash = matched_key.as_ref().map(MatchedKey::hash);
        let waits_as_another =
            context.waits_once && found.is_none() && matched_hash.is_some_and(|hash| self.waiting_keys.contains(&hash));
        let cascaded = found.is_some() || waits_as_another || (matched && self.cascade_element(context, node, backed));
        let outcome = if waits_as_another {
            self.statistics.waiting.push(node);
            Outcome::ComputeDeclined
        } else if !cascaded {
            Outcome::CascadeDeclined
        } else {
            let computed = match found {
                Some(record) => Ok((
                    record,
                    ComputedFacts {
                        directly: false,
                        element_dependent: false,
                        reused: true,
                    },
                )),
                None => self.compute(context, node, matched_key),
            };
            match computed {
                Ok((record, computed)) => {
                    if let Some(before) = &matched_before {
                        self.backing_elements_moved = Self::moves_backing_elements(
                            context,
                            node,
                            before.as_ref().map(|before| &before[..]),
                            Some(self.matches.iter().map(|matched| matched.entry)),
                        );
                    }
                    let matches = context.keeps_matches.then(|| self.interned_matches());
                    context.records.insert(node, record.clone(), node, matches);
                    // The document element's font is what the root-relative lengths of every
                    // element after it resolve against.
                    if is_document_element(state, node) {
                        context.note_root_font(node);
                    }
                    let pseudo_elements_element_dependent = self.compute_pseudo_elements(context, node);
                    if let Some(reads) = self.container_reads.take() {
                        context.records.insert_container_reads(node, std::sync::Arc::new(reads));
                    }
                    // An element sharing this style takes its pseudo-elements' records too, so
                    // neither may compute against the element itself, nor against the containers
                    // above it.
                    if let (true, Some(key), false) = (
                        facts.is_some(),
                        key,
                        computed.element_dependent || pseudo_elements_element_dependent || self.gated,
                    ) {
                        let revalidation = super::sharing::revalidation_of(context.index, &self.matches);
                        self.sharing.insert(key, record, node, revalidation);
                    }
                    self.audit_caches(
                        context,
                        node,
                        match computed.reused {
                            true => cache_audit::Reuse::MatchedDeclarations,
                            false => cache_audit::Reuse::ComputationCaches,
                        },
                    );
                    match computed.reused {
                        true => Outcome::Reused,
                        false => Outcome::Computed {
                            directly: computed.directly,
                        },
                    }
                }
                Err(outcome) => {
                    if let Some(hash) = matched_hash
                        && self.statistics.waiting.last() == Some(&node)
                    {
                        self.waiting_keys.push(hash);
                    }
                    outcome
                }
            }
        };
        let unstyled = matches!(outcome, Outcome::CascadeDeclined | Outcome::ComputeDeclined);
        // An element waiting on a font that matched what it held matched before keeps those
        // matches, which the next pass computes it from again.
        if unstyled
            && matched
            && self.statistics.waiting.last() == Some(&node)
            && context.records.matches_of(node).is_some_and(|before| {
                before.len() == self.matches.len()
                    && before
                        .iter()
                        .zip(&self.matches)
                        .all(|(&entry, matched)| entry == matched.entry)
            })
        {
            self.statistics.waiting_with_kept_matches.push(node);
        }
        self.statistics.outcomes.note(&outcome);
        #[cfg(test)]
        context.set_output(node, || ElementOutput {
            matches: matched.then(|| self.matches.clone()),
            outcome,
        });
        // An element this pass holds no record for, such as one waiting on a font, has no style
        // for its children to inherit. The pass that styles it styles them.
        !unstyled && Self::children_are_styled(context, node)
    }

    /// Whether the shadow host `node`, matching `after` where it matched `before`, moved the
    /// matches of a pseudo-element an element of its shadow tree stands for.
    fn moves_backing_elements(
        context: &PassContext<'_>,
        node: StyleNodeID,
        before: Option<&[u32]>,
        after: Option<impl Iterator<Item = u32>>,
    ) -> bool {
        let state = context.state;
        let pseudo_entries = |entries: &mut dyn Iterator<Item = u32>| -> SmallVec<[u32; 8]> {
            entries
                .filter(|&entry| context.index.entries.view(entry as usize).pseudo_element())
                .collect()
        };
        let (Some(before), Some(after)) = (before, after) else {
            return true;
        };
        let before = pseudo_entries(&mut before.iter().copied());
        let after = pseudo_entries(&mut after.into_iter());
        if before == after {
            return false;
        }
        let moved_kinds: SmallVec<[u8; 4]> = before
            .iter()
            .filter(|entry| !after.contains(entry))
            .chain(after.iter().filter(|entry| !before.contains(entry)))
            .map(|&entry| context.index.entries.view(entry as usize).pseudo_kind())
            .collect();
        let tree = &state.projection;
        tree.shadow_root_of(node).is_some_and(|root| {
            tree.preorder(root).skip(1).any(|element| {
                state
                    .projection
                    .associated_pseudo_kind(element)
                    .is_some_and(|kind| moved_kinds.contains(&kind))
            })
        })
    }

    /// Cascade `node` from its matches, or from the matches of the shadow host whose
    /// pseudo-element it stands for where it is `backed`, and answer whether it matched.
    fn cascade_element(
        &mut self,
        context: &PassContext<'_>,
        node: StyleNodeID,
        backed: Option<(u8, StyleNodeID)>,
    ) -> bool {
        let state = context.state;
        let Some((kind, host)) = backed else {
            let matches = match self.gated {
                true => &self.applied,
                false => &self.matches,
            };
            self.cascade.cascade_element(state, context.index, node, matches);
            return true;
        };
        let projection = &state.projection;
        let Some(host_facts) = projection.facts(host) else {
            return false;
        };
        // The host is matched in its own tree, against the keys of its ancestors there.
        let mut filter = AncestorFilter::new();
        for row in projection
            .ancestors(host)
            .filter_map(|ancestor| projection.row_of(ancestor))
        {
            for key in super::ancestor_keys_of(row) {
                filter.insert(key);
            }
        }
        let mut host_matches = Vec::new();
        let matched = self
            .matcher
            .match_element(host, host_facts, &filter, &mut host_matches)
            .is_ok();
        // The host's rules for its pseudo-element apply where their container conditions hold
        // for that pseudo-element.
        let index = context.index;
        if index.has_gates() {
            let mut last: Option<(ContainerGateID, bool)> = None;
            host_matches.retain(|matched| {
                let Some(gate) = index.gate_of_entry(matched.entry) else {
                    return true;
                };
                match last {
                    Some((last_gate, holds)) if last_gate == gate => holds,
                    _ => {
                        let verdict = gate_verdict(context, gate, host, true);
                        let holds = verdict.matches;
                        self.container_reads.get_or_insert_default().add_reads(verdict);
                        last = Some((gate, holds));
                        holds
                    }
                }
            });
        }
        if matched {
            self.cascade
                .cascade_backing_element(state, context.index, node, &host_matches, kind);
        }
        matched
    }

    /// Compute the styles of the pseudo-elements of `node` that generate a box, one kind at a time,
    /// each cascading its own rules over the element's style. Returns whether any of their records
    /// computes against the element itself, such as its place among its siblings.
    fn compute_pseudo_elements(&mut self, context: &PassContext<'_>, node: StyleNodeID) -> bool {
        let waiting = self.statistics.waiting_pseudo_elements.len();
        let Some((pseudo_element_records, element_dependent)) = self.styled_pseudo_elements(context, node) else {
            return false;
        };
        // A pass that suspends styles the pseudo-elements of an element again in its next round
        // where some waited, and holds back their records until then.
        if context.suspends && self.statistics.waiting_pseudo_elements.len() != waiting {
            self.statistics
                .suspended
                .push(Suspended::PseudoElements(SuspendedPseudoElements {
                    node,
                    matches: self.matches.clone(),
                    applied: match self.gated {
                        true => self.applied.clone(),
                        false => Vec::new(),
                    },
                    gated: self.gated,
                    records: pseudo_element_records,
                }));
            return element_dependent;
        }
        context
            .records
            .insert_pseudo_element_records(node, pseudo_element_records);
        element_dependent
    }

    /// Style the pseudo-elements of an element a round before styled but for some of them, from
    /// the matches it styled them from, now that what they waited on resolved.
    fn resume_pseudo_elements(&mut self, context: &PassContext<'_>, suspended: SuspendedPseudoElements) {
        let SuspendedPseudoElements {
            node,
            matches,
            applied,
            gated,
            records: _,
        } = suspended;
        self.matches = matches;
        self.applied = applied;
        self.gated = gated;
        self.container_reads = None;
        self.pseudo_element_cache_hits.clear();
        self.compute_pseudo_elements(context, node);
        // NB: The round before kept what the container conditions of the element's rules read,
        //     which reading them again here repeats.
        self.container_reads = None;
    }

    /// The records of the pseudo-elements of `node` that generate a box, over the record `context`
    /// holds for it, and whether any of them computes against the element itself, or none where
    /// it holds no record.
    fn styled_pseudo_elements(
        &mut self,
        context: &PassContext<'_>,
        node: StyleNodeID,
    ) -> Option<(super::records::PseudoElementRecords, bool)> {
        let mut pseudo_element_records = super::records::PseudoElementRecords::new();
        let mut element_dependent = false;
        let element = context.records.get_shared(node).cloned()?;
        if element.facts().display().is_none() {
            return Some((pseudo_element_records, element_dependent));
        }
        // NB: Only what a rule styles generates a box here, the marker of a list item and the
        //     highlights the host shows in the element apart, each of which is unstyled otherwise.
        let matches = match self.gated {
            true => &self.applied,
            false => &self.matches,
        };
        let mut kinds = matches.iter().fold(0_u32, |kinds, matched| {
            kinds | 1_u32.checked_shl(u32::from(matched.pseudo_kind)).unwrap_or(0)
        });
        if element.facts().display().is_list_item() {
            kinds |= 1 << super::super::publication::pseudo_kind::MARKER;
        }
        if context.state.highlight_observers.kinds_of(node) != 0 {
            for kind in super::HIGHLIGHT_KINDS {
                if context.state.highlight_observers.observes(node, kind) {
                    kinds |= 1 << kind;
                }
            }
        }
        let custom_highlights = context.state.highlight_observers.custom_highlights_of(node);
        if kinds == 0 && custom_highlights.is_empty() {
            return Some((pseudo_element_records, element_dependent));
        }
        let inherited = PseudoElementInheritance::of(context, node, &element);
        // The conditions of a pseudo-element's rules ask about its element's record first.
        self.apply_container_gates(context, node, true);
        let targets = super::PSEUDO_ELEMENT_KINDS
            .into_iter()
            .filter(|&kind| kinds & (1 << kind) != 0)
            .map(super::PseudoElementTarget::of_kind)
            .chain(
                custom_highlights
                    .iter()
                    .map(|&name| super::PseudoElementTarget::custom_highlight(name)),
            );
        for target in targets {
            match self.compute_pseudo_element(
                context,
                node,
                &element,
                &inherited,
                target,
                PseudoElementRead::Generating,
            ) {
                PseudoElementOutcome::Computed {
                    record,
                    element_dependent: dependent,
                } => {
                    element_dependent |= dependent;
                    pseudo_element_records.push((target, record));
                }
                // A pseudo-element waiting on a font or a random base value is styled again with
                // its element once those resolved. Until then its element shares its records with
                // no other.
                PseudoElementOutcome::Waiting => element_dependent = true,
                PseudoElementOutcome::Declined
                | PseudoElementOutcome::Unstyled
                | PseudoElementOutcome::GeneratesNoBox => {}
            }
        }
        Some((pseudo_element_records, element_dependent))
    }

    /// Compute the style of the pseudo-element `target` of `node`, whose record is `element`, from the
    /// rules it matched for it, cascaded over what it inherits, `inherited`. Which pseudo-elements
    /// are styled is what `read` says.
    fn compute_pseudo_element(
        &mut self,
        context: &PassContext<'_>,
        node: StyleNodeID,
        element: &std::sync::Arc<Record>,
        inherited: &PseudoElementInheritance<'_>,
        target: super::PseudoElementTarget,
        read: PseudoElementRead,
    ) -> PseudoElementOutcome {
        let state = context.state;
        let kind = target.kind;
        let matches: &[ElementMatch] = match self.gated {
            true => &self.applied,
            false => &self.matches,
        };
        let has_rules = matches.iter().any(|matched| matched.styles(context.index, target));
        let generating = read == PseudoElementRead::Generating;
        let highlight = super::super::publication::pseudo_kind::is_highlight(kind);
        // A pass styles the highlight pseudo-elements of the elements the host shows a highlight
        // of their kind in, and a read of their styles those of any other.
        if generating && highlight && !state.highlight_observers.observes_target(node, target) {
            return PseudoElementOutcome::Unstyled;
        }
        // https://drafts.csswg.org/css-position-4/#backdrop
        // Only an element rendered in the top layer generates a ::backdrop box.
        if generating
            && kind == super::super::publication::pseudo_kind::BACKDROP
            && state.projection.adjustment_facts(node) & bridge::element_adjustment_fact::RENDERED_IN_TOP_LAYER == 0
        {
            return PseudoElementOutcome::Unstyled;
        }
        // https://drafts.csswg.org/css-pseudo-4/#highlight-cascade
        // A highlight pseudo-element no rule styles still inherits the one of an ancestor.
        let highlight_parent = highlight
            .then(|| context.records.highlight_inheritance_parent(node, target))
            .flatten();
        if !read.styles(kind, has_rules || highlight_parent.is_some(), element) {
            return PseudoElementOutcome::Unstyled;
        }
        // A pseudo-element that generates a box and inherits what one that left a record inherited
        // takes that record without a cascade, as an element does.
        let matched_key = (generating && !highlight)
            .then(|| Self::pseudo_element_matched_key(context, node, matches, inherited, kind))
            .flatten();
        if let Some(record) = matched_key.as_ref().and_then(|key| {
            context
                .matched_declarations()
                .find(state, node, Declared::Matched(matches), key, context.root_font())
                .map(|(record, _)| record.clone())
        }) {
            if self.cache_audit.is_some() {
                self.pseudo_element_cache_hits.push(target);
            }
            return PseudoElementOutcome::Computed {
                record,
                element_dependent: false,
            };
        }
        let matched_hash = matched_key.as_ref().map(MatchedKey::hash);
        if context.waits_once && matched_hash.is_some_and(|hash| self.waiting_keys.contains(&hash)) {
            if self.statistics.waiting_pseudo_elements.last() != Some(&node) {
                self.statistics.waiting_pseudo_elements.push(node);
            }
            return PseudoElementOutcome::Waiting;
        }
        self.cascade
            .cascade_pseudo_element(state, context.index, node, matches, target);
        // A pseudo-element's own custom properties resolve over what its element hands down,
        // as a child's do.
        let subject = SubstitutionSubject::new(context, node, Some(kind));
        let custom_properties_read_subject = self
            .cascade
            .custom_winners
            .iter()
            .any(|(_, value)| value_reads_substitution_subject(value.data()));
        let Some(custom_properties) = state.custom_property_store(
            inherited.handed_down.as_ref(),
            inherited.held.as_ref(),
            &self.cascade.custom_winners,
            &mut self.custom_property_stores,
            &subject,
        ) else {
            return PseudoElementOutcome::Declined;
        };
        if generating && !state.pseudo_content_generates(&self.cascade.winners, kind) {
            return PseudoElementOutcome::GeneratesNoBox;
        }
        // https://drafts.css-houdini.org/css-properties-values-api-1/#animation-behavior-of-custom-properties
        // The pseudo-element's other properties substitute the animated values of its custom
        // properties, which the host sampled over the environment it holds, as an element's do.
        let sampled_custom_properties = context.records.sampled_pseudo_element_custom_properties(node, kind);
        let group_seeds = context
            .initial
            .group_seeds(context.records.get_shared(inherited.parent));
        let requested = self.requested();
        // NB: The pseudo-element's custom properties above resolved their style queries against
        //     its element's record, as it computed nothing yet.
        subject.asked_pseudo_element_style.set(false);
        let driven = drive_with_registered_properties(|computed_store| {
            let mut suspended_font = None;
            let substituted = computed_store.unwrap_or(&custom_properties);
            let sampled = sampled_custom_properties
                .as_ref()
                .map(|sampled| sampled.animation_overlay_over(substituted.as_ref()));
            let driven = state.drive_element(
                context.records,
                node,
                Some((inherited.parent, inherited.color)),
                context.initial,
                &self.cascade.winners,
                &super::cascade::CascadeBelow {
                    cascade: &self.cascade,
                    state,
                    node,
                },
                &mut suspended_font,
                &mut self.statistics.random_requests,
                super::custom_properties::CustomPropertyStores {
                    own: sampled.as_ref().or(substituted.as_ref()),
                    inherited: inherited.held.as_ref(),
                    substitutions: &self.substitutions,
                    subject: &subject,
                },
                &mut self.groups,
                &mut self.absolutized,
                target,
                context.root_font(),
                group_seeds,
                None,
            );
            if let Some(request) = suspended_font {
                self.statistics.font_requests.push(request);
            }
            let Some(mut computed) = driven else {
                return Err(match self.pseudo_element_waits(node, requested) {
                    true => PseudoElementOutcome::Waiting,
                    false => PseudoElementOutcome::Declined,
                });
            };
            computed.element_dependent |= custom_properties_read_subject;
            // NB: As for an element, what its sampled custom properties make of `display` is not what ends its animations.
            if sampled.is_some() && computed_display_is_none(&computed) {
                let ignoring_animations = state.drive_element(
                    context.records,
                    node,
                    Some((inherited.parent, inherited.color)),
                    context.initial,
                    &self.cascade.winners,
                    &super::cascade::CascadeBelow {
                        cascade: &self.cascade,
                        state,
                        node,
                    },
                    &mut None,
                    &mut Default::default(),
                    super::custom_properties::CustomPropertyStores {
                        own: substituted.as_ref(),
                        inherited: inherited.held.as_ref(),
                        substitutions: &self.substitutions,
                        subject: &subject,
                    },
                    &mut self.groups,
                    &mut self.absolutized,
                    target,
                    context.root_font(),
                    group_seeds,
                    None,
                );
                computed.reads.display_none_is_animated = ignoring_animations
                    .is_some_and(|ignoring_animations| !computed_display_is_none(&ignoring_animations));
            }
            let registered = state.registered_custom_property_store(
                node,
                inherited.handed_down.as_ref(),
                inherited.held.as_ref(),
                &self.cascade.custom_winners,
                &mut computed,
                &subject,
                &mut self.statistics.random_requests,
            );
            if self.pseudo_element_waits(node, requested) {
                return Err(PseudoElementOutcome::Waiting);
            }
            let computes_again = subject
                .computes_again_against_own_style(&computed.groups, computed.facts.publication_dependency_flags());
            Ok(DrivenWithRegisteredProperties {
                computed,
                registered,
                computes_again,
            })
        });
        let (mut computed, registered) = match driven {
            Ok(driven) => driven,
            Err(outcome) => {
                if let (PseudoElementOutcome::Waiting, Some(hash)) = (&outcome, matched_hash) {
                    self.waiting_keys.push(hash);
                }
                return outcome;
            }
        };
        let custom_properties = registered.unwrap_or(custom_properties);
        let asked_own_style = subject.asked_pseudo_element_style.get();
        let container_reads = subject.container_reads.into_inner();
        let reads_container_style = container_reads.is_some();
        if let Some(reads) = container_reads {
            self.container_reads.get_or_insert_default().add_reads(reads);
        }
        computed.reads.reads_attributes = Self::note_substitution_reads(&mut computed, &self.cascade.custom_winners);
        let storable = !asked_own_style
            && !reads_container_style
            && !self.recascades_monospace_font_size(&computed)
            && Self::reads_nothing_of_the_element(&computed);
        let value_of = |property| computed.groups.value(property);
        let counter_style_environment_identity = state.values_counter_style_environment_identity(
            super::super::computed::ComputedStyleTarget::of_pseudo_element(node, target),
            |property| {
                computed
                    .value_facts
                    .value_read_in_place(property)
                    .unwrap_or_else(|| value_of(property))
            },
        );
        let table_flags = self.shared_table_flags(computed.flags);
        let record = std::sync::Arc::new(Record {
            payloads: Self::interned_rare(
                &mut self.rare_facts,
                RareStyleFacts::of_computation(
                    super::records::InheritableCustomProperties::Held,
                    counter_style_environment_identity,
                    table_flags,
                    computed.value_facts,
                    computed.custom_property_reads,
                    computed.reads,
                    self.cascade.animation_name_scope,
                ),
                computed.groups.adopt_sharing(&[
                    context
                        .records
                        .get_shared(inherited.parent)
                        .map(|parent| &parent.payloads),
                    Some(&context.initial.payloads),
                    self.last_record.as_ref().map(|record| &record.payloads),
                ]),
                &self.cascade.custom_winners,
            )
            .with_custom_properties(custom_properties.as_ref()),
            header: super::records::RecordHeader::new(computed.facts),
        });
        record.settle_holds_image_values();
        if let Some(matched_key) = matched_key
            && storable
            && sampled_custom_properties.is_none()
        {
            let matches = match self.gated {
                true => &self.applied,
                false => &self.matches,
            };
            context.matched_declarations().insert(
                state,
                node,
                Declared::Matched(matches),
                matched_key,
                matched_conditions(context, computed.reads),
                record.clone(),
                &mut self.own_declaration_lookups,
            );
        }
        PseudoElementOutcome::Computed {
            record,
            element_dependent: computed.element_dependent,
        }
    }

    /// Decide the container conditions of the rules `node` matched for the element itself, or
    /// for its pseudo-elements once the element holds its record, which their conditions ask
    /// about first. The matches whose conditions hold are `applied`, and what the conditions read
    /// of the containers is kept in `container_reads`.
    fn apply_container_gates(&mut self, context: &PassContext<'_>, node: StyleNodeID, pseudo_elements: bool) {
        let index = context.index;
        if !index.has_gates() {
            return;
        }
        if !pseudo_elements {
            self.gated = self
                .matches
                .iter()
                .any(|matched| index.gate_of_entry(matched.entry).is_some());
            if !self.gated {
                return;
            }
            self.applied.clear();
            self.applied.extend_from_slice(&self.matches);
        } else if !self.gated {
            return;
        }
        // A rule's entries for one target are adjacent, and decide alike.
        let mut last: Option<(ContainerGateID, bool)> = None;
        let reads = &mut self.container_reads;
        self.applied.retain(|matched| {
            let Some(gate) = index.gate_of_entry(matched.entry) else {
                return true;
            };
            let is_pseudo_element = matched.pseudo_kind != u8::MAX;
            if is_pseudo_element != pseudo_elements {
                return true;
            }
            match last {
                Some((last_gate, holds)) if last_gate == gate => holds,
                _ => {
                    let verdict = gate_verdict(context, gate, node, is_pseudo_element);
                    let holds = verdict.matches;
                    reads.get_or_insert_default().add_reads(verdict);
                    last = Some((gate, holds));
                    holds
                }
            }
        });
    }

    /// Have `node`, whose parent hands down other custom properties than it did and nothing else
    /// it computes from moved, take them without computing again: its own custom properties are
    /// built anew over them, and it keeps its style otherwise, where it substitutes none that
    /// moved. Answers whether it did.
    fn move_custom_properties(&mut self, context: &PassContext<'_>, node: StyleNodeID) -> bool {
        let state = context.state;
        let records = context.records;
        let (Some(parent), Some(old)) = (state.projection.inheritance_parent(node), records.get_shared(node)) else {
            return false;
        };
        let reads = match &old.rare().custom_property_reads {
            LonghandCustomPropertyReads::None => &[][..],
            LonghandCustomPropertyReads::Named(names) => &names[..],
            LonghandCustomPropertyReads::Any => return false,
        };
        // What the element's pseudo-elements and animations compute reads the element's custom
        // properties beyond its own record, as do transitions, which start from the values that
        // moved. A custom property the element declares that is registered computes against the
        // element's own context, where one it only inherits arrives computed.
        if !records.pseudo_element_records(node).is_empty()
            || records.composition(node).is_some()
            || records.sampled_custom_properties(node).is_some()
            || state.projection.adjustment_facts(node) & bridge::element_adjustment_fact::HAS_ANIMATIONS != 0
            || !crate::css::style_compute::transition_delay_and_duration_values_are_single_zero(
                old.longhand(LonghandID::new(
                    crate::css::property_metadata::property_id::TRANSITION_DELAY,
                )),
                old.longhand(LonghandID::new(
                    crate::css::property_metadata::property_id::TRANSITION_DURATION,
                )),
            )
            || old.custom_declarations().is_some_and(|declarations| {
                declarations
                    .iter()
                    .any(|(declared, _)| state.custom_property_name_is_registered(declared.name))
            })
        {
            return false;
        }
        let Some((handed_down, held)) = records.custom_stores_of(parent) else {
            return false;
        };
        let custom_properties = match &old.custom_declarations() {
            Some(declarations) => {
                match state.custom_property_store(
                    handed_down.as_deref(),
                    held.as_deref(),
                    declarations,
                    &mut self.custom_property_stores,
                    &SubstitutionSubject::new(context, node, None),
                ) {
                    Some(store) => store,
                    None => return false,
                }
            }
            None => handed_down.as_deref().cloned(),
        };
        let moved = self
            .moved_names
            .between(old.custom_properties(), custom_properties.as_ref());
        if moved.is_empty() {
            // The element holds the values it held: the move stops here.
            return self.keep_moved(records, node, old.clone());
        }
        if !super::custom_property_moves::reads_none_moved(
            reads,
            &moved,
            old.custom_properties(),
            custom_properties.as_ref(),
        ) {
            return false;
        }
        let inheritable = super::records::InheritableCustomProperties::of(
            custom_properties.as_ref(),
            state.inheritable_custom_property_store(custom_properties.as_ref(), handed_down.as_deref()),
        );
        let record = std::sync::Arc::new(Record {
            payloads: old
                .payloads
                .clone()
                .with_rare_facts(
                    old.rare().with_inheritable_custom_properties(inheritable),
                    old.custom_declarations().cloned(),
                    &[],
                )
                .with_custom_properties(custom_properties.as_ref()),
            header: super::records::RecordHeader::new(old.facts()),
        });
        self.keep_moved(records, node, record)
    }

    fn keep_moved(&mut self, records: &Records<'_>, node: StyleNodeID, record: std::sync::Arc<Record>) -> bool {
        let kept = records.keep_previous_over_moved_custom_properties(node, record);
        if kept {
            self.statistics.moved_custom_properties.push(node);
        }
        kept
    }

    fn children_are_styled(context: &PassContext<'_>, node: StyleNodeID) -> bool {
        // The descendants of an element that is not displayed have no style to compute.
        !context
            .records
            .assigned_record_view(node)
            .is_none_or(|view| view.longhands().effective_display(None).is_none())
    }

    /// Compute the style of `node` from its cascade into a record.
    fn compute(
        &mut self,
        context: &PassContext<'_>,
        node: StyleNodeID,
        key: Option<MatchedKey<'_>>,
    ) -> Result<(std::sync::Arc<Record>, ComputedFacts), Outcome> {
        let state = context.state;
        // An element that matched other rules than one styled before, whose cascade picked the same
        // declarations all the same, takes its record, and the next to match what it did takes it
        // without a cascade.
        let picked_key = key
            .as_ref()
            .map(|key| key.for_picked(&self.cascade.winners, &self.cascade.custom_winners));
        if let (Some(key), Some(picked_key)) = (key.as_ref(), picked_key.as_ref()) {
            let declared = Declared::Picked {
                winners: &self.cascade.winners,
                custom_winners: &self.cascade.custom_winners,
                animation_name_scope: self.cascade.animation_name_scope,
            };
            if let Some((record, conditions)) =
                context
                    .matched_declarations()
                    .find(state, node, declared, picked_key, context.root_font())
            {
                let record = record.clone();
                let matches = match self.gated {
                    true => &self.applied,
                    false => &self.matches,
                };
                context.matched_declarations().insert(
                    state,
                    node,
                    Declared::Matched(matches),
                    *key,
                    conditions,
                    record.clone(),
                    &mut self.own_declaration_lookups,
                );
                return Ok((
                    record,
                    ComputedFacts {
                        directly: false,
                        element_dependent: false,
                        reused: true,
                    },
                ));
            }
        }
        let inheritance_parent = state.projection.inheritance_parent(node);
        let parent = match inheritance_parent {
            Some(parent) => match context.records.current_color_of(parent) {
                Some(color) => Some((parent, color)),
                None => return Err(Outcome::ComputeDeclined),
            },
            None => None,
        };
        // The custom properties the parent hands down, and the ones it holds, which an inheriting
        // lookup reads.
        let (parent_store, parent_held) = match inheritance_parent {
            Some(parent) => match context.records.custom_stores_of(parent) {
                Some(stores) => stores,
                None => return Err(Outcome::ComputeDeclined),
            },
            None => (None, None),
        };
        let subject = SubstitutionSubject::new(context, node, None);
        // A custom property substituting something of the element itself is the element's alone.
        let custom_properties_read_subject = self
            .cascade
            .custom_winners
            .iter()
            .any(|(_, value)| value_reads_substitution_subject(value.data()));
        let Some(custom_properties) = state.custom_property_store(
            parent_store.as_deref(),
            parent_held.as_deref(),
            &self.cascade.custom_winners,
            &mut self.custom_property_stores,
            &subject,
        ) else {
            return Err(Outcome::ComputeDeclined);
        };

        // https://drafts.css-houdini.org/css-properties-values-api-1/#animation-behavior-of-custom-properties
        // The element's other properties substitute the animated values of its custom properties,
        // which the host sampled over the environment the element holds. Where this pass moves
        // the element's own custom properties, they lie over what it resolved, and the host
        // samples its animations over the record the pass installs.
        let sampled_custom_properties = context.records.sampled_custom_properties(node).flatten();

        let quirks_color = if state.is_quirks_table(node) {
            let requested = self.requested();
            let body = context.quirks_table_color(node);
            self.statistics.font_requests.extend(body.fonts.iter().cloned());
            self.statistics
                .random_requests
                .extend(body.random_bases.iter().map(|request| RandomBaseRequest {
                    node: request.node,
                    name: request.name.clone(),
                    element_shared: request.element_shared,
                }));
            if self.waits(node, requested) || body.color.is_none() {
                return Err(Outcome::ComputeDeclined);
            }
            body.color.as_ref()
        } else {
            None
        };

        let group_seeds = context
            .initial
            .group_seeds(parent.and_then(|(parent, _)| context.records.get_shared(parent)));
        let requested = self.requested();
        let (mut computed, registered) = drive_with_registered_properties(|computed_store| {
            let mut suspended_font = None;
            let substituted = computed_store.unwrap_or(&custom_properties);
            let sampled = sampled_custom_properties
                .as_ref()
                .map(|sampled| sampled.animation_overlay_over(substituted.as_ref()));
            let driven = state.drive_element(
                context.records,
                node,
                parent,
                context.initial,
                &self.cascade.winners,
                &super::cascade::CascadeBelow {
                    cascade: &self.cascade,
                    state,
                    node,
                },
                &mut suspended_font,
                &mut self.statistics.random_requests,
                super::custom_properties::CustomPropertyStores {
                    own: sampled.as_ref().or(substituted.as_ref()),
                    inherited: parent_held.as_deref(),
                    substitutions: &self.substitutions,
                    subject: &subject,
                },
                &mut self.groups,
                &mut self.absolutized,
                super::PseudoElementTarget::of_kind(super::rules::ELEMENT_PSEUDO_KIND),
                context.root_font(),
                group_seeds,
                quirks_color,
            );
            if let Some(request) = suspended_font {
                self.statistics.font_requests.push(request);
            }
            self.waits(node, requested);
            let Some(mut computed) = driven else {
                return Err(Outcome::ComputeDeclined);
            };
            computed.element_dependent |= custom_properties_read_subject;
            // https://drafts.csswg.org/css-animations-2/#owning-element-section
            // If the display property of an element is set to none and its display value would compute to none when
            // ignoring the Transitions and Animations cascade origins, then terminate running animations with this
            // owning element.
            // NB: The sampled custom properties are part of the Animations origin, so what they make of `display` is
            //     not what ends the animations. Only an element they leave with none computes this again.
            if sampled.is_some() && computed_display_is_none(&computed) {
                let ignoring_animations = state.drive_element(
                    context.records,
                    node,
                    parent,
                    context.initial,
                    &self.cascade.winners,
                    &super::cascade::CascadeBelow {
                        cascade: &self.cascade,
                        state,
                        node,
                    },
                    &mut None,
                    &mut Default::default(),
                    super::custom_properties::CustomPropertyStores {
                        own: substituted.as_ref(),
                        inherited: parent_held.as_deref(),
                        substitutions: &self.substitutions,
                        subject: &subject,
                    },
                    &mut self.groups,
                    &mut self.absolutized,
                    super::PseudoElementTarget::of_kind(super::rules::ELEMENT_PSEUDO_KIND),
                    context.root_font(),
                    group_seeds,
                    quirks_color,
                );
                computed.reads.display_none_is_animated = ignoring_animations
                    .is_some_and(|ignoring_animations| !computed_display_is_none(&ignoring_animations));
            }
            let registered = state.registered_custom_property_store(
                node,
                parent_store.as_deref(),
                parent_held.as_deref(),
                &self.cascade.custom_winners,
                &mut computed,
                &subject,
                &mut self.statistics.random_requests,
            );
            if self.waits(node, requested) {
                return Err(Outcome::ComputeDeclined);
            }
            Ok(DrivenWithRegisteredProperties {
                computed,
                registered,
                computes_again: false,
            })
        })?;
        let custom_properties = registered.unwrap_or(custom_properties);
        let container_reads = subject.container_reads.into_inner();
        let reads_container_style = container_reads.is_some();
        if let Some(reads) = container_reads {
            self.container_reads.get_or_insert_default().add_reads(reads);
        }
        computed.reads.reads_attributes = Self::note_substitution_reads(&mut computed, &self.cascade.custom_winners);
        let storable = !reads_container_style
            && !self.recascades_monospace_font_size(&computed)
            && Self::reads_nothing_of_the_element(&computed);
        let value_of = |property| computed.groups.value(property);
        let counter_style_environment_identity = state.values_counter_style_environment_identity(
            super::super::computed::ComputedStyleTarget::new(node, u8::MAX),
            |property| {
                computed
                    .value_facts
                    .value_read_in_place(property)
                    .unwrap_or_else(|| value_of(property))
            },
        );
        let table_flags = self.shared_table_flags(computed.flags);
        let record = std::sync::Arc::new(Record {
            header: super::records::RecordHeader::new(computed.facts),
            payloads: Self::interned_rare(
                &mut self.rare_facts,
                RareStyleFacts::of_computation(
                    super::records::InheritableCustomProperties::of(
                        custom_properties.as_ref(),
                        state.inheritable_custom_property_store(custom_properties.as_ref(), parent_store.as_deref()),
                    ),
                    counter_style_environment_identity,
                    table_flags,
                    computed.value_facts,
                    computed.custom_property_reads,
                    computed.reads,
                    self.cascade.animation_name_scope,
                ),
                computed.groups.adopt_sharing(&[
                    parent
                        .and_then(|(parent, _)| context.records.get_shared(parent))
                        .map(|parent| &parent.payloads),
                    Some(&context.initial.payloads),
                    self.last_record.as_ref().map(|record| &record.payloads),
                ]),
                &self.cascade.custom_winners,
            )
            .with_custom_properties(custom_properties.as_ref()),
        });
        self.last_record = Some(record.clone());
        let record = match context.records.held_before_holding_the_same_style_as(node, &record) {
            Some(held) => held.clone(),
            None => {
                record.settle_holds_image_values();
                record
            }
        };
        // Only what the key and the conditions of an entry name is read of what may differ between
        // the elements that find it.
        if let Some(key) = key
            && storable
        {
            let conditions = matched_conditions(context, computed.reads);
            let matches = match self.gated {
                true => &self.applied,
                false => &self.matches,
            };
            let cache = context.matched_declarations();
            let lookups = &mut self.own_declaration_lookups;
            cache.insert(
                state,
                node,
                Declared::Matched(matches),
                key,
                conditions,
                record.clone(),
                lookups,
            );
            if let Some(picked_key) = picked_key {
                let declared = Declared::Picked {
                    winners: &self.cascade.winners,
                    custom_winners: &self.cascade.custom_winners,
                    animation_name_scope: self.cascade.animation_name_scope,
                };
                cache.insert(state, node, declared, picked_key, conditions, record.clone(), lookups);
            }
        }
        Ok((
            record,
            ComputedFacts {
                directly: computed.directly,
                element_dependent: computed.element_dependent,
                reused: false,
            },
        ))
    }

    /// How many fonts and random base values the elements styled so far waited on.
    fn requested(&self) -> (usize, usize) {
        (
            self.statistics.font_requests.len(),
            self.statistics.random_requests.len(),
        )
    }

    /// Whether the style of `node` waits on a font or a random base value requested since
    /// `requested`, noting that it does.
    fn waits(&mut self, node: StyleNodeID, requested: (usize, usize)) -> bool {
        let waits = self.requested() != requested;
        if waits && self.statistics.waiting.last() != Some(&node) {
            self.statistics.waiting.push(node);
        }
        waits
    }

    /// Whether the style of a pseudo-element of `node` waits on a font or a random base value
    /// requested since `requested`, noting that it does.
    fn pseudo_element_waits(&mut self, node: StyleNodeID, requested: (usize, usize)) -> bool {
        let waits = self.requested() != requested;
        if waits && self.statistics.waiting_pseudo_elements.last() != Some(&node) {
            self.statistics.waiting_pseudo_elements.push(node);
        }
        waits
    }

    /// Note in `computed` what the substitutions of its winners and of the custom properties
    /// `custom_winners` it declares read beyond its custom properties, and answer whether they
    /// read its attributes.
    fn note_substitution_reads(
        computed: &mut super::compute::ComputedElement,
        custom_winners: &[(CustomDeclaration, RetainedStyleValueData)],
    ) -> bool {
        use super::super::bridge::FfiNodeRecordReads;
        let reads = custom_winners
            .iter()
            .fold(computed.substitution_reads, |reads, (_, value)| {
                reads | super::super::custom_property_cascade::substitution_reads(value.data())
            });
        // https://drafts.csswg.org/css-values-5/#if-notation
        // The media features an `if()` or a custom function reads move with the viewport, which
        // restyles the element, and the lengths its style queries compare may count the element's
        // siblings.
        if reads & (FfiNodeRecordReads::IfFunction as u8 | FfiNodeRecordReads::CustomFunction as u8) != 0 {
            computed
                .facts
                .add_dependency_flags(crate::css::computed_longhand_table::DEPENDS_ON_VIEWPORT_METRICS);
            computed.reads.reads_sibling_position = true;
            computed.reads.environment_reads |= crate::css::style::input_types::StyleEnvironment::MEDIA_FEATURES
                | crate::css::style::input_types::StyleEnvironment::VIEWPORT;
        }
        reads & FfiNodeRecordReads::Attributes as u8 != 0
    }

    /// What `node`, which just matched, is looked for by in the matched declarations cache, where
    /// its record may be one computed before: it has a parent this pass or one before it styled,
    /// and neither reads the animated values a record does not hold.
    fn matched_key<'c>(
        &mut self,
        context: &PassContext<'c>,
        node: StyleNodeID,
        backed: Option<(u8, StyleNodeID)>,
    ) -> Option<MatchedKey<'c>> {
        if !context.uses_caches || backed.is_some() || context.state.is_quirks_table(node) {
            return None;
        }
        let state = context.state;
        // Elements styled by rules alone repeat; one with its own declarations mostly does not.
        let declares_own = matched_declarations::declares_own(state, node);
        if declares_own && !self.own_declaration_lookups.worth_looking() {
            return None;
        }
        let facts = state.projection.facts(node)?;
        let parent = state.projection.inheritance_parent(node)?;
        let records = context.records;
        let parent_record = records.get_shared(parent)?;
        // A parent that generates no box makes the box its child would be in an ancestor's.
        let parent_display = parent_record.facts().display();
        if parent_display.is_contents() {
            return None;
        }
        // What a child computes under the composition of its parent's animations is not the
        // parent's record's alone, and the custom properties animations sample are no record's.
        if records.composition(parent).is_some()
            || records.sampled_custom_properties(parent).flatten().is_some()
            || records.sampled_custom_properties(node).flatten().is_some()
        {
            return None;
        }
        let (parent_store, parent_held) = records.custom_stores_of(parent)?;
        let matches = match self.gated {
            true => &self.applied,
            false => &self.matches,
        };
        Some(MatchedKey::new(
            state,
            node,
            matches,
            MatchedContext {
                parent_inherited_groups: std::array::from_fn(|group| parent_record.payloads[group].as_ptr() as usize),
                parent_color: parent_record.payloads.current_color(),
                parent_inherited_values: parent_record.payloads.inherited_values(),
                parent_dependency_flags: parent_record.facts().publication_dependency_flags(),
                parent_display,
                parent_custom_properties: [
                    custom_store_identity(parent_store.as_deref()),
                    custom_store_identity(parent_held.as_deref()),
                ],
                pseudo_kind: u8::MAX,
                adjustment_facts: state.projection.adjustment_facts(node),
                language: facts.language(),
                directionality: facts.directionality(),
                tree_scope: state.projection.tree_scope(node),
                counter_style_environment: state.counter_style_environment_identity_of(node),
            },
            parent_record,
            declares_own,
        ))
    }

    /// What the pseudo-element of `kind` of `node`, inheriting `inherited`, is looked for by in the
    /// matched declarations cache, where its record may be one computed before.
    fn pseudo_element_matched_key<'k>(
        context: &PassContext<'_>,
        node: StyleNodeID,
        matches: &[ElementMatch],
        inherited: &PseudoElementInheritance<'k>,
        kind: u8,
    ) -> Option<MatchedKey<'k>> {
        if !context.uses_caches {
            return None;
        }
        // An element that generates no box makes the box its pseudo-element would be in an ancestor's.
        if inherited.record.facts().display().is_contents() {
            return None;
        }
        let state = context.state;
        let facts = state.projection.facts(node)?;
        let records = context.records;
        // NB: Only where these are absent is what `inherited` names held by `inherited.record`, which
        //     the entry holds to keep those addresses from going to others.
        if records.composition(inherited.parent).is_some()
            || records.sampled_custom_properties(inherited.parent).flatten().is_some()
            || records.sampled_pseudo_element_custom_properties(node, kind).is_some()
        {
            return None;
        }
        Some(MatchedKey::new(
            state,
            node,
            matches,
            MatchedContext {
                parent_inherited_groups: inherited.groups,
                parent_color: inherited.color,
                parent_inherited_values: inherited.inherited_values,
                parent_dependency_flags: inherited.dependency_flags,
                parent_display: inherited.record.facts().display(),
                parent_custom_properties: [
                    custom_store_identity(inherited.handed_down.as_ref()),
                    custom_store_identity(inherited.held.as_ref()),
                ],
                pseudo_kind: kind,
                adjustment_facts: state.projection.adjustment_facts(node),
                language: facts.language(),
                directionality: facts.directionality(),
                tree_scope: state.projection.tree_scope(node),
                counter_style_environment: state.counter_style_environment_identity_of(node),
            },
            inherited.record,
            false,
        ))
    }

    /// Whether a winning `font-family: monospace` recascaded the font size of `computed` over the
    /// whole ancestor chain, which the key of the matched declarations cache does not name.
    fn recascades_monospace_font_size(&self, computed: &super::compute::ComputedElement) -> bool {
        use crate::css::property_metadata::property_id::FONT_FAMILY;
        self.cascade.winners.iter().any(|winner| winner.property == FONT_FAMILY)
            && computed
                .groups
                .value(FONT_FAMILY)
                .optional_data()
                .is_some_and(crate::css::style_compute::font_family_is_monospace)
    }

    /// Whether `computed` read nothing of its element that the key and the conditions of the
    /// matched declarations cache do not name.
    fn reads_nothing_of_the_element(computed: &super::compute::ComputedElement) -> bool {
        !computed.element_dependent
            && !computed.reads.reads_sibling_position
            && !computed.reads.reads_attributes
            && computed.reads.container_units == 0
            && !computed.reads.display_none_is_animated
    }

    /// The record the matched declarations cache holds for the element `key` looks for.
    fn find_matched_declarations(
        &mut self,
        context: &PassContext<'_>,
        node: StyleNodeID,
        key: &MatchedKey<'_>,
    ) -> Option<std::sync::Arc<Record>> {
        let matches = match self.gated {
            true => &self.applied,
            false => &self.matches,
        };
        let found = context
            .matched_declarations()
            .find(
                context.state,
                node,
                Declared::Matched(matches),
                key,
                context.root_font(),
            )
            .map(|(record, _)| record.clone());
        if key.declares_own() {
            self.own_declaration_lookups.note_answer(found.is_some());
        }
        found
    }
}

/// The identity of a store of custom properties a parent holds, which an entry of the matched
/// declarations cache holds the parent's record for.
fn custom_store_identity(store: Option<&std::sync::Arc<crate::css::custom_properties::CustomPropertyStore>>) -> usize {
    store.map_or(0, |store| std::sync::Arc::as_ptr(store) as usize)
}

/// What a computation that read `reads` needs to hold still for an entry of the matched declarations
/// cache to answer for another element.
fn matched_conditions(context: &PassContext<'_>, reads: super::records::ComputationReads) -> MatchedConditions {
    MatchedConditions {
        root_font: (reads.reads_root_font != RootFontReads::None).then(|| context.root_font()),
        reads_parent_reset_values: reads.parent_reset_groups != 0,
    }
}

/// What the container conditions of `gate` say for `subject`, or for its pseudo-elements, over the
/// records of the pass, which hold the containers above the subject.
///
/// NB: Conditions the records cannot decide, as about a container whose record holds no font to
///     resolve a length against, do not hold, and read nothing the host restyles the subject for.
/// The query containers of the elements a pass computes, as its records have them, with the font
/// of the document element as the pass computed it, which a `rem` in a query resolves against.
struct PassContainerRecords<'c, 'p>(&'c PassContext<'p>);

impl ContainerRecords for PassContainerRecords<'_, '_> {
    fn query_row(&self, node: StyleNodeID) -> Option<super::super::tree::ContainerQueryInputs<'_>> {
        self.0.records.query_row(node)
    }

    fn any_element_row(&self, node: StyleNodeID) -> Option<super::super::tree::ContainerQueryInputs<'_>> {
        self.0.records.any_element_row(node)
    }

    fn record_view(
        &self,
        node: StyleNodeID,
        row: super::super::tree::ContainerQueryInputs<'_>,
    ) -> Option<super::super::computed::StyleRecordView<'_>> {
        self.0.records.record_view(node, row)
    }

    fn custom_store(
        &self,
        node: StyleNodeID,
        row: super::super::tree::ContainerQueryInputs<'_>,
    ) -> Option<Option<&crate::css::custom_properties::CustomPropertyStore>> {
        self.0.records.custom_store(node, row)
    }

    fn root_font(&self) -> Option<RootFontInputs> {
        self.0.root_font()
    }
}

/// What a substitution for an element, or one of its pseudo-elements, reads of it in a pass.
struct SubstitutionSubject<'c, 'p> {
    context: &'c PassContext<'p>,
    node: StyleNodeID,
    pseudo_kind: Option<u8>,
    /// What the container conditions of the custom functions the element calls read of its
    /// containers.
    container_reads: std::cell::RefCell<Option<ContainerVerdict>>,
    /// The groups and dependency flags a pseudo-element computed, which its style queries resolve
    /// against once it computed them, and whether a style query asked for them before.
    pseudo_element_style: std::cell::OnceCell<(crate::css::computed_values::StyleGroups, u8)>,
    asked_pseudo_element_style: std::cell::Cell<bool>,
}

impl<'c, 'p> SubstitutionSubject<'c, 'p> {
    fn new(context: &'c PassContext<'p>, node: StyleNodeID, pseudo_kind: Option<u8>) -> Self {
        Self {
            context,
            node,
            pseudo_kind,
            container_reads: std::cell::RefCell::new(None),
            pseudo_element_style: std::cell::OnceCell::new(),
            asked_pseudo_element_style: std::cell::Cell::new(false),
        }
    }

    /// Whether a style query of the pseudo-element asked for the style it computes before it
    /// computed it, and so resolved against its element's: answering it with the style it computed
    /// now, `groups` with `dependency_flags`, it computes again.
    fn computes_again_against_own_style(
        &self,
        groups: &crate::css::computed_values::ComputedGroups,
        dependency_flags: u8,
    ) -> bool {
        if !self.asked_pseudo_element_style.take() || self.pseudo_element_style.get().is_some() {
            return false;
        }
        // SAFETY: The pseudo-element's groups are live payloads of each group.
        let groups =
            unsafe { crate::css::computed_values::StyleGroups::retained(groups.pointers(), groups.style_bits()) };
        let _ = self.pseudo_element_style.set((groups, dependency_flags));
        true
    }
}

impl SubstitutingSubject for SubstitutionSubject<'_, '_> {
    fn target(&self) -> (StyleNodeID, Option<u8>) {
        (self.node, self.pseudo_kind)
    }

    /// The conditions compute against the style the element inherits: its parent's record. A
    /// pseudo-element's compute against the style it computes itself, its own font for its
    /// lengths: until it computed that, against its element's record, which this pass computed
    /// before it.
    fn style_query_inputs(&self) -> Option<crate::css::cascaded_properties::StyleQueryInputs> {
        let state = self.context.state;
        let records = self.context.records;
        let pseudo_element_style = self.pseudo_element_style.get();
        let view = match self.pseudo_kind {
            Some(_) => match pseudo_element_style {
                Some((payloads, dependency_flags)) => Some((payloads, *dependency_flags)),
                None => {
                    self.asked_pseudo_element_style.set(true);
                    records
                        .assigned_record_view(self.node)
                        .map(|view| (view.payloads, view.dependency_flags))
                }
            },
            None => state
                .projection
                .inheritance_parent(self.node)
                .and_then(|parent| records.assigned_record_view(parent))
                .map(|view| (view.payloads, view.dependency_flags)),
        };
        let mut inputs = state.style_query_inputs_of(view, self.node)?;
        // A `rem` resolves against the document element's font as the pass computed it.
        if let Some(root_font) = self.context.root_font() {
            (
                inputs.length.root_font_metrics,
                inputs.length.root_font_metrics_depend_on_viewport_metrics,
            ) = root_font.font_metrics();
        }
        Some(inputs)
    }

    /// A pseudo-element's container-relative lengths resolve against its element's query
    /// containers, the element itself included.
    fn apply_container_unit_bases(&self, length: &mut crate::css::style_compute::FfiLengthResolutionContext) {
        self.context
            .state
            .container_unit_bases_with(
                &PassContainerRecords(self.context),
                self.node,
                self.pseudo_kind.is_some(),
            )
            .apply_to(length);
    }

    fn custom_functions(&self) -> Option<PreparedCustomFunctions<'_>> {
        let state = self.context.state;
        let mut functions =
            state.prepare_custom_functions_with(&state.document_functions, self.node, |containers| {
                Some(
                    state
                        .container_conditions_verdict_with(
                            &PassContainerRecords(self.context),
                            containers,
                            self.node,
                            self.pseudo_kind.is_some(),
                        )
                        .unwrap_or_else(|| undecided_verdict(containers)),
                )
            })?;
        if let Some(effects) = functions.container_effects.take() {
            self.container_reads
                .borrow_mut()
                .get_or_insert_default()
                .add_reads(effects);
        }
        Some(functions)
    }
}

/// What container conditions the engine cannot decide for their subject read: they do not hold.
fn undecided_verdict(
    conditions: &[std::sync::Arc<crate::css::container_conditions::ContainerConditionsData>],
) -> ContainerVerdict {
    ContainerVerdict {
        matches: false,
        depends_on_size: conditions.iter().any(|conditions| conditions.contains_size_feature()),
        depends_on_style: super::super::container_queries::conditions_ask_container_style(conditions),
        ..Default::default()
    }
}

fn gate_verdict(
    context: &PassContext<'_>,
    gate: ContainerGateID,
    subject: StyleNodeID,
    subject_is_pseudo_element: bool,
) -> ContainerVerdict {
    let conditions = context.index.gate(gate);
    context
        .state
        .container_conditions_verdict_with(
            &PassContainerRecords(context),
            conditions,
            subject,
            subject_is_pseudo_element,
        )
        .unwrap_or_else(|| undecided_verdict(conditions))
}

/// What a pseudo-element inherits from its originating element.
struct PseudoElementInheritance<'r> {
    /// The element inherited from: the originating element, or for a `::first-letter` the element
    /// containing the letter.
    parent: StyleNodeID,
    /// The record of that element.
    record: &'r std::sync::Arc<Record>,
    /// The inherited longhands that element holds as bits, and its effective zoom.
    inherited_values: crate::css::computed_value_types::InheritedSetValues,
    /// What a view of that element says its style depends on.
    dependency_flags: u8,
    groups: [usize; super::super::computed::ENGINE_INHERITED_GROUP_COUNT],
    color: u32,
    /// The custom properties the element hands down, and the ones it holds.
    handed_down: super::records::HeldCustomProperties,
    held: super::records::HeldCustomProperties,
}

impl<'r> PseudoElementInheritance<'r> {
    // https://drafts.csswg.org/css-pseudo-4/#treelike
    // A pseudo-element inherits from its originating element, the animated values of the
    // composition the host lays over the element's record included, as a child does.
    fn of(context: &PassContext<'_>, node: StyleNodeID, element: &'r std::sync::Arc<Record>) -> Self {
        let composition = context.records.composition(node);
        let inherited_payloads: &crate::css::computed_values::StyleGroups = match &composition {
            Some(view) => view.payloads,
            None => &element.payloads,
        };
        let (handed_down, held) = match context.records.custom_stores_of(node) {
            Some((handed_down, held)) => (handed_down.map(Cow::into_owned), held.map(Cow::into_owned)),
            None => (
                element.inheritable_custom_properties().cloned(),
                element.custom_properties().cloned(),
            ),
        };
        Self {
            parent: node,
            record: element,
            inherited_values: inherited_payloads.inherited_values(),
            dependency_flags: composition.as_ref().map_or_else(
                || element.facts().publication_dependency_flags(),
                |view| view.dependency_flags,
            ),
            groups: std::array::from_fn(|group| inherited_payloads[group].as_ptr() as usize),
            color: context
                .records
                .current_color_of(node)
                .unwrap_or(element.payloads.current_color()),
            handed_down,
            held,
        }
    }
}

/// Which pseudo-elements a computation styles.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum PseudoElementRead {
    /// Those that generate a box: a `::marker` of a list item, and a `::before` or `::after` a rule
    /// styles whose content generates something.
    Generating,
    /// Those a rule styles, and the `::marker` of a list item, whether or not they generate a box.
    Styled,
    /// Any, as a read of what a pseudo-element computes to does whether or not a rule styles it.
    Any,
}

impl PseudoElementRead {
    /// Whether the read styles the pseudo-element of `kind` of an element whose record is
    /// `element`, where `has_rules` says whether a rule matched for it, or, for a highlight
    /// pseudo-element, whether it inherits one of an ancestor.
    fn styles(self, kind: u8, has_rules: bool, element: &Record) -> bool {
        use super::super::publication::pseudo_kind::MARKER;
        // A list item generates a marker whatever rules match for it, and no other element does.
        let list_item_marker = kind == MARKER && element.facts().display().is_list_item();
        match self {
            Self::Generating => match kind {
                MARKER => list_item_marker,
                _ => has_rules,
            },
            Self::Styled => has_rules || list_item_marker,
            Self::Any => true,
        }
    }
}

/// What came of computing the style of a pseudo-element.
enum PseudoElementOutcome {
    Computed {
        record: std::sync::Arc<Record>,
        /// Whether the record computes against the element itself, such as its place among its
        /// siblings.
        element_dependent: bool,
    },
    /// No rule styles it.
    Unstyled,
    /// A `::before` or `::after` whose content generates nothing.
    GeneratesNoBox,
    /// The style waits on fonts or random base values the pass requested.
    Waiting,
    /// The style needed what the pass does not do.
    Declined,
}

/// What came of computing an element besides its record.
struct ComputedFacts {
    directly: bool,
    element_dependent: bool,
    /// Whether the record is one computed for an element before, reused.
    reused: bool,
}

// SAFETY: A thread's scratch moves to the thread that styles with it and stays there; its payload
// buffer holds no reference between elements.
unsafe impl Send for Scratch<'_> {}

/// The scratch of every thread of the pool, plus one for a traversal on the calling thread.
struct Scratches<'a> {
    pool: Option<&'a rayon::ThreadPool>,
    slots: Vec<Mutex<Option<Scratch<'a>>>>,
}

impl<'a> Scratches<'a> {
    fn with<R>(&self, context: &PassContext<'a>, run: impl FnOnce(&mut Scratch<'a>) -> R) -> R {
        let index = self
            .pool
            .and_then(rayon::ThreadPool::current_thread_index)
            .unwrap_or(self.slots.len() - 1);
        let mut slot = self.slots[index]
            .lock()
            .expect("a thread's scratch is only ever held by that thread");
        run(slot.get_or_insert_with(|| {
            let mut scratch = Scratch::new(context);
            if context.uses_caches
                && let Some(cache) = context.group_caches.and_then(|caches| caches.get(index))
            {
                scratch.groups = std::mem::take(&mut *cache.lock().expect("no thread panicked while styling"));
            }
            scratch
        }))
    }
}

/// `hint`, rematching what it restyles against the whole rule index.
pub(super) fn rematched_in_full(hint: HintBits) -> HintBits {
    let mut hint = hint;
    if hint & RESTYLE_SELF != 0 {
        hint |= MATCH_SELF;
    }
    if hint & RESTYLE_DESCENDANTS != 0 {
        hint |= MATCH_DESCENDANTS;
    }
    hint
}

/// An element to style, with the hint its ancestors pass it and the entries they restyle it for.
#[derive(Clone)]
pub(super) struct Queued {
    node: StyleNodeID,
    hint: HintBits,
    reasons: Option<super::records::MatchList>,
}

/// Hands the elements of `given` and their descendants to other tasks, a work unit each.
fn hand_off<'scope, 'a: 'scope>(
    scope: &rayon::ScopeFifo<'scope>,
    context: &'scope PassContext<'a>,
    scratches: &'scope Scratches<'a>,
    mut given: VecDeque<Queued>,
) {
    while !given.is_empty() {
        let unit: VecDeque<Queued> = given.drain(..WORK_UNIT.min(given.len())).collect();
        scope.spawn_fifo(move |scope| {
            style_trees(Some(scope), context, scratches, unit, 0, usize::MAX);
        });
    }
}

/// Style the elements of `discovered` and their descendants breadth-first, handing the children
/// discovered at each level beyond `local_queue_size` elements to other tasks once they make up at
/// least a work unit. Without a scope to hand them to, stops once `hand_off_size` elements are
/// queued, and answers those.
fn style_trees<'scope, 'a: 'scope>(
    scope: Option<&rayon::ScopeFifo<'scope>>,
    context: &'scope PassContext<'a>,
    scratches: &'scope Scratches<'a>,
    mut discovered: VecDeque<Queued>,
    local_queue_size: usize,
    hand_off_size: usize,
) -> VecDeque<Queued> {
    scratches.with(context, |scratch| {
        let mut remaining_at_current_depth = discovered.len();
        while let Some(Queued {
            node,
            hint: inherited,
            reasons: inherited_reasons,
        }) = discovered.pop_front()
        {
            context.state.element_declarations.prefetch([
                discovered.front().map(|queued| queued.node),
                discovered.get(1).map(|queued| queued.node),
                discovered.get(2).map(|queued| queued.node),
            ]);
            // What the element's ancestors ask of it, and what it asks of its descendants.
            let mut hint = context.hint(node) | inherited;
            // An element reading the containers above it reads them anew where one moved.
            if hint & (CONTAINERS_MOVED | RESTYLE_SELF) == CONTAINERS_MOVED && context.records.reads_containers(node) {
                hint |= RESTYLE_SELF;
            }
            // An element reading the root element's font reads it anew where it moved.
            if hint & (ROOT_FONT_MOVED | RESTYLE_SELF) == ROOT_FONT_MOVED && context.reads_root_font_move(node) {
                hint |= RESTYLE_SELF;
            }
            // An element that takes the custom properties its parent hands down now without
            // computing again hands its own down in turn.
            let moved_custom_properties = hint & (PARENT_CUSTOM_PROPERTIES_MOVED | RESTYLES_SELF)
                == PARENT_CUSTOM_PROPERTIES_MOVED
                && scratch.move_custom_properties(context, node);
            if moved_custom_properties {
                scratch.audit_caches(context, node, cache_audit::Reuse::MovedCustomProperties);
            }
            if hint & PARENT_CUSTOM_PROPERTIES_MOVED != 0 && !moved_custom_properties {
                hint |= RESTYLE_SELF | PARENT_CHANGED;
            }
            if hint & RESTYLE_PSEUDO_ELEMENTS != 0 {
                if hint & RESTYLE_SELF == 0 {
                    scratch.statistics.restyled_for_pseudo_elements.push(node);
                }
                hint |= RESTYLE_SELF | PARENT_CHANGED;
            }
            let restyled = hint & RESTYLE_SELF != 0;
            let styles_children = match restyled {
                true => {
                    let own = context.reasons.and_then(|reasons| Reasons::of(reasons.own, node));
                    let rematch = union(inherited_reasons.clone(), own.as_ref());
                    let reasons = match hint & MATCH_SELF {
                        0 => Some(rematch.as_ref().map_or(&[][..], |rematch| &rematch[..])),
                        _ => None,
                    };
                    scratch.style_element(
                        context,
                        node,
                        reasons,
                        hint & (PARENT_CHANGED | PARENT_RESTYLED | CONTAINERS_MOVED | RECOMPUTE_SELF),
                    )
                }
                false => Scratch::children_are_styled(context, node),
            };
            // An element waiting on a font or a random base value is styled in the next round of
            // a pass that suspends, with what its ancestors ask of it.
            if restyled
                && context.suspends
                && !styles_children
                && scratch.statistics.waiting.last() == Some(&node)
                && !context.records.computed(node)
            {
                scratch.statistics.suspended.push(Suspended::Element(Queued {
                    node,
                    hint: inherited,
                    reasons: inherited_reasons.clone(),
                }));
            }
            let record_moved = restyled || moved_custom_properties;
            // Every element under a document element whose font moved that resolves root-relative
            // lengths against what moved resolves them again. What the others inherit moves with
            // their parents.
            if restyled && context.root_font_moved() != RootFontReads::None && is_document_element(context.state, node)
            {
                hint |= ROOT_FONT_MOVED;
            }
            // The elements of a shadow tree standing for its host's pseudo-elements are styled
            // from the host's matches for them.
            if restyled && std::mem::take(&mut scratch.backing_elements_moved) {
                hint |= RESTYLE_DESCENDANTS | MATCH_DESCENDANTS;
            }
            // The elements below one displayed again were not styled while it was not, whatever
            // changed for them meanwhile.
            if restyled && context.records.revealed(node) {
                hint |= RESTYLE_DESCENDANTS | MATCH_DESCENDANTS;
            }
            if styles_children {
                // The elements below one that describes another container than it did, or below
                // such an element, read their containers anew.
                let containers_moved = match hint & CONTAINERS_MOVED != 0
                    || record_moved
                        && context
                            .records
                            .container_moved(node, context.index.gates_ask_container_style)
                {
                    true => CONTAINERS_MOVED,
                    false => 0,
                };
                let impact = match record_moved && hint & RESTYLE_DESCENDANTS == 0 {
                    true => context.records.child_impact(node),
                    false => ChildImpact::None,
                };
                scratch.statistics.changed_elements += u64::from(impact != ChildImpact::None);
                if impact != ChildImpact::None {
                    scratch.statistics.children_moved.push(node);
                }
                let children = match hint & RESTYLE_DESCENDANTS != 0 {
                    true => {
                        RESTYLE_SELF
                            | RESTYLE_DESCENDANTS
                            | if record_moved { PARENT_RESTYLED } else { 0 }
                            | match hint & MATCH_DESCENDANTS {
                                0 => 0,
                                _ => MATCH_SELF | MATCH_DESCENDANTS,
                            }
                    }
                    false if impact == ChildImpact::All => RESTYLE_SELF | PARENT_CHANGED,
                    // A `display: contents` element generates no box, so its children's boxes are
                    // in its parent's, whose display they read (blockification, for one) where
                    // it changed, whether or not this element's own record did.
                    false
                        if restyled
                            && hint & PARENT_CHANGED != 0
                            && context
                                .records
                                .get(node)
                                .is_some_and(|record| record.facts().display().is_contents()) =>
                    {
                        RESTYLE_SELF | PARENT_CHANGED
                    }
                    false
                        if matches!(
                            impact,
                            ChildImpact::CustomProperties | ChildImpact::ResetReadersAndCustomProperties
                        ) =>
                    {
                        PARENT_CUSTOM_PROPERTIES_MOVED
                    }
                    false => 0,
                } | containers_moved
                    | hint & ROOT_FONT_MOVED;
                let children_reasons = match hint & RESTYLE_DESCENDANTS != 0 {
                    true => union(
                        inherited_reasons,
                        context
                            .reasons
                            .and_then(|reasons| Reasons::of(reasons.descendants, node))
                            .as_ref(),
                    ),
                    false => None,
                };
                // The children of an element whose record moved beneath the composition of its
                // animations inherit the composition, which the host lays over the new record once
                // it is installed: they wait for the next pass.
                let defers_children =
                    restyled && context.hints.is_some() && context.records.moves_beneath_a_composition(node);
                if children != 0 || impact != ChildImpact::None || hint & DIRTY_DESCENDANTS != 0 {
                    // The traversal walks the flat tree, where an element's children are the
                    // ones that inherit from it.
                    for current in context.state.projection.style_children(node) {
                        let children = match impact {
                            ChildImpact::ResetReaders | ChildImpact::ResetReadersAndCustomProperties
                                if context
                                    .records
                                    .get(current)
                                    .is_none_or(|record| context.state.reads_parent_reset_values(current, record)) =>
                            {
                                RESTYLE_SELF | PARENT_CHANGED | containers_moved | hint & ROOT_FONT_MOVED
                            }
                            _ => children,
                        };
                        if children != 0 || context.hint(current) != 0 {
                            match defers_children {
                                // NB: The next pass rematches the child against the whole index,
                                //     as it keeps no reasons.
                                true => scratch.statistics.deferred.push((current, rematched_in_full(children))),
                                false => discovered.push_back(Queued {
                                    node: current,
                                    hint: children,
                                    reasons: children_reasons.clone(),
                                }),
                            }
                        }
                    }
                }
            }
            remaining_at_current_depth -= 1;
            let discovered_children = discovered.len() - remaining_at_current_depth;
            if let Some(scope) = scope
                && discovered_children >= WORK_UNIT
                && discovered.len() >= local_queue_size + WORK_UNIT
            {
                let kept = remaining_at_current_depth.max(local_queue_size);
                hand_off(scope, context, scratches, discovered.split_off(kept));
            }
            if scope.is_none() && discovered.len() >= hand_off_size {
                break;
            }
            if remaining_at_current_depth == 0 {
                remaining_at_current_depth = discovered.len();
            }
        }
        discovered
    })
}

/// Style every element of the document tree under `root`.
pub(super) fn style_document(
    context: &PassContext<'_>,
    pool: Option<&rayon::ThreadPool>,
    root: StyleNodeID,
) -> Statistics {
    style_subtrees(context, pool, &[root])
}

/// Style the elements under each of `roots`, none of which is under another, as the hints of the
/// pass ask.
pub(super) fn style_subtrees(
    context: &PassContext<'_>,
    pool: Option<&rayon::ThreadPool>,
    roots: &[StyleNodeID],
) -> Statistics {
    style_queued(
        context,
        pool,
        roots,
        |&root| Queued {
            node: root,
            hint: 0,
            reasons: None,
        },
        Vec::new(),
    )
}

/// Style what the round before suspended, now that the fonts and random base values it waited on
/// resolved: the pseudo-elements of the elements it styled but for those, and the elements, with
/// the elements under them, as the round before would have.
pub(super) fn resume(
    context: &PassContext<'_>,
    pool: Option<&rayon::ThreadPool>,
    suspended: Vec<Suspended>,
) -> Statistics {
    let mut elements = Vec::new();
    let mut pseudo_elements = Vec::new();
    for suspended in suspended {
        match suspended {
            Suspended::Element(queued) => elements.push(queued),
            Suspended::PseudoElements(suspended) => pseudo_elements.push(suspended),
        }
    }
    style_queued(context, pool, &elements, Queued::clone, pseudo_elements)
}

/// Keep the records of the pseudo-elements `suspended` holds back that did not wait, as no round
/// resumes them: the next pass styles the others.
///
/// Answers the elements whose ancestors asked this pass to restyle what is below them, which only the elements themselves
/// knew: the next pass restyles those too.
pub(super) fn settle_suspended(context: &PassContext<'_>, suspended: Vec<Suspended>) -> HashSet<StyleNodeID> {
    let mut restyling_descendants = HashSet::default();
    for suspended in suspended {
        match suspended {
            Suspended::Element(queued) => {
                if queued.hint & RESTYLE_DESCENDANTS != 0 {
                    restyling_descendants.insert(queued.node);
                }
            }
            Suspended::PseudoElements(suspended) => context
                .records
                .insert_pseudo_element_records(suspended.node, suspended.records),
        }
    }
    restyling_descendants
}

/// Style the pseudo-elements of `pseudo_elements`, and the elements `queued` makes of `elements`
/// with the elements under them.
fn style_queued<T: Sync>(
    context: &PassContext<'_>,
    pool: Option<&rayon::ThreadPool>,
    elements: &[T],
    queued: impl Fn(&T) -> Queued + Copy + Send,
    pseudo_elements: Vec<SuspendedPseudoElements>,
) -> Statistics {
    let workers = pool.map_or(0, rayon::ThreadPool::current_num_threads);
    let scratches = Scratches {
        pool,
        slots: (0..=workers).map(|_| Mutex::new(None)).collect(),
    };
    if !pseudo_elements.is_empty() {
        scratches.with(context, |scratch| {
            for suspended in pseudo_elements {
                scratch.resume_pseudo_elements(context, suspended);
            }
        });
    }
    // The calling thread styles the first elements, and the pool joins in for the others, or once
    // a level of the tree is wide enough to spare work for it.
    let mut units = elements.chunks(WORK_UNIT);
    let first: VecDeque<Queued> = units.next().unwrap_or_default().iter().map(queued).collect();
    match pool {
        // A pass with few roots runs on the calling thread until it finds enough to style for the
        // pool's threads to join in.
        Some(pool) if elements.len() < PARALLEL_PASS_SIZE => {
            let roots = elements.iter().map(queued).collect();
            let mut left = style_trees(
                None,
                context,
                &scratches,
                roots,
                CALLING_THREAD_QUEUE_SIZE,
                PARALLEL_PASS_SIZE,
            );
            if !left.is_empty() {
                pool.in_place_scope_fifo(|scope| {
                    let scratches = &scratches;
                    hand_off(
                        scope,
                        context,
                        scratches,
                        left.split_off(CALLING_THREAD_QUEUE_SIZE.min(left.len())),
                    );
                    style_trees(
                        Some(scope),
                        context,
                        scratches,
                        left,
                        CALLING_THREAD_QUEUE_SIZE,
                        usize::MAX,
                    );
                });
            }
        }
        Some(pool) => pool.in_place_scope_fifo(|scope| {
            let scratches = &scratches;
            for unit in units {
                let unit: VecDeque<Queued> = unit.iter().map(queued).collect();
                scope.spawn_fifo(move |scope| {
                    style_trees(Some(scope), context, scratches, unit, 0, usize::MAX);
                });
            }
            style_trees(
                Some(scope),
                context,
                scratches,
                first,
                CALLING_THREAD_QUEUE_SIZE,
                usize::MAX,
            );
        }),
        None => {
            for unit in std::iter::once(first).chain(units.map(|unit| unit.iter().map(queued).collect())) {
                style_trees(None, context, &scratches, unit, CALLING_THREAD_QUEUE_SIZE, usize::MAX);
            }
        }
    }
    let mut statistics = Statistics::default();
    for (index, scratch) in scratches.slots.into_iter().enumerate() {
        let Some(mut scratch) = scratch.into_inner().expect("no thread panicked while styling") else {
            continue;
        };
        if context.uses_caches
            && let Some(cache) = context.group_caches.and_then(|caches| caches.get(index))
        {
            *cache.lock().expect("no thread panicked while styling") = std::mem::take(&mut scratch.groups).kept();
        }
        statistics.add(scratch);
    }
    statistics.cache_audit.conclude(context.cache_audit);
    statistics
}

/// Style each element of `chain` in turn, each the parent the next one inherits from, matching it
/// against the whole rule index whatever the display of the elements above it, for a read of the
/// style of the last one. The elements after one left without a record are not styled.
pub(super) fn style_chain(context: &PassContext<'_>, chain: &[StyleNodeID]) -> Statistics {
    let mut scratch = Scratch::new(context);
    for &node in chain {
        scratch.style_element(context, node, None, PARENT_CHANGED);
        if !context.records.computed(node) {
            break;
        }
    }
    let mut statistics = Statistics::default();
    statistics.add(scratch);
    statistics.cache_audit.conclude(context.cache_audit);
    statistics
}

/// What a read of the style of one pseudo-element finds.
pub(super) enum DemandedPseudoElement {
    Styled(std::sync::Arc<Record>),
    /// No rule styles it.
    Unstyled,
    /// Its style waits on the fonts and random base values it asks for.
    Waiting(Vec<font_resolution::FontRequest>, Vec<RandomBaseRequest>),
    Declined,
}

/// Whether the computed values of `computed` hold `display: none`.
fn computed_display_is_none(computed: &super::compute::ComputedElement) -> bool {
    crate::css::style_compute::display_of(
        computed
            .groups
            .value(crate::css::property_metadata::property_id::DISPLAY),
    )
    .is_none()
}

/// Style the pseudo-element `target` of `node` for a read of its style, whether or not it
/// generates a box, from the rules the element matches for it, over the record `context` holds for
/// `node`, which it inherits from, or for `inheritance_parent` where that names an element. Which
/// pseudo-elements are styled is what `read` says.
pub(super) fn style_pseudo_element_inheriting_from(
    context: &PassContext<'_>,
    node: StyleNodeID,
    target: super::PseudoElementTarget,
    read: PseudoElementRead,
    inheritance_parent: Option<StyleNodeID>,
) -> DemandedPseudoElement {
    let Some(element) = context.records.get_shared(node).cloned() else {
        return DemandedPseudoElement::Declined;
    };
    let state = context.state;
    let Some(facts) = state.projection.facts(node) else {
        return DemandedPseudoElement::Unstyled;
    };
    let mut scratch = Scratch::new(context);
    scratch.enter_parent(context, state.projection.parent(node));
    if scratch
        .matcher
        .match_element(node, facts, &scratch.filter, &mut scratch.matches)
        .is_err()
    {
        return DemandedPseudoElement::Declined;
    }
    scratch.apply_container_gates(context, node, false);
    scratch.apply_container_gates(context, node, true);
    let inherited = match inheritance_parent {
        Some(parent) => {
            let Some(parent_record) = context.records.get_shared(parent) else {
                return DemandedPseudoElement::Declined;
            };
            PseudoElementInheritance::of(context, parent, parent_record)
        }
        None => PseudoElementInheritance::of(context, node, &element),
    };
    let outcome = scratch.compute_pseudo_element(context, node, &element, &inherited, target, read);
    if !scratch.statistics.font_requests.is_empty() || !scratch.statistics.random_requests.is_empty() {
        return DemandedPseudoElement::Waiting(
            std::mem::take(&mut scratch.statistics.font_requests),
            std::mem::take(&mut scratch.statistics.random_requests),
        );
    }
    match outcome {
        PseudoElementOutcome::Computed { record, .. } => DemandedPseudoElement::Styled(record),
        PseudoElementOutcome::Unstyled | PseudoElementOutcome::GeneratesNoBox => DemandedPseudoElement::Unstyled,
        PseudoElementOutcome::Waiting | PseudoElementOutcome::Declined => DemandedPseudoElement::Declined,
    }
}

impl Statistics {
    /// Take what the pass suspended, for its next round to resume, with the fonts and random base
    /// values it waits on. The elements it resumes wait no longer, and the round styles them.
    pub(super) fn take_suspended(
        &mut self,
    ) -> (
        Vec<Suspended>,
        Vec<font_resolution::FontRequest>,
        Vec<RandomBaseRequest>,
    ) {
        let suspended = std::mem::take(&mut self.suspended);
        let elements: HashSet<StyleNodeID> = suspended
            .iter()
            .filter(|suspended| matches!(suspended, Suspended::Element(_)))
            .map(Suspended::node)
            .collect();
        let pseudo_elements: HashSet<StyleNodeID> = suspended
            .iter()
            .filter(|suspended| matches!(suspended, Suspended::PseudoElements(_)))
            .map(Suspended::node)
            .collect();
        self.styled.retain(|node| !elements.contains(node));
        self.waiting.retain(|node| !elements.contains(node));
        self.waiting_with_kept_matches.retain(|node| !elements.contains(node));
        self.waiting_pseudo_elements
            .retain(|node| !pseudo_elements.contains(node));
        (
            suspended,
            std::mem::take(&mut self.font_requests),
            std::mem::take(&mut self.random_requests),
        )
    }

    /// Add what a round resuming what this one suspended measured and gathered.
    pub(super) fn add_round(&mut self, round: Statistics) {
        let statistics = self;
        statistics.match_counters.add(&round.match_counters);
        statistics.cascade_counters.add(&round.cascade_counters);
        statistics.font_requests.extend(round.font_requests);
        statistics.random_requests.extend(round.random_requests);
        statistics.waiting.extend(round.waiting);
        statistics.suspended.extend(round.suspended);
        statistics
            .waiting_with_kept_matches
            .extend(round.waiting_with_kept_matches);
        statistics.waiting_pseudo_elements.extend(round.waiting_pseudo_elements);
        statistics.styled.extend(round.styled);
        statistics.moved_custom_properties.extend(round.moved_custom_properties);
        statistics.changed_elements += round.changed_elements;
        statistics.children_moved.extend(round.children_moved);
        statistics.deferred.extend(round.deferred);
        statistics
            .restyled_for_pseudo_elements
            .extend(round.restyled_for_pseudo_elements);
        statistics.outcomes.add(&round.outcomes);
        statistics.cache_audit.add(round.cache_audit);
    }

    /// Add what a thread's scratch measured and gathered.
    fn add(&mut self, scratch: Scratch<'_>) {
        self.match_counters.add(&scratch.matcher.counters());
        self.cascade_counters.add(&scratch.cascade.statistics);
        self.add_round(scratch.statistics);
        if let Some(audit) = scratch.cache_audit {
            self.cache_audit.add(*audit);
        }
    }
}
