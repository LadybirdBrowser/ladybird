/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Counters for StyleEngine's own decisions.
//!
//! The engine chooses between selective and batch plans, between resident state and cold
//! evaluation, and between narrow and widened impact regions. Those choices are only defensible if
//! they are visible, so each one bumps a counter.
//!
//! Counters are per document and read from one thread, so they are plain integers rather than
//! atomics.

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
    LocalFeatureDeltas => "localFeatureDeltas",
    StateDeltas => "stateDeltas",
    ElementDeclarationDeltas => "elementDeclarationDeltas",
    ProgramDeltas => "programDeltas",
    CascadeTopologyDeltas => "cascadeTopologyDeltas",
    EnvironmentDeltas => "environmentDeltas",
    InitialBulkLoads => "initialBulkLoads",
    InitialBulkTreeRows => "initialBulkTreeRows",
    InitialBulkMatchLoads => "initialBulkMatchLoads",
    InitialBulkMatchRows => "initialBulkMatchRows",
    // Normalization journal. Raw mutation count is not the default work multiplier, and the gap
    // between these two numbers is the evidence for that.
    RawMutationRecords => "rawMutationRecords",
    NormalizedUniqueKeys => "normalizedUniqueKeys",
    JournalCancellations => "journalCancellations",
    CoarsenedScopeMarkers => "coarsenedScopeMarkers",
    AtomSweeps => "atomSweeps",
    AtomSweepsDeferredForActiveTraversal => "atomSweepsDeferredForActiveTraversal",
    AtomSweepRootSlotsVisited => "atomSweepRootSlotsVisited",
    AtomSweepPinReleasesSkipped => "atomSweepPinReleasesSkipped",
    StyleAtomsReclaimed => "styleAtomsReclaimed",
    LanguageTextsPublished => "languageTextsPublished",

    // Stylesheet program.
    StyleRulesCompiled => "styleRulesCompiled",
    ExactSelectorEntries => "exactSelectorEntries",
    // Why an entry had to be conservative, so the remaining gap is a list rather than a feeling.
    UnsupportedPseudoClass => "unsupportedPseudoClass",
    UnsupportedUninternedName => "unsupportedUninternedName",
    UnsupportedMalformed => "unsupportedMalformed",
    UnsupportedCombinator => "unsupportedCombinator",
    UnsupportedPseudoElement => "unsupportedPseudoElement",

    // Executed selector primitives, by category.
    LocalFeatureTests => "localFeatureTests",
    StateTests => "stateTests",
    CombinatorSteps => "combinatorSteps",
    StructuralTests => "structuralTests",
    RelationalTests => "relationalTests",

    // DOM selector queries. Candidate rows are the elements admitted by the query plan, while
    // evaluations are the rows that reached the exact matcher after selector-list deduplication.
    SelectorQueryCandidateRows => "selectorQueryCandidateRows",
    SelectorQueryEvaluations => "selectorQueryEvaluations",
    SelectorQueryAttributeValuePlanHits => "selectorQueryAttributeValuePlanHits",
    SelectorQueryAttributeValueCatalogScans => "selectorQueryAttributeValueCatalogScans",

    // Candidate enumeration and cold evaluation.
    ColdMatchingBatchMissingRows => "coldMatchingBatchMissingRows",
    ColdMatchingBatchRows => "coldMatchingBatchRows",
    PreparedMatchingBatchCompletenessRowsInspected => "preparedMatchingBatchCompletenessRowsInspected",
    PreparedMatchingBatchRowsCloned => "preparedMatchingBatchRowsCloned",
    ColdNodesEvaluated => "coldNodesEvaluated",
    CandidateChecks => "candidateChecks",
    RuleMatchesEmitted => "ruleMatchesEmitted",
    PrefixCompoundsEvaluated => "prefixCompoundsEvaluated",
    PrefixConvergencePasses => "prefixConvergencePasses",
    PrefixConvergenceNodes => "prefixConvergenceNodes",
    PrefixConvergenceStops => "prefixConvergenceStops",
    PrefixConvergenceBypasses => "prefixConvergenceBypasses",
    PrefixConvergenceUpqueries => "prefixConvergenceUpqueries",
    PrefixDeadDeltaTransitions => "prefixDeadDeltaTransitions",
    EngineComputedRecordDeltas => "engineComputedRecordDeltas",
    EngineComputedRecordCohortHits => "engineComputedRecordCohortHits",
    EngineComputedRecordSharedHits => "engineComputedRecordSharedHits",
    EngineComputedRecordGateReaction => "engineComputedRecordGateReaction",
    EngineComputedRecordGateDeclarations => "engineComputedRecordGateDeclarations",
    EngineComputedRecordGateIncompleteAnswer => "engineComputedRecordGateIncompleteAnswer",
    EngineComputedPseudoRecords => "engineComputedPseudoRecords",
    EngineComputedRecordUnchangedWinners => "engineComputedRecordUnchangedWinners",
    EngineComputedRecordsAbandoned => "engineComputedRecordsAbandoned",
    EngineComputedRecordBailPseudoStale => "engineComputedRecordBailPseudoStale",
    EngineComputedRecordBailPseudoBackdrop => "engineComputedRecordBailPseudoBackdrop",
    EngineComputedRecordBailPseudoMask => "engineComputedRecordBailPseudoMask",
    EngineComputedRecordBailPseudoRow => "engineComputedRecordBailPseudoRow",
    EngineComputedRecordBailPseudoFlip => "engineComputedRecordBailPseudoFlip",
    EngineComputedRecordGateAncestors => "engineComputedRecordGateAncestors",
    EngineComputedLonghandEvaluations => "engineComputedLonghandEvaluations",
    EngineComputedRecordBailNoCascadeState => "engineComputedRecordBailNoCascadeState",
    EngineComputedRecordBailStaleCascadeState => "engineComputedRecordBailStaleCascadeState",
    EngineComputedRecordBailWinnerMissingNode => "engineComputedRecordBailWinnerMissingNode",
    EngineComputedRecordBailWinnerStaleProgram => "engineComputedRecordBailWinnerStaleProgram",
    EngineComputedRecordBailWinnerStalePriority => "engineComputedRecordBailWinnerStalePriority",
    EngineComputedRecordBailUnchangedWinners => "engineComputedRecordBailUnchangedWinners",
    EngineComputedRecordBailIncompleteWinners => "engineComputedRecordBailIncompleteWinners",
    EngineComputedRecordBailRecord => "engineComputedRecordBailRecord",
    EngineComputedRecordBailProperty => "engineComputedRecordBailProperty",
    EngineComputedRecordBailWinner => "engineComputedRecordBailWinner",
    EngineComputedRecordBailWinnerOperator => "engineComputedRecordBailWinnerOperator",
    EngineComputedRecordBailWinnerSpelling => "engineComputedRecordBailWinnerSpelling",
    EngineComputedRecordBailWinnerElement => "engineComputedRecordBailWinnerElement",
    EngineComputedRecordBailWinnerAnimated => "engineComputedRecordBailWinnerAnimated",
    EngineComputedRecordBailNoEnvironment => "engineComputedRecordBailNoEnvironment",
    EngineComputedRecordBailFontPhase => "engineComputedRecordBailFontPhase",
    EngineComputedRecordBailRecordParent => "engineComputedRecordBailRecordParent",
    EngineComputedRecordBailDrive => "engineComputedRecordBailDrive",
    EngineComputedRecordBailValue => "engineComputedRecordBailValue",
    EngineComputedRecordBailAssemble => "engineComputedRecordBailAssemble",
    EngineComputedRecordBailRecordOverlay => "engineComputedRecordBailRecordOverlay",
    EngineComputedRecordBailRecordTable => "engineComputedRecordBailRecordTable",
    PrefixDeadDeltaBailMatched => "prefixDeadDeltaBailMatched",
    PrefixDeadDeltaBailOwnAdditions => "prefixDeadDeltaBailOwnAdditions",
    PrefixDeadDeltaBailEndpoints => "prefixDeadDeltaBailEndpoints",
    PendingSelectorRoutes => "pendingSelectorRoutes",
    PrefixEligibleRoutes => "prefixEligibleRoutes",
    GroupedExactSelectorRoutes => "groupedExactSelectorRoutes",
    PrefixTransitionCacheHits => "prefixTransitionCacheHits",
    PrefixTransitionCacheMatchHits => "prefixTransitionCacheMatchHits",
    PrefixTransitionCacheMatchMisses => "prefixTransitionCacheMatchMisses",
    PrefixTransitionMemoHits => "prefixTransitionMemoHits",
    PrefixTransitionMemoMisses => "prefixTransitionMemoMisses",
    PrefixLocalFactIdentityHits => "prefixLocalFactIdentityHits",
    PrefixLocalFactIdentityMisses => "prefixLocalFactIdentityMisses",
    PrefixAnswerCacheHits => "prefixAnswerCacheHits",
    PrefixAnswerCacheMisses => "prefixAnswerCacheMisses",
    Tier3BenefitEvictions => "tier3BenefitEvictions",
    Tier3RefusalRetainedMatchAnswerBytes => "tier3RefusalRetainedMatchAnswerBytes",
    MatchAnswerSignatures => "matchAnswerSignatures",
    MatchAnswerSignatureReuses => "matchAnswerSignatureReuses",
    SelectorTruthSetMisses => "selectorTruthSetMisses",
    SelectorTruthSetHits => "selectorTruthSetHits",
    SelectorTruthSetRows => "selectorTruthSetRows",
    SelectorTruthDerivedAnswerMisses => "selectorTruthDerivedAnswerMisses",
    SelectorTruthDerivedAnswerHits => "selectorTruthDerivedAnswerHits",
    RetainedMatchAnswerReuses => "retainedMatchAnswerReuses",
    RetainedSelectorIncidenceBatchPrograms => "retainedSelectorIncidenceBatchPrograms",
    RetainedSelectorIncidenceBatchRows => "retainedSelectorIncidenceBatchRows",
    RetainedSelectorIncidenceBatchMissingRows => "retainedSelectorIncidenceBatchMissingRows",
    PublishedMatchAnswerConsumptions => "publishedMatchAnswerConsumptions",
    PublishedMatchAnswerIdentityReads => "publishedMatchAnswerIdentityReads",
    PublishedMatchAnswerRecords => "publishedMatchAnswerRecords",
    PublishedMatchAnswerSharedPayloads => "publishedMatchAnswerSharedPayloads",
    PublishedMatchAnswerSharedPayloadReuses => "publishedMatchAnswerSharedPayloadReuses",
    PublishedMatchAnswerContextualPayloads => "publishedMatchAnswerContextualPayloads",
    PublishedMatchAnswerIdentityRepairs => "publishedMatchAnswerIdentityRepairs",
    PublishedMatchAnswerIdentityRepairStops => "publishedMatchAnswerIdentityRepairStops",
    PublishedMatchAnswerRetainedIdentityComparisons => "publishedMatchAnswerRetainedIdentityComparisons",
    PublishedMatchAnswerRetainedIdentityMatches => "publishedMatchAnswerRetainedIdentityMatches",
    PublishedExactCascadeStops => "publishedExactCascadeStops",
    PublishedMatchAnswerClosureCompletions => "publishedMatchAnswerClosureCompletions",
    PublishedClosureRetainedIdentityStops => "publishedClosureRetainedIdentityStops",
    MatchElementCallsDuringPublishedStyleTransaction => "matchElementCallsDuringPublishedStyleTransaction",
    RetainedMatchAnswerRefusals => "retainedMatchAnswerRefusals",
    RetainedMatchAnswerPatches => "retainedMatchAnswerPatches",
    RetainedMatchAnswerDeltaPatches => "retainedMatchAnswerDeltaPatches",
    RetainedMatchAnswerDeltaMemoHits => "retainedMatchAnswerDeltaMemoHits",
    RetainedMatchAnswerDeltaEntries => "retainedMatchAnswerDeltaEntries",
    RetainedMatchAnswerFilteredPatches => "retainedMatchAnswerFilteredPatches",
    RetainedPatchesUnattributed => "retainedPatchesUnattributed",
    RetainedPatchesPoisoned => "retainedPatchesPoisoned",
    RetainedPatchesCoarseCovered => "retainedPatchesCoarseCovered",
    RetainedMatchAnswerPatchMisses => "retainedMatchAnswerPatchMisses",
    RetainedMatchAnswerPatchStops => "retainedMatchAnswerPatchStops",
    // Consolidated semantic reactions and typed exact repair asks. These count the rows which
    // cross operator boundaries, not the routes or evaluator work which produced them.
    SelectorTruthAdditions => "selectorTruthAdditions",
    SelectorTruthRemovals => "selectorTruthRemovals",
    SelectorTruthCancellations => "selectorTruthCancellations",
    SelectorTruthRefreshes => "selectorTruthRefreshes",
    SelectorTruthRepairUpqueries => "selectorTruthRepairUpqueries",
    SelectorTruthRepairAdditions => "selectorTruthRepairAdditions",
    SelectorTruthRepairRemovals => "selectorTruthRepairRemovals",
    MatchAnswerChanges => "matchAnswerChanges",
    MatchAnswerUpqueries => "matchAnswerUpqueries",
    CascadeMatchesBeforeCompaction => "cascadeMatchesBeforeCompaction",
    CascadeNodeHandlesPublished => "cascadeNodeHandlesPublished",
    CascadeStatesInterned => "cascadeStatesInterned",
    CascadeWinnerGroupsInterned => "cascadeWinnerGroupsInterned",
    CascadeWinnerEntriesInterned => "cascadeWinnerEntriesInterned",
    CascadeWinnerDeltaProperties => "cascadeWinnerDeltaProperties",
    CascadeWinnerDeltaStops => "cascadeWinnerDeltaStops",
    ComputedWinnerDeltaPropertiesConsumed => "computedWinnerDeltaPropertiesConsumed",
    ComputedOutputGroupsCanonicalized => "computedOutputGroupsCanonicalized",
    ComputedWinnerPropagationStops => "computedWinnerPropagationStops",
    SpecifiedValuesReused => "specifiedValuesReused",
    ComputedGroupNodeHandlesPublished => "computedGroupNodeHandlesPublished",
    ComputedGroupsReused => "computedGroupsReused",
    ComputedGroupsRetained => "computedGroupsRetained",
    ComputedGroupsReachable => "computedGroupsReachable",
    ComputedGroupSetsReused => "computedGroupSetsReused",
    InheritedGroupNodeHandlesPublished => "inheritedGroupNodeHandlesPublished",
    InheritedGroupSetsReused => "inheritedGroupSetsReused",
    CustomPropertyEnvironmentNodeHandlesPublished => "customPropertyEnvironmentNodeHandlesPublished",
    CustomPropertyEnvironmentsReused => "customPropertyEnvironmentsReused",
    ComputedFixedMetadataNodeHandlesPublished => "computedFixedMetadataNodeHandlesPublished",
    ComputedFixedMetadataInterned => "computedFixedMetadataInterned",
    ComputedFixedMetadataReused => "computedFixedMetadataReused",
    StyleRecordNodeHandlesPublished => "styleRecordNodeHandlesPublished",
    StyleRecordsInterned => "styleRecordsInterned",
    StyleRecordsReused => "styleRecordsReused",
    AnimationOverlaySlotsAllocated => "animationOverlaySlotsAllocated",
    AnimationOverlaySlotsReleased => "animationOverlaySlotsReleased",
    AnimationOverlayRecordsUpdated => "animationOverlayRecordsUpdated",
    LiveAnimationOverlayRecords => "liveAnimationOverlayRecords",
    ComputedPseudoAssignmentsPublished => "computedPseudoAssignmentsPublished",
    ComputedPseudoAssignmentsRemoved => "computedPseudoAssignmentsRemoved",
    ProgramCandidatesRejectedByCascade => "programCandidatesRejectedByCascade",
    SelectorRoutesRejectedByCascade => "selectorRoutesRejectedByCascade",
    CascadeCandidatesRejectedByWinner => "cascadeCandidatesRejectedByWinner",

    // Impact regions and plan selection.
    RemainingPostingBuilds => "remainingPostingBuilds",
    RemainingPostingReuses => "remainingPostingReuses",
    RemainingPostingRowsCopied => "remainingPostingRowsCopied",
    RemainingPostingRowsInspected => "remainingPostingRowsInspected",
    // How much logical membership the compiled exact-region representation covers, and how much
    // interval storage it needs to do so.
    ExactRegionBatchIntervals => "exactRegionBatchIntervals",
    ExactRegionBatchNodes => "exactRegionBatchNodes",
    RoutedEntryPoints => "routedEntryPoints",
    OriginTruthRoutesFired => "originTruthRoutesFired",
    OriginTruthRoutesSkipped => "originTruthRoutesSkipped",
    ArrivingNodeFactsFolded => "arrivingNodeFactsFolded",
    SheetChangeCandidatesRejected => "sheetChangeCandidatesRejected",
    RelationalAnchorsConsidered => "relationalAnchorsConsidered",
    RelationalAnchorsSkippedByWitness => "relationalAnchorsSkippedByWitness",
    InvalidatedStyleNodes => "invalidatedStyleNodes",
    PlannedNodesWithDirectAction => "plannedNodesWithDirectAction",
    PlannedNodesWithSignedDelta => "plannedNodesWithSignedDelta",
    PlannedNodesWithOutputChange => "plannedNodesWithOutputChange",
    PlannedNodesWithUpquery => "plannedNodesWithUpquery",
    PlannedNodesUnattributed => "plannedNodesUnattributed",

    // Style node identity lifecycle.
    StyleNodesAllocated => "styleNodesAllocated",
    DocumentWidenings => "documentWidenings",
    TransitionProofConfirmed => "transitionProofConfirmed",
    TransitionProofNoPreviousState => "transitionProofNoPreviousState",
    TransitionProofGenerationGap => "transitionProofGenerationGap",
    TransitionProofMissingAnswer => "transitionProofMissingAnswer",
    TransitionProofElementDeclarations => "transitionProofElementDeclarations",
    TransitionProofRemoval => "transitionProofRemoval",
    TransitionProofUnsafeRule => "transitionProofUnsafeRule",
    TransitionProofPseudoOrScope => "transitionProofPseudoOrScope",
    TransitionProofElementWinnerGap => "transitionProofElementWinnerGap",
    TransitionProofOperatorOrContinuation => "transitionProofOperatorOrContinuation",
    TransitionProofWinnerGap => "transitionProofWinnerGap",
    TransitionProofPriorityWin => "transitionProofPriorityWin",
}

/// The counter set for one document.
#[derive(Clone, Debug)]
pub struct Counters {
    values: [u64; COUNTER_COUNT],
}

impl Default for Counters {
    fn default() -> Self {
        Self::new()
    }
}

impl Counters {
    #[must_use]
    pub fn new() -> Self {
        Self {
            values: [0; COUNTER_COUNT],
        }
    }

    pub fn bump(&mut self, counter: Counter) {
        self.values[counter as usize] += 1;
    }

    pub fn add(&mut self, counter: Counter, amount: u64) {
        self.values[counter as usize] += amount;
    }

    pub fn set(&mut self, counter: Counter, value: u64) {
        self.values[counter as usize] = value;
    }

    #[must_use]
    pub fn get(&self, counter: Counter) -> u64 {
        self.values[counter as usize]
    }

    pub fn iter(&self) -> impl Iterator<Item = (&'static str, u64)> {
        COUNTER_NAMES.iter().copied().zip(self.values)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn peaks_and_totals_accumulate_differently() {
        let mut counters = Counters::new();
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
