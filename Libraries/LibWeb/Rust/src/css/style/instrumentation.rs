/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Counters for StyleEngine's own decisions.
//!
//! Each decision is only defensible if it is visible, so each one bumps a counter.

use std::sync::atomic::{AtomicU64, Ordering};

macro_rules! define_counters {
    ($($variant:ident => $name:literal,)+) => {
        /// One countable StyleEngine event.
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        #[repr(usize)]
        pub enum Counter {
            $($variant,)+
        }

        pub const COUNTER_COUNT: usize = 0 $(+ { let _ = Counter::$variant; 1 })+;

        static COUNTER_NAMES: [&str; COUNTER_COUNT] = [$($name,)+];

        impl Counter {
            #[must_use]
            #[cfg(test)]
            pub fn name(self) -> &'static str {
                COUNTER_NAMES[self as usize]
            }
        }
    };
}

define_counters! {
    // Typed input deltas, counted by kind. A generic version bump would hide exactly the
    // distinction that decides how much work a change causes.
    TreeDeltas => "treeDeltas",
    TreeDepartureDeltas => "treeDepartureDeltas",
    StyleNodesRetired => "styleNodesRetired",
    LocalFeatureDeltas => "localFeatureDeltas",
    StateDeltas => "stateDeltas",
    ElementDeclarationDeltas => "elementDeclarationDeltas",
    ProgramDeltas => "programDeltas",
    CascadeTopologyDeltas => "cascadeTopologyDeltas",
    EnvironmentDeltas => "environmentDeltas",
    // Raw input records against the unique keys transactions take. Raw mutation count is not the
    // default work multiplier, and the gap between these two numbers is the evidence for that.
    RawMutationRecords => "rawMutationRecords",
    NormalizedUniqueKeys => "normalizedUniqueKeys",
    AtomSweeps => "atomSweeps",
    AtomSweepRootSlotsVisited => "atomSweepRootSlotsVisited",
    StyleAtomsReclaimed => "styleAtomsReclaimed",
    LanguageTextsPublished => "languageTextsPublished",
    CustomPropertyNamesPublished => "customPropertyNamesPublished",
    SlotAssignedNodeListsPublished => "slotAssignedNodeListsPublished",

    // Exclusive transaction intervals. Fused stages retain one name until execution splits.
    TransactionMicroseconds => "transactionMicroseconds",
    CommitMicroseconds => "commitMicroseconds",
    TransactionRemainderMicroseconds => "transactionRemainderMicroseconds",

    // Physical work, including drives whose output is later abandoned.
    FontRefillRounds => "fontRefillRounds",
    FontResolutionRequests => "fontResolutionRequests",
    FontRefillResumedDrives => "fontRefillResumedDrives",
    FontRefillPreservedLonghands => "fontRefillPreservedLonghands",
    FontRefillBlockedDepth => "fontRefillBlockedDepth",
    EngineFullDrivesStarted => "engineFullDrivesStarted",
    EnginePhysicalLonghandEvaluations => "enginePhysicalLonghandEvaluations",

    // Stylesheet program.
    StyleRulesCompiled => "styleRulesCompiled",

    // Computation and publication.
    SpecifiedValuesReused => "specifiedValuesReused",
    // The user agent declaration blocks the document's rules declare: those the process interned
    // for this document, which no document of the process interned before, and those it shared.
    // The declaration blocks of the document's own sheets its rules declare, which the process
    // interns as it does the user agent's: those it interned for this document, and those it
    // shared with a document reaching the same blocks before.
    SheetDeclarationsInterned => "sheetDeclarationsInterned",
    SheetDeclarationsShared => "sheetDeclarationsShared",
    StyleRecordNodeHandlesPublished => "styleRecordNodeHandlesPublished",
    AnimationOverlaySlotsAllocated => "animationOverlaySlotsAllocated",
    AnimationOverlaySlotsReleased => "animationOverlaySlotsReleased",
    AnimationOverlayRecordsUpdated => "animationOverlayRecordsUpdated",
    LiveAnimationOverlayRecords => "liveAnimationOverlayRecords",
    // Read when counters are read: the reclaimed overlay records a published frame's lease still keeps.
    RetiredAnimationOverlayRecords => "retiredAnimationOverlayRecords",
    ComputedPseudoAssignmentsRemoved => "computedPseudoAssignmentsRemoved",

    // Input intake.
    ArrivingNodeFactsFolded => "arrivingNodeFactsFolded",

    // Style node identity lifecycle.
    StyleNodesAllocated => "styleNodesAllocated",
}

/// The counter set for one document.
///
/// NB: Counters are bumped through shared references from wherever the engine runs, so each is an atomic. Only one
///     thread runs the engine at a time, so a relaxed load and store is enough, and costs what a plain add does.
#[derive(Debug)]
pub struct Counters {
    values: [AtomicU64; COUNTER_COUNT],
}

impl Default for Counters {
    fn default() -> Self {
        Self::new()
    }
}

impl Clone for Counters {
    fn clone(&self) -> Self {
        let clone = Self::new();
        clone.restore(self);
        clone
    }
}

impl Counters {
    #[must_use]
    pub fn new() -> Self {
        Self {
            values: [const { AtomicU64::new(0) }; COUNTER_COUNT],
        }
    }

    pub fn bump(&self, counter: Counter) {
        self.add(counter, 1);
    }

    pub fn add(&self, counter: Counter, amount: u64) {
        let value = &self.values[counter as usize];
        value.store(value.load(Ordering::Relaxed) + amount, Ordering::Relaxed);
    }

    pub fn set(&self, counter: Counter, value: u64) {
        self.values[counter as usize].store(value, Ordering::Relaxed);
    }

    #[must_use]
    pub fn get(&self, counter: Counter) -> u64 {
        self.values[counter as usize].load(Ordering::Relaxed)
    }

    /// Puts every counter back to what `other` holds.
    pub fn restore(&self, other: &Self) {
        for (value, other) in self.values.iter().zip(&other.values) {
            value.store(other.load(Ordering::Relaxed), Ordering::Relaxed);
        }
    }

    pub fn iter(&self) -> impl Iterator<Item = (&'static str, u64)> + '_ {
        COUNTER_NAMES
            .iter()
            .copied()
            .zip(self.values.iter().map(|value| value.load(Ordering::Relaxed)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn peaks_and_totals_accumulate_differently() {
        let counters = Counters::new();
        counters.add(Counter::RawMutationRecords, 3);
        counters.add(Counter::RawMutationRecords, 4);
        assert_eq!(counters.get(Counter::RawMutationRecords), 7);
    }

    #[test]
    fn every_counter_is_reportable_by_name() {
        let counters = Counters::new();
        let names: Vec<&'static str> = counters.iter().map(|(name, _)| name).collect();
        assert_eq!(names.len(), COUNTER_COUNT);
        assert_eq!(names[0], Counter::TreeDeltas.name());
        let mut sorted = names.clone();
        sorted.sort_unstable();
        sorted.dedup();
        assert_eq!(sorted.len(), names.len(), "counter names must be unique");
    }
}
