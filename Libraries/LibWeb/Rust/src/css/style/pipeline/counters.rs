/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the engine did over the life of a document's render state, counted for tests to bound,
//! and the passes a test traces.
//!
//! A pass counts what it does in counts of its own, kept with the engine between the
//! transactions it restyles after, which the state takes into its counters once the pass kept the
//! records it computed. Unlike the live report, which a fork of the state starts anew, the counters
//! are the state's: a fork copies them as they are and counts its own passes in its copy, which
//! the state never sees, as the host never installs what a fork computes.

use super::super::tree::StyleNodeID;
use super::live::LiveField;
use super::projection::Projection;
use super::rules::RuleIndex;
use super::traversal::{Outcome, Statistics};

/// One of the counters of [`Counters`], in the order the host names them.
#[derive(Clone, Copy)]
#[repr(u32)]
pub(in super::super) enum StyleCounter {
    /// The passes of the engine: each restyle, of everything, of what transactions changed, or
    /// of the elements a pass before left waiting on fonts. A transaction that hints no element
    /// runs none.
    Passes,
    /// The passes that restyled every element: the one the engine starts from, and one after an
    /// input that can restyle any element, such as a stylesheet change.
    FullPasses,
    /// The elements passes styled: each once per pass that styled it.
    StyledElements,
    /// The elements passes styled differently than before in what their children compute from.
    ChangedElements,
    /// The elements whose hints asked a pass restyling what changed to restyle them, or their
    /// pseudo-elements, counted as the pass starts.
    HintedElements,
    /// The elements whose hints asked a pass restyling what changed to restyle their descendants,
    /// counted as the pass starts.
    HintedSubtrees,
    /// The elements the invalidation of transactions visited, following the selectors of the
    /// inputs they changed: each once per visit.
    InvalidationVisits,
    /// The compounds the invalidation of transactions tested against an element.
    InvalidationTests,
    /// The cold passes the engine started from: one each time it started.
    ColdPasses,
    /// The elements passes gave the record of an element they styled before with the same parent,
    /// the same inputs and the same revalidation answers, without matching them.
    SharedElements,
    /// The elements passes gave the record of an element that matched the same rules and declared
    /// the same declarations, under a parent they inherit the same from, which they matched but
    /// did not cascade or compute.
    ReusedElements,
    /// The rule indexes the engine built: one as it started, and one for each pass after a rule
    /// change.
    IndexBuilds,
    /// The entries of those indexes, summed.
    IndexedEntries,
    /// The selector entries those indexes, and the ones cold passes built, compiled: those of the
    /// programs no rule index build of the process compiled before.
    CompiledSelectors,
    /// The user agent indexes those indexes built, which no document of the process had built
    /// before: a document whose user agent rules decide as another's did shares its index.
    UserAgentIndexBuilds,
    /// The elements passes matched against the whole rule index, not only against the entries
    /// they were restyled for.
    MatchedElements,
    /// The walks of the argument of a `:has()` that passes took to answer it for an anchor.
    RelativeWalks,
}

impl StyleCounter {
    pub(in super::super) const COUNT: usize = Self::RelativeWalks as usize + 1;
}

/// What a pass counts until the state takes it, and what it styled while a trace is open.
#[derive(Clone, Default)]
pub(super) struct PassCounts {
    counts: [u64; StyleCounter::COUNT],
    traced: Vec<TracedPass>,
    /// What the engine did to its rule index since the state last took the counts, which a trace keeps.
    program_events: Vec<(ProgramEvent, u64)>,
}

/// Something the engine did to the rules it styles from, which a trace lists in a `style PROGRAM` block, with how many
/// times it happened in a row.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub(in super::super) enum ProgramEvent {
    RuleIndexBuilt = 1,
    RuleIndexUpdated = 2,
    UserAgentRuleIndexBuilt = 3,
    SelectorsCompiled = 4,
    StyleRulesCompiled = 5,
    SheetDeclarationsInterned = 6,
    SheetDeclarationsShared = 7,
}

/// How an element of a traced pass came by its record.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub(super) enum TracedOutcome {
    Computed = 1,
    Shared = 2,
    Reused = 3,
    Declined = 4,
    /// The element took the custom properties its parent hands down now without being styled.
    KeptWithMovedCustomProperties = 5,
    /// The host composed the element's animations over its record, rebuilding only the groups they write.
    Sampled = 6,
    /// The host composed the element's animations over its record, rebuilding every group of it.
    SampledEveryGroup = 7,
}

impl From<&Outcome> for TracedOutcome {
    fn from(outcome: &Outcome) -> Self {
        match outcome {
            Outcome::Computed => Self::Computed,
            Outcome::Shared => Self::Shared,
            Outcome::Reused => Self::Reused,
            Outcome::CascadeDeclined | Outcome::ComputeDeclined => Self::Declined,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
enum TracedPassKind {
    Full = 1,
    Partial = 2,
    Cold = 3,
    /// The animation samples the host composed over records, which no pass ran.
    Animation = 5,
    /// What the engine did to the rules it styles from.
    Program = 6,
}

/// A pass a test traces: the elements it styled, each with how it came by its record, and the elements whose records
/// it changed, or, for a program block, what the engine did to its rules.
#[derive(Clone)]
struct TracedPass {
    kind: TracedPassKind,
    elements: Vec<(StyleNodeID, Option<TracedOutcome>)>,
    changed: Vec<StyleNodeID>,
    program_events: Vec<(ProgramEvent, u64)>,
}

/// The passes the engine ran since a test began tracing, with their entries in tree order once a test reads them.
#[derive(Clone, Default)]
struct StyleTrace {
    passes: Vec<TracedPass>,
    entries: Option<Vec<u64>>,
}

/// The kind of an entry of a style trace, in bits 32 to 39, as the host decodes it.
const TRACE_ENTRY_KIND_SHIFT: u32 = 32;
/// The outcome of an element entry, in bits 40 to 47.
const TRACE_ENTRY_OUTCOME_SHIFT: u32 = 40;
/// Whether the record of the element of an entry changed.
const TRACE_ENTRY_CHANGED: u64 = 1 << 48;
const TRACE_ENTRY_KIND_ELEMENT: u64 = 4;
const TRACE_ENTRY_KIND_PROGRAM_EVENT: u64 = 7;

impl StyleTrace {
    /// The passes as entries: one per pass, followed by one per element it lists, in the order a style traversal from
    /// `root` reaches them. A full pass lists only the elements whose records it changed, as it styled every one.
    fn entries(&mut self, tree: &Projection, root: StyleNodeID) -> &[u64] {
        self.entries.get_or_insert_with(|| {
            let order: std::collections::HashMap<StyleNodeID, usize> = if tree.is_live(root) {
                tree.style_preorder(root)
                    .enumerate()
                    .map(|(position, node)| (node, position))
                    .collect()
            } else {
                Default::default()
            };
            let mut entries = Vec::new();
            for pass in &self.passes {
                entries.push((pass.kind as u64) << TRACE_ENTRY_KIND_SHIFT);
                for &(event, times) in &pass.program_events {
                    entries.push(
                        times.min(u64::from(u32::MAX))
                            | (TRACE_ENTRY_KIND_PROGRAM_EVENT << TRACE_ENTRY_KIND_SHIFT)
                            | ((event as u64) << TRACE_ENTRY_OUTCOME_SHIFT),
                    );
                }
                let changed: std::collections::HashSet<StyleNodeID> = pass.changed.iter().copied().collect();
                let mut elements: Vec<(StyleNodeID, Option<TracedOutcome>)> = pass
                    .elements
                    .iter()
                    .copied()
                    .filter(|(node, _)| order.contains_key(node))
                    .filter(|(node, _)| pass.kind != TracedPassKind::Full || changed.contains(node))
                    .collect();
                elements.sort_by_key(|(node, _)| order[node]);
                elements.dedup_by_key(|(node, _)| *node);
                for (node, outcome) in elements {
                    let mut entry = u64::from(node.raw()) | (TRACE_ENTRY_KIND_ELEMENT << TRACE_ENTRY_KIND_SHIFT);
                    if let Some(outcome) = outcome {
                        entry |= (outcome as u64) << TRACE_ENTRY_OUTCOME_SHIFT;
                    }
                    if changed.contains(&node) {
                        entry |= TRACE_ENTRY_CHANGED;
                    }
                    entries.push(entry);
                }
            }
            entries
        })
    }
}

impl PassCounts {
    /// Note `value` of `field` of the live report, where it is one of the counters.
    pub(super) fn note(&mut self, field: LiveField, value: u64) {
        let counter = match field {
            LiveField::Passes => StyleCounter::Passes,
            LiveField::FullPasses => StyleCounter::FullPasses,
            LiveField::StyledElements => StyleCounter::StyledElements,
            LiveField::ChangedElements => StyleCounter::ChangedElements,
            LiveField::HintedElements => StyleCounter::HintedElements,
            LiveField::HintedSubtrees => StyleCounter::HintedSubtrees,
            LiveField::InvalidationVisits => StyleCounter::InvalidationVisits,
            LiveField::InvalidationTests => StyleCounter::InvalidationTests,
        };
        self.counts[counter as usize] += value;
    }

    /// Note that the engine updated `index`, indexing `arrived` entries.
    pub(super) fn note_index_update(&mut self, index: &RuleIndex, arrived: usize) {
        self.counts[StyleCounter::IndexedEntries as usize] += arrived as u64;
        self.counts[StyleCounter::CompiledSelectors as usize] += index.compiled_selectors;
        self.counts[StyleCounter::UserAgentIndexBuilds as usize] += u64::from(index.built_user_agent_index);
        self.program_events.push((ProgramEvent::RuleIndexUpdated, 1));
        self.note_index_program(index);
    }

    /// Note that the engine built `index`.
    pub(super) fn note_index_build(&mut self, index: &RuleIndex) {
        self.counts[StyleCounter::IndexBuilds as usize] += 1;
        self.counts[StyleCounter::IndexedEntries as usize] += index.indexed_entries;
        self.counts[StyleCounter::CompiledSelectors as usize] += index.compiled_selectors;
        self.counts[StyleCounter::UserAgentIndexBuilds as usize] += u64::from(index.built_user_agent_index);
        self.program_events.push((ProgramEvent::RuleIndexBuilt, 1));
        self.note_index_program(index);
    }

    fn note_index_program(&mut self, index: &RuleIndex) {
        if index.built_user_agent_index {
            self.program_events.push((ProgramEvent::UserAgentRuleIndexBuilt, 1));
        }
        if index.compiled_selectors > 0 {
            self.program_events
                .push((ProgramEvent::SelectorsCompiled, index.compiled_selectors));
        }
    }

    /// Note a pass, which restyled everything where `full` is, and, where `traces` is, what it styled and `changed`,
    /// the elements whose records, or pseudo-element records, are not the ones they held.
    pub(super) fn note_pass(&mut self, full: bool, statistics: &Statistics, changed: &[StyleNodeID], traces: bool) {
        self.counts[StyleCounter::SharedElements as usize] += statistics.outcomes.shared;
        self.counts[StyleCounter::ReusedElements as usize] += statistics.outcomes.reused;
        self.counts[StyleCounter::MatchedElements as usize] += statistics.outcomes.matched;
        self.counts[StyleCounter::RelativeWalks as usize] += statistics.match_counters.relative_walks;
        if !traces {
            return;
        }
        let outcomes: std::collections::HashMap<StyleNodeID, TracedOutcome> =
            statistics.traced_outcomes.iter().copied().collect();
        let mut elements: Vec<(StyleNodeID, Option<TracedOutcome>)> = statistics
            .styled
            .iter()
            .map(|node| (*node, outcomes.get(node).copied()))
            .collect();
        elements.extend(
            statistics
                .moved_custom_properties
                .iter()
                .map(|node| (*node, Some(TracedOutcome::KeptWithMovedCustomProperties))),
        );
        self.traced.push(TracedPass {
            kind: if full {
                TracedPassKind::Full
            } else {
                TracedPassKind::Partial
            },
            elements,
            changed: changed.to_vec(),
            program_events: Vec::new(),
        });
    }
}

/// How many elements of a pass took the record of another, by how they found it.
#[derive(Clone, Copy, Default)]
pub(super) struct OutcomeCounts {
    shared: u64,
    reused: u64,
    /// The elements matched against the whole rule index.
    matched: u64,
}

impl OutcomeCounts {
    pub(super) fn note(&mut self, outcome: &Outcome) {
        match outcome {
            Outcome::Shared => self.shared += 1,
            Outcome::Reused => self.reused += 1,
            Outcome::Computed | Outcome::CascadeDeclined | Outcome::ComputeDeclined => {}
        }
    }

    /// Note an element matched against the whole rule index.
    pub(super) fn note_matched(&mut self) {
        self.matched += 1;
    }

    pub(super) fn add(&mut self, other: &Self) {
        self.shared += other.shared;
        self.reused += other.reused;
        self.matched += other.matched;
    }
}

/// What the engine did on a document's render state, and the passes it ran while a test traces
/// them.
#[derive(Clone, Default)]
pub(in super::super) struct Counters {
    counts: [u64; StyleCounter::COUNT],
    /// A fork starts without one: it restyles nothing until the host moves on under it.
    trace: crate::fork::ForkReset<Option<Box<StyleTrace>>>,
}

impl Counters {
    /// Take what a pass counted, once it kept the records it computed.
    pub(super) fn take_pass(&mut self, pass: &mut PassCounts) {
        let PassCounts {
            counts,
            traced,
            program_events,
        } = std::mem::take(pass);
        for (total, count) in self.counts.iter_mut().zip(counts) {
            *total += count;
        }
        for (event, times) in program_events {
            self.note_program_event(event, times);
        }
        if let Some(trace) = &mut *self.trace {
            trace.passes.extend(traced);
            trace.entries = None;
        }
    }

    /// Note, for a trace, that the engine did `event` to its rules `times` times.
    pub(in super::super) fn note_program_event(&mut self, event: ProgramEvent, times: u64) {
        let Some(trace) = &mut *self.trace else {
            return;
        };
        trace.entries = None;
        if let Some(pass) = trace.passes.last_mut()
            && pass.kind == TracedPassKind::Program
        {
            match pass.program_events.last_mut() {
                Some((last, total)) if *last == event => *total += times,
                _ => pass.program_events.push((event, times)),
            }
            return;
        }
        trace.passes.push(TracedPass {
            kind: TracedPassKind::Program,
            elements: Vec::new(),
            changed: Vec::new(),
            program_events: vec![(event, times)],
        });
    }

    /// Note that the engine started from a cold pass, with `index`, the rule index it built.
    pub(super) fn note_live_start(&mut self, index: &RuleIndex) {
        if let Some(trace) = &mut *self.trace {
            trace.passes.push(TracedPass {
                kind: TracedPassKind::Cold,
                elements: Vec::new(),
                changed: Vec::new(),
                program_events: Vec::new(),
            });
            trace.entries = None;
        }
        self.note_program_event(ProgramEvent::RuleIndexBuilt, 1);
        if index.built_user_agent_index {
            self.note_program_event(ProgramEvent::UserAgentRuleIndexBuilt, 1);
        }
        if index.compiled_selectors > 0 {
            self.note_program_event(ProgramEvent::SelectorsCompiled, index.compiled_selectors);
        }
        self.counts[StyleCounter::ColdPasses as usize] += 1;
        self.counts[StyleCounter::IndexBuilds as usize] += 1;
        self.counts[StyleCounter::IndexedEntries as usize] += index.indexed_entries;
        self.counts[StyleCounter::CompiledSelectors as usize] += index.compiled_selectors;
        self.counts[StyleCounter::UserAgentIndexBuilds as usize] += u64::from(index.built_user_agent_index);
    }

    /// Note that a cold pass built `index`, which counts the selectors it compiled.
    pub(super) fn note_cold_pass_index(&mut self, index: &RuleIndex) {
        self.counts[StyleCounter::CompiledSelectors as usize] += index.compiled_selectors;
    }

    /// The counter of index `index` of [`StyleCounter`], or zero for none.
    #[cfg(test)]
    pub(in super::super) fn get(&self, index: u32) -> u64 {
        self.counts.get(index as usize).copied().unwrap_or(0)
    }

    /// Start tracing the passes of the engine anew.
    pub(in super::super) fn begin_trace(&mut self) {
        *self.trace = Some(Default::default());
    }

    /// Stop tracing the passes of the engine, forgetting the ones traced.
    pub(in super::super) fn end_trace(&mut self) {
        *self.trace = None;
    }

    pub(in super::super) fn is_tracing(&self) -> bool {
        self.trace.is_some()
    }

    /// The elements the traced passes styled, without the ones that only took moved custom properties, in the order a
    /// style traversal from `root` reaches them, or `None` where one of the passes restyled every element.
    #[cfg(test)]
    pub(in super::super) fn elements_styled_by_traced_partial_passes(
        &self,
        tree: &Projection,
        root: StyleNodeID,
    ) -> Option<Vec<StyleNodeID>> {
        let passes = &self.trace.as_ref()?.passes;
        if passes
            .iter()
            .any(|pass| matches!(pass.kind, TracedPassKind::Full | TracedPassKind::Cold))
        {
            return None;
        }
        let styled: std::collections::HashSet<StyleNodeID> = passes
            .iter()
            .filter(|pass| pass.kind == TracedPassKind::Partial)
            .flat_map(|pass| &pass.elements)
            .filter(|(_, outcome)| *outcome != Some(TracedOutcome::KeptWithMovedCustomProperties))
            .map(|(node, _)| *node)
            .collect();
        Some(tree.style_preorder(root).filter(|node| styled.contains(node)).collect())
    }

    /// The traced passes as the host reads them: one entry per pass, its kind in bits 32 to 39, followed by one entry
    /// per element it lists, in the order a style traversal from `root` reaches them, with the element's node in bits 0
    /// to 31, the kind of an element entry, its outcome in bits 40 to 47, and bit 48 set where its record changed.
    pub(in super::super) fn trace_entries(&mut self, tree: &Projection, root: StyleNodeID) -> &[u64] {
        match &mut *self.trace {
            Some(trace) => trace.entries(tree, root),
            None => &[],
        }
    }

    /// Note that the host composed an animation sample over the record `node` installed,
    /// rebuilding every group of it where `rebuilt_every_group` is.
    pub(in super::super) fn note_animated_sample(&mut self, node: StyleNodeID, rebuilt_every_group: bool) {
        if let Some(trace) = &mut *self.trace {
            let outcome = match rebuilt_every_group {
                true => TracedOutcome::SampledEveryGroup,
                false => TracedOutcome::Sampled,
            };
            match trace.passes.last_mut() {
                Some(pass) if pass.kind == TracedPassKind::Animation => pass.elements.push((node, Some(outcome))),
                _ => trace.passes.push(TracedPass {
                    kind: TracedPassKind::Animation,
                    elements: vec![(node, Some(outcome))],
                    changed: Vec::new(),
                    program_events: Vec::new(),
                }),
            }
            trace.entries = None;
        }
    }
}
