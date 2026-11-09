/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The engine, which owns the document: after one cold pass, every style update restyles
//! the elements its inputs can restyle, and only those, against the records the passes before
//! kept, and the records it computes are the ones installed.

use super::super::component_matching::DocumentMatching;
use super::super::index::dispatch_bloom_bit;
use super::super::input_types::TransactionFactSide;
use super::super::input_types::{
    STYLE_REACTION_FONT_INPUTS_CHANGED, STYLE_REACTION_PUBLISHED_STYLE, STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES,
    StyleEnvironment, StyleUpdate, TreeRelations,
};
use super::super::selector_evaluation::StructuralRead;
use super::super::*;
use super::drive::DriveRecords;
use super::invalidation::{
    AnchorReach, Dependency, HeldDependency, InvalidationMap, PositionKey, ReachCombinator, Step, Trigger, TriggerReach,
};
use super::projection::ProjectionEvaluator;
use super::projection::{ElementChange, FactListsIdentity, Projection, ProjectionRow, StructuralFlags};
use super::records::{ChildImpact, Record, RecordCells, Records, Replaced, RetainedRecords};
use super::rule_changes::Restyled;
use super::rule_store::RuleChanges;
use super::rules::{IndexChanges, RuleIndex};
use super::traversal::{
    self, CONTAINERS_MOVED, DIRTY_DESCENDANTS, HintBits, InitialStyle, MATCH_DESCENDANTS, MATCH_SELF, PARENT_CHANGED,
    PassContext, RECOMPUTE_SELF, RESTYLE_DESCENDANTS, RESTYLE_PSEUDO_ELEMENTS, RESTYLE_SELF, RESTYLES_SELF,
    STYLES_SOMETHING,
};
use crate::cow_column::CowColumn;
use crate::css::style::custom_property_cascade::{
    LonghandCustomPropertyReads, value_calls_custom_functions, value_reads_any_custom_property_of,
};
use crate::css::style::memory_report::{MemoryOwner, MemoryReport};
use crate::fork::ForkReset;
use smallvec::SmallVec;
use std::cell::{Cell, LazyCell};
use std::sync::Arc;
use std::time::Instant;

/// How many passes in a row can leave children for the next, as levels of elements composing
/// animations below one another.
const MAX_DEFERRING_PASSES: u32 = 1024;

#[derive(Clone, Copy)]
#[repr(u32)]
pub(super) enum LiveField {
    Passes,
    FullPasses,
    StyledElements,
    InvalidationNanoseconds,
    StyleNanoseconds,
    VerifiedElements,
    MissingRecords,
    Differences,
    HintedElements,
    HintedSubtrees,
    ChangedElements,
    InvalidationVisits,
    InvalidationTests,
    /// How many cold passes the engine started from, on the render state and its forks.
    ColdPasses,
}

impl LiveField {
    pub(super) const COUNT: usize = Self::ColdPasses as usize + 1;
}

/// The engine of a document's render state. A fork of the render state takes it as it is, and
/// goes on restyling what its own transactions change from the records it shares with the state
/// it forked: the rule index, invalidation map and initial style by reference, and the records in
/// pages either side copies as it writes one. What only a pass or a transaction works in, what
/// the last pass measured and what the main thread does to take the last installation are the
/// state's own, which a fork starts without.
#[derive(Clone)]
pub(in super::super) struct Live {
    pub(super) index: Arc<RuleIndex>,
    invalidation: Arc<InvalidationMap>,
    pub(super) initial: Arc<InitialStyle>,
    /// What `initial` was computed from, which it is computed again for where it moves.
    initial_inputs: InitialStyleInputs,
    pub(super) records: RetainedRecords,
    /// What the next pass restyles.
    hints: Hints,
    /// What passes and the invalidation of transactions work in.
    scratch: ForkReset<LiveScratch>,
    /// Whether an input since the last pass can restyle any element, such as a stylesheet change.
    pending_everything: bool,
    /// Whether a rule changed since the last pass, which the rule index is rebuilt for.
    pending_index_rebuild: bool,
    /// The rules changed since the last pass, which the rule index indexes again.
    changed_rules: HashSet<RuleID>,
    /// Those of `changed_rules` where more than their conditions moved.
    changed_rule_contents: HashSet<RuleID>,
    /// The sheets attached, detached, enabled or disabled since the last pass, or whose
    /// conditions moved, which the rule index indexes again where they started or stopped
    /// deciding.
    changed_sheets: HashSet<SheetID>,
    /// The scopes whose sheet or layer order moved since the last pass, and those whose layer
    /// order did, whose entries the rule index ranks again.
    reordered_scopes: HashSet<TreeScopeID>,
    layered_scopes: HashSet<TreeScopeID>,
    report: ForkReset<[u64; LiveField::COUNT]>,
    /// What passes counted since the state last took it into its counters.
    counts: super::counters::PassCounts,
    /// How many threads passes run on.
    threads: u32,
    /// What `LIBWEB_STYLE_AUDIT_CACHES` asked for as the engine first started.
    pub(super) cache_audit: super::self_check::Mode,
    /// What the audits of the passes found, while `cache_audit` asks for them.
    #[cfg(test)]
    pub(super) cache_audits: traversal::CacheAudit,
    /// The elements whose records the passes replaced since they were last installed.
    uninstalled: Vec<StyleNodeID>,
    /// The `@keyframes` rules an input since the last pass moved, which the animations naming
    /// them refer to.
    keyframes_moved: MovedKeyframes,
    /// The elements whose animations, or those of their pseudo-elements, the host plans again as
    /// it installs them, as the `@keyframes` rules their names refer to may have moved since they
    /// were last installed: the records they hold name the same animations.
    plans_owed: Vec<StyleNodeID>,
    /// The elements with animations a pass styled, whose animations the host composes again as it
    /// installs them, as a host composing them over their records does wherever it restyles them:
    /// what their keyframes read beside their records, such as the parent's values, the viewport,
    /// the root font, the registrations or their place among their siblings, may have moved.
    compositions_owed: Vec<StyleNodeID>,
    /// What the main thread does to take the last installation. The records it pinned are pinned
    /// in the state that installed them, which alone lets go of them.
    installation: ForkReset<super::install::StyleInstallation>,
    /// The elements an installation let go of the animation overlay of, where it lay over the
    /// record installed, with the overlay's view, which their children and pseudo-elements
    /// computed from: the host composes them again over the installed record, and what moved is
    /// what its composition moved from that view, not from the uncomposed record installed
    /// between.
    released_compositions: ForkReset<Vec<(StyleNodeID, HeldComposition)>>,
    /// How many passes in a row left children for the next, to inherit the composition of the
    /// animations of their parents: one per level of elements composing below one another.
    deferring_passes: u32,
    /// What moves whenever a record could: a transaction, a pass, an installation or a
    /// composition the children inherit. The records computed on demand are those of one
    /// generation.
    pub(super) generation: u64,
    /// The records computed on demand in the current generation.
    pub(super) demanded: ForkReset<super::demand::DemandOverlay>,
    /// The elements whose installed record a read demanded below an element that is not displayed, which says so. A
    /// pass styling one of them where it holds what it held before installs nothing else on its own account.
    pub(super) installed_as_undisplayed: HashSet<StyleNodeID>,
    /// The elements a transaction restyled outright, which each installation computes on demand
    /// and installs, with the elements they inherit through, where they are below an element that
    /// is not displayed: the host asks for the styles of the SVG resources such a subtree holds,
    /// which paint wherever they are referenced, and of its elements with animations.
    pub(super) kept_hidden: Vec<StyleNodeID>,
    /// Which records computed for `kept_hidden` still stand, so that an installation computes
    /// only those something could have moved.
    pub(super) kept_hidden_records: ForkReset<super::demand::KeptHiddenRecords>,
}

/// What passes and the invalidation of transactions work in, which holds nothing between them.
#[derive(Default)]
struct LiveScratch {
    /// The cells passes write their records to.
    cells: RecordCells,
    /// What a changed element matches, by thread, for the threads that followed a change.
    memos: super::per_thread::PerThread<FollowScratch>,
    /// The group caches the passes keep for the next, by thread, for the threads that styled.
    groups: super::per_thread::PerThread<super::compute::GroupCache>,
    /// The matched declarations cache the passes keep for the next.
    matched_declarations: super::matched_declarations::MatchedDeclarationsCache,
    /// The inputs of the document the entries of the matched declarations cache read, as they
    /// were when the last pass started.
    matched_declarations_inputs: Option<MatchedDeclarationsInputs>,
    /// The DOM order version of the tree the memos measured where siblings stand in, where the
    /// transaction they measured it for moved no element, so the tree before it was the same.
    memos_dom_order_version: Option<u64>,
    /// What the invalidation of a transaction works in.
    invalidate: InvalidateScratch,
    /// The records the last pass replaced.
    replaced: Replaced,
    /// The elements whose records the last pass kept.
    kept: Vec<StyleNodeID>,
    /// What the last installation worked in.
    install: super::install_scratch::InstallScratch,
}

impl LiveScratch {
    /// The bytes the scratch keeps on the heap between passes, but for the cells, which count
    /// toward the records, and what the group caches hold, which counts toward the worker caches.
    fn heap_size(&self) -> usize {
        let memos = self
            .memos
            .made()
            .map(|memo| {
                size_of_val(memo)
                    + memo
                        .lock()
                        .expect("no pass holds a memo while the report runs")
                        .heap_size()
            })
            .sum::<usize>();
        let groups = self.groups.made().map(size_of_val).sum::<usize>();
        size_of_val(&self.memos)
            + memos
            + size_of_val(&self.groups)
            + groups
            + self.invalidate.heap_size()
            + self.install.heap_size()
    }
}

/// How many records a pass may replace for them to be dropped on the thread that ran it.
const REPLACED_DROPPED_INLINE: usize = 64;

/// The rounds a pass styles in at most, each resuming what the one before it suspended on fonts
/// and random base values. What waits after the last one is left to the next pass.
const MAX_SUSPENDING_ROUNDS: usize = 8;

/// What resolves the fonts and draws the random base values the round of a pass waited on, before
/// the next round resumes what waited on them.
pub(in super::super) type ResolveWaits<'a> = dyn FnMut(
        &mut RetainedState,
        Vec<super::super::font_resolution::FontRequest>,
        Vec<super::super::random_bases::RandomBaseRequest>,
    ) + 'a;

/// How many entries a scratch collection keeps room for between transactions: one that needed
/// more grows it, and gives the excess back once done.
pub(super) const KEPT_SCRATCH_CAPACITY: usize = 1024;

/// Empty `collection`, keeping its room up to `KEPT_SCRATCH_CAPACITY` entries.
pub(super) fn recycle<T>(collection: &mut Vec<T>) {
    collection.clear();
    if collection.capacity() > KEPT_SCRATCH_CAPACITY {
        collection.shrink_to(KEPT_SCRATCH_CAPACITY);
    }
}

/// The type of an element, by tag and namespace.
type ElementType = (StyleAtomID, StyleAtomID);

/// What the invalidation of a transaction works in, kept between transactions so that one
/// allocates only where it needs more than the ones before.
#[derive(Default)]
struct InvalidateScratch {
    /// The facts that changed, by the element they changed on, sorted by element before they are
    /// followed.
    /// NB: Each fact comes with whether a compound tests it, which its change is followed for.
    by_element: Vec<(StyleNodeID, Trigger, bool)>,
    /// The parents of the elements whose relations changed, before and now, sorted and deduplicated
    /// once all are in.
    sibling_parents: Vec<StyleNodeID>,
    /// The elements that arrived or left, with whether they left and the first of their later
    /// siblings.
    moves: Vec<(StyleNodeID, bool, Option<StyleNodeID>)>,
    /// The elements whose previous sibling changed, with that sibling before and now.
    new_previous_siblings: Vec<(StyleNodeID, [Option<StyleNodeID>; 2])>,
    /// The relations every element whose relations changed had before and has now.
    relations_moved: HashMap<StyleNodeID, (Option<TreeRelations>, Option<TreeRelations>)>,
    /// The children of one parent, on one side.
    children: Vec<StyleNodeID>,
    /// Where each child of one parent stood before, sorted by child.
    positions_before: Vec<(StyleNodeID, SiblingPosition)>,
    /// Where each child of one parent stands now, in their order.
    positions: Vec<(StyleNodeID, SiblingPosition)>,
    /// The type of each child of one parent whose type is known, with its index, sorted.
    types: Vec<(ElementType, usize)>,
    /// The links followed from one element.
    links: Vec<Dependency>,
    /// The runs of `by_element` of each element whose changes are followed.
    changes: Vec<std::ops::Range<usize>>,
}

impl InvalidateScratch {
    fn heap_size(&self) -> usize {
        use crate::heap_size::{map_bytes, vec_bytes};
        vec_bytes(&self.by_element)
            + vec_bytes(&self.sibling_parents)
            + vec_bytes(&self.moves)
            + vec_bytes(&self.new_previous_siblings)
            + map_bytes(&self.relations_moved)
            + vec_bytes(&self.children)
            + vec_bytes(&self.positions_before)
            + vec_bytes(&self.positions)
            + vec_bytes(&self.types)
            + vec_bytes(&self.links)
            + vec_bytes(&self.changes)
    }

    fn recycle(&mut self) {
        recycle(&mut self.by_element);
        recycle(&mut self.sibling_parents);
        recycle(&mut self.moves);
        recycle(&mut self.new_previous_siblings);
        self.relations_moved.clear();
        if self.relations_moved.capacity() > KEPT_SCRATCH_CAPACITY {
            self.relations_moved.shrink_to(KEPT_SCRATCH_CAPACITY);
        }
        recycle(&mut self.children);
        recycle(&mut self.positions_before);
        recycle(&mut self.positions);
        recycle(&mut self.types);
        recycle(&mut self.links);
        recycle(&mut self.changes);
    }
}

/// Measure where each of `children` stands among them into `positions`, and among those of its
/// type where `type_of` tells it, which `types` is room for.
fn measure_sibling_positions(
    children: &[StyleNodeID],
    type_of: Option<&dyn Fn(StyleNodeID) -> Option<ElementType>>,
    types: &mut Vec<(ElementType, usize)>,
    positions: &mut Vec<(StyleNodeID, SiblingPosition)>,
) {
    positions.clear();
    positions.extend(children.iter().enumerate().map(|(index, &child)| {
        let position = SiblingPosition {
            from_start: index,
            from_end: children.len() - 1 - index,
            of_type: None,
        };
        (child, position)
    }));
    let Some(type_of) = type_of else {
        return;
    };
    types.clear();
    types.extend(
        children
            .iter()
            .enumerate()
            .filter_map(|(index, &child)| Some((type_of(child)?, index))),
    );
    types.sort_unstable();
    for run in types.chunk_by(|a, b| a.0 == b.0) {
        for (rank, &(_, index)) in run.iter().enumerate() {
            positions[index].1.of_type = Some((rank, run.len() - 1 - rank));
        }
    }
}

/// The `@keyframes` rules that moved since the last pass, by the names they declare.
#[derive(Clone, Default)]
enum MovedKeyframes {
    #[default]
    None,
    Named(Vec<StyleAtomID>),
    /// A rule declaring no name moved.
    Any,
}

impl MovedKeyframes {
    fn is_empty(&self) -> bool {
        matches!(self, Self::None)
    }

    /// Note a `@keyframes` rule declaring `name` moved.
    fn note(&mut self, name: Option<StyleAtomID>) {
        match (&mut *self, name) {
            (Self::Any, _) => {}
            (_, None) => *self = Self::Any,
            (Self::None, Some(name)) => *self = Self::Named(vec![name]),
            (Self::Named(names), Some(name)) => {
                if !names.contains(&name) {
                    names.push(name);
                }
            }
        }
    }
}

/// The animation names the computed `animation-name` of `record` holds.
fn record_animation_names(record: &Record) -> impl Iterator<Item = &crate::css::css_string::CssString> {
    use crate::css::property_metadata::property_id as prop;
    let names = super::super::computed::ViewLonghands::of_record(record).handle(prop::ANIMATION_NAME);
    names
        .and_then(|names| match names.data() {
            StyleValueData::ValueList { values, .. } => Some(values.as_slice()),
            _ => None,
        })
        .unwrap_or_default()
        .iter()
        .filter_map(|name| match name.data() {
            StyleValueData::CustomIdent { custom_ident } => Some(custom_ident),
            StyleValueData::String { string, .. } => Some(string),
            _ => None,
        })
}

/// What the document's initial style computes from among the inputs every transaction replaces:
/// the device pixel ratio lengths snap by, the document's default and initial font sizes, the
/// metrics of its initial font and the generation of its font environment, which moves as a font
/// face loads or fails.
#[derive(Clone, Copy, PartialEq, Eq)]
struct InitialStyleInputs {
    device_pixels_per_css_pixel: u64,
    initial_font_metrics: [u64; 5],
    initial_font_size_raw: i32,
    default_font_size_raw: i32,
    font_environment_generation: u64,
}

impl InitialStyleInputs {
    fn of(inputs: &super::super::bridge::FfiDocumentStyleComputationInputs) -> Self {
        Self {
            device_pixels_per_css_pixel: inputs.device_pixels_per_css_pixel.to_bits(),
            initial_font_metrics: [
                inputs.initial_font_size,
                inputs.initial_font_x_height,
                inputs.initial_font_cap_height,
                inputs.initial_font_zero_advance,
                inputs.initial_line_height,
            ]
            .map(f64::to_bits),
            initial_font_size_raw: inputs.initial_font_size_raw,
            default_font_size_raw: inputs.default_font_size_raw,
            font_environment_generation: inputs.font_environment_generation,
        }
    }
}

/// What every computation may read of the document, which the elements reading it are restyled for
/// as it moves, and the entries of the matched declarations cache do not hold on to: what the
/// initial style computes from, the viewport, the color schemes, the custom property registrations
/// and what a `url()` resolves against.
#[derive(Clone, Copy, PartialEq, Eq)]
struct MatchedDeclarationsInputs {
    initial_style: InitialStyleInputs,
    in_quirks_mode: bool,
    viewport: [u64; 2],
    preferred_color_scheme: u8,
    supported_color_schemes: Option<(u8, [u8; 4])>,
    custom_property_registration_generation: u64,
    style_sheet_resource_contexts_version: u64,
}

impl MatchedDeclarationsInputs {
    /// Whether these inputs differ from `other` in nothing but the size of the viewport, which the
    /// records reading it name.
    fn same_besides_viewport(&self, other: &Self) -> bool {
        Self {
            viewport: other.viewport,
            ..*self
        } == *other
    }

    fn of(inputs: &super::super::bridge::FfiDocumentStyleComputationInputs) -> Self {
        Self {
            initial_style: InitialStyleInputs::of(inputs),
            in_quirks_mode: inputs.in_quirks_mode,
            viewport: [inputs.viewport_width.to_bits(), inputs.viewport_height.to_bits()],
            preferred_color_scheme: inputs.preferred_color_scheme,
            supported_color_schemes: inputs.has_document_supported_schemes.then_some((
                inputs.document_supported_scheme_count,
                inputs.document_supported_scheme_codes,
            )),
            custom_property_registration_generation: inputs.custom_property_registration_generation,
            style_sheet_resource_contexts_version: inputs.style_sheet_resource_contexts_version,
        }
    }
}

impl Live {
    /// The matched declarations cache the passes keep for the next.
    #[cfg(test)]
    pub(super) fn matched_declarations(&self) -> &super::matched_declarations::MatchedDeclarationsCache {
        &self.scratch.matched_declarations
    }

    /// Counts the rule data, initial style and records the engine holds, by owner. Rule data
    /// and initial style come first, as every record shares their groups and declarations.
    pub(in super::super) fn report_memory(&self, report: &mut MemoryReport) {
        use crate::heap_size::{ARC_HEADER_BYTES, vec_bytes};
        if report.sizer.first_sight(Arc::as_ptr(&self.initial)) {
            let initial = &*self.initial;
            let mut bytes = ARC_HEADER_BYTES + size_of::<InitialStyle>();
            for (group, payload) in initial.payloads.iter().enumerate() {
                if !report.sizer.first_sight(payload.as_ptr()) {
                    continue;
                }
                bytes += crate::css::computed_values::group_payload_allocation_bytes(group, payload.as_ptr());
                // SAFETY: The initial style holds a reference to each of its groups.
                bytes += unsafe {
                    crate::css::computed_values::group_held_values_heap_size(group, payload.as_ptr(), &mut report.sizer)
                };
            }
            report.add(MemoryOwner::Initial, bytes);
            report.count(MemoryOwner::Initial, &initial.value_facts);
        }
        let user_agent = self.index.user_agent_heap_size(&mut report.sizer);
        report.add(MemoryOwner::UserAgentIndex, user_agent);
        let segments = self.index.segments_heap_size(&mut report.sizer);
        report.add(MemoryOwner::SheetSegments, segments);
        if report.sizer.first_sight(Arc::as_ptr(&self.index)) {
            let bytes = ARC_HEADER_BYTES + size_of::<RuleIndex>() + self.index.heap_size(&mut report.sizer);
            report.add(MemoryOwner::RuleIndex, bytes);
        }
        if report.sizer.first_sight(Arc::as_ptr(&self.invalidation)) {
            report.add(
                MemoryOwner::Invalidation,
                ARC_HEADER_BYTES + size_of::<InvalidationMap>() + self.invalidation.heap_size(),
            );
        }
        self.records.report_memory(report);
        self.scratch.cells.report_memory(report);
        report.add(
            MemoryOwner::RecordCells,
            vec_bytes(&self.uninstalled)
                + vec_bytes(&self.plans_owed)
                + vec_bytes(&self.compositions_owed)
                + vec_bytes(&self.kept_hidden)
                + vec_bytes(&self.scratch.kept),
        );
        self.scratch.matched_declarations.report_memory(report);
        for cache in self.scratch.groups.made() {
            cache
                .lock()
                .expect("no pass holds a cache while the report runs")
                .report_memory(report);
        }
        let hints = self.hints.heap_size(&mut report.sizer);
        report.add(MemoryOwner::LiveScratch, self.scratch.heap_size() + hints);
    }

    /// The invalidation map, where it describes the rules and elements as they stand: none where
    /// an input restyling every element, or a change to the rules, waits for the next pass.
    /// The invalidation map [`Self::current_invalidation`] answers, shared.
    pub(super) fn current_invalidation_shared(&self) -> Option<&Arc<InvalidationMap>> {
        (!self.pending_everything && !self.pending_index_rebuild).then_some(&self.invalidation)
    }

    pub(super) fn current_invalidation(&self) -> Option<&InvalidationMap> {
        (!self.pending_everything && !self.pending_index_rebuild).then_some(&*self.invalidation)
    }

    /// Note what a transaction restyles, for the pass after it, while what it replaced is at hand:
    /// the projection's side of the transaction from before it shows the facts and the tree as
    /// they were.
    pub(in super::super) fn note_transaction(&mut self, state: &RetainedState, transaction: &StyleUpdate) {
        #[cfg(test)]
        let _section = super::allocation_count::StyleSection::enter();
        let started = Instant::now();
        self.generation += 1;
        // NB: An environment change or a restart can move how any element computes.
        if transaction.restyles_everything {
            self.pending_everything = true;
        }
        if transaction.restyles_everything
            || !transaction.environment.is_empty()
            || !transaction.rule_changes.is_empty()
        {
            self.scratch.matched_declarations.forget();
        }
        let changes = state.projection.changes();
        if transaction.restyles_everything {
            self.kept_hidden_records.forget();
        } else {
            self.kept_hidden_records.note_environment_moved(transaction.environment);
        }
        self.kept_hidden_records
            .note_moved(transaction.style_inputs.iter().map(|&(node, _)| node));
        self.kept_hidden_records
            .note_left(changes.iter().filter_map(|&(node, change)| match change {
                ElementChange::Relations(_, None) => Some(node),
                _ => None,
            }));
        if transaction.has_no_element_inputs() && changes.is_empty() && transaction.rule_changes.is_empty() {
            return;
        }
        self.counts.note_transaction();
        self.kept_hidden
            .extend(transaction.style_inputs.iter().map(|&(node, _)| node));
        self.kept_hidden
            .extend(changes.iter().filter_map(|&(node, change)| match change {
                ElementChange::ArrivingFacts | ElementChange::Id(..) => Some(node),
                _ => None,
            }));
        self.note_rule_changes(state, &transaction.rule_changes);
        // What moved of the document's environment restyles the elements whose computation read
        // it. What the others inherit moves with their parents.
        if !transaction.environment.is_empty() && !self.pending_everything {
            let moved = transaction.environment;
            self.restyle_records_where(state, |_, _, record| {
                record.rare().reads.environment_reads.intersects(moved)
            });
        }
        if !self.pending_everything {
            self.hints.reserve(state.projection.element_slot_count());
            self.invalidate(state, transaction, changes);
        }
        self.bump(LiveField::InvalidationNanoseconds, started.elapsed().as_nanos() as u64);
    }

    /// Note what no pass tells of the hints: a pass styles the elements holding a record and tells
    /// whether their children compute from something else, but not those below an element that is
    /// not displayed, or what restyling descendants reaches.
    pub(super) fn note_hints_moved_kept_hidden_records(&mut self) {
        let (hints, records, kept) = (&self.hints, &self.records, &mut *self.kept_hidden_records);
        if !kept.holds_computations() {
            return;
        }
        for &node in &hints.marked {
            if hints.get(node) & RESTYLE_DESCENDANTS != 0 {
                kept.note_descendants_moved(node);
            } else if records.get(node).is_none() || kept.computed(node) {
                kept.note_moved([node]);
            }
        }
    }

    /// Whether the next pass has anything to do: an input since the last pass can restyle any
    /// element or moved the rules, or something hinted an element.
    /// The scratch an installation works in.
    pub(super) fn begin_installation(&mut self) -> super::install_scratch::InstallScratch {
        std::mem::take(&mut self.scratch.install)
    }

    /// Takes in what an installation found, and keeps the scratch it worked in for the next.
    pub(super) fn end_installation(&mut self, mut scratch: super::install_scratch::InstallScratch) {
        self.released_compositions.append(&mut scratch.released_compositions);
        for (node, own, installed) in scratch.adopted.drain(..) {
            self.records.adopt(node, own, installed);
        }
        scratch.recycle();
        self.scratch.install = scratch;
    }

    pub(in super::super) fn has_pending(&self) -> bool {
        self.pending_everything
            || self.pending_index_rebuild
            || !self.keyframes_moved.is_empty()
            || !self.hints.marked.is_empty()
    }

    /// Clear the hints of the pass that styled the elements, but for the children it left for the
    /// next pass, `deferred`, the elements whose styles waited on a font or a random base value,
    /// `waiting`, and the elements below either, whose hints that pass takes.
    fn keep_deferred_hints(
        &mut self,
        tree: &Projection,
        deferred: &[(StyleNodeID, HintBits)],
        waiting: &[(StyleNodeID, HintBits)],
    ) {
        if deferred.is_empty() {
            self.deferring_passes = 0;
        } else {
            self.deferring_passes += 1;
            // NB: Each pass in a row that defers reaches one level of composing elements further
            //     down.
            debug_assert!(
                self.deferring_passes <= MAX_DEFERRING_PASSES,
                "the passes deferring children to compositions end"
            );
        }
        if deferred.is_empty() && waiting.is_empty() {
            self.hints.reset();
            return;
        }
        let deferring: HashSet<StyleNodeID> = deferred
            .iter()
            .filter_map(|&(child, _)| tree.inheritance_parent(child))
            .chain(waiting.iter().map(|&(node, _)| node))
            .collect();
        // NB: The reasons of the hints kept are not, so the elements they restyle for some entries
        //     rematch against the whole index. Those restyled for none keep their matches.
        let kept: Vec<(StyleNodeID, HintBits)> = self
            .hints
            .marked
            .iter()
            .map(|&node| {
                let bits = self.hints.get(node);
                match self.hints.restyled_for_entries(node) {
                    true => (node, traversal::rematched_in_full(bits)),
                    false => (node, bits),
                }
            })
            .filter(|&(node, bits)| {
                bits != 0
                    && std::iter::successors(tree.inheritance_parent(node), |&ancestor| {
                        tree.inheritance_parent(ancestor)
                    })
                    .any(|ancestor| deferring.contains(&ancestor))
            })
            .collect();
        self.hints.reset();
        for &(node, bits) in &kept {
            self.hints.mark(node, bits);
        }
        for &(node, bits) in deferred {
            self.hints.mark(node, traversal::rematched_in_full(bits));
        }
        for &(node, bits) in waiting {
            self.hints.mark(node, bits);
        }
    }

    /// What the next pass restyles of the elements whose styles waited on a font or a random base
    /// value in the pass that hinted `hinted`.
    ///
    /// The pass styled nothing of an element in `waiting`, nor anything below it: the next one
    /// restyles the element, and its descendants where this one restyled every element or the
    /// descendants of the element or of an ancestor, as `waiting_restyling_descendants` holds where the pass's
    /// walk asked that of the element, such as below an element displayed again. Elsewhere the hints of the elements below it
    /// stay for the next pass, and its children are restyled where its style moves.
    ///
    /// The next pass computes an element in `waiting_with_kept_matches`, which the pass matched to
    /// what it held matched before, from those matches again without matching it.
    ///
    /// The pass styled an element in `waiting_pseudo_elements` and the elements below it, but for
    /// some of its pseudo-elements, which the next one styles.
    fn hints_after_waiting(
        &self,
        state: &RetainedState,
        hinted: bool,
        waiting: &[StyleNodeID],
        waiting_with_kept_matches: &[StyleNodeID],
        waiting_pseudo_elements: &[StyleNodeID],
        waiting_restyling_descendants: &HashSet<StyleNodeID>,
    ) -> Vec<(StyleNodeID, HintBits)> {
        let kept_matches: HashSet<StyleNodeID> = waiting_with_kept_matches.iter().copied().collect();
        let element = waiting.iter().map(|&node| {
            let tree = &state.projection;
            let restyles_descendants = !hinted
                || waiting_restyling_descendants.contains(&node)
                || std::iter::successors(Some(node), |&ancestor| tree.inheritance_parent(ancestor))
                    .any(|ancestor| self.hints.get(ancestor) & RESTYLE_DESCENDANTS != 0);
            let bits = match restyles_descendants {
                true => RESTYLE_SELF | RESTYLE_DESCENDANTS,
                false => RESTYLE_SELF,
            };
            match kept_matches.contains(&node) {
                true => (node, traversal::rematched_in_full(bits) & !MATCH_SELF | RECOMPUTE_SELF),
                false => (node, traversal::rematched_in_full(bits)),
            }
        });
        let pseudo_elements = waiting_pseudo_elements
            .iter()
            .map(|&node| (node, RESTYLE_PSEUDO_ELEMENTS));
        element.chain(pseudo_elements).collect()
    }

    /// Restyle the children and pseudo-elements of `node` that compute from what the composition
    /// of its animations moved, in the next pass. They inherit the composition, which no input of
    /// theirs notes.
    fn note_composition_moved(&mut self, state: &RetainedState, node: StyleNodeID, impact: ChildImpact) {
        if impact == ChildImpact::None {
            return;
        }
        self.hints.reserve(state.projection.element_slot_count());
        // The element's pseudo-elements inherit the composition too: the element is restyled
        // with them, as under a parent that changed. The host composed its animations, and owes
        // it no other composition for it.
        if !self.records.pseudo_element_records(node).is_empty() {
            self.hints.mark(node, RESTYLE_PSEUDO_ELEMENTS);
        }
        for child in state.projection.style_children(node) {
            // A child holding no record is under an element that is not displayed, and has no
            // style to compute.
            let Some(record) = self.records.get(child) else {
                continue;
            };
            if impact == ChildImpact::ResetReaders && !state.reads_parent_reset_values(child, record) {
                continue;
            }
            self.hints.mark(child, RESTYLE_SELF | PARENT_CHANGED);
        }
    }

    /// Restyle, in the next pass, the elements and pseudo-elements below `node` whose `font-family: monospace` recascades
    /// their font size, which goes on from the computed size of the nearest ancestor whose animations give it one, where
    /// such animations of `node` came or went. What they inherit need not have moved.
    fn note_animated_font_size_came_or_went(&mut self, state: &RetainedState, node: StyleNodeID) {
        let is_monospace = |record: &Record| {
            record
                .longhand(crate::css::computed_values::LonghandID::new(
                    crate::css::property_metadata::property_id::FONT_FAMILY,
                ))
                .optional_data()
                .is_some_and(crate::css::style_compute::font_family_is_monospace)
        };
        self.hints.reserve(state.projection.element_slot_count());
        for descendant in state.projection.style_preorder(node).skip(1) {
            let Some(record) = self.records.get(descendant) else {
                continue;
            };
            if is_monospace(record) {
                self.hints.mark(descendant, RESTYLE_SELF | PARENT_CHANGED);
            } else if self
                .records
                .pseudo_element_records(descendant)
                .iter()
                .any(|(_, record)| is_monospace(record))
            {
                self.hints.mark(descendant, RESTYLE_PSEUDO_ELEMENTS);
            }
        }
    }

    /// Restyle, in the next pass, the elements whose records or pseudo-element records name a
    /// counter-style registry their style scope no longer has. A record reading the counter-style
    /// environment names the registry it was computed against, and the host minting a new one for
    /// a scope is no input of the elements reading it.
    ///
    /// A record computed while its scope had no registry names none, though its style reads one,
    /// so what a record names is compared with what it would name now.
    fn note_counter_style_environment_moved(&mut self, state: &RetainedState) {
        self.scratch.matched_declarations.forget();
        self.kept_hidden_records.forget();
        self.restyle_records_where(state, |_, target, record| {
            record.rare().counter_style_environment_identity
                != state.values_counter_style_environment_identity(target, |property| record.effective_value(property))
        });
    }

    /// Whether the style of `node`, its own or one of its pseudo-elements', may have resolved a
    /// length against an axis of the viewport `moved` names: one reading the viewport through a
    /// viewport-relative length of that axis, or through anything else, such as a root font or a
    /// container-relative length resolved against the viewport, or one with no record.
    pub(in super::super) fn reads_viewport_axes(&self, node: StyleNodeID, moved: StyleEnvironment) -> bool {
        let reads = |record: &Record| {
            let axes = record
                .rare()
                .reads
                .environment_reads
                .intersection(StyleEnvironment::VIEWPORT);
            record.facts().dependency_flags() & crate::css::computed_longhand_table::DEPENDS_ON_VIEWPORT_METRICS != 0
                && (axes.intersects(moved) || axes.is_empty() || record.rare().reads.container_units != 0)
        };
        self.records.get(node).is_none_or(|record| reads(record))
            || self
                .records
                .pseudo_element_records(node)
                .iter()
                .any(|(_, record)| reads(record))
    }

    /// Restyle, in the next pass, the elements whose records `moved` holds for, and the
    /// pseudo-elements of those whose pseudo-element records it holds for.
    fn restyle_records_where(
        &mut self,
        state: &RetainedState,
        moved: impl Fn(&RetainedRecords, super::super::computed::ComputedStyleTarget, &Record) -> bool,
    ) {
        use super::super::computed::ComputedStyleTarget;
        let (records, hints) = (&self.records, &mut self.hints);
        hints.reserve(state.tree.element_slot_count());
        for node in state.tree.live_nodes() {
            if records
                .get(node)
                .is_some_and(|record| moved(records, ComputedStyleTarget::new(node, u8::MAX), record))
            {
                hints.add(node, RESTYLE_SELF);
            } else if records
                .pseudo_element_records(node)
                .iter()
                .any(|(target, record)| moved(records, ComputedStyleTarget::of_pseudo_element(node, *target), record))
            {
                hints.mark(node, RESTYLE_PSEUDO_ELEMENTS);
            }
        }
    }

    /// Restyle, in the next pass, the elements a registration of one of the custom properties
    /// `names` moved can move how they compute: those declaring one, holding a value of one,
    /// substituting one or something that may read any, whose animations may interpolate one, or
    /// that read the style of a container, which may hold one. The children of an element whose
    /// custom properties moved follow them as the pass restyles it.
    fn note_registrations_moved(&mut self, state: &RetainedState, names: &[ak::Utf16FlyString]) {
        if names.is_empty() {
            return;
        }
        self.scratch.matched_declarations.forget();
        let raw: HashSet<usize> = names.iter().map(ak::Utf16FlyString::raw_identity).collect();
        self.kept_hidden_records.forget();
        let texts: Vec<std::borrow::Cow<'_, [u16]>> = names.iter().map(ak::Utf16FlyString::to_utf16).collect();
        self.restyle_records_where(state, |records, target, record| {
            let node = target.node();
            // NB: A custom property substituting a moved one computes anew, as one of the moved
            //     ones does.
            record.custom_declarations().is_some_and(|declarations| {
                declarations.iter().any(|(declaration, written)| {
                    state
                        .declared_custom_property_name(declaration.name)
                        .is_some_and(|name| raw.contains(&name.raw()))
                        || written.optional_data().is_some_and(|value| {
                            value_reads_any_custom_property_of(value, |name| texts.iter().any(|text| **text == *name))
                        })
                })
            }) || record
                .custom_properties()
                .is_some_and(|store| names.iter().any(|name| store.holds(name)))
                || record
                    .rare()
                    .custom_property_reads
                    .read_any_of(|name| texts.iter().any(|text| **text == *name))
                || record.facts().declares_css_animations()
                || state
                    .animation_effect_descriptions
                    .animates_custom_property(node, |name| texts.iter().any(|text| **text == *name))
                || records
                    .container_reads(node)
                    .is_some_and(|reads| reads.depends_on_style)
        });
    }

    /// Note a change to the rules, which the pass after it rebuilds the rule index for and restyles
    /// the elements the rules that changed match or matched, or restyle every element where it can.
    ///
    /// A `@keyframes` rule matches no element: the animations naming the rules that moved are
    /// planned again without a restyle, and so are they where a sheet holding one moved or a scope
    /// reordered which of those sharing a name wins. A reordering reaches the rules of every kind
    /// its scope holds, but for the user agent's, which no author or user rule comes before.
    fn note_rule_changes(&mut self, state: &RetainedState, changes: &RuleChanges) {
        if changes.is_empty() {
            return;
        }
        // NB: `@property` rules and `CSS.registerProperty()` reach the elements through the
        //     registrations that moved, whichever rule or call moved them.
        self.note_registrations_moved(state, &changes.registrations);
        if changes.rules.is_empty()
            && changes.sheets.is_empty()
            && changes.reordered_scopes.is_empty()
            && changes.layered_scopes.is_empty()
        {
            return;
        }
        let program = &state.rules;
        let kind_of = |rule| program.rule_version(rule).kind;
        // NB: Only what a style rule declares reaches an element through what it matches.
        if changes.rules.iter().any(|&rule| kind_of(rule) != RuleKind::Style) {
            self.kept_hidden_records.forget();
        }
        let rules_of = |sheet| program.rules_in_sheet(sheet).into_iter().map(kind_of);
        // The `@keyframes` rules that moved, and those that may decide another rule of their name
        // where their scope was reordered.
        for rule in changes
            .rules
            .iter()
            .copied()
            .chain(changes.sheets.iter().flat_map(|&sheet| program.rules_in_sheet(sheet)))
            .chain(changes.reordered_scopes.iter().flat_map(|&scope| {
                program
                    .sheets_in_scope(scope)
                    .iter()
                    .flat_map(|&sheet| program.rules_in_sheet(sheet))
            }))
        {
            let version = program.rule_version(rule);
            if version.kind == RuleKind::Keyframes {
                self.keyframes_moved.note(version.declared_name());
            }
        }
        let reached = |wanted: RuleKind| {
            changes.rules.iter().any(|&rule| kind_of(rule) == wanted)
                || changes
                    .sheets
                    .iter()
                    .any(|&sheet| rules_of(sheet).any(|kind| kind == wanted))
                || changes.reordered_scopes.iter().any(|&scope| {
                    program.sheets_in_scope(scope).iter().any(|&sheet| {
                        program.sheet_origin(sheet) != CascadeOrigin::UserAgent
                            && rules_of(sheet).any(|kind| kind == wanted)
                    })
                })
        };
        // NB: A `@counter-style` rule moves the counter-style registry of its scope, which the
        //     host mints anew, and the elements naming the one it replaced are restyled as it
        //     says so. A `@font-feature-values` rule moves the fonts of the families it names,
        //     which the host forgets, restyling the elements whose fonts name one.
        // The elements calling a custom function resolve their calls against the `@function`
        // rules anew.
        if reached(RuleKind::Function) {
            self.note_custom_functions_moved(state);
        }
        self.changed_rules.extend(changes.rules.iter().copied());
        self.changed_rule_contents
            .extend(changes.rules_with_moved_contents.iter().copied());
        self.changed_sheets.extend(changes.sheets.iter().copied());
        self.reordered_scopes.extend(changes.reordered_scopes.iter().copied());
        self.layered_scopes.extend(changes.layered_scopes.iter().copied());
        self.pending_index_rebuild = true;
    }

    /// Restyle, in the next pass, the elements a change to the `@function` rules can move how
    /// they compute: those whose custom properties call a custom function, or whose longhands
    /// substitute something that may.
    fn note_custom_functions_moved(&mut self, state: &RetainedState) {
        self.scratch.matched_declarations.forget();
        self.kept_hidden_records.forget();
        self.restyle_records_where(state, |_, _, record| {
            record.rare().custom_property_reads == LonghandCustomPropertyReads::Any
                || record.custom_declarations().is_some_and(|declarations| {
                    declarations
                        .iter()
                        .any(|(_, written)| value_calls_custom_functions(written.data()))
                })
        });
    }

    pub(super) fn index(&self) -> &RuleIndex {
        &self.index
    }

    /// Whether a rule of the document's sheets styles `::selection`.
    pub(in super::super) fn styles_selection(&self) -> bool {
        self.index
            .styles_pseudo_element(super::super::publication::pseudo_kind::SELECTION)
    }

    #[cfg(test)]
    pub(super) fn records(&self) -> &RetainedRecords {
        &self.records
    }

    /// Whether the style the passes kept for `node`, its own or one of its pseudo-elements',
    /// derives from the metrics of a first available font, or is not known not to.
    pub(in super::super) fn reads_font_metrics(&self, node: StyleNodeID) -> bool {
        self.records.reads_font_metrics(node)
    }

    /// The containers the container conditions of the rules the passes kept for `node` were
    /// evaluated against, whose first available font's metrics the conditions' lengths read.
    pub(in super::super) fn font_metric_query_containers(&self, node: StyleNodeID) -> &[StyleNodeID] {
        self.records
            .container_reads(node)
            .map_or(&[], |reads| reads.font_metric_containers.as_slice())
    }

    /// The custom declarations the cascade of `node`, or of its pseudo-element of the kind
    /// `pseudo` names, decided, as written, where the pass holds a record for it.
    pub(in super::super) fn custom_declarations_of(
        &self,
        node: StyleNodeID,
        pseudo: Option<u8>,
    ) -> Option<Vec<(CustomDeclaration, RetainedStyleValueData)>> {
        let record = match pseudo {
            None => self.records.get(node)?,
            Some(kind) => self
                .records
                .pseudo_element_records(node)
                .iter()
                .find_map(|(pseudo, record)| {
                    (*pseudo == super::PseudoElementTarget::of_kind(kind)).then_some(record)
                })?,
        };
        Some(
            record
                .custom_declarations()
                .map_or_else(Vec::new, |declarations| declarations.to_vec()),
        )
    }

    /// The custom properties the longhands of the record of `node`, or of its pseudo-element
    /// `pseudo`, substitute.
    pub(in super::super) fn custom_property_reads_of(
        &self,
        node: StyleNodeID,
        pseudo: Option<u8>,
    ) -> Option<&super::super::custom_property_cascade::LonghandCustomPropertyReads> {
        let record = match pseudo {
            None => self.records.get(node)?,
            Some(kind) => self
                .records
                .pseudo_element_records(node)
                .iter()
                .find_map(|(pseudo, record)| {
                    (*pseudo == super::PseudoElementTarget::of_kind(kind)).then_some(record)
                })?,
        };
        Some(&record.rare().custom_property_reads)
    }

    fn bump(&mut self, field: LiveField, value: u64) {
        self.report[field as usize] += value;
        self.counts.note(field, value);
    }

    /// Note what `changes` and the transaction's element inputs restyle in `hints`, reading what
    /// they replaced on the projection's side from before the transaction, or answer that they can
    /// restyle any element.
    fn invalidate(
        &mut self,
        state: &RetainedState,
        transaction: &StyleUpdate,
        changes: &[(StyleNodeID, ElementChange)],
    ) {
        if self.restyles_nothing(state, transaction, changes) {
            return;
        }
        let mut scratch = std::mem::take(&mut self.scratch.invalidate);
        self.invalidate_in(state, transaction, changes, &mut scratch);
        scratch.recycle();
        self.scratch.invalidate = scratch;
    }

    fn invalidate_in(
        &mut self,
        state: &RetainedState,
        transaction: &StyleUpdate,
        element_changes: &[(StyleNodeID, ElementChange)],
        scratch: &mut InvalidateScratch,
    ) {
        let (tree, projection) = (&state.tree, &state.projection);
        let InvalidateScratch {
            by_element,
            sibling_parents,
            moves,
            new_previous_siblings,
            relations_moved,
            children: children_scratch,
            positions_before,
            positions,
            types,
            links,
            changes,
        } = scratch;
        // NB: A fact only a `:has()` argument tests is kept until the anchors it reaches are found.
        let add_trigger = |by_element: &mut Vec<(StyleNodeID, Trigger, bool)>, node, trigger| match self
            .invalidation
            .reach_of(&trigger)
        {
            TriggerReach::None => {}
            TriggerReach::Indirect => by_element.push((node, trigger, false)),
            TriggerReach::Direct => by_element.push((node, trigger, true)),
        };
        // NB: A fact of an element that does not arrive or leave only moves where the host makes
        //     it anew, which restyles the element and what its old and new values reach.
        let facts_of = |node| {
            [TransactionFactSide::Before, TransactionFactSide::After].map(|side| projection.facts_on(side, node))
        };
        for &(node, change) in element_changes {
            match change {
                ElementChange::Relations(old, new) => {
                    relations_moved.insert(node, (old, new));
                    let old_parent = old.and_then(|relations| relations.parent);
                    let new_parent = new.and_then(|relations| relations.parent);
                    match (old, new) {
                        (Some(old), None) => {
                            self.records.forget(node);
                            moves.push((node, true, old.next_element_sibling));
                            new_previous_siblings.extend(
                                old.next_element_sibling
                                    .map(|next| (next, [old.previous_element_sibling, None])),
                            );
                        }
                        (None, None) => self.records.forget(node),
                        (old, Some(new)) if old.is_none_or(|old| old.parent != new.parent) => {
                            self.hints.add(node, RESTYLE_SELF | RESTYLE_DESCENDANTS);
                            // NB: The element and its children are matched again, which notes anew what they read.
                            projection.forget_structural_flags(node);
                            moves.push((node, false, new.next_element_sibling));
                            new_previous_siblings.extend(
                                new.next_element_sibling
                                    .map(|next| (next, [new.previous_element_sibling, None])),
                            );
                            if let Some(old) = old {
                                moves.push((node, true, old.next_element_sibling));
                                new_previous_siblings.extend(
                                    old.next_element_sibling
                                        .map(|next| (next, [old.previous_element_sibling, None])),
                                );
                            }
                        }
                        (old, Some(new)) => {
                            if let Some(old) = old
                                && old.previous_element_sibling != new.previous_element_sibling
                            {
                                new_previous_siblings
                                    .push((node, [old.previous_element_sibling, new.previous_element_sibling]));
                            }
                            // An element assigned to another slot inherits from it, and matches
                            // the `::slotted()` rules of its tree, as do its descendants the
                            // rules of the trees it is slotted into from them.
                            if old.is_some_and(|old| old.assigned_slot != new.assigned_slot) {
                                self.hints.add(node, RESTYLE_SELF | RESTYLE_DESCENDANTS);
                            }
                        }
                    }
                    if old != new {
                        sibling_parents.extend(old_parent);
                        sibling_parents.extend(new_parent);
                    }
                }
                ElementChange::Id(old, new) => {
                    for id in [old, new] {
                        if !id.is_none() {
                            add_trigger(by_element, node, Trigger::Id(id));
                        }
                    }
                }
                // An element arriving is restyled with its descendants.
                ElementChange::ArrivingFacts => self.hints.add(node, RESTYLE_SELF | RESTYLE_DESCENDANTS),
                ElementChange::State(fact) => add_trigger(by_element, node, Trigger::State(fact)),
                ElementChange::Feature(feature) => {
                    match feature {
                        LocalFeatureKey::Class(class) => add_trigger(by_element, node, Trigger::Class(class)),
                        LocalFeatureKey::Attribute(name) => {
                            let mut names = projection.attribute_name_keys(name);
                            names.sort_unstable();
                            for (position, &name) in names.iter().enumerate() {
                                if !name.is_none() && (position == 0 || names[position - 1] != name) {
                                    add_trigger(by_element, node, Trigger::Attribute(name));
                                }
                            }
                            let readers: SmallVec<[StyleNodeID; 2]> = self.attribute_readers(state, node).collect();
                            for reader in readers {
                                self.hints.add(reader, RESTYLE_SELF);
                            }
                        }
                        LocalFeatureKey::CustomState(state) => {
                            add_trigger(by_element, node, Trigger::CustomState(state));
                        }
                        LocalFeatureKey::Emptiness => add_trigger(by_element, node, Trigger::Emptiness),
                        LocalFeatureKey::Language => add_trigger(by_element, node, Trigger::Language),
                        LocalFeatureKey::Directionality => {
                            add_trigger(by_element, node, Trigger::Directionality);
                        }
                        LocalFeatureKey::HeadingLevel => add_trigger(by_element, node, Trigger::Heading),
                        LocalFeatureKey::Part(part) => add_trigger(by_element, node, Trigger::Part(part)),
                        // The hosts an element is exposed to `::part()` rules of moved, which
                        // decides the rules it matches.
                        LocalFeatureKey::PartExposure => self.hints.add(node, RESTYLE_SELF),
                        LocalFeatureKey::TagName | LocalFeatureKey::FoldedTagName => {
                            self.hints.add(node, RESTYLE_SELF);
                            for facts in facts_of(node).into_iter().flatten() {
                                add_trigger(by_element, node, Trigger::Tag(facts.tag()));
                            }
                        }
                        LocalFeatureKey::Id => {
                            for facts in facts_of(node).into_iter().flatten() {
                                if !facts.id().is_none() {
                                    add_trigger(by_element, node, Trigger::Id(facts.id()));
                                }
                            }
                        }
                    }
                }
            }
        }
        for &node in &transaction.declarations {
            self.hints.add(node, RESTYLE_SELF);
        }
        for &(node, input) in &transaction.style_inputs {
            // No selector reads a font: an element whose font environment moved, and nothing else
            // it owes, is computed again from the matches it holds.
            if input.reaction & !(STYLE_REACTION_PUBLISHED_STYLE | STYLE_REACTION_FONT_INPUTS_CHANGED) == 0 {
                self.hints.mark(node, RESTYLE_SELF | RECOMPUTE_SELF);
                continue;
            }
            let descendants = input.reaction & STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES != 0;
            self.hints
                .add(node, RESTYLE_SELF | if descendants { RESTYLE_DESCENDANTS } else { 0 });
        }

        sibling_parents.sort_unstable();
        sibling_parents.dedup();
        Self::note_moves_within_parents(&mut self.hints, projection, relations_moved, moves);

        // https://drafts.csswg.org/css-values-5/#tree-counting
        // An element arriving, leaving or moving among its siblings changes what `sibling-index()`
        // and `sibling-count()` compute to for its siblings.
        for &parent in sibling_parents.iter() {
            // NB: Every record reading it noted so on its element's parent as it was held, and every style query
            //     reading it on its container's parent as it answered.
            if !tree.is_live(parent)
                || !projection
                    .structural_flags(parent)
                    .contains(StructuralFlags::CHILD_READS_SIBLING_POSITION)
            {
                continue;
            }
            let counted_containers = state
                .containers_counted_by_style_queries
                .lock()
                .expect("the counted containers are never held across a panic");
            for child in projection.children(parent) {
                if self
                    .records
                    .get(child)
                    .is_some_and(|record| record.rare().reads.reads_sibling_position)
                    || self
                        .records
                        .pseudo_element_records(child)
                        .iter()
                        .any(|(_, record)| record.rare().reads.reads_sibling_position)
                {
                    self.hints.add(child, RESTYLE_SELF);
                }
                // A style() query counted where this child stands as a container, which no record of it may read: the
                // elements below it that read their containers read them anew.
                if counted_containers.contains(&child) {
                    self.hints.add(child, CONTAINERS_MOVED);
                }
            }
        }

        // An element arriving or leaving moves where its siblings stand, counted from the start
        // for those after it and from the end for those before it, among all of them and among
        // those of its type.
        let reads_positions = self.invalidation.reads_positions;
        let type_on = |side, node| {
            projection
                .facts_on(side, node)
                .map(|facts| (facts.tag(), facts.namespace()))
        };
        // NB: An element whose facts are not known from before has the type it has now.
        let type_before =
            |node| type_on(TransactionFactSide::Before, node).or_else(|| type_on(TransactionFactSide::After, node));
        let type_now = |node| type_on(TransactionFactSide::After, node);
        for &parent in sibling_parents.iter().filter(|_| reads_positions.any()) {
            let flags = projection.structural_flags(parent);
            if !tree.is_live(parent) || !flags.contains(StructuralFlags::CHILDREN_READ_POSITIONS) {
                continue;
            }
            // NB: What no child of the parent matched read of where it stands cannot move what it matched.
            let reads_position = |from_end: bool, of_type: bool| {
                reads_positions.reads(from_end, of_type)
                    && (!from_end || flags.contains(StructuralFlags::CHILDREN_READ_POSITIONS_FROM_END))
                    && (!of_type || flags.contains(StructuralFlags::CHILDREN_READ_POSITIONS_OF_TYPE))
            };
            let reads_type_positions = reads_position(false, true) || reads_position(true, true);
            let only_edges = !flags.contains(StructuralFlags::CHILDREN_READ_POSITIONS_BEYOND_EDGES);
            children_scratch.clear();
            if projection.children_were_connected(parent) {
                children_scratch.extend(projection.children_on(TransactionFactSide::Before, parent));
            }
            let before_count = children_scratch.len();
            children_scratch.extend(projection.children(parent));
            let (before, now) = children_scratch.split_at(before_count);
            // NB: A child among those both sides start with alike stands where it stood counted
            //     from the start, and one among those they end with alike counted from the end,
            //     among those of its type too where the types are alike. Where every child of one
            //     side is among those, the others arrived, which are restyled whole, or left.
            let alike = |(&before, &now): (&StyleNodeID, &StyleNodeID)| {
                before == now && (!reads_type_positions || type_before(before) == type_now(now))
            };
            let common_start = before.iter().zip(now).take_while(|&pair| alike(pair)).count();
            let common_end = before
                .iter()
                .rev()
                .zip(now.iter().rev())
                .take(before.len().min(now.len()) - common_start)
                .take_while(|&pair| alike(pair))
                .count();
            let may_move = |from_end: bool, index: usize| match from_end {
                false => index >= common_start,
                true => index < now.len() - common_end,
            };
            let reads_from = |from_end: bool| reads_position(from_end, false) || reads_position(from_end, true);
            if !(reads_from(false) && common_start < before.len().min(now.len())
                || reads_from(true) && common_end < before.len().min(now.len()))
            {
                continue;
            }
            positions_before.clear();
            if !before.is_empty() {
                measure_sibling_positions(
                    before,
                    reads_type_positions.then_some(&type_before as &dyn Fn(_) -> _),
                    types,
                    positions_before,
                );
                positions_before.sort_unstable_by_key(|&(child, _)| child);
            }
            measure_sibling_positions(
                now,
                reads_type_positions.then_some(&type_now as &dyn Fn(_) -> _),
                types,
                positions,
            );
            for (index, &(child, position)) in positions.iter().enumerate() {
                if ![false, true]
                    .into_iter()
                    .any(|from_end| reads_from(from_end) && may_move(from_end, index))
                {
                    continue;
                }
                // An element that arrived is restyled with its descendants.
                let Ok(before_index) = positions_before.binary_search_by_key(&child, |&(child, _)| child) else {
                    continue;
                };
                let before_position = positions_before[before_index].1;
                let Some(row) = projection.row_of(child) else {
                    continue;
                };
                for (from_end, of_type) in [(false, false), (true, false), (false, true), (true, true)]
                    .into_iter()
                    .filter(|&(from_end, of_type)| reads_position(from_end, of_type) && may_move(from_end, index))
                {
                    let index = |position: &SiblingPosition| match (of_type, from_end) {
                        (false, false) => Some(position.from_start),
                        (false, true) => Some(position.from_end),
                        (true, false) => position.of_type.map(|of_type| of_type.0),
                        (true, true) => position.of_type.map(|of_type| of_type.1),
                    };
                    // NB: A sibling whose type is unknown is taken to have moved. Where selectors read only
                    //     whether a child stands first, one standing elsewhere before and now answers the same.
                    let moved = match (index(&before_position), index(&position)) {
                        (Some(before), Some(now)) => before != now && (!only_edges || before == 0 || now == 0),
                        _ => true,
                    };
                    // NB: What an `of <selector>` counts changes with the siblings that came and left, whose count
                    //     can leave the child where it stood.
                    let moved = moved || (!only_edges && reads_positions.counts_by_selector());
                    if !moved {
                        continue;
                    }
                    let mut add = |key| add_trigger(by_element, child, Trigger::Position { from_end, of_type, key });
                    add(PositionKey::Any);
                    for &class in row.classes() {
                        add(PositionKey::Class(class));
                    }
                    let id = row.id();
                    if !id.is_none() {
                        add(PositionKey::Id(id));
                    }
                    for tag in [row.tag(), row.folded_tag()] {
                        if !tag.is_none() {
                            add(PositionKey::Tag(tag));
                        }
                    }
                }
            }
        }

        // Where siblings stand is measured again where the tree moved since the memos measured it,
        // or the transaction moved an element, which the tree before it places elsewhere.
        let dom_order_version = tree.dom_order_version();
        let moved_any = !relations_moved.is_empty();
        if moved_any || self.scratch.memos_dom_order_version != Some(dom_order_version) {
            for scratch in self.scratch.memos.made() {
                let mut scratch = scratch.lock().expect("no invalidation panicked");
                scratch.after = MatchScratch::default();
                scratch.before = MatchScratch::default();
            }
        }
        self.scratch.memos_dom_order_version = (!moved_any).then_some(dom_order_version);
        let (map, index) = (&self.invalidation, &self.index);
        let mut serial_memo = self
            .scratch
            .memos
            .get(0)
            .expect("the calling thread has a memo")
            .lock()
            .expect("no invalidation panicked");
        // NB: An element leaving with the subtree of another names no parent it left, and the
        //     tree as it was holds no subtree of an element that left, which a `:has()` can only
        //     be answered for as it is now.
        let subtrees_left = relations_moved
            .values()
            .any(|&(old, new)| new.is_none() && old.is_some_and(|old| old.parent.is_none()));
        let mut follower = Follower::new(state, map, index, &mut serial_memo);
        follower.subtrees_left = subtrees_left;

        // https://drafts.csswg.org/selectors-4/#relational
        // A change to an element a `:has()` argument reaches can flip the answer of the anchors it
        // stands to as the argument's axis says, whose compounds testing the query are followed
        // from them as from a change to their own facts.
        if map.query_count() != 0 {
            let mut anchors = RelativeAnchors::default();
            for &(node, trigger, _) in by_element.iter() {
                for (query, nested) in map.queries_testing(&trigger) {
                    match nested {
                        false => {
                            follower.reach_anchors(query, node, TransactionFactSide::After, false, &mut anchors);
                        }
                        true => follower.reach_anchors_through_nested(query, node, &mut anchors),
                    }
                }
            }
            // An anchor whose argument an element that left with the subtree of another can be a
            // witness of is restyled, from the element whose subtree it was.
            let mut departed_within: SmallVec<[u32; 4]> = SmallVec::new();
            let mut departed_roots: SmallVec<[StyleNodeID; 4]> = SmallVec::new();
            for (&node, &(old, new)) in relations_moved.iter() {
                let (Some(old), None) = (old, new) else {
                    continue;
                };
                if old.parent.is_some() {
                    departed_roots.push(node);
                    continue;
                }
                let carried = follower.facts_carried(node, TransactionFactSide::Before, false);
                for query in map.queries() {
                    if !departed_within.contains(&query)
                        && carries_any(map.relative(query).arrival_keys.as_ref(), carried.as_deref())
                    {
                        departed_within.push(query);
                    }
                }
            }
            for &query in &departed_within {
                for &root in &departed_roots {
                    follower.reach_anchors(query, root, TransactionFactSide::Before, true, &mut anchors);
                }
            }
            for (&node, &(old, new)) in relations_moved.iter() {
                let (old_parent, new_parent) = (old.and_then(|old| old.parent), new.and_then(|new| new.parent));
                let previous_siblings = (
                    old.and_then(|old| old.previous_element_sibling),
                    new.and_then(|new| new.previous_element_sibling),
                );
                if old.is_some() == new.is_some()
                    && old_parent == new_parent
                    && previous_siblings.0 == previous_siblings.1
                {
                    continue;
                }
                // An element moving between parents takes its subtree along, which no input names.
                let with_subtree = old.is_some() && new.is_some() && old_parent != new_parent;
                for (side, parent, present) in [
                    (TransactionFactSide::Before, old_parent, old.is_some()),
                    (TransactionFactSide::After, new_parent, new.is_some()),
                ] {
                    if present {
                        follower.note_relative_arrival(node, side, parent, with_subtree, &mut anchors);
                    }
                }
            }
            for (anchor, query) in anchors.flipped {
                if !anchors.restyled.contains(&(anchor, query)) {
                    let trigger = Trigger::Relative(query);
                    by_element.push((anchor, trigger, map.has_effects(&trigger)));
                }
            }
            for (anchor, query) in anchors.restyled {
                follower.restyle_relative_anchor(anchor, query);
            }
        }

        // The next sibling an element that left had, which may have left as well.
        let departed_next: LazyCell<HashMap<StyleNodeID, Option<StyleNodeID>>, _> = LazyCell::new(|| {
            moves
                .iter()
                .filter(|&&(node, left, _)| left && !tree.is_live(node))
                .map(|&(node, _, next)| (node, next))
                .collect()
        });
        let live_from = |mut node: Option<StyleNodeID>| {
            while let Some(current) = node {
                if tree.is_live(current) {
                    return Some(current);
                }
                node = departed_next.get(&current).copied().flatten();
            }
            None
        };
        // What an element arriving or leaving changes for its later siblings, as all its facts
        // appearing or disappearing.
        // The elements that arrived are restyled with their descendants, which nothing need be
        // followed to.
        let arrived: LazyCell<HashSet<StyleNodeID>, _> = LazyCell::new(|| {
            moves
                .iter()
                .filter(|&&(_, left, _)| !left)
                .map(|&(node, ..)| node)
                .collect()
        });
        let restyled_whole = |node: Option<StyleNodeID>| {
            std::iter::successors(node, |&node| projection.parent(node)).any(|node| arrived.contains(&node))
        };
        // NB: What the later siblings of each element are is found once over a transaction, which may move
        //     every child of a long list.
        let next_sibling = |node: StyleNodeID| projection.next_element_sibling(node);
        let bloom_of = |node: StyleNodeID| projection.facts(node).map_or(0, |facts| facts.dispatch_bloom(false));
        let mut past_arrived: HashMap<StyleNodeID, Option<StyleNodeID>> = HashMap::default();
        let mut later_blooms: HashMap<StyleNodeID, u64> = HashMap::default();
        for &(node, left, first_later_sibling) in moves.iter() {
            // NB: What no child of the parent matched read of its earlier siblings cannot move with them.
            let parent = relations_moved
                .get(&node)
                .and_then(|&(old, new)| if left { old } else { new })
                .and_then(|relations| relations.parent);
            if parent.is_some_and(|parent| {
                !projection
                    .structural_flags(parent)
                    .contains(StructuralFlags::CHILDREN_READ_SIBLINGS)
            }) {
                continue;
            }
            if !left && restyled_whole(projection.parent(node)) {
                continue;
            }
            let Some(first_later_sibling) = live_from(first_later_sibling)
                .and_then(|sibling| first_not_arrived(sibling, &arrived, next_sibling, &mut past_arrived))
            else {
                continue;
            };
            let triggers = match left {
                true => match follower.row_before(node).map(triggers_of) {
                    Some(found) => found,
                    // What a departed element carried is lost: everything after it restyles.
                    None => {
                        let mut sibling = Some(first_later_sibling);
                        while let Some(current) = sibling {
                            follower.add(current, RESTYLE_SELF | RESTYLE_DESCENDANTS);
                            sibling = projection.next_element_sibling(current);
                        }
                        continue;
                    }
                },
                false => match projection.row_of(node) {
                    Some(row) => triggers_of(row),
                    None => continue,
                },
            };
            links.clear();
            // NB: A compound is matched leftwards only where the compound right of it could match a later sibling
            //     and its ancestors could match around the element, which rejects most of them for far less.
            let next_bloom = bloom_of(first_later_sibling);
            let mut held_effects: SmallVec<[HeldDependency; 16]> = triggers
                .iter()
                .flat_map(|trigger| map.held_sibling_effects(trigger))
                .filter(|held| {
                    map.right(held.dependency).is_some_and(|link| {
                        let step = map.step(link);
                        step.required_key.is_none_or(|key| {
                            let carried = match step.combinator {
                                ReachCombinator::NextSibling => next_bloom,
                                _ => bloom_from(first_later_sibling, next_sibling, bloom_of, &mut later_blooms),
                            };
                            carried & dispatch_bloom_bit(key) != 0
                        })
                    })
                })
                .collect();
            follower.retain_effects_around(node, &mut held_effects);
            // NB: A compound testing several of the element's facts is matched once.
            let mut effects: SmallVec<[Dependency; 16]> = held_effects.iter().map(|held| held.dependency).collect();
            effects.sort_unstable();
            effects.dedup();
            for effect in effects {
                // A departed element is matched where it stood.
                let matched = follower.matches_from(effect, node, left).unwrap_or(true);
                if matched {
                    links.extend(follower.map.right(effect));
                }
            }
            links.sort_unstable();
            links.dedup();
            if !links.is_empty() {
                follower.follow_across(first_later_sibling, links);
            }
        }
        // An element whose previous sibling changed is the next sibling of another element than
        // before, which the next-sibling combinators of the compounds both match are followed to
        // it for.
        for &(node, previous_siblings) in new_previous_siblings.iter() {
            let Some(node) = live_from(Some(node)) else {
                continue;
            };
            if projection.parent(node).is_some_and(|parent| {
                !projection
                    .structural_flags(parent)
                    .contains(StructuralFlags::CHILDREN_READ_PREVIOUS_SIBLING)
            }) || restyled_whole(Some(node))
            {
                continue;
            }
            links.clear();
            let carried = projection.facts(node).map_or(0, |facts| facts.dispatch_bloom(false));
            for previous in previous_siblings.into_iter().flatten() {
                let Some(row) = projection.row_of(previous).filter(|_| tree.is_live(previous)) else {
                    continue;
                };
                // NB: The sibling may have changed in the same transaction, which its own change followed to
                //     the sibling after it now and not to this one: it stands for what it matched then too.
                let mut triggers = triggers_of(row);
                let facts_moved = projection.facts_on(TransactionFactSide::Before, previous)
                    != projection.facts_on(TransactionFactSide::After, previous);
                if facts_moved {
                    triggers.extend(follower.row_before(previous).map(triggers_of).into_iter().flatten());
                }
                // NB: As for the moves above, only where `node` could match the compound right of it.
                let mut held_effects: SmallVec<[HeldDependency; 16]> = triggers
                    .iter()
                    .flat_map(|trigger| map.held_sibling_effects(trigger))
                    .filter(|held| {
                        map.right(held.dependency).is_some_and(|link| {
                            let step = map.step(link);
                            step.combinator == ReachCombinator::NextSibling
                                && step
                                    .required_key
                                    .is_none_or(|key| carried & dispatch_bloom_bit(key) != 0)
                        })
                    })
                    .collect();
                follower.retain_effects_around(node, &mut held_effects);
                let mut effects: SmallVec<[Dependency; 16]> = held_effects.iter().map(|held| held.dependency).collect();
                effects.sort_unstable();
                effects.dedup();
                for effect in effects {
                    let Some(link) = follower.map.right(effect) else {
                        continue;
                    };
                    if follower.matches_from(effect, previous, false).unwrap_or(true)
                        // NB: A compound testing names alone with nothing left of it answers by the sibling's
                        //     facts, which answer alike on both sides where they did not move.
                        || (facts_moved
                            || follower.map.left(effect).is_some()
                            || follower.map.operand_names(effect).is_empty())
                            && follower.matches_from(effect, previous, true).unwrap_or(true)
                    {
                        links.push(link);
                    }
                }
            }
            links.sort_unstable();
            links.dedup();
            if !links.is_empty() {
                follower.follow_across(node, links);
            }
        }
        // An element child arriving or leaving can change whether its parent is empty, which no
        // fact tells the side before of.
        if map.has_effects(&Trigger::Emptiness) {
            let effects = map.effects(&Trigger::Emptiness);
            for parent in sibling_parents.iter() {
                if !tree.is_live(*parent)
                    || !projection
                        .structural_flags(*parent)
                        .contains(StructuralFlags::READ_EMPTINESS)
                {
                    continue;
                }
                links.clear();
                for effect in effects.clone() {
                    match follower.map.right(effect) {
                        None => follower.add_for(*parent, RESTYLE_SELF, effect.entry),
                        Some(link) => links.push(link),
                    }
                }
                if !links.is_empty() {
                    follower.follow(*parent, links);
                }
            }
        }
        let (mut visits, mut tests) = (follower.visits, follower.tests);
        drop(serial_memo);

        // What each element's own changes reach is followed apart from the others'.
        by_element.retain(|&(_, _, has_effects)| has_effects);
        by_element.sort_unstable_by_key(|&(node, ..)| node);
        let mut start = 0;
        for run in by_element.chunk_by(|a, b| a.0 == b.0) {
            changes.push(start..start + run.len());
            start += run.len();
        }
        let by_element = &*by_element;
        const CHANGES_PER_TASK: usize = 8;
        let pool = state.style_layout_pool.as_ref().and_then(|pool| pool.started());
        let follow_all = |chunk: &[std::ops::Range<usize>]| {
            let thread = pool.as_ref().and_then(|pool| pool.current_thread_index());
            let memo = self
                .scratch
                .memos
                .get(thread.map_or(0, |index| index + 1) % self.scratch.memos.len())
                .expect("every thread has a memo");
            let mut memo = memo.lock().expect("no invalidation panicked");
            let mut follower = Follower::new(state, map, index, &mut memo);
            follower.subtrees_left = subtrees_left;
            for run in chunk {
                let changed = &by_element[run.clone()];
                follower.follow_changes(changed[0].0, changed);
            }
            (follower.visits, follower.tests)
        };
        let (followed_visits, followed_tests) = match pool.as_deref() {
            Some(pool) if changes.len() > CHANGES_PER_TASK => {
                use rayon::prelude::*;
                pool.install(|| {
                    changes
                        .par_chunks(CHANGES_PER_TASK)
                        .map(follow_all)
                        .reduce(|| (0, 0), |a, b| (a.0 + b.0, a.1 + b.1))
                })
            }
            _ => follow_all(changes),
        };
        visits += followed_visits;
        tests += followed_tests;
        // NB: Each thread's followers add to the memo of the thread.
        for memo in self.scratch.memos.made() {
            let mut memo = memo.lock().expect("no invalidation panicked");
            for (node, bits, entry) in memo.added.drain(..) {
                self.hints.add_for(node, bits, entry);
            }
            recycle(&mut memo.added);
        }
        self.bump(LiveField::InvalidationTests, tests);
        self.bump(LiveField::InvalidationVisits, visits);
    }

    /// Whether a transaction restyles no element, which leaves nothing to follow: it names no
    /// element's declarations or style reaction, and each change changes the rules, which was
    /// noted before, or a fact of an element that no selector tests, outside a `:has()` argument
    /// or in one, and that no `attr()` the element's style substitutes reads.
    fn restyles_nothing(
        &self,
        state: &RetainedState,
        transaction: &StyleUpdate,
        changes: &[(StyleNodeID, ElementChange)],
    ) -> bool {
        let map = &self.invalidation;
        let untested = |trigger| !map.tests(&trigger);
        let changes_restyle_nothing = changes.iter().all(|&(node, change)| match change {
            ElementChange::Id(old, new) => [old, new]
                .into_iter()
                .all(|id| id.is_none() || untested(Trigger::Id(id))),
            ElementChange::State(fact) => untested(Trigger::State(fact)),
            ElementChange::Relations(..) | ElementChange::ArrivingFacts => false,
            ElementChange::Feature(feature) => match feature {
                LocalFeatureKey::Class(class) => untested(Trigger::Class(class)),
                LocalFeatureKey::Attribute(name) => {
                    state
                        .projection
                        .attribute_name_keys(name)
                        .into_iter()
                        .all(|name| name.is_none() || untested(Trigger::Attribute(name)))
                        && self.attribute_readers(state, node).next().is_none()
                }
                LocalFeatureKey::CustomState(custom_state) => untested(Trigger::CustomState(custom_state)),
                LocalFeatureKey::Emptiness => untested(Trigger::Emptiness),
                LocalFeatureKey::Language => untested(Trigger::Language),
                LocalFeatureKey::Directionality => untested(Trigger::Directionality),
                LocalFeatureKey::HeadingLevel => untested(Trigger::Heading),
                LocalFeatureKey::Part(part) => untested(Trigger::Part(part)),
                LocalFeatureKey::PartExposure
                | LocalFeatureKey::TagName
                | LocalFeatureKey::FoldedTagName
                | LocalFeatureKey::Id => false,
            },
        });
        changes_restyle_nothing && transaction.declarations.is_empty() && transaction.style_inputs.is_empty()
    }

    /// https://drafts.csswg.org/css-values-5/#attr-notation
    /// The elements whose style reads the attributes of `node` through an `attr()`: the element,
    /// and the elements standing for its pseudo-elements.
    fn attribute_readers<'a>(
        &'a self,
        state: &'a RetainedState,
        node: StyleNodeID,
    ) -> impl Iterator<Item = StyleNodeID> + 'a {
        std::iter::once(node)
            .chain(state.backing_elements(node, u64::MAX))
            .filter(|&reader| {
                self.records
                    .get(reader)
                    .is_some_and(|record| record.rare().reads.reads_attributes)
                    || self
                        .records
                        .pseudo_element_records(reader)
                        .iter()
                        .any(|(_, record)| record.rare().reads.reads_attributes)
            })
    }

    /// Note the elements that moved among their siblings within a parent, which leave where they
    /// stood and arrive where they stand now: they restyle with their descendants, and what their
    /// facts change for the siblings after them is followed from both places, as for an element
    /// leaving one parent and arriving under another. `relations_moved` has the relations each
    /// element whose relations changed had before and has now, and `moves` takes what is followed.
    ///
    /// The siblings that kept their order are the longest run of them in the same order before and
    /// now, so the fewest elements count as moved.
    fn note_moves_within_parents(
        hints: &mut Hints,
        projection: &Projection,
        relations_moved: &HashMap<StyleNodeID, (Option<TreeRelations>, Option<TreeRelations>)>,
        moves: &mut Vec<(StyleNodeID, bool, Option<StyleNodeID>)>,
    ) {
        let arrived_under = |node: StyleNodeID, parent: StyleNodeID| {
            relations_moved.get(&node).is_some_and(|(old, new)| {
                new.is_some_and(|new| new.parent == Some(parent)) && old.is_none_or(|old| old.parent != Some(parent))
            })
        };
        let left_from = |node: StyleNodeID, parent: StyleNodeID| {
            relations_moved.get(&node).is_some_and(|(old, new)| {
                old.is_some_and(|old| old.parent == Some(parent)) && new.is_none_or(|new| new.parent != Some(parent))
            })
        };
        // The nearest previous sibling of an element that stood under the parent both before and
        // now, on either side: past the siblings that arrived, or past those that left.
        let staying_before = |mut node: Option<StyleNodeID>, parent: StyleNodeID, now: bool| {
            while let Some(current) = node {
                let passed = match now {
                    true => arrived_under(current, parent),
                    false => left_from(current, parent),
                };
                if !passed {
                    break;
                }
                let (old, new) = relations_moved[&current];
                node = match now {
                    true => new.and_then(|new| new.previous_element_sibling),
                    false => old.and_then(|old| old.previous_element_sibling),
                };
            }
            node
        };
        // The children staying under a parent keep their order where each one follows the one it
        // followed, and only an element whose previous sibling changed can follow another.
        let mut reordered: SmallVec<[StyleNodeID; 4]> = SmallVec::new();
        for (old, new) in relations_moved.values() {
            let (Some(old), Some(new)) = (old, new) else {
                continue;
            };
            let Some(parent) = new.parent.filter(|&parent| old.parent == Some(parent)) else {
                continue;
            };
            if old.previous_element_sibling != new.previous_element_sibling
                && staying_before(old.previous_element_sibling, parent, false)
                    != staying_before(new.previous_element_sibling, parent, true)
                && !reordered.contains(&parent)
            {
                reordered.push(parent);
            }
        }
        for parent in reordered {
            if !projection.is_live(parent) || !projection.children_were_connected(parent) {
                continue;
            }
            let children_before: Vec<StyleNodeID> =
                projection.children_on(TransactionFactSide::Before, parent).collect();
            let children: Vec<StyleNodeID> = projection.children(parent).collect();
            let staying: HashSet<StyleNodeID> = children.iter().copied().collect();
            let rank_before: HashMap<StyleNodeID, usize> = children_before
                .iter()
                .copied()
                .filter(|child| staying.contains(child))
                .enumerate()
                .map(|(rank, child)| (child, rank))
                .collect();
            let staying: Vec<(StyleNodeID, usize)> = children
                .iter()
                .filter_map(|&child| Some((child, *rank_before.get(&child)?)))
                .collect();
            let kept = longest_increasing_run(&staying);
            for &(node, _) in staying.iter().filter(|(node, _)| !kept.contains(node)) {
                hints.add(node, RESTYLE_SELF | RESTYLE_DESCENDANTS);
                moves.push((node, false, projection.next_element_sibling(node)));
                // The siblings that kept their order after where it stood all stand after the
                // first of them now.
                let first_kept_after = children_before
                    .iter()
                    .skip_while(|&&child| child != node)
                    .skip(1)
                    .copied()
                    .find(|child| kept.contains(child));
                moves.push((node, true, first_kept_after));
            }
        }
    }

    /// Mark the flat tree ancestors of every restyled element, through which the traversal
    /// reaches it, and note the restyled elements no restyled element is above.
    fn mark_dirty_ancestors(&mut self, tree: &Projection) {
        let hints = &mut self.hints;
        hints.topmost.clear();
        for index in 0..hints.marked.len() {
            let node = hints.marked[index];
            if !restyles(hints.get(node)) {
                continue;
            }
            if !tree.is_live(node) {
                hints.set_bits(node, 0);
                continue;
            }
            // The walk stops at an ancestor an earlier walk went through, which knows what is
            // above it, and tells the ones below it on the way back down.
            // NB: An element a pass deferred keeps its marks, but not what is above it.
            let mut under_restyled = false;
            let mut ancestor = tree.inheritance_parent(node);
            while let Some(current) = ancestor {
                match hints.under_restyled(current) {
                    UnderRestyled::Unknown => {}
                    known => {
                        under_restyled = known == UnderRestyled::Yes || restyles(hints.get(current));
                        break;
                    }
                }
                hints.path.push(current);
                ancestor = tree.inheritance_parent(current);
            }
            while let Some(current) = hints.path.pop() {
                hints.mark(current, DIRTY_DESCENDANTS);
                hints.set_under_restyled(
                    current,
                    match under_restyled {
                        true => UnderRestyled::Yes,
                        false => UnderRestyled::No,
                    },
                );
                under_restyled |= restyles(hints.get(current));
            }
            if !under_restyled {
                hints.topmost.push(node);
            }
        }
        recycle(&mut hints.path);
    }

    /// Have the host plan the animations of the elements under `root` naming one of the `@keyframes`
    /// rules `moved` names again.
    ///
    /// https://drafts.csswg.org/css-animations-1/#keyframes
    /// The animations an element names run the keyframes of the `@keyframes` rules the names
    /// refer to, which the host plans again where they may have moved, whether or not the
    /// element's record did.
    fn owe_animation_plans(&mut self, state: &RetainedState, root: StyleNodeID, moved: &MovedKeyframes) {
        // NB: A name is compared by the identity of the interned string it was published as, which
        //     the process-wide atom table keeps alive.
        let moved_names: Option<HashSet<usize>> = match moved {
            MovedKeyframes::Named(names) => state
                .atoms
                .raw_identities_of(names.iter().copied())
                .map(|raws| raws.into_iter().collect()),
            MovedKeyframes::Any | MovedKeyframes::None => None,
        };
        let names_moved = |record: &Record| {
            record.facts().declares_css_animations()
                && moved_names.as_ref().is_none_or(|moved_names| {
                    record_animation_names(record)
                        .any(|name| moved_names.contains(&ak::Utf16FlyString::from_utf16(name.units()).raw_identity()))
                })
        };
        // NB: An element the pass did not restyle names the animations it named.
        for node in state.projection.style_preorder(root) {
            if self.records.get(node).is_some_and(|record| names_moved(record))
                || self
                    .records
                    .pseudo_element_records(node)
                    .iter()
                    .any(|(_, record)| names_moved(record))
            {
                self.plans_owed.push(node);
                self.uninstalled.push(node);
            }
        }
    }

    /// Build the rule index again from every rule, and renumber the matches the records and hints
    /// hold to its entries. The rules it indexes are the ones the index it replaces indexed.
    fn rebuild_index(&mut self, state: &RetainedState) {
        let index = RuleIndex::build(&state.rules, state.process_rule_indexes());
        self.counts.note_index_build(&index);
        self.kept_hidden_records.forget();
        let renumbering = index.renumbering_from(&self.index);
        self.records.renumber_matches(&renumbering);
        self.hints.renumber_reasons(&renumbering);
        self.invalidation = Arc::new(InvalidationMap::build(&index));
        self.index = Arc::new(index);
    }

    /// Restyle what the transactions since the last pass changed, or everything, and keep the
    /// records. Answers what the pass measured, with the fonts it waited on. A pass with nothing
    /// hinted to restyle styles nothing, and is not counted.
    ///
    /// The pass styles in rounds: what waits on fonts or random base values in one is suspended,
    /// `resolve` resolves what it waited on, and the next round styles it, and the elements under
    /// it, as the round before would have.
    fn restyle(
        &mut self,
        retained: &mut RetainedState,
        root: StyleNodeID,
        pool: Option<&rayon::ThreadPool>,
        scope: RestyleScope,
        resolve: &mut ResolveWaits<'_>,
    ) -> traversal::Statistics {
        let state: &RetainedState = retained;
        let started = Instant::now();
        self.generation += 1;
        self.note_hints_moved_kept_hidden_records();
        let slot_count = state.projection.element_slot_count();
        self.hints.reserve(slot_count);
        self.scratch.cells.reserve(slot_count);
        let rebuilds_index = self.pending_everything;
        let updates_index = self.pending_index_rebuild;
        let hinted = match scope {
            RestyleScope::Everything => false,
            RestyleScope::Pending => !self.pending_everything,
        };
        self.pending_everything = false;
        self.pending_index_rebuild = false;
        let keyframes_moved = std::mem::take(&mut self.keyframes_moved);
        let keyframes_moved = (!keyframes_moved.is_empty()).then_some(keyframes_moved);
        let changed_rules = std::mem::take(&mut self.changed_rules);
        let changed_rule_contents = std::mem::take(&mut self.changed_rule_contents);
        let changed_sheets = std::mem::take(&mut self.changed_sheets);
        let reordered_scopes = std::mem::take(&mut self.reordered_scopes);
        let layered_scopes = std::mem::take(&mut self.layered_scopes);
        let matched_declarations_inputs = MatchedDeclarationsInputs::of(&state.document_style_computation_inputs);
        if keyframes_moved.is_some()
            || self
                .scratch
                .matched_declarations_inputs
                .is_none_or(|inputs| !inputs.same_besides_viewport(&matched_declarations_inputs))
        {
            self.kept_hidden_records.forget();
        }
        if rebuilds_index
            || updates_index
            || self.scratch.matched_declarations_inputs != Some(matched_declarations_inputs)
        {
            self.scratch.matched_declarations.forget();
            self.scratch.matched_declarations_inputs = Some(matched_declarations_inputs);
        }
        self.scratch.matched_declarations.prepare(slot_count);
        if rebuilds_index {
            self.rebuild_index(state);
        } else if updates_index {
            // Only what the changes name is indexed again, and the elements the entries that
            // died or moved matched, as their records tell, or that the entries that arrived
            // match, are restyled.
            let changes = Arc::make_mut(&mut self.index).update(
                &state.rules,
                state.process_rule_indexes(),
                &IndexChanges {
                    rules: &changed_rules,
                    rule_contents: &changed_rule_contents,
                    sheets: &changed_sheets,
                    reordered_scopes: &reordered_scopes,
                    layered_scopes: &layered_scopes,
                },
            );
            self.counts.note_index_update(&self.index, changes.arrived_entries);
            self.kept_hidden_records
                .note_rule_index_update(state, &self.index, &self.records, &changes);
            if hinted && !changes.is_empty() {
                let roots = rule_change_roots(state, root, &changes.scopes);
                let hints = &mut self.hints;
                // NB: Entries numbered anew leave no entry to rematch an element for alone.
                let renumbers_entries = self.index.wants_compaction() || changes.user_agent_moved;
                let mut kept_styles: Vec<(StyleNodeID, super::records::MatchList)> = Vec::new();
                changes.for_each_restyled(state, &self.index, &self.records, &roots, pool, |node, restyled| {
                    match restyled {
                        Restyled::Anew => hints.add(node, RESTYLE_SELF),
                        Restyled::Arrived(_) if renumbers_entries => hints.add(node, RESTYLE_SELF),
                        Restyled::Arrived(arrived) => {
                            hints.mark(node, RESTYLE_SELF | RECOMPUTE_SELF);
                            for entry in arrived {
                                hints.add_for(node, RESTYLE_SELF, entry);
                            }
                        }
                        Restyled::Kept(matches) => kept_styles.push((node, matches)),
                    }
                });
                for (node, matches) in kept_styles {
                    self.records.replace_matches(node, matches);
                }
            }
            // The elements a pass does not restyle keep their matches, but for those matching
            // an entry that died, which match again.
            if changes.any_died() {
                let numbering = changes.numbering(&self.index);
                self.records.renumber_matches(&numbering);
                self.hints.renumber_reasons(&numbering);
            }
            // The entries that died leave the invalidation map, and those that arrived are added
            // to it, but for another user agent index, from whose map it is built again.
            if self.index.wants_compaction() {
                self.rebuild_index(state);
            } else if changes.user_agent_moved {
                self.invalidation = Arc::new(InvalidationMap::build(&self.index));
            } else if changes.any_died() || changes.arrived_entries > 0 {
                let invalidation = Arc::make_mut(&mut self.invalidation);
                let entries = self.index.entries.len();
                let first_arrived = changes.first_arrived_entry.map_or(entries, |first| first as usize);
                invalidation.update(&self.index, changes.died(), first_arrived..entries);
            }
        }
        if hinted {
            self.mark_dirty_ancestors(&state.projection);
        }
        let invalidated = Instant::now();
        if hinted {
            let hinted = |bits: HintBits| {
                self.hints
                    .marked
                    .iter()
                    .filter(|&&node| self.hints.get(node) & bits != 0)
                    .count()
            };
            let (hinted_elements, hinted_subtrees) = (hinted(RESTYLES_SELF), hinted(RESTYLE_DESCENDANTS));
            self.bump(LiveField::HintedElements, hinted_elements as u64);
            self.bump(LiveField::HintedSubtrees, hinted_subtrees as u64);
        }

        // A pass that restyles some elements starts from the topmost of them, whose parents keep
        // their records.
        let roots = hinted.then(|| {
            let mut roots = std::mem::take(&mut self.hints.topmost);
            // An element whose parent holds no record is under one that is not displayed, and has
            // no style to compute, as has one below an element that is not displayed, unless the
            // main thread holds a style for it, which a read asked for. It and its descendants
            // forget the records they held, which their matches no longer follow.
            // NB: The elements below one that stopped being displayed keep the records they held,
            //     which the pass did not keep current, so every ancestor is asked.
            roots.retain(|&root| {
                let parent = state.projection.inheritance_parent(root);
                let styled = parent.is_none_or(|parent| self.records.get(parent).is_some())
                    && (state.held_style_records.contains(root)
                        || std::iter::successors(parent, |&ancestor| state.projection.inheritance_parent(ancestor))
                            .all(|ancestor| self.records.get(ancestor).is_some_and(|record| record.is_displayed())));
                if !styled {
                    for node in state.projection.style_preorder(root) {
                        self.records.forget(node);
                    }
                }
                styled
            });
            // Elements near each other in the tree tend to style alike, and a work unit's
            // caches find more of them where they are together.
            roots.sort_unstable();
            roots
        });
        if roots.as_ref().is_some_and(Vec::is_empty) {
            self.hints.topmost = roots.unwrap_or_default();
            if let Some(moved) = &keyframes_moved {
                self.owe_animation_plans(state, root, moved);
            }
            self.keep_deferred_hints(&state.projection, &[], &[]);
            self.bump(LiveField::InvalidationNanoseconds, started.elapsed().as_nanos() as u64);
            return traversal::Statistics::default();
        }
        self.hints.sort_reasons();
        let held_root_font = traversal::held_root_font(
            state,
            &Records::new(state, &self.scratch.cells, Some(&self.records)),
            root,
        );
        // What the document element's record says of its font is the pass's, whichever round
        // styled it.
        let (mut root_font, mut root_font_moved) = (None, None);
        let mut statistics = traversal::Statistics::default();
        let mut suspended = None;
        let mut waiting_restyling_descendants = HashSet::default();
        for round in 1..=MAX_SUSPENDING_ROUNDS {
            let state: &RetainedState = retained;
            let reasons = self.hints.reasons();
            let records = Records::new(state, &self.scratch.cells, Some(&self.records));
            let context = PassContext {
                hints: hinted.then_some(&self.hints.cells),
                reasons: hinted.then_some(&reasons),
                keeps_matches: true,
                suspends: true,
                group_caches: Some(&self.scratch.groups),
                kept_matched_declarations: Some(&self.scratch.matched_declarations),
                waits_once: round + 1 < MAX_SUSPENDING_ROUNDS,
                cache_audit: self.cache_audit,
                root_font: root_font.map_or_else(Default::default, std::sync::OnceLock::from),
                root_font_moved: root_font_moved.map_or_else(Default::default, std::sync::OnceLock::from),
                ..PassContext::new(state, &self.index, &self.initial, &records, held_root_font)
            };
            match suspended.take() {
                None => {
                    statistics = match &roots {
                        Some(roots) => traversal::style_subtrees(&context, pool, roots),
                        None => traversal::style_document(&context, pool, root),
                    }
                }
                Some(suspended) => statistics.add_round(traversal::resume(&context, pool, suspended)),
            }
            root_font = context.root_font.get().copied();
            root_font_moved = context.root_font_moved.get().copied();
            if statistics.suspended.is_empty() {
                break;
            }
            if round == MAX_SUSPENDING_ROUNDS {
                waiting_restyling_descendants =
                    traversal::settle_suspended(&context, std::mem::take(&mut statistics.suspended));
                break;
            }
            let (next, fonts, random_bases) = statistics.take_suspended();
            resolve(retained, fonts, random_bases);
            suspended = Some(next);
        }
        let state: &RetainedState = retained;
        if let Some(root_font) = root_font
            && let Some(document_element) = traversal::document_element_at(state, root)
        {
            self.records.keep_root_font(document_element, root_font);
        }
        #[cfg(test)]
        self.cache_audits.add(statistics.cache_audit.clone());
        if let Some(roots) = roots {
            self.hints.topmost = roots;
        }
        let mut kept = std::mem::take(&mut self.scratch.kept);
        kept.extend(statistics.styled.iter().chain(&statistics.moved_custom_properties));
        let changed_start = self.uninstalled.len();
        let scratch = &mut *self.scratch;
        self.records.absorb(
            &mut scratch.cells,
            &kept,
            Some(&mut self.uninstalled),
            &mut scratch.replaced,
        );
        self.counts
            .note_pass(!hinted, &statistics, &self.uninstalled[changed_start..]);
        // NB: A record holding what the one it replaces does gives way to it where that one is
        //     installed, which it is not for an element that arrived in the place of another.
        self.uninstalled.extend(
            statistics
                .styled
                .iter()
                .copied()
                .filter(|&node| state.computed_group_sets.assigned_style_record(node).is_none()),
        );
        // NB: An element moved out from below an undisplayed one may hold what it held before it moved there.
        if !self.installed_as_undisplayed.is_empty() {
            for node in &statistics.styled {
                if self.installed_as_undisplayed.remove(node) {
                    self.uninstalled.push(*node);
                }
            }
        }
        // A read may have had the host install the record of a pseudo-element that generates no box,
        // which no pass keeps: it goes once its element is restyled, for the next read to demand
        // anew.
        for &node in &kept {
            let holds_demanded = state.held_pseudo_style_records.get(&node).is_some_and(|held| {
                held.iter().any(|&(target, record)| {
                    record != 0
                        && super::PSEUDO_ELEMENT_KINDS.contains(&target.kind)
                        && !self
                            .records
                            .pseudo_element_records(node)
                            .iter()
                            .any(|(generated, _)| *generated == target)
                })
            });
            if holds_demanded {
                self.uninstalled.push(node);
            }
        }
        recycle(&mut kept);
        self.scratch.kept = kept;
        // The host clears the styles of the descendants of an element that is not displayed, but
        // for those SVG resources read: the descendants of one displayed again install the
        // records they hold anew, which the host takes as first records, whether or not the pass
        // moved them.
        for &node in &self.scratch.replaced.revealed {
            self.uninstalled.extend(
                state
                    .projection
                    .style_preorder(node)
                    .skip(1)
                    .filter(|&descendant| self.records.get(descendant).is_some()),
            );
        }
        if let Some(moved) = &keyframes_moved {
            self.owe_animation_plans(state, root, moved);
        }
        // https://drafts.csswg.org/web-animations-1/#calculating-computed-keyframes
        // The computed keyframes of an element's animations resolve against its context, which
        // the inputs that restyled it may have moved however its record did: the host composes
        // the animations of every element with animations the pass styled again.
        let pseudo_element_restyles: HashSet<StyleNodeID> =
            statistics.restyled_for_pseudo_elements.iter().copied().collect();
        for &node in &statistics.styled {
            if state.projection.adjustment_facts(node) & bridge::element_adjustment_fact::HAS_ANIMATIONS != 0
                && self.records.get(node).is_some()
                && !pseudo_element_restyles.contains(&node)
            {
                self.compositions_owed.push(node);
                self.uninstalled.push(node);
            }
        }
        // Dropping what a pass replaced need not hold up the next, where it is enough to be
        // worth a task of the pool. The scratch keeps no room for that much.
        let replaced = &mut self.scratch.replaced;
        if replaced.len() <= REPLACED_DROPPED_INLINE {
            replaced.clear();
        } else {
            let replaced = std::mem::take(replaced);
            match pool {
                Some(pool) => pool.spawn(move || drop(replaced)),
                None => drop(replaced),
            }
        }
        // What waited on a font or a random base value is styled by the next pass.
        let waiting = self.hints_after_waiting(
            state,
            hinted,
            &statistics.waiting,
            &statistics.waiting_with_kept_matches,
            &statistics.waiting_pseudo_elements,
            &waiting_restyling_descendants,
        );
        self.keep_deferred_hints(&state.projection, &statistics.deferred, &waiting);
        self.kept_hidden_records
            .note_moved(statistics.children_moved.iter().copied());
        // NB: A pass tells nothing of the children of an element that is not displayed, as it walks none of them.
        let records = &self.records;
        self.kept_hidden_records.note_moved(
            statistics
                .styled
                .iter()
                .copied()
                .filter(|&node| records.get(node).is_none_or(|record| !record.is_displayed())),
        );
        self.kept_hidden_records
            .note_moved(statistics.moved_custom_properties.iter().copied());
        let styled = Instant::now();
        self.bump(LiveField::Passes, 1);
        self.bump(LiveField::FullPasses, u64::from(!hinted));
        self.bump(LiveField::StyledElements, statistics.styled.len() as u64);
        self.bump(LiveField::ChangedElements, statistics.changed_elements);

        self.bump(
            LiveField::InvalidationNanoseconds,
            invalidated.duration_since(started).as_nanos() as u64,
        );
        self.bump(
            LiveField::StyleNanoseconds,
            styled.duration_since(invalidated).as_nanos() as u64,
        );
        statistics
    }
}

/// Where an element stands among its siblings, and among those of its type where that is known.
#[derive(Clone, Copy)]
struct SiblingPosition {
    from_start: usize,
    from_end: usize,
    of_type: Option<(usize, usize)>,
}

/// What a pass restyles.
enum RestyleScope {
    /// What the inputs since the last pass changed.
    Pending,
    Everything,
}

/// The roots of the flat subtrees holding every element the rules of `scopes` can match: the
/// document under `root`, or the hosts of the shadow trees. A shadow tree's rules reach out of it
/// only through `:host`, `::slotted()` and `::part()`, to the host and the elements slotted into it.
fn rule_change_roots(state: &RetainedState, root: StyleNodeID, scopes: &[TreeScopeID]) -> Vec<StyleNodeID> {
    if scopes.contains(&TreeScopeID::DOCUMENT) {
        return vec![root];
    }
    let tree = &state.projection;
    // NB: A scope with no root, or a root with no host, holds no element the flat tree reaches; a
    //     host taking one restyles its flat tree then.
    let hosts: Vec<StyleNodeID> = scopes
        .iter()
        .filter_map(|&scope| state.scope_root(scope).and_then(|scope_root| tree.host_of(scope_root)))
        .filter(|&host| tree.is_live(host))
        .collect();
    hosts
        .iter()
        .copied()
        .filter(|&host| {
            std::iter::successors(tree.inheritance_parent(host), |&ancestor| {
                tree.inheritance_parent(ancestor)
            })
            .all(|ancestor| !hosts.contains(&ancestor))
        })
        .collect()
}

/// The facts of the element at `row` that selectors test, each as a trigger.
/// The elements of the longest run of `sequence` whose ranks increase, which keep their order.
fn longest_increasing_run(sequence: &[(StyleNodeID, usize)]) -> HashSet<StyleNodeID> {
    // The index of the element ending the shortest-ranked run of each length, and the element
    // before each one in its run.
    let mut ends: Vec<usize> = Vec::new();
    let mut previous: Vec<Option<usize>> = Vec::with_capacity(sequence.len());
    for (index, &(_, rank)) in sequence.iter().enumerate() {
        let length = ends.partition_point(|&end| sequence[end].1 < rank);
        previous.push(length.checked_sub(1).map(|shorter| ends[shorter]));
        match ends.get_mut(length) {
            Some(end) => *end = index,
            None => ends.push(index),
        }
    }
    std::iter::successors(ends.last().copied(), |&index| previous[index])
        .map(|index| sequence[index].0)
        .collect()
}

fn triggers_of(facts: ProjectionRow<'_>) -> SmallVec<[Trigger; 16]> {
    let mut triggers = unordered_triggers_of(facts);
    triggers.sort_unstable_by_key(|trigger| trigger_order(*trigger));
    triggers.dedup();
    triggers
}

/// [`triggers_of`], in no order and with repeats.
fn unordered_triggers_of(facts: ProjectionRow<'_>) -> SmallVec<[Trigger; 16]> {
    let mut triggers: SmallVec<[Trigger; 16]> = SmallVec::new();
    triggers.extend(facts.classes().iter().map(|&class| Trigger::Class(class)));
    let id = facts.id();
    if !id.is_none() {
        triggers.push(Trigger::Id(id));
    }
    for tag in [facts.tag(), facts.folded_tag()] {
        if !tag.is_none() {
            triggers.push(Trigger::Tag(tag));
        }
    }
    for attribute in facts.attributes() {
        for name in facts.attribute_name_keys(attribute.name) {
            if !name.is_none() {
                triggers.push(Trigger::Attribute(name));
            }
        }
    }
    triggers.extend(facts.states().facts().map(Trigger::State));
    triggers.extend(facts.custom_states().iter().map(|&state| Trigger::CustomState(state)));
    triggers.extend(facts.parts().iter().map(|&part| Trigger::Part(part)));
    triggers.push(Trigger::Any);
    triggers
}

/// The order of triggers, to drop repeats.
fn trigger_order(trigger: Trigger) -> (u8, u32) {
    match trigger {
        Trigger::Class(atom) => (0, atom.0),
        Trigger::Id(atom) => (1, atom.0),
        Trigger::Attribute(atom) => (2, atom.0),
        Trigger::State(fact) => (3, fact as u32),
        Trigger::CustomState(atom) => (4, atom.0),
        Trigger::Directionality => (5, 0),
        Trigger::Language => (6, 0),
        Trigger::Heading => (7, 0),
        Trigger::Emptiness => (8, 0),
        Trigger::Tag(atom) => (9, atom.0),
        Trigger::Any => (10, 0),
        Trigger::Part(atom) => (11, atom.0),
        Trigger::Position { from_end, of_type, key } => {
            let (kind, atom) = match key {
                PositionKey::Any => (0, 0),
                PositionKey::Class(atom) => (1, atom.0),
                PositionKey::Id(atom) => (2, atom.0),
                PositionKey::Tag(atom) => (3, atom.0),
            };
            (12 + kind * 4 + u8::from(of_type) * 2 + u8::from(from_end), atom)
        }
        Trigger::Relative(query) => (28, query),
    }
}

/// Whether an element carrying `carried`, in `trigger_order`, where it is known, can carry one of
/// `keys`, where there are any.
fn carries_any(keys: Option<&SmallVec<[Trigger; 4]>>, carried: Option<&[Trigger]>) -> bool {
    match (keys, carried) {
        (Some(keys), Some(carried)) => keys.iter().any(|key| {
            carried
                .binary_search_by_key(&trigger_order(*key), |held| trigger_order(*held))
                .is_ok()
        }),
        _ => true,
    }
}

/// The anchors of `:has()` queries a transaction's changes reach.
#[derive(Default)]
struct RelativeAnchors {
    /// Whether a walk past each anchor tested goes on, by query and anchor.
    tested: HashMap<(u32, StyleNodeID), bool>,
    /// The parents whose children were walked from, by query and side, for the queries reading
    /// positions.
    walked_children: HashSet<(u32, StyleNodeID, bool)>,
    /// The subtrees walked for the elements a selector nested in an argument reaches, by query and
    /// subtree root: the elements of the transaction arriving beside each other reach the same.
    walked_subtrees: HashSet<(u32, StyleNodeID)>,
    /// The anchors walks along ancestors or previous siblings reached, by query and whether they
    /// restyle what they reach.
    reached: HashSet<(u32, StyleNodeID, bool)>,
    /// The queries each element a walk of a subtree reached carries a key of the argument chain
    /// of, as bits by query, worked out from its facts the first time a walk of any query reaches
    /// it: the walks of the queries reach the same elements.
    ///
    /// NB: An element's keys are collected once per change and tested against the keys of every
    ///     argument at once.
    chain_keys_carried: HashMap<StyleNodeID, SmallVec<[u64; 1]>>,
    /// The anchors whose answer flipped, or might have, with the query.
    flipped: HashSet<(StyleNodeID, u32)>,
    /// The anchors whose answer before is not known, with the query, which are restyled where
    /// they can match.
    restyled: HashSet<(StyleNodeID, u32)>,
}

/// What a thread following changes keeps between the changes it follows in a transaction.
#[derive(Default)]
struct FollowScratch {
    memo: CompoundMemo,
    /// Where siblings stand, now and before the transaction.
    after: MatchScratch,
    before: MatchScratch,
    /// What the followers on the thread added, which the invalidation takes once they are done.
    added: Vec<(StyleNodeID, HintBits, u32)>,
    /// The keys of the ancestors of the changed element being followed, before and after, which
    /// a follower leaves empty: made the first time the thread follows a change.
    ancestors: Option<super::bloom::AncestorFilter>,
}

/// What the changed element being followed matches of each compound on its own and from it
/// leftwards, after and before, by the map's memo slot of the compound or of its kind of own tests,
/// which a follower forgets for the next element.
struct CompoundMemo {
    /// The stamp of the element whose answers each slot holds: starting the next element is a new
    /// stamp, not a clearing.
    stamps: Vec<u16>,
    /// The answers of each slot, two bits per kind: none yet, unknown, false or true.
    answers: Vec<u8>,
    stamp: u16,
}

/// What a memo answer of a compound is: of the element on its own or from it leftwards, after or
/// before the transaction.
#[derive(Clone, Copy, PartialEq, Eq)]
enum MemoKind {
    Own { before: bool },
    Leftwards { before: bool },
}

/// What a memo says of a question to it.
#[derive(Clone, Copy)]
enum MemoAnswer {
    /// The question was not asked of the element yet.
    Unasked,
    /// The answer, or none where it could not be told.
    Answered(Option<bool>),
}

impl MemoKind {
    fn shift(self) -> u32 {
        match self {
            Self::Own { before } => u32::from(before) * 2,
            Self::Leftwards { before } => 4 + u32::from(before) * 2,
        }
    }
}

impl Default for CompoundMemo {
    fn default() -> Self {
        Self {
            stamps: Vec::new(),
            answers: Vec::new(),
            stamp: 1,
        }
    }
}

impl FollowScratch {
    fn heap_size(&self) -> usize {
        self.memo.heap_size()
            + self.after.heap_size()
            + self.before.heap_size()
            + crate::heap_size::vec_bytes(&self.added)
            + self
                .ancestors
                .as_ref()
                .map_or(0, super::bloom::AncestorFilter::heap_size)
    }
}

impl CompoundMemo {
    fn heap_size(&self) -> usize {
        use crate::heap_size::vec_bytes;
        vec_bytes(&self.stamps) + vec_bytes(&self.answers)
    }

    /// Forget every answer, for the next element, holding a slot for each of `slots`.
    fn start(&mut self, slots: usize) {
        if self.stamps.len() < slots {
            self.stamps.resize(slots, 0);
            self.answers.resize(slots, 0);
        }
        self.stamp = self.stamp.wrapping_add(1);
        if self.stamp == 0 {
            self.stamps.fill(0);
            self.stamp = 1;
        }
    }

    fn get(&self, slot: u32, kind: MemoKind) -> MemoAnswer {
        let slot = slot as usize;
        if self.stamps[slot] != self.stamp {
            return MemoAnswer::Unasked;
        }
        match (self.answers[slot] >> kind.shift()) & 0b11 {
            0 => MemoAnswer::Unasked,
            1 => MemoAnswer::Answered(None),
            answer => MemoAnswer::Answered(Some(answer == 3)),
        }
    }

    fn set(&mut self, slot: u32, kind: MemoKind, answer: Option<bool>) {
        let slot = slot as usize;
        if self.stamps[slot] != self.stamp {
            self.stamps[slot] = self.stamp;
            self.answers[slot] = 0;
        }
        let bits = match answer {
            None => 1,
            Some(false) => 2,
            Some(true) => 3,
        };
        self.answers[slot] = (self.answers[slot] & !(0b11 << kind.shift())) | bits << kind.shift();
    }
}

/// How many element slots a chunk of [`HintCells`] holds.
const HINT_CELLS_PER_CHUNK: usize = 64;

/// The hint and the mark of each element, by element slot, in chunks a fork of the engine shares
/// until it writes there: a fork costs the chunks the passes after it write, not one cell per
/// element.
pub(super) type HintCells = CowColumn<HintCell, HINT_CELLS_PER_CHUNK>;

/// What a pass restyles of an element and below it, and whether a restyled element is above it.
#[derive(Clone, Copy, Default, PartialEq)]
pub(super) struct HintCell {
    pub(super) bits: HintBits,
    /// Whether a restyled element is above the element in the flat tree, where it is marked with
    /// `DIRTY_DESCENDANTS` and marking its ancestors found out.
    under_restyled: UnderRestyled,
}

const _: () = assert!(size_of::<HintCell>() == 4);

/// What a pass restyles, by element slot, with the elements holding a hint.
#[derive(Clone, Default)]
struct Hints {
    cells: HintCells,
    marked: Vec<StyleNodeID>,
    /// The rule index entries elements are restyled for, which they are rematched against alone,
    /// and those their descendants are.
    reasons: Vec<(StyleNodeID, u32)>,
    descendant_reasons: Vec<(StyleNodeID, u32)>,
    /// The elements restyled that no restyled element is above in the flat tree, which marking
    /// their ancestors finds.
    topmost: Vec<StyleNodeID>,
    /// The ancestors marking an element's ancestors walks through.
    path: Vec<StyleNodeID>,
}

#[derive(Clone, Copy, Default, PartialEq)]
enum UnderRestyled {
    #[default]
    Unknown,
    No,
    Yes,
}

/// Whether `bits` style their element or its descendants.
fn restyles(bits: HintBits) -> bool {
    bits & STYLES_SOMETHING != 0
}

impl Hints {
    /// The bytes the hints keep on the heap between passes that `sizer` reaches for the first
    /// time: the chunks of cells, and the lists of the last pass.
    fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        use crate::heap_size::vec_bytes;
        self.cells.heap_size(sizer, |_, _| 0)
            + vec_bytes(&self.marked)
            + vec_bytes(&self.reasons)
            + vec_bytes(&self.descendant_reasons)
            + vec_bytes(&self.topmost)
            + vec_bytes(&self.path)
    }

    /// Clear every hint.
    fn reset(&mut self) {
        for node in self.marked.drain(..) {
            self.cells.set(node.element_slot(), HintCell::default());
        }
        self.reasons.clear();
        self.descendant_reasons.clear();
        self.topmost.clear();
    }

    /// Renumber the entries elements are restyled for for a rebuilt rule index, where
    /// `renumbering` has the entry standing for each one of the index before. An entry the rebuilt
    /// index lost has no match to change.
    fn renumber_reasons(&mut self, renumbering: &[Option<u32>]) {
        for reasons in [&mut self.reasons, &mut self.descendant_reasons] {
            reasons.retain_mut(|(_, entry)| match renumbering.get(*entry as usize).copied().flatten() {
                Some(renumbered) => {
                    *entry = renumbered;
                    true
                }
                None => false,
            });
        }
    }

    /// Sort the entries elements are restyled for by element, for `reasons`.
    fn sort_reasons(&mut self) {
        for reasons in [&mut self.reasons, &mut self.descendant_reasons] {
            reasons.sort_unstable();
            reasons.dedup();
        }
    }

    /// Whether `node` is restyled, or its descendants are, for some entries alone, once sorted.
    fn restyled_for_entries(&self, node: StyleNodeID) -> bool {
        [&self.reasons, &self.descendant_reasons].into_iter().any(|pairs| {
            pairs
                .get(pairs.partition_point(|&(held, _)| held < node))
                .is_some_and(|&(held, _)| held == node)
        })
    }

    /// The entries elements are restyled for, by element, once sorted.
    fn reasons(&self) -> traversal::Reasons<'_> {
        traversal::Reasons {
            own: &self.reasons,
            descendants: &self.descendant_reasons,
        }
    }

    /// Note `bits` in the hint of `node`, for the rule index entry `entry` alone where it is not
    /// `u32::MAX`, and for every entry otherwise.
    fn add_for(&mut self, node: StyleNodeID, bits: HintBits, entry: u32) {
        if entry == u32::MAX {
            return self.add(node, bits);
        }
        if bits & RESTYLE_SELF != 0 {
            self.reasons.push((node, entry));
        }
        if bits & RESTYLE_DESCENDANTS != 0 {
            self.descendant_reasons.push((node, entry));
        }
        self.mark(node, bits);
    }

    /// Make room for `slot_count` elements.
    fn reserve(&mut self, slot_count: usize) {
        self.cells.grow_to(slot_count);
    }

    fn get(&self, node: StyleNodeID) -> HintBits {
        self.cells.get(node.element_slot()).map_or(0, |cell| cell.bits)
    }

    fn under_restyled(&self, node: StyleNodeID) -> UnderRestyled {
        self.cells
            .get(node.element_slot())
            .map_or(UnderRestyled::Unknown, |cell| cell.under_restyled)
    }

    fn set_bits(&mut self, node: StyleNodeID, bits: HintBits) {
        if let Some(mut cell) = self.cells.row_mut(node.element_slot()) {
            cell.bits = bits;
        }
    }

    fn set_under_restyled(&mut self, node: StyleNodeID, under_restyled: UnderRestyled) {
        if let Some(mut cell) = self.cells.row_mut(node.element_slot()) {
            cell.under_restyled = under_restyled;
        }
    }

    /// Note `bits` in the hint of `node`, rematching it, and its descendants where they are
    /// restyled, against the whole rule index.
    fn add(&mut self, node: StyleNodeID, bits: HintBits) {
        let mut bits = bits;
        if bits & RESTYLE_SELF != 0 {
            bits |= MATCH_SELF;
        }
        if bits & RESTYLE_DESCENDANTS != 0 {
            bits |= MATCH_DESCENDANTS;
        }
        self.mark(node, bits);
    }

    fn mark(&mut self, node: StyleNodeID, bits: HintBits) {
        let Some(mut cell) = self.cells.row_mut(node.element_slot()) else {
            return;
        };
        if cell.bits == 0 {
            self.marked.push(node);
        }
        cell.bits |= bits;
    }
}

/// The first of `first` and its later siblings not in `arrived`, as `next` tells them, remembering in `past_arrived`
/// what it finds for each one it walks over: a run of arrived siblings is walked once.
fn first_not_arrived(
    first: StyleNodeID,
    arrived: &HashSet<StyleNodeID>,
    next: impl Fn(StyleNodeID) -> Option<StyleNodeID>,
    past_arrived: &mut HashMap<StyleNodeID, Option<StyleNodeID>>,
) -> Option<StyleNodeID> {
    let mut walked: SmallVec<[StyleNodeID; 16]> = SmallVec::new();
    let mut current = Some(first);
    let found = loop {
        let Some(sibling) = current.filter(|sibling| arrived.contains(sibling)) else {
            break current;
        };
        if let Some(&known) = past_arrived.get(&sibling) {
            break known;
        }
        walked.push(sibling);
        current = next(sibling);
    };
    past_arrived.extend(walked.into_iter().map(|sibling| (sibling, found)));
    found
}

/// The union of `bloom_of` over `first` and its later siblings, as `next` tells them, remembering in `known` what it
/// finds for each one it walks over: a run of siblings is walked once, however many of them it is asked from.
fn bloom_from(
    first: StyleNodeID,
    next: impl Fn(StyleNodeID) -> Option<StyleNodeID>,
    bloom_of: impl Fn(StyleNodeID) -> u64,
    known: &mut HashMap<StyleNodeID, u64>,
) -> u64 {
    let mut walked: SmallVec<[StyleNodeID; 16]> = SmallVec::new();
    let mut current = Some(first);
    let mut bloom = loop {
        let Some(sibling) = current else {
            break 0;
        };
        if let Some(&found) = known.get(&sibling) {
            break found;
        }
        walked.push(sibling);
        current = next(sibling);
    };
    for &sibling in walked.iter().rev() {
        bloom |= bloom_of(sibling);
        known.insert(sibling, bloom);
    }
    bloom
}

/// The links a descendant is tested against, by the key an element matching each one's compound
/// carries, where it has one.
#[derive(Default)]
struct LinkFilter {
    classes: SmallVec<[(StyleAtomID, Dependency); 8]>,
    others: SmallVec<[(DispatchKey, Dependency); 4]>,
    keyless: SmallVec<[Dependency; 4]>,
    /// The dispatch bloom bits of the keys above. An element carrying a key has its bit, as a
    /// compound's `required_key` is an id, class, attribute name, type or state, all of which
    /// the bloom holds, so an element sharing no bit with these carries none of them.
    required: u64,
    /// Whether a state is among the keys, which an element's lists and tag do not tell.
    reads_states: bool,
    /// The lists and tag of the last element with no candidates, which the next element of a
    /// descent often shares. Equal lists and tags carry the same ids, classes, attributes and types.
    last_rejected: Cell<Option<(FactListsIdentity, StyleAtomID)>>,
}

impl LinkFilter {
    fn of(map: &InvalidationMap, links: &[Dependency]) -> Self {
        let mut filter = Self::default();
        for &link in links {
            match map.step(link).required_key {
                Some(DispatchKey::Class(class)) => {
                    filter.required |= dispatch_bloom_bit(DispatchKey::Class(class));
                    filter.classes.push((class, link));
                }
                Some(key) => {
                    filter.required |= dispatch_bloom_bit(key);
                    filter.reads_states |= matches!(key, DispatchKey::State(_));
                    filter.others.push((key, link));
                }
                None => filter.keyless.push(link),
            }
        }
        filter.classes.sort_unstable();
        filter
    }

    /// The links the element with `facts` carries the key of, into `out`, which is empty.
    fn candidates(&self, facts: ProjectionRow<'_>, out: &mut SmallVec<[Dependency; 8]>) {
        if !self.keyless.is_empty() {
            out.extend_from_slice(&self.keyless);
        } else if facts.facts.dispatch_bloom(false) & self.required == 0 {
            return;
        }
        let shape = (facts.facts.lists_identity(), facts.tag());
        if self.keyless.is_empty() && self.last_rejected.get() == Some(shape) {
            return;
        }
        self.carried_keys(facts, out);
        if out.is_empty() && !self.reads_states {
            self.last_rejected.set(Some(shape));
        }
    }

    /// The links with a key the element with `facts` carries.
    fn carried_keys(&self, facts: ProjectionRow<'_>, out: &mut SmallVec<[Dependency; 8]>) {
        if !self.classes.is_empty() {
            for &class in facts.classes() {
                let start = self.classes.partition_point(|&(wanted, _)| wanted < class);
                out.extend(
                    self.classes[start..]
                        .iter()
                        .take_while(|(wanted, _)| *wanted == class)
                        .map(|&(_, link)| link),
                );
            }
        }
        for &(key, link) in &self.others {
            if carries_key(facts, key) {
                out.push(link);
            }
        }
    }
}

/// Whether an element with `facts` carries `key`, as far as its facts tell.
fn carries_key(facts: ProjectionRow<'_>, key: DispatchKey) -> bool {
    match key {
        DispatchKey::Class(class) => facts.classes().contains(&class),
        DispatchKey::TagName(tag) => facts.tag() == tag || facts.folded_tag() == tag,
        DispatchKey::Id(id) => facts.id() == id,
        DispatchKey::AttributeName(name) => facts
            .attributes()
            .iter()
            .any(|attribute| facts.attribute_name_keys(attribute.name).contains(&name)),
        DispatchKey::State(fact) => facts.states().facts().any(|held| held == fact),
        _ => true,
    }
}

/// Follows the links of the compounds testing a changed fact from the element it changed on to the
/// elements they restyle: one descent from the element carries the links its descendants are
/// tested against, and one walk across each run of siblings carries the links its later siblings
/// are.
struct Follower<'a> {
    state: &'a RetainedState,
    map: &'a InvalidationMap,
    index: &'a RuleIndex,
    projection: &'a Projection,
    own_facts: ProjectionEvaluator<'a>,
    /// The facts the elements had before, in the tree as it was.
    before: ProjectionEvaluator<'a>,
    document: DocumentMatching,
    /// Whether an element left with the subtree of another, which the tree as it was holds none
    /// of, so that what a `:has()` answered before is not known.
    subtrees_left: bool,
    /// Whether every entry decides in the document's tree, which is the group answers are shared in.
    all_in_the_document: bool,
    memo: &'a mut CompoundMemo,
    /// The hints the elements followed to get, with the rule index entry each is for, or
    /// `u32::MAX` for every entry.
    added: &'a mut Vec<(StyleNodeID, HintBits, u32)>,
    ancestors: &'a mut Option<super::bloom::AncestorFilter>,
    visits: u64,
    tests: u64,
}

/// Links an element is tested against, by the combinator that reached it.
type Links = SmallVec<[Dependency; 8]>;

/// The links that reach every child of one parent, sorted, with their filter.
struct Level {
    links: Links,
    filter: LinkFilter,
    /// Whether every link is a descendant combinator's, which reaches the children's descendants
    /// too.
    all_descendant: bool,
}

impl Level {
    fn of(map: &InvalidationMap, links: Links) -> Self {
        debug_assert!(links.is_sorted());
        Self {
            filter: LinkFilter::of(map, &links),
            all_descendant: links
                .iter()
                .all(|&link| map.step(link).combinator == ReachCombinator::Descendant),
            links,
        }
    }
}

impl<'a> Follower<'a> {
    fn new(
        state: &'a RetainedState,
        map: &'a InvalidationMap,
        index: &'a RuleIndex,
        scratch: &'a mut FollowScratch,
    ) -> Self {
        let projection = &state.projection;
        let FollowScratch {
            memo,
            after: after_workspace,
            before: before_workspace,
            added,
            ancestors,
        } = scratch;
        Self {
            state,
            map,
            index,
            projection,
            own_facts: projection
                .evaluator()
                .with_match_workspace(after_workspace)
                .indexing_stepped_positions_only(),
            before: projection
                .evaluator_on(TransactionFactSide::Before)
                .with_match_workspace(before_workspace),
            document: state.document_matching(),
            subtrees_left: false,
            all_in_the_document: index.decides_only_in_the_document(),
            memo,
            added,
            ancestors,
            visits: 0,
            tests: 0,
        }
    }

    /// Follow what the changes to the facts of `node` in `changed` reach: through the compounds
    /// testing them whose selectors, from them leftwards, the element matched before and not now
    /// or the other way around.
    fn follow_changes(&mut self, node: StyleNodeID, changed: &[(StyleNodeID, Trigger, bool)]) {
        if !self.projection.is_live(node) {
            return;
        }
        self.memo.start(self.map.memo_slot_count());
        let mut ancestors = self.ancestors.take().unwrap_or_else(super::bloom::AncestorFilter::new);
        let ancestor_keys = self.ancestor_keys(node);
        for &key in &ancestor_keys {
            ancestors.insert(key);
        }
        let map = self.map;
        let mut links = Vec::new();
        let mut reasons: SmallVec<[u32; 8]> = SmallVec::new();
        for (_, trigger, _) in changed {
            // NB: An element a change to the name reaches gained or lost the name these are of. A
            //     rematch tells where it matched neither way, as on the host of the tree deciding.
            reasons.extend(map.lone_entries(trigger));
            for held in map.held_effects(trigger) {
                let effect = held.dependency;
                if !ancestors.might_contain_all(&held.ancestor_hashes) || !self.changed_matching(effect, node) {
                    continue;
                }
                // NB: Answering the compound above has read its record, which holds what it leads on to.
                match map.right(effect) {
                    None => reasons.push(effect.entry),
                    Some(link) => links.push(link),
                }
            }
        }
        for &key in &ancestor_keys {
            ancestors.remove(key);
        }
        *self.ancestors = Some(ancestors);
        for entry in reasons {
            self.add_for(node, RESTYLE_SELF, entry);
        }
        if !links.is_empty() {
            self.follow(node, &links);
        }
    }

    /// Find the anchors of the relative query `query` an element arriving under `parent`, or
    /// leaving it, on `side`, can flip the answer of, with its subtree where it is `with_subtree`.
    fn note_relative_arrival(
        &mut self,
        node: StyleNodeID,
        side: TransactionFactSide,
        parent: Option<StyleNodeID>,
        with_subtree: bool,
        anchors: &mut RelativeAnchors,
    ) {
        let bits = self.bits_carried(node, side, with_subtree);
        // NB: Where every query needs a key of the element's to be reached, one sharing no bit with
        //     any of them carries none.
        if self.map.every_query_needs_an_arrival_key() && bits & self.map.arrival_bits() == 0 {
            return;
        }
        // NB: What the element carries is gathered where a query's keys may be among it.
        let mut carried = None;
        for query in self.map.queries().collect::<SmallVec<[u32; 8]>>() {
            let entry = self.map.relative(query);
            let (reads_positions, reads_emptiness) = (entry.reads_positions, entry.reads_emptiness);
            let nested_reaches_out = entry.nested_reaches_out;
            let across = entry.nested_steps_sideways || reads_positions;
            if bits & entry.arrival_bits != 0
                && carries_any(
                    entry.arrival_keys.as_ref(),
                    carried
                        .get_or_insert_with(|| self.facts_carried(node, side, with_subtree))
                        .as_deref(),
                )
            {
                self.reach_anchors(query, node, side, false, anchors);
            }
            // A selector nested in the argument reaches from the element to those of its subtree,
            // and those of its siblings' where it steps sideways, which can stand anywhere.
            if nested_reaches_out {
                match side {
                    TransactionFactSide::After => self.reach_anchors_through_nested(query, node, anchors),
                    TransactionFactSide::Before if across => {
                        let siblings: SmallVec<[StyleNodeID; 16]> = parent
                            .filter(|&parent| self.projection.is_live(parent))
                            .into_iter()
                            .flat_map(|parent| self.projection.children(parent))
                            .collect();
                        self.reach_anchors_in_subtrees(query, &siblings, anchors);
                    }
                    TransactionFactSide::Before => {}
                }
            }
            let Some(parent) = parent else {
                continue;
            };
            // Every sibling after an element arriving or leaving stands elsewhere among them, and
            // every one before it stands elsewhere from the end.
            if reads_positions
                && anchors
                    .walked_children
                    .insert((query, parent, side == TransactionFactSide::Before))
            {
                let children: SmallVec<[StyleNodeID; 16]> = match side {
                    TransactionFactSide::Before => self.projection.children_on(side, parent).collect(),
                    TransactionFactSide::After => self.projection.children(parent).collect(),
                };
                for child in children {
                    self.reach_anchors(query, child, side, false, anchors);
                }
            }
            // NB: Whether the parent was empty before is not known: its anchors are restyled.
            if reads_emptiness {
                self.reach_anchors(query, parent, side, true, anchors);
            }
        }
    }

    /// The facts `node` had before the transaction, or for an element whose facts are not known
    /// from before, the ones it has.
    fn row_before(&self, node: StyleNodeID) -> Option<ProjectionRow<'a>> {
        self.projection
            .row_on(TransactionFactSide::Before, node)
            .or_else(|| self.projection.row_of(node))
    }

    /// The element's row on `side`, as what it carries there is read.
    fn row_carrying(&self, side: TransactionFactSide, node: StyleNodeID) -> Option<ProjectionRow<'a>> {
        match side {
            TransactionFactSide::Before => self.row_before(node),
            TransactionFactSide::After => self.projection.row_of(node),
        }
    }

    /// The dispatch bloom bits of what [`Self::facts_carried`] answers, or every bit where that is lost.
    fn bits_carried(&self, node: StyleNodeID, side: TransactionFactSide, with_subtree: bool) -> u64 {
        if !with_subtree {
            return self
                .row_carrying(side, node)
                .map_or(u64::MAX, |row| row.facts.dispatch_bloom(false));
        }
        let mut bits = 0;
        for element in self.projection.preorder(node) {
            for side in [TransactionFactSide::Before, TransactionFactSide::After] {
                if let Some(row) = self.row_carrying(side, element) {
                    bits |= row.facts.dispatch_bloom(false);
                }
            }
        }
        bits
    }

    /// The facts `node` carries on `side`, with those of the elements of its subtree, before and
    /// now, where it is `with_subtree`, or none where they are lost, in `trigger_order`.
    fn facts_carried(
        &self,
        node: StyleNodeID,
        side: TransactionFactSide,
        with_subtree: bool,
    ) -> Option<SmallVec<[Trigger; 16]>> {
        if !with_subtree {
            // NB: What an element that left carried is lost, and taken to be anything.
            return self.row_carrying(side, node).map(triggers_of);
        }
        let mut carried = SmallVec::new();
        for element in self.projection.preorder(node) {
            for row in [TransactionFactSide::Before, TransactionFactSide::After]
                .into_iter()
                .filter_map(|side| self.row_carrying(side, element))
            {
                carried.extend(unordered_triggers_of(row));
            }
        }
        carried.sort_unstable_by_key(|trigger| trigger_order(*trigger));
        carried.dedup();
        Some(carried)
    }

    /// Test the anchors of the relative query `query` whose argument a selector nested in it
    /// reaches from `element` as it changes: those of the elements of its subtree, and of its
    /// siblings and their subtrees where the selector steps sideways or counts siblings.
    fn reach_anchors_through_nested(&mut self, query: u32, element: StyleNodeID, anchors: &mut RelativeAnchors) {
        let entry = self.map.relative(query);
        let across = entry.nested_steps_sideways || entry.reads_positions;
        let tree = self.projection;
        let mut roots: SmallVec<[StyleNodeID; 16]> = match tree.parent(element).filter(|_| across) {
            Some(parent) => tree.children(parent).collect(),
            None => smallvec::smallvec![element],
        };
        // A `:host()` nested in the argument reaches into the shadow tree of the element.
        roots.extend(
            tree.shadow_root_of(element)
                .into_iter()
                .flat_map(|root| tree.children(root)),
        );
        self.reach_anchors_in_subtrees(query, &roots, anchors);
    }

    /// Test the anchors of the relative query `query` of the elements of the subtrees of `roots`
    /// that can be of its argument's chain.
    fn reach_anchors_in_subtrees(&mut self, query: u32, roots: &[StyleNodeID], anchors: &mut RelativeAnchors) {
        let tree = self.projection;
        let slot = self.map.query_slot(query);
        let (word, bit) = (slot / 64, 1u64 << (slot % 64));
        for &root in roots {
            // NB: What a walk reaches is tested as the tree stands now, which the walk of a subtree it reached
            //     before tests the same.
            if !anchors.walked_subtrees.insert((query, root)) {
                continue;
            }
            for reached in tree.preorder(root) {
                // NB: As facts_carried() and carries_any() answer, without collecting the element's triggers into a
                //     set: an element whose facts are lost can carry anything.
                let carries = match self.projection.row_of(reached) {
                    Some(row) => {
                        anchors
                            .chain_keys_carried
                            .entry(reached)
                            .or_insert_with(|| self.chain_keys_carried(row))[word]
                            & bit
                            != 0
                    }
                    None => true,
                };
                if carries {
                    self.reach_anchors(query, reached, TransactionFactSide::After, false, anchors);
                }
            }
        }
    }

    /// The queries an element with the facts `row` carries a key of the argument chain of, or that
    /// name no keys, as bits by query.
    fn chain_keys_carried(&self, row: ProjectionRow<'_>) -> SmallVec<[u64; 1]> {
        let triggers = unordered_triggers_of(row);
        let mut carried: SmallVec<[u64; 1]> = smallvec::smallvec![0; self.map.query_count().div_ceil(64)];
        for query in self.map.queries() {
            let entry = self.map.relative(query);
            let query = self.map.query_slot(query);
            if entry
                .chain_keys
                .as_ref()
                .is_none_or(|keys| keys.iter().any(|key| triggers.contains(key)))
            {
                carried[query / 64] |= 1 << (query % 64);
            }
        }
        carried
    }

    /// Test the anchors of the relative query `query` that `element`, on `side`, stands to as the
    /// query's axis says, nearest first, or restyle those that can match where `restyled`.
    fn reach_anchors(
        &mut self,
        query: u32,
        element: StyleNodeID,
        side: TransactionFactSide,
        restyled: bool,
        anchors: &mut RelativeAnchors,
    ) {
        let projection = self.projection;
        let entry = self.map.relative(query);
        let (reach, in_shadow_tree) = (entry.reach, entry.in_shadow_tree);
        let parent = |node| match side {
            TransactionFactSide::Before => projection.parent_on(side, node),
            TransactionFactSide::After => projection.parent(node),
        };
        let previous = |node| match side {
            TransactionFactSide::Before => projection.previous_element_sibling_on(side, node),
            TransactionFactSide::After => projection.previous_element_sibling(node),
        };
        // An argument walked in a shadow tree is anchored on its host alone, and goes down from it.
        if in_shadow_tree {
            let mut current = element;
            while let Some(above) = parent(current) {
                if let Some(host) = projection.host_of(above) {
                    if reach == AnchorReach::Ancestors || current == element {
                        self.test_anchor(query, host, restyled, anchors);
                    }
                    return;
                }
                if reach != AnchorReach::Ancestors {
                    return;
                }
                current = above;
            }
            return;
        }
        // An argument walked from an element of a tree stays in that projection.
        let above = |node| parent(node).filter(|&parent| projection.host_of(parent).is_none());
        match reach {
            AnchorReach::Parent => {
                if let Some(anchor) = above(element) {
                    self.test_anchor(query, anchor, restyled, anchors);
                }
            }
            AnchorReach::Ancestors => {
                let mut anchor = above(element);
                while let Some(current) = anchor {
                    if !anchors.reached.insert((query, current, restyled))
                        || !self.test_anchor(query, current, restyled, anchors)
                    {
                        break;
                    }
                    anchor = above(current);
                }
            }
            AnchorReach::PreviousSibling => {
                if let Some(anchor) = previous(element) {
                    self.test_anchor(query, anchor, restyled, anchors);
                }
            }
            AnchorReach::PreviousSiblings => {
                for anchor in std::iter::successors(previous(element), |&node| previous(node)) {
                    // NB: A walk reaching a sibling another walk of the query reached the same way reaches every
                    //     sibling before it as that one did. So it is for the walks along ancestors and the
                    //     siblings before them.
                    if !anchors.reached.insert((query, anchor, restyled)) {
                        break;
                    }
                    self.test_anchor(query, anchor, restyled, anchors);
                }
            }
            AnchorReach::PreviousSiblingOfAncestors => {
                for node in std::iter::successors(Some(element), |&node| above(node)) {
                    if let Some(anchor) = previous(node) {
                        self.test_anchor(query, anchor, restyled, anchors);
                    }
                }
            }
            AnchorReach::PreviousSiblingsOfAncestors => {
                for node in std::iter::successors(Some(element), |&node| above(node)) {
                    for anchor in std::iter::successors(previous(node), |&node| previous(node)) {
                        if !anchors.reached.insert((query, anchor, restyled)) {
                            break;
                        }
                        self.test_anchor(query, anchor, restyled, anchors);
                    }
                }
            }
        }
    }

    /// Note whether the relative query `query` answers differently for `anchor` now than before,
    /// or might, or that it is restyled where `restyled` and it can match, and answer whether the
    /// anchors above it can answer differently.
    fn test_anchor(&mut self, query: u32, anchor: StyleNodeID, restyled: bool, anchors: &mut RelativeAnchors) -> bool {
        if restyled {
            if anchors.restyled.contains(&(anchor, query)) {
                return true;
            }
        } else if let Some(&goes_on) = anchors.tested.get(&(query, anchor)) {
            return goes_on;
        }
        self.visits += 1;
        let goes_on = match self
            .projection
            .facts(anchor)
            .filter(|_| self.projection.is_live(anchor))
        {
            None => true,
            Some(_) => {
                let map = self.map;
                // NB: Only an element that matches the rest of a compound testing the query can be
                //     restyled for it, and one carrying the key the compound requires on neither side
                //     of the transaction matches it on neither.
                let can_anchor = map.effects(&Trigger::Relative(query)).any(|effect| {
                    let carried = map.step(effect).required_key.is_none_or(|key| {
                        [self.projection.row_of(anchor), self.row_before(anchor)]
                            .into_iter()
                            .any(|row| row.is_none_or(|row| carries_key(row, key)))
                    });
                    carried
                        && [false, true]
                            .into_iter()
                            .any(|before| self.evaluate_around_relative(effect, anchor, before) != Some(false))
                });
                match can_anchor {
                    false => true,
                    true if restyled => {
                        anchors.restyled.insert((anchor, query));
                        return true;
                    }
                    true => {
                        let after = self.relative_answer(query, anchor, false);
                        let before = self.relative_answer(query, anchor, true);
                        if after.is_none() || after != before {
                            anchors.flipped.insert((anchor, query));
                        }
                        let holds_above = map.relative(query).answers_above_descendants;
                        !(holds_above && before == Some(true) && after == Some(true))
                    }
                }
            }
        };
        anchors.tested.insert((query, anchor), goes_on);
        goes_on
    }

    /// Restyle what the compounds testing the relative query `query` that `anchor` can match
    /// reach from it, without asking whether it matched them before.
    fn restyle_relative_anchor(&mut self, anchor: StyleNodeID, query: u32) {
        let map = self.map;
        let mut links = Vec::new();
        for effect in map.effects(&Trigger::Relative(query)) {
            if [false, true]
                .into_iter()
                .all(|before| self.evaluate_around_relative(effect, anchor, before) == Some(false))
            {
                continue;
            }
            match self.map.right(effect) {
                None => self.add_for(anchor, RESTYLE_SELF, effect.entry),
                Some(link) => links.push(link),
            }
        }
        links.sort_unstable();
        links.dedup();
        if !links.is_empty() {
            self.follow(anchor, &links);
        }
    }

    /// What the relative query `query` answers for `anchor`, before the transaction where it is
    /// `before`, or none where that cannot be told.
    fn relative_answer(&mut self, query: u32, anchor: StyleNodeID, before: bool) -> Option<bool> {
        self.tests += 1;
        let entry = self.map.relative(query);
        let entry_number = self.map.relative_entry(query);
        let in_shadow_tree = entry.in_shadow_tree;
        let shadow_root = self.scope_shadow_root(entry_number, anchor)?;
        let selector = self.index.entry_selector(entry_number);
        let argument = entry.argument(selector);
        let document = self.document;
        let evaluator = match before {
            true => {
                // An element that arrived answered nothing before.
                if !self.projection.is_connected_on(TransactionFactSide::Before, anchor) {
                    return Some(false);
                }
                if self.subtrees_left {
                    return None;
                }
                &mut self.before
            }
            false => &mut self.own_facts,
        };
        evaluator.set_scope_shadow_root(shadow_root);
        evaluator
            .matches_relative_query(selector, argument, in_shadow_tree, anchor, document)
            .ok()
    }

    /// The shadow root of the tree the sheet of the rule index entry `entry` is attached to,
    /// whose host `:host` names, where it is not the document, as it decides for `element`. None
    /// where that cannot be told: the entry's group is gone, or several of the shadow trees
    /// sharing its entries reach the element.
    fn scope_shadow_root(&self, entry: u32, element: StyleNodeID) -> Option<Option<StyleNodeID>> {
        // NB: Where every entry decides in the document's tree, none is looked up.
        if self.all_in_the_document {
            return Some(None);
        }
        let scopes = self.index.scopes_of(self.index.entries.placement(entry as usize).group);
        let scope = match *scopes {
            [] => return None,
            [TreeScopeID::DOCUMENT] => return Some(None),
            [scope] => scope,
            // The trees of a group share its entries, and the element says which reaches it: its
            // own, the one it hosts, or one it is slotted into or exposed to as a part.
            _ => {
                let state = self.state;
                let mut reaching = std::iter::once(self.projection.tree_scope(element))
                    .chain(
                        self.projection
                            .shadow_root_of(element)
                            .and_then(|shadow_root| state.scope_by_root.get(shadow_root)),
                    )
                    .chain(state.scopes_slotted_into(element))
                    .chain(state.part_exposure_scopes(element))
                    .filter(|scope| scopes.contains(scope));
                let scope = reaching.next()?;
                if reaching.any(|other| other != scope) {
                    return None;
                }
                scope
            }
        };
        Some(self.state.scope_root(scope))
    }

    fn add(&mut self, node: StyleNodeID, bits: HintBits) {
        self.added.push((node, bits, u32::MAX));
    }

    fn add_for(&mut self, node: StyleNodeID, bits: HintBits, entry: u32) {
        self.added.push((node, bits, entry));
    }

    /// Follow `links` from `node`, the element left of their combinators.
    fn follow(&mut self, node: StyleNodeID, links: &[Dependency]) {
        let mut down = Links::new();
        let mut across = Links::new();
        for &link in links {
            self.sort(node, link, &mut down, &mut across);
        }
        for links in [&mut down, &mut across] {
            links.sort_unstable();
            links.dedup();
        }
        if !down.is_empty() {
            self.descend(node, &down);
        }
        if !across.is_empty() {
            let first = self.projection.next_element_sibling(node);
            self.walk_siblings(first, &Level::of(self.map, Links::new()), across);
        }
    }

    /// Follow `links`, all of sibling combinators, across `first` and its later siblings, as from
    /// the sibling before it.
    fn follow_across(&mut self, first: StyleNodeID, links: &[Dependency]) {
        let across: Links = links.iter().copied().collect();
        self.walk_siblings(Some(first), &Level::of(self.map, Links::new()), across);
    }

    /// File a link followed from `node` with the ones reaching its children and descendants or
    /// with the ones reaching its later siblings, or follow it out of the tree `node` is in, or
    /// restyle what it reaches outright.
    fn sort(&mut self, node: StyleNodeID, link: Dependency, down: &mut Links, across: &mut Links) {
        let followed = self.map.step(link);
        let links = match followed.combinator {
            ReachCombinator::Descendant | ReachCombinator::ShadowDescendant if followed.reaches_every_subject => {
                self.add_for(node, RESTYLE_DESCENDANTS, followed.entry);
                return;
            }
            ReachCombinator::Subtree => return self.add_for(node, RESTYLE_SELF | RESTYLE_DESCENDANTS, followed.entry),
            ReachCombinator::ParentSubtree => {
                // The children of a shadow root are the host's, and the root element is the only
                // child of its parent that is styled.
                let parent = self
                    .projection
                    .parent(node)
                    .map(|parent| self.projection.host_of(parent).unwrap_or(parent))
                    .filter(|parent| parent.element_index().is_some());
                return match parent {
                    Some(parent) => self.add_for(parent, RESTYLE_DESCENDANTS, followed.entry),
                    None => self.add_for(node, RESTYLE_SELF | RESTYLE_DESCENDANTS, followed.entry),
                };
            }
            ReachCombinator::Child | ReachCombinator::Descendant => down,
            ReachCombinator::NextSibling | ReachCombinator::LaterSibling => across,
            ReachCombinator::ShadowChild
            | ReachCombinator::ShadowDescendant
            | ReachCombinator::AssignedElements
            | ReachCombinator::PartsOfHost
            | ReachCombinator::HostOfBackingElement => return self.jump(node, link),
        };
        links.push(link);
    }

    /// Test the elements `link` reaches from `node` across a tree boundary against it: the
    /// elements of the shadow tree of a host, the elements assigned to a slot, or the parts a host
    /// is exposed.
    fn jump(&mut self, node: StyleNodeID, link: Dependency) {
        /// How many slots assigned to slots an assignment chain passes through at most.
        const MAX_REASSIGNMENTS: u32 = 32;
        let tree = self.projection;
        let followed = self.map.step(link);
        let (entry, next) = (followed.entry, followed.next);
        let mut reached: SmallVec<[StyleNodeID; 16]> = SmallVec::new();
        match followed.combinator {
            ReachCombinator::ShadowChild => {
                reached.extend(
                    tree.shadow_root_of(node)
                        .into_iter()
                        .flat_map(|root| tree.children(root)),
                );
            }
            ReachCombinator::ShadowDescendant => {
                if let Some(root) = tree.shadow_root_of(node) {
                    reached.extend(tree.preorder(root).skip(1));
                }
            }
            // A slot assigned to a slot stands for the elements assigned to it there.
            ReachCombinator::AssignedElements => {
                let mut slots: SmallVec<[(StyleNodeID, u32); 4]> = smallvec::smallvec![(node, 0)];
                while let Some((slot, depth)) = slots.pop() {
                    for &assigned in tree.assigned_nodes_of(slot) {
                        if assigned.element_index().is_none() {
                            continue;
                        }
                        reached.push(assigned);
                        if depth < MAX_REASSIGNMENTS && !tree.assigned_nodes_of(assigned).is_empty() {
                            slots.push((assigned, depth + 1));
                        }
                    }
                }
            }
            // A part forwarded by `exportparts` is exposed to the hosts around the one whose tree
            // it is in.
            ReachCombinator::PartsOfHost => {
                let mut roots: SmallVec<[StyleNodeID; 4]> = tree.shadow_root_of(node).into_iter().collect();
                while let Some(root) = roots.pop() {
                    for element in tree.preorder(root).skip(1) {
                        if tree.part_hosts_of(element).iter().any(|&(_, host)| host == node) {
                            reached.push(element);
                        }
                        roots.extend(tree.shadow_root_of(element));
                    }
                }
            }
            ReachCombinator::HostOfBackingElement => reached.extend(tree.shadow_host_of(node)),
            ReachCombinator::Child
            | ReachCombinator::Descendant
            | ReachCombinator::NextSibling
            | ReachCombinator::LaterSibling => {
                unreachable!("a combinator within one tree is followed by a walk of it")
            }
            ReachCombinator::Subtree | ReachCombinator::ParentSubtree => {
                unreachable!("the end of a chain reaches no elements")
            }
        }
        for element in reached {
            self.visits += 1;
            if !self.matches(element, link) {
                continue;
            }
            match next {
                None => self.add_for(element, RESTYLE_SELF, entry),
                Some(next) => self.follow(element, &[next]),
            }
        }
    }

    /// Test the children of `parent` against the links in `down`, and their descendants against
    /// the descendant combinators' among them.
    fn descend(&mut self, parent: StyleNodeID, down: &[Dependency]) {
        let level = Level::of(self.map, down.iter().copied().collect());
        let first = self.projection.first_element_child(parent);
        self.walk_siblings(first, &level, Links::new());
    }

    /// Test `first` and its later siblings against the links of `level`, which reach every child
    /// of their parent, and those in `across`, which reach the siblings after the one they were
    /// followed from.
    fn walk_siblings(&mut self, first: Option<StyleNodeID>, level: &Level, across: Links) {
        let (tree, map) = (self.projection, self.map);
        // The links the descendants of every sibling are tested against: the level's own, where
        // they all are a descendant combinator's.
        let mut built: Option<Level> = None;
        let mut candidates: SmallVec<[Dependency; 8]> = SmallVec::new();
        // NB: The links reaching later siblings are tested against each sibling of the run, so
        //     their steps are looked up once, as they join it.
        let mut across: SmallVec<[(Dependency, Step); 4]> =
            across.into_iter().map(|link| (link, map.step(link))).collect();
        let mut sibling = first;
        while let Some(current) = sibling {
            // NB: With no link left to test or to descend with, no later sibling can be reached.
            if level.links.is_empty() && across.is_empty() {
                break;
            }
            sibling = tree.next_element_sibling(current);
            let Some(row) = self.projection.row_of(current) else {
                continue;
            };
            self.visits += 1;
            candidates.clear();
            level.filter.candidates(row, &mut candidates);
            let continuing = match level.all_descendant {
                true => level,
                false => &*built.get_or_insert_with(|| {
                    Level::of(
                        map,
                        level
                            .links
                            .iter()
                            .copied()
                            .filter(|&link| map.step(link).combinator == ReachCombinator::Descendant)
                            .collect(),
                    )
                }),
            };
            let mut added = Links::new();
            let mut next_across = Links::new();
            let mut matched: SmallVec<[Step; 8]> = SmallVec::new();
            for &link in &candidates {
                if self.matches(current, link) {
                    matched.push(map.step(link));
                }
            }
            for &(link, step) in &across {
                // An element lacking the key the compound requires cannot match it.
                if step.required_key.is_some_and(|key| !carries_key(row, key)) || !self.matches(current, link) {
                    continue;
                }
                matched.push(step);
            }
            for followed in matched {
                match followed.next {
                    None => self.add_for(current, RESTYLE_SELF, followed.entry),
                    Some(next) if continuing.links.binary_search(&next).is_ok() => {}
                    Some(next) => self.sort(current, next, &mut added, &mut next_across),
                }
            }
            added.sort_unstable();
            added.dedup();
            // A next-sibling combinator reaches only the sibling right after.
            across.retain(|(_, step)| step.combinator == ReachCombinator::LaterSibling);
            for link in next_across {
                if !across.iter().any(|&(held, _)| held == link) {
                    across.push((link, map.step(link)));
                }
            }
            let first_child = tree.first_element_child(current);
            if first_child.is_none() {
                continue;
            }
            if added.is_empty() {
                if !continuing.links.is_empty() {
                    self.walk_siblings(first_child, continuing, Links::new());
                }
            } else {
                let mut links = continuing.links.clone();
                links.extend(added);
                links.sort_unstable();
                links.dedup();
                self.walk_siblings(first_child, &Level::of(map, links), Links::new());
            }
        }
    }

    /// Whether `element` matches the compound right of `link` on its own.
    fn matches(&mut self, element: StyleNodeID, link: Dependency) -> bool {
        // What cannot be told from the element's own facts is followed.
        self.evaluate(link, element, false).unwrap_or(true)
    }

    /// Keep of `effects` those whose ancestor keys the ancestors of `node` might carry on either side of the
    /// transaction, whichever parents it had on them. The ancestors are read only where an effect asks.
    fn retain_effects_around(&mut self, node: StyleNodeID, effects: &mut SmallVec<[HeldDependency; 16]>) {
        if effects.iter().all(|held| held.ancestor_hashes.is_empty()) {
            return;
        }
        let mut keys = self.ancestor_keys(node);
        let mut ancestor = self.projection.parent_on(TransactionFactSide::Before, node);
        while let Some(current) = ancestor {
            if let Some(row) = self.row_before(current) {
                keys.extend(super::ancestor_keys_of(row));
            }
            ancestor = self.projection.parent_on(TransactionFactSide::Before, current);
        }
        let mut ancestors = self.ancestors.take().unwrap_or_else(super::bloom::AncestorFilter::new);
        for &key in &keys {
            ancestors.insert(key);
        }
        effects.retain(|held| ancestors.might_contain_all(&held.ancestor_hashes));
        for &key in &keys {
            ancestors.remove(key);
        }
        *self.ancestors = Some(ancestors);
    }

    /// The keys the ancestors of `node` carry, now and before the transaction.
    fn ancestor_keys(&self, node: StyleNodeID) -> SmallVec<[u32; 32]> {
        let mut keys = SmallVec::new();
        let mut ancestor = self.projection.parent(node);
        while let Some(current) = ancestor {
            if let Some(row) = self.projection.row_of(current) {
                keys.extend(super::ancestor_keys_of(row));
            }
            if let Some(before) = self.projection.row_on(TransactionFactSide::Before, current)
                && self.projection.facts(current) != Some(before.facts)
            {
                keys.extend(super::ancestor_keys_of(before));
            }
            ancestor = self.projection.parent(current);
        }
        keys
    }

    /// Whether `element` matched the selector `compound` is of, from that compound leftwards,
    /// before its transaction and not after or the other way around, or might have. What the
    /// changed element being followed matches is remembered by compound.
    fn changed_matching(&mut self, compound: Dependency, element: StyleNodeID) -> bool {
        // NB: What the compounds left of this one match is of other elements, whose changes are
        //     followed from them: only where the compound's own tests answer differently for the
        //     element before and now can the element's change reach through it.
        // NB: The compounds of other selectors testing the same facts alike share the answers of
        //     their own tests, which most selectors of a sheet have a few kinds of.
        // NB: Which shadow root a compound is matched in is of the group of trees its rule decides
        //     in, so only the compounds of the document's sheets share answers by their own tests.
        let compound_slot = self.map.compound_memo_slot(compound);
        let shares = self.all_in_the_document
            || self.index.entries.placement(compound.entry as usize).group == super::rules::ScopeGroupID::DOCUMENT;
        let own = match self.map.own_tests_memo_slot(compound) {
            Some(slot) if shares => slot,
            _ => compound_slot,
        };
        let leftwards = match self.map.chain_memo_slot(compound) {
            Some(slot) if shares => slot,
            _ => compound_slot,
        };
        let memoized = |follower: &mut Self, kind: MemoKind| {
            let slot = match kind {
                MemoKind::Own { .. } => own,
                MemoKind::Leftwards { .. } => leftwards,
            };
            if let MemoAnswer::Answered(answer) = follower.memo.get(slot, kind) {
                return answer;
            }
            let answer = match kind {
                MemoKind::Own { before } => follower.evaluate(compound, element, before),
                // NB: The compound's own tests were answered first, which the walk leftwards starts from.
                MemoKind::Leftwards { before } => {
                    let known = follower.memo.get(own, MemoKind::Own { before });
                    follower.matches_from_knowing(compound, element, before, known)
                }
            };
            follower.memo.set(slot, kind, answer);
            answer
        };
        let after = memoized(self, MemoKind::Own { before: false });
        if after.is_some() && after == memoized(self, MemoKind::Own { before: true }) {
            return false;
        }
        let after = memoized(self, MemoKind::Leftwards { before: false });
        let before = memoized(self, MemoKind::Leftwards { before: true });
        !matches!((before, after), (Some(before), Some(after)) if before == after)
    }

    /// Whether `element` matches the selector `compound` is of, from that compound leftwards, in
    /// the facts from before the transaction where it is `before`, or none where that cannot be
    /// told.
    fn matches_from(&mut self, compound: Dependency, element: StyleNodeID, before: bool) -> Option<bool> {
        self.matches_from_knowing(compound, element, before, MemoAnswer::Unasked)
    }

    /// As [`Self::matches_from`], with what the element answers for the compound's own tests
    /// where `own` says it is known.
    fn matches_from_knowing(
        &mut self,
        compound: Dependency,
        element: StyleNodeID,
        before: bool,
        own: MemoAnswer,
    ) -> Option<bool> {
        let own = match own {
            MemoAnswer::Answered(own) => own,
            MemoAnswer::Unasked => self.evaluate(compound, element, before),
        };
        if !own? {
            return Some(false);
        }
        let Some((combinator, left)) = self.map.left(compound) else {
            return Some(true);
        };
        // The elements before the transaction stood where it found them.
        let projection = self.projection;
        let parent = |node| match before {
            true => projection.parent_on(TransactionFactSide::Before, node),
            false => projection.parent(node),
        };
        let previous_sibling = |node| match before {
            true => projection.previous_element_sibling_on(TransactionFactSide::Before, node),
            false => projection.previous_element_sibling(node),
        };
        // NB: An element this answers does not match for may never be matched across its siblings, which is what has its
        //     parent's later changes look at them.
        let note_read = |read| {
            let side = match before {
                true => TransactionFactSide::Before,
                false => TransactionFactSide::After,
            };
            projection.note_structural_read(side, element, read);
        };
        // Whether any of the candidates matches, or might.
        let any = |follower: &mut Self, candidates: &mut dyn Iterator<Item = StyleNodeID>| {
            let mut answer = Some(false);
            for candidate in candidates {
                match follower.matches_from(left, candidate, before) {
                    Some(true) => return Some(true),
                    Some(false) => {}
                    None => answer = None,
                }
            }
            answer
        };
        match combinator {
            ReachCombinator::Child => any(self, &mut parent(element).into_iter()),
            ReachCombinator::NextSibling => {
                note_read(StructuralRead::PreviousSibling);
                any(self, &mut previous_sibling(element).into_iter())
            }
            ReachCombinator::Descendant => any(self, &mut std::iter::successors(parent(element), |&node| parent(node))),
            ReachCombinator::LaterSibling => {
                note_read(StructuralRead::EarlierSiblings);
                any(
                    self,
                    &mut std::iter::successors(previous_sibling(element), |&node| previous_sibling(node)),
                )
            }
            // NB: An element that left stood below the host it had.
            ReachCombinator::ShadowChild => any(
                self,
                &mut parent(element)
                    .and_then(|parent| projection.host_of(parent))
                    .into_iter(),
            ),
            ReachCombinator::ShadowDescendant => {
                let root = std::iter::successors(Some(element), |&node| parent(node))
                    .last()
                    .expect("the element is its own first ancestor");
                any(self, &mut projection.host_of(root).into_iter())
            }
            // NB: An element whose slot or exposure moved is restyled whole, so the ones these reach
            //     are the same before as now.
            ReachCombinator::AssignedElements => any(
                self,
                &mut std::iter::successors(projection.assigned_slot_of(element), |&slot| {
                    projection.assigned_slot_of(slot)
                })
                .take(32),
            ),
            ReachCombinator::PartsOfHost => {
                let mut hosts: SmallVec<[StyleNodeID; 4]> = SmallVec::new();
                for &(_, host) in projection.part_hosts_of(element) {
                    if !hosts.contains(&host) {
                        hosts.push(host);
                    }
                }
                any(self, &mut hosts.into_iter())
            }
            ReachCombinator::HostOfBackingElement => {
                let backing: SmallVec<[StyleNodeID; 4]> = projection
                    .shadow_root_of(element)
                    .into_iter()
                    .flat_map(|root| projection.preorder(root).skip(1))
                    .filter(|&node| {
                        projection
                            .facts(node)
                            .is_some_and(|facts| projection.backing_pseudo_kind(facts).is_some())
                    })
                    .collect();
                any(self, &mut backing.into_iter())
            }
            ReachCombinator::Subtree | ReachCombinator::ParentSubtree => {
                unreachable!("the end of a chain has nothing left of it")
            }
        }
    }

    /// Whether `element` matches `compound` in its own facts, in those from before the
    /// transaction where it is `before` and has any, or none where that cannot be told.
    fn evaluate(&mut self, compound: Dependency, element: StyleNodeID, before: bool) -> Option<bool> {
        self.evaluate_operands(compound, element, before, false)
    }

    /// Whether `element` can match `compound` in its own facts whatever its `:has()` operands
    /// answer, as [`Self::evaluate`] tells.
    fn evaluate_around_relative(&mut self, compound: Dependency, element: StyleNodeID, before: bool) -> Option<bool> {
        self.evaluate_operands(compound, element, before, true)
    }

    fn evaluate_operands(
        &mut self,
        compound: Dependency,
        element: StyleNodeID,
        before: bool,
        around_relative: bool,
    ) -> Option<bool> {
        self.tests += 1;
        // An element that arrived matched nothing before.
        if before && !self.projection.is_connected_on(TransactionFactSide::Before, element) {
            return Some(false);
        }
        let shadow_root = self.scope_shadow_root(compound.entry, element)?;
        // NB: A compound testing names alone is answered by the element's facts, but on the host of
        //     the shadow tree its rule decides in, which is featureless there.
        let names = self.map.operand_names(compound);
        if !names.is_empty()
            && shadow_root.is_none_or(|shadow_root| self.projection.shadow_root_of(element) != Some(shadow_root))
        {
            let row = match before {
                true => self.row_before(element)?,
                // An element without facts is not one selectors match.
                false => match self.projection.row_of(element) {
                    Some(row) => row,
                    None => return Some(false),
                },
            };
            return Some(names.carried_by(&row));
        }
        let (selector, at) = self.map.resolve(compound, self.index);
        let document = self.document;
        if before {
            if self.subtrees_left && !around_relative && self.map.reads_relative(compound) {
                return None;
            }
            self.row_before(element)?;
            self.before.set_scope_shadow_root(shadow_root);
            return self
                .before
                .matches_compound_alone(selector, at, element, document, around_relative);
        }
        // An element without facts is not one selectors match.
        if self.projection.row_of(element).is_none() {
            return Some(false);
        }
        self.own_facts.set_scope_shadow_root(shadow_root);
        self.own_facts
            .matches_compound_alone(selector, at, element, document, around_relative)
    }
}
impl StyleEngine {
    /// Have the engine own the document under `root` from a new cold pass of it, which it keeps
    /// owning, and answer how long the cold pass took in nanoseconds.
    #[cfg(test)]
    pub(in super::super) fn restart_live_style(&mut self, root: StyleNodeID, threads: u32) -> u64 {
        self.start_live_style(root, threads, false)
    }

    /// Have the engine own the document whose document element is `root` from its first style
    /// transaction with a document element on. Its passes run on as many threads as
    /// `LIBWEB_STYLE_THREADS` asks for, or on the default number for the machine where it names
    /// no number of them. A fork of the render state goes on with the engine of the state it
    /// forked, and never starts it.
    pub(in super::super) fn own_from_the_start(&mut self, root: StyleNodeID) {
        if !self.retained.live_style_may_start.is_original()
            || self.retained.live_style.is_some()
            || !self.retained.projection.is_live(root)
        {
            return;
        }
        self.start_live_style(root, crate::style_layout_pool::threads_from_environment(), true);
    }

    /// Have the engine own the document under `root` from a cold pass of it, with what the cold
    /// pass computed to install where `installs_cold_pass` is, and answer how long the cold pass
    /// took in nanoseconds.
    fn start_live_style(&mut self, root: StyleNodeID, threads: u32, installs_cold_pass: bool) -> u64 {
        debug_assert!(
            self.retained.live_style_may_start.is_original(),
            "the engine starts on the render state the host writes, never on a fork of it"
        );
        // NB: The engine owns the document from its first transaction with a document element on. Where the
        //     initial style waits on fonts it cannot resolve yet, the live style it holds, if any, goes on
        //     owning the document, and the next transaction starts it again.
        let initial_inputs = InitialStyleInputs::of(&self.retained.document_style_computation_inputs);
        let initial = self.computed_initial(root);
        let Some(initial) = initial else {
            return 0;
        };
        // NB: A document the engine goes on owning keeps the records it installed, which the records of
        //     the new cold pass replace as they change.
        if self.has_live_style() {
            self.host.element_inputs.restyle_everything();
        }
        let cache_audit = self
            .retained
            .live_style
            .as_ref()
            .map_or_else(traversal::cache_audit_mode_from_environment, |live| live.cache_audit);
        self.retained.live_style = None;
        // NB: How many threads the machine has is asked once, as the engine starts: the answer reads the
        //     process's CPU affinity and quota from the kernel, which takes longer than a small pass does.
        let threads = match threads {
            0 => crate::style_layout_pool::default_thread_count() as u32,
            threads => threads,
        };
        let index = RuleIndex::build(&self.retained.rules, self.retained.process_rule_indexes());
        self.retained.live_counters.note_live_start(&index);
        let invalidation = InvalidationMap::build(&index);
        self.retained.live_style = Some(Box::new(Live {
            index: Arc::new(index),
            invalidation: Arc::new(invalidation),
            initial: Arc::new(initial),
            initial_inputs,
            records: RetainedRecords::default(),
            hints: Hints::default(),
            scratch: Default::default(),
            installed_as_undisplayed: HashSet::default(),
            pending_everything: false,
            pending_index_rebuild: false,
            changed_rules: HashSet::default(),
            changed_rule_contents: HashSet::default(),
            changed_sheets: HashSet::default(),
            reordered_scopes: HashSet::default(),
            layered_scopes: HashSet::default(),
            report: Default::default(),
            counts: Default::default(),
            threads,
            cache_audit,
            #[cfg(test)]
            cache_audits: Default::default(),
            uninstalled: Vec::new(),
            keyframes_moved: MovedKeyframes::default(),
            plans_owed: Vec::new(),
            compositions_owed: Vec::new(),
            installation: Default::default(),
            released_compositions: Default::default(),
            deferring_passes: 0,
            generation: 0,
            demanded: Default::default(),
            kept_hidden: self
                .retained
                .projection
                .style_preorder(root)
                .filter(|&node| self.retained.keeps_style_in_display_none_subtree(node))
                .collect(),
            kept_hidden_records: Default::default(),
        }));
        self.retained
            .cold_passes
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let started = Instant::now();
        self.live_pass(root, true);
        let nanoseconds = started.elapsed().as_nanos() as u64;
        if let Some(live) = self.retained.live_style.as_mut() {
            *live.report = [0; LiveField::COUNT];
            // NB: The cold pass computes the records that are installed already, where it does.
            if !installs_cold_pass {
                live.uninstalled.clear();
                live.plans_owed.clear();
                live.compositions_owed.clear();
            }
        }
        nanoseconds
    }

    /// What the main thread does to take the last installation, taken by the one who hands the
    /// installation to the host.
    pub(in super::super) fn take_installation(&mut self) -> super::install::StyleInstallation {
        self.retained
            .live_style
            .as_mut()
            .map(|live| std::mem::take(&mut *live.installation))
            .unwrap_or_default()
    }

    /// What the children of `node` compute from of its view, before the composition of its
    /// animations moves, where the engine owns the document: the view of the animation overlay
    /// an installation let go of, where one did.
    pub(in super::super) fn hold_composition(&mut self, node: StyleNodeID) -> Option<HeldComposition> {
        let live = self.retained.live_style.as_mut()?;
        if let Some(index) = live
            .released_compositions
            .iter()
            .position(|(released, _)| *released == node)
        {
            return Some(live.released_compositions.swap_remove(index).1);
        }
        self.retained.hold_composition(node)
    }

    /// Whether an installation let go of the animation overlay of `node`, whose composition the
    /// host settles again.
    pub(in super::super) fn composition_released(&self, node: StyleNodeID) -> bool {
        self.retained
            .live_style
            .as_ref()
            .is_some_and(|live| live.released_compositions.iter().any(|(released, _)| *released == node))
    }

    /// The host goes on to sample the animations of `node` and run its transition step, which composes a transition and
    /// then takes it out again where the step ends it. What they compose is told as one move, once they are over, so
    /// that a pair that leaves the composition as it found it restyles nothing.
    pub(in super::super) fn begin_composition_settling(&mut self, node: StyleNodeID) {
        debug_assert!(self.host.settling_composition.is_none());
        self.host.settling_composition = Some(SettlingComposition { node, held: None });
    }

    pub(in super::super) fn end_composition_settling(&mut self) {
        let settling = self
            .host
            .settling_composition
            .take()
            .expect("a composition settling ends after it began");
        if let Some(held) = settling.held {
            self.note_composition_moved(settling.node, held);
        }
    }

    /// Restyle the children of `node` that compute from what the composition of its animations
    /// moved since `held`, in the next pass of the engine.
    pub(in super::super) fn note_composition_moved(&mut self, node: StyleNodeID, held: HeldComposition) {
        if let Some(settling) = self
            .host
            .settling_composition
            .as_mut()
            .filter(|settling| settling.node == node)
        {
            settling.held.get_or_insert(held);
            return;
        }
        let Some(view) = DriveRecords::assigned_record_view(&self.retained, node) else {
            return;
        };
        let custom_properties = self
            .retained
            .element_custom_property_data
            .get(&node)
            .and_then(|held| held.sampled_over.map(|_| held.identity));
        // The children compute from the element's display as from its inherited groups: a flex or grid container
        // blockifies them.
        let display = |payloads: &crate::css::computed_values::StyleGroups| {
            crate::css::computed_value_views::ComputedValuesView::of(payloads).display()
        };
        let font_size_animated = HeldComposition::animates_font_size(&view);
        let impact =
            match custom_properties == held.custom_properties && display(view.payloads) == display(&held.payloads) {
                true => ChildImpact::of_groups(view.payloads, &held.payloads),
                false => ChildImpact::All,
            };
        // What resolved root-relative lengths against the font of the document element resolves them again, which the
        // next pass tells as it restyles the element.
        let root_font = super::compute::RootFontInputs::from_payloads(view.payloads, view.dependency_flags);
        let root_font_moved = traversal::is_document_element(&self.retained, node)
            && self
                .retained
                .live_style
                .as_ref()
                .and_then(|live| live.records.kept_root_font(node))
                .is_some_and(|kept| kept != root_font);
        if let Some(mut live) = self.retained.live_style.take() {
            live.generation += 1;
            live.note_composition_moved(&self.retained, node, impact);
            if font_size_animated != held.font_size_animated {
                live.note_animated_font_size_came_or_went(&self.retained, node);
            }
            if root_font_moved {
                live.hints.reserve(self.retained.projection.element_slot_count());
                live.hints.mark(node, RESTYLE_PSEUDO_ELEMENTS);
            }
            self.retained.live_style = Some(live);
        }
    }
}

impl RetainedState {
    /// What the children of `node` compute from of the view it is assigned.
    pub(super) fn hold_composition(&self, node: StyleNodeID) -> Option<HeldComposition> {
        let view = DriveRecords::assigned_record_view(self, node)?;
        let custom_properties = self
            .element_custom_property_data
            .get(&node)
            .and_then(|held| held.sampled_over.map(|_| held.identity));
        Some(HeldComposition::new(&view, custom_properties))
    }

    /// Restyle the elements naming a counter-style registry their style scope no longer has, in
    /// the next pass of the engine.
    pub(in super::super) fn note_counter_style_environment_moved(&mut self) {
        if let Some(mut live) = self.live_style.take() {
            live.generation += 1;
            live.note_counter_style_environment_moved(self);
            self.live_style = Some(live);
        }
    }
}

impl StyleEngine {
    /// Whether the engine styled the document yet.
    pub(in super::super) fn has_live_style(&self) -> bool {
        self.retained.live_style.is_some()
    }

    /// Restyle what the transactions since the last pass changed, where the engine owns the
    /// document, and answer the rows installing the records that replaced the ones the host has.
    pub(in super::super) fn install_restyled(&mut self, root: StyleNodeID) -> Vec<PublishedStyleDeltaRecord> {
        #[cfg(test)]
        let _section = super::allocation_count::StyleSection::enter();
        self.live_restyle(root);
        let kept_hidden = self.demand_kept_hidden();
        let Some(mut live) = self.retained.live_style.take() else {
            return Vec::new();
        };
        live.plans_owed.sort_unstable();
        live.plans_owed.dedup();
        live.compositions_owed.sort_unstable();
        live.compositions_owed.dedup();
        let mut scratch = live.begin_installation();
        let mut installation = super::install::StyleInstallation::default();
        let rows = self.retained.installation_rows(
            root,
            &live,
            live.uninstalled.iter().chain(&kept_hidden).copied(),
            &live.plans_owed,
            &live.compositions_owed,
            &mut scratch,
            &mut installation,
        );
        live.end_installation(scratch);
        recycle(&mut live.uninstalled);
        recycle(&mut live.plans_owed);
        recycle(&mut live.compositions_owed);
        // NB: An installation no one took, such as one a hover on a fork of the render state made,
        //     lets go of the records it pinned as the next takes its place.
        for record in std::mem::replace(&mut *live.installation, installation).pinned_records {
            self.unpin_layout_style_record(record);
        }
        live.generation += 1;
        self.retained.live_style = Some(live);
        rows
    }

    /// Restyle what the transactions since the last pass changed, when the engine is live.
    pub(in super::super) fn live_restyle(&mut self, root: StyleNodeID) {
        // The host settled the compositions the last installation let go of, or composes none over
        // the installed record any more.
        if let Some(live) = self.retained.live_style.as_mut()
            && !live.released_compositions.is_empty()
        {
            let mut released = std::mem::take(&mut *live.released_compositions);
            for (node, held) in released.drain(..) {
                self.note_composition_moved(node, held);
            }
            if let Some(live) = self.retained.live_style.as_mut() {
                *live.released_compositions = released;
            }
        }
        self.refresh_initial(root);
        if self.retained.live_style.as_ref().is_some_and(|live| live.has_pending()) {
            self.live_pass(root, false);
        } else if let Some(live) = self.retained.live_style.as_mut() {
            // NB: The invalidation of a transaction that hinted nothing counts as well.
            self.retained.live_counters.take_pass(&mut live.counts);
        }
    }

    /// Compute the initial style of the engine again where what it computes from moved since,
    /// and restyle every element against it where it computes to other values. Where the initial
    /// style waits on fonts it cannot resolve yet, the one the engine holds stays until a later
    /// transaction computes it.
    fn refresh_initial(&mut self, root: StyleNodeID) {
        let inputs = InitialStyleInputs::of(&self.retained.document_style_computation_inputs);
        if self
            .retained
            .live_style
            .as_ref()
            .is_none_or(|live| live.initial_inputs == inputs)
        {
            return;
        }
        let initial = self.computed_initial(root);
        let Some(initial) = initial else {
            return;
        };
        let live = self
            .retained
            .live_style
            .as_mut()
            .expect("computing the initial style keeps the engine");
        live.initial_inputs = inputs;
        // NB: A font face loading or failing moves the font environment, and the device pixel ratio
        //     moving moves the initial widths of borders where they snap to other device pixels,
        //     but the initial style is as it was unless it computes to other values or its font
        //     resolves to other fonts now. Every element holds the initial values it does not
        //     decide, so one that moved restyles every element.
        if live.initial.font == initial.font
            && live.initial.color == initial.color
            && live.initial.holds_the_same_values_as(&initial)
        {
            return;
        }
        live.initial = Arc::new(initial);
        live.pending_everything = true;
    }

    fn live_pass(&mut self, root: StyleNodeID, everything: bool) {
        self.matching_fonts_by_content(|engine| engine.live_pass_rounds(root, everything));
    }

    fn live_pass_rounds(&mut self, root: StyleNodeID, everything: bool) {
        let threads = self.retained.live_style.as_ref().map_or(0, |live| live.threads);
        let pool = self.style_layout_pool(threads);
        // NB: A thread that first asks for a font resolves it there and then, under a lock, rather than leave it for a
        //     round between all the threads of the pass, each a barrier.
        self.retained.pass_fonts = self
            .host
            .font_resolver
            .as_ref()
            .zip(self.retained.font_resolution.as_ref())
            .map(|(resolver, cache)| Arc::new(super::super::font_resolution::PassFonts::new(resolver, cache)));
        let mut scope = match everything {
            true => RestyleScope::Everything,
            false => RestyleScope::Pending,
        };
        for _ in 0..super::MAX_RESOLVE_ROUNDS {
            let Some(mut live) = self.retained.live_style.take() else {
                break;
            };
            let resolver = self.host.font_resolver.as_ref();
            let mut resolve = |retained: &mut RetainedState, fonts, random_bases| {
                resolve_waits(retained, resolver, root, fonts, random_bases);
            };
            let statistics = live.restyle(&mut self.retained, root, pool.as_deref(), scope, &mut resolve);
            self.retained.live_style = Some(live);
            if statistics.font_requests.is_empty() && statistics.random_requests.is_empty() {
                break;
            }
            resolve_waits(
                &mut self.retained,
                self.host.font_resolver.as_ref(),
                root,
                statistics.font_requests,
                statistics.random_requests,
            );
            // A pass after the first restyles what waited on a font or a random base value, now
            // that it resolved, as the last pass left it hinted.
            scope = RestyleScope::Pending;
        }
        if let Some(pass_fonts) = self.retained.pass_fonts.take().and_then(Arc::into_inner)
            && let Some(cache) = self.retained.font_resolution.as_mut()
        {
            let resolved = cache.take_in_pass_fonts(pass_fonts);
            self.retained
                .counters
                .add(Counter::FontResolutionRequests, resolved as u64);
        }
        if let Some(live) = self.retained.live_style.as_mut() {
            self.retained.live_counters.take_pass(&mut live.counts);
        }
    }
}

/// Resolve the fonts and draw the random base values a pass waited on. A pass waits on fonts mostly
/// after the font environment moved, as a font face loaded, which forgets every resolution: the
/// elements styled again then ask for most of what the generations before resolved, a wave of
/// rounds deeper each, which are resolved anew together with what the pass waited on.
fn resolve_waits(
    retained: &mut RetainedState,
    resolver: Option<&super::super::font_resolution::FontResolverHost>,
    root: StyleNodeID,
    fonts: Vec<super::super::font_resolution::FontRequest>,
    random_bases: Vec<super::super::random_bases::RandomBaseRequest>,
) {
    let carried = match (fonts.first(), retained.font_resolution.as_mut()) {
        (Some(request), Some(cache)) => {
            let generation = request.generation();
            cache
                .take_carried_requests()
                .into_iter()
                .map(|carried| carried.for_generation(generation))
                .collect()
        }
        _ => Vec::new(),
    };
    for request in fonts.into_iter().chain(carried) {
        retained.refill_font_request(resolver.expect("a request has a font resolver"), root, request);
    }
    for request in random_bases {
        retained.draw_random_base_value(request);
    }
}

/// What the children of an element compute from of the view it held before the composition of
/// its animations moved: its groups, each holding a reference, and the environment its animations
/// sampled custom properties into, where they do.
#[derive(Clone)]
pub(in super::super) struct SettlingComposition {
    node: StyleNodeID,
    /// What the element's children computed from before the first move.
    held: Option<HeldComposition>,
}

#[derive(Clone)]
pub(in super::super) struct HeldComposition {
    payloads: crate::css::computed_values::StyleGroups,
    custom_properties: Option<u64>,
    /// Whether its animations gave it a font size.
    font_size_animated: bool,
}

impl HeldComposition {
    fn new(view: &computed::StyleRecordView<'_>, custom_properties: Option<u64>) -> Self {
        Self {
            payloads: view.payloads.clone(),
            custom_properties,
            font_size_animated: Self::animates_font_size(view),
        }
    }

    fn animates_font_size(view: &computed::StyleRecordView<'_>) -> bool {
        view.longhands()
            .overlay_value(
                view.animated_overlay,
                crate::css::property_metadata::property_id::FONT_SIZE,
            )
            .is_some()
    }
}

impl RetainedState {
    /// Check the records the engine holds for the document under `root` against the installed
    /// ones, when asked to, and answer one field of what it measured since it started.
    pub(in super::super) fn live_report(&mut self, root: StyleNodeID, verify: bool, index: u32) -> u64 {
        if verify && let Some(mut live) = self.live_style.take() {
            let mut verified = 0;
            let mut missing = 0;
            for node in self.projection.style_preorder(root) {
                if node.element_index().is_none() || self.computed_group_sets.assigned_style_record(node).is_none() {
                    continue;
                }
                if live.records.get(node).is_none() {
                    missing += 1;
                    continue;
                }
                verified += 1;
            }
            live.report[LiveField::VerifiedElements as usize] = verified;
            live.report[LiveField::MissingRecords as usize] = missing;
            live.report[LiveField::Differences as usize] =
                super::self_check::installed_differences(self, root, &live.records, &|_| String::new()).len() as u64;
            self.live_style = Some(live);
        }
        if index == LiveField::ColdPasses as u32 {
            return self.cold_passes.load(std::sync::atomic::Ordering::Relaxed);
        }
        self.live_style
            .as_ref()
            .and_then(|live| live.report.get(index as usize).copied())
            .unwrap_or(0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    const SIBLINGS: u32 = 1000;

    #[test]
    fn compound_memo_forgets_answers_when_the_stamp_wraps() {
        let mut memo = CompoundMemo::default();
        let kind = MemoKind::Own { before: false };
        memo.start(1);
        memo.stamp = 1;
        memo.set(0, kind, Some(true));

        memo.stamp = u16::MAX;
        memo.start(1);
        assert!(matches!(memo.get(0, kind), MemoAnswer::Unasked));
    }

    fn next_of(steps: &Cell<u32>) -> impl Fn(StyleNodeID) -> Option<StyleNodeID> + '_ {
        |node| {
            steps.set(steps.get() + 1);
            let index = node.element_index().unwrap();
            (index < SIBLINGS).then(|| StyleNodeID::element(index + 1))
        }
    }

    #[test]
    fn the_bloom_of_later_siblings_is_found_once_from_any_of_them() {
        let bloom_of = |node: StyleNodeID| 1u64 << (node.element_index().unwrap() % 64);
        let expected = |first: u32| (first..=SIBLINGS).fold(0, |bits, index| bits | 1u64 << (index % 64));
        for firsts in [
            vec![1; SIBLINGS as usize],
            (1..=SIBLINGS).rev().collect(),
            (1..=SIBLINGS).collect(),
        ] {
            let steps = Cell::new(0);
            let mut known = HashMap::default();
            for first in firsts {
                let bloom = bloom_from(StyleNodeID::element(first), next_of(&steps), bloom_of, &mut known);
                assert_eq!(bloom, expected(first));
            }
            assert!(steps.get() <= SIBLINGS);
        }
    }

    #[test]
    fn a_run_of_arrived_siblings_is_walked_once() {
        let arrived: HashSet<StyleNodeID> = (1..SIBLINGS).map(StyleNodeID::element).collect();
        let steps = Cell::new(0);
        let mut past_arrived = HashMap::default();
        for first in 1..SIBLINGS {
            let found = first_not_arrived(
                StyleNodeID::element(first),
                &arrived,
                next_of(&steps),
                &mut past_arrived,
            );
            assert_eq!(found, Some(StyleNodeID::element(SIBLINGS)));
        }
        assert!(steps.get() <= SIBLINGS);

        let all: HashSet<StyleNodeID> = (1..=SIBLINGS).map(StyleNodeID::element).collect();
        let found = first_not_arrived(StyleNodeID::element(1), &all, next_of(&steps), &mut HashMap::default());
        assert_eq!(found, None);
    }
}
