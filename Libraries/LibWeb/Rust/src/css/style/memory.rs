/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Tiered memory accounting for StyleEngine.
//!
//! Every byte StyleEngine retains belongs to exactly one [`MemoryCategory`], and every category
//! belongs to one [`Tier`]. The tier decides both the reclamation rule and the budget the bytes are
//! charged against:
//!
//! * Tier 0 is authoritative input owned by another subsystem. It is referenced, never charged.
//! * Tier 1 is the minimal live state required to answer current observers. It is tracked by
//!   category but not capped or aggregated, because refusing it would mean refusing a style read.
//! * Tier 2 is the shared semantic IR: selector programs, transpose bytecode, and the routing
//!   registry. It can never absorb selector-result state.
//! * Tier 3 is pure acceleration. It is strictly budgeted and fully evictable, and eviction changes
//!   no semantic version - a later observer reconstructs from authoritative inputs.
//! * Tier 4 is transaction scratch, capped at a peak rather than accumulated across transactions.
//!
//! Tier 3 and Tier 4 reserve their full capacity charge *before* allocating or growing. Allocating
//! over budget and evicting afterwards is forbidden, so a refused reservation means the requesting
//! view is not built and evaluation continues cold.

use std::cell::Cell;
use std::rc::Rc;

const KIB: u64 = 1024;
const MIB: u64 = 1024 * KIB;

/// Retained-state tier. The discriminants follow the tier numbers in the memory model.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
#[repr(usize)]
pub enum Tier {
    /// Tier 0: DOM, CSSOM, stylesheet programs, browser state.
    Authoritative,
    /// Tier 1: compact handles and shared payloads required to answer current observers.
    Live,
    /// Tier 2: canonical selector, declaration, condition, and cascade nodes; routing registry.
    Program,
    /// Tier 3: indexes, match sets, inverse maps, witnesses, proofs, flattened environments.
    Acceleration,
    /// Tier 4: reusable arenas for relation ranges, delta queues, batch evaluation, comparison.
    Scratch,
}

pub const TIER_COUNT: usize = 5;

impl Tier {
    #[must_use]
    pub fn index(self) -> usize {
        self as usize
    }
}

macro_rules! define_memory_categories {
    ($($variant:ident => ($tier:ident, $name:literal),)+) => {
        /// One kind of retained StyleEngine state. Categories exist so that a byte total can be
        /// attributed to the logical operator that retained it, not just to a tier.
        #[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
        #[repr(usize)]
        pub enum MemoryCategory {
            $($variant,)+
        }

        pub const MEMORY_CATEGORY_COUNT: usize = 0 $(+ { let _ = MemoryCategory::$variant; 1 })+;

        pub const MEMORY_CATEGORIES: [MemoryCategory; MEMORY_CATEGORY_COUNT] = [$(MemoryCategory::$variant,)+];
        static MEMORY_CATEGORY_TIERS: [Tier; MEMORY_CATEGORY_COUNT] = [$(Tier::$tier,)+];
        static MEMORY_CATEGORY_NAMES: [&str; MEMORY_CATEGORY_COUNT] = [$($name,)+];

        impl MemoryCategory {
            #[must_use]
            pub fn tier(self) -> Tier {
                MEMORY_CATEGORY_TIERS[self as usize]
            }

            #[must_use]
            pub fn name(self) -> &'static str {
                MEMORY_CATEGORY_NAMES[self as usize]
            }
        }
    };
}

define_memory_categories! {
    // Tier 1: reclaimed only once no observer and no live read epoch can reach the state.
    StyleNodeMapping => (Live, "styleNodeMapping"),
    RelationColumns => (Live, "relationColumns"),
    MatchAnswerIdentity => (Live, "matchAnswerIdentity"),
    ComputedGroupSet => (Live, "computedGroupSet"),
    CustomPropertyEnvironment => (Live, "customPropertyEnvironment"),
    ComputedFixedMetadata => (Live, "computedFixedMetadata"),
    ComputedReconstructionMetadata => (Live, "computedReconstructionMetadata"),
    StyleRecord => (Live, "styleRecord"),
    AnimationOverlayRecord => (Live, "animationOverlayRecord"),
    ComputedPseudoAssignment => (Live, "computedPseudoAssignment"),
    // Tier 2: reclaimed on semantic detachment plus epoch retirement.
    RuleProgram => (Program, "ruleProgram"),
    RoutingRegistry => (Program, "routingRegistry"),

    // Tier 3: evictable without semantic effect, in roughly this preference order.
    RetainedWitness => (Acceleration, "retainedWitness"),
    FeaturePosting => (Acceleration, "featurePosting"),
    SpecifiedValueTable => (Acceleration, "specifiedValueTable"),
    CascadeWinnerGroup => (Acceleration, "cascadeWinnerGroup"),
    RetainedSelectorIncidence => (Acceleration, "retainedSelectorIncidence"),
    RetainedMatchAnswer => (Acceleration, "retainedMatchAnswer"),
    PrefixTransitionCache => (Acceleration, "prefixTransitionCache"),
    PrefixAnswerCache => (Acceleration, "prefixAnswerCache"),
    // Tier 4: released at transaction end or scratch shrink.
    NormalizationJournal => (Scratch, "normalizationJournal"),
    BatchScratch => (Scratch, "batchScratch"),
    BridgeBuffer => (Scratch, "bridgeBuffer"),
    // Appended to preserve record-replay category ordinals.
    ParsedSubstitutionCache => (Acceleration, "parsedSubstitutionCache"),
    SelectorQuery => (Scratch, "selectorQuery"),
}

impl MemoryCategory {
    /// Whether the Rust owner can immediately release this category when the controller selects it.
    /// Externally owned acceleration, such as the parsed-substitution cache, instead evicts itself
    /// when its next capacity settlement is refused.
    #[must_use]
    pub(super) fn is_controller_evictable(self) -> bool {
        matches!(
            self,
            Self::RetainedWitness
                | Self::FeaturePosting
                | Self::SpecifiedValueTable
                | Self::CascadeWinnerGroup
                | Self::RetainedSelectorIncidence
                | Self::RetainedMatchAnswer
                | Self::PrefixTransitionCache
                | Self::PrefixAnswerCache
        )
    }
}

/// Acceleration categories whose reservation refusals are exposed in memory-pressure reports.
pub const TIER3_REFUSAL_CATEGORIES: [MemoryCategory; 9] = [
    MemoryCategory::RetainedWitness,
    MemoryCategory::FeaturePosting,
    MemoryCategory::SpecifiedValueTable,
    MemoryCategory::CascadeWinnerGroup,
    MemoryCategory::RetainedSelectorIncidence,
    MemoryCategory::RetainedMatchAnswer,
    MemoryCategory::PrefixTransitionCache,
    MemoryCategory::PrefixAnswerCache,
    MemoryCategory::ParsedSubstitutionCache,
];

/// The sole document memory class exposed by the browser.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeviceClass {
    ForegroundDesktop,
}

const DEVICE_CAP: u64 = 64 * MIB;
const BASE_ALLOWANCE: u64 = MIB;
const PER_CONNECTED_NODE: u64 = 2048;
const PER_PROGRAM_BYTE: u64 = 2;
const SCRATCH_CAP: u64 = 32 * MIB;
/// What one element of a broad transaction may cost in scratch. A fact batch is one row per
/// element by construction, so a limit that does not scale with the document refuses it on any
/// page big enough to want it. The device cap still binds, so a large enough document runs without
/// the batch, which is the intended degradation rather than an accident of a floor.
const PER_TRANSACTION_NODE: u64 = 768;

/// The document-shaped terms of the budget formula.
#[derive(Clone, Copy, Debug, Default)]
pub struct BudgetInputs {
    /// Connected styleable DOM elements in the live DOM at the read epoch. Not pseudo style nodes,
    /// not arena capacity, not retired generations.
    pub connected_element_count: u32,
    /// The byte length of a minimal non-commoned encoding of the attached selectors, match and
    /// transpose bytecode, routing registry, declarations, and conditions, counted once for an
    /// explicitly shared constructed program. Excludes allocator padding, optional indexes,
    /// results, and StyleEngine's own capacity, so acceleration overhead cannot inflate its own
    /// allowance.
    pub compact_style_program_bytes: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[must_use]
pub enum ReservationOutcome {
    Granted,
    /// The reservation was refused, and this many bytes would have to be freed for it to succeed.
    /// The caller either evicts that much and retries, or continues without the view.
    Refused {
        shortfall_bytes: u64,
    },
}

struct ChargeLedger {
    category_bytes: [Cell<u64>; MEMORY_CATEGORY_COUNT],
    tier_bytes: [Cell<u64>; TIER_COUNT],
}

impl ChargeLedger {
    fn new() -> Self {
        Self {
            category_bytes: std::array::from_fn(|_| Cell::new(0)),
            tier_bytes: std::array::from_fn(|_| Cell::new(0)),
        }
    }

    fn add(&self, category: MemoryCategory, bytes: u64, count_tier: bool) {
        let category_bytes = &self.category_bytes[category as usize];
        category_bytes.set(category_bytes.get() + bytes);
        if count_tier {
            let tier_bytes = &self.tier_bytes[category.tier().index()];
            tier_bytes.set(tier_bytes.get() + bytes);
        }
    }

    fn release(&self, category: MemoryCategory, bytes: u64, count_tier: bool) {
        let category_bytes = &self.category_bytes[category as usize];
        assert!(
            category_bytes.get() >= bytes,
            "released {bytes} bytes of {} with only {} reserved",
            category.name(),
            category_bytes.get()
        );
        category_bytes.set(category_bytes.get() - bytes);
        if count_tier {
            let tier_bytes = &self.tier_bytes[category.tier().index()];
            tier_bytes.set(tier_bytes.get() - bytes);
        }
    }
}

/// One materialization's shared accounting lifetime.
///
/// Optional growth is admitted through the controller, while required growth records capacity
/// which is already committed. Shrinking and dropping release the charge directly. The retained
/// container remains owned by its caller; this type owns only its accounting lifetime.
pub struct MemoryLease {
    category: MemoryCategory,
    ledger: Option<Rc<ChargeLedger>>,
    bytes: u64,
}

impl MemoryLease {
    #[must_use]
    pub fn new(category: MemoryCategory) -> Self {
        Self {
            category,
            ledger: None,
            bytes: 0,
        }
    }

    #[must_use]
    pub fn bytes(&self) -> u64 {
        self.bytes
    }

    pub fn grow(&mut self, memory: &mut MemoryController, bytes: u64) -> ReservationOutcome {
        assert!(matches!(self.category.tier(), Tier::Acceleration | Tier::Scratch));
        self.bind(memory);
        let outcome = memory.reserve(self.category, bytes);
        if outcome.is_granted() {
            self.bytes += bytes;
        }
        outcome
    }

    fn bind(&mut self, memory: &MemoryController) {
        if let Some(ledger) = &self.ledger {
            assert!(
                Rc::ptr_eq(ledger, &memory.charges),
                "memory lease moved between documents"
            );
        } else {
            self.ledger = Some(Rc::clone(&memory.charges));
        }
    }

    pub fn resize_to(&mut self, memory: &mut MemoryController, bytes: u64) -> ReservationOutcome {
        if bytes > self.bytes {
            self.grow(memory, bytes - self.bytes)
        } else {
            self.shrink_to(bytes);
            ReservationOutcome::Granted
        }
    }

    /// Account capacity which already exists and therefore cannot be refused.
    ///
    /// Acceleration state must use [`Self::resize_to`] before allocating instead.
    pub fn resize_required_to(&mut self, memory: &mut MemoryController, bytes: u64) -> bool {
        assert_ne!(self.category.tier(), Tier::Acceleration);
        if bytes > self.bytes {
            self.bind(memory);
            let over_limit = memory.reserve_required(self.category, bytes - self.bytes);
            self.bytes = bytes;
            over_limit
        } else {
            self.shrink_to(bytes);
            false
        }
    }

    /// Add capacity which already exists and therefore cannot be refused.
    pub fn grow_required(&mut self, memory: &mut MemoryController, bytes: u64) -> bool {
        let new_bytes = self.bytes.checked_add(bytes).expect("memory charge overflow");
        self.resize_required_to(memory, new_bytes)
    }

    /// Record capacity which has changed at an owner's existing settlement boundary.
    ///
    /// This is the mutation-side half of a coarse lease: it keeps the exact retained total in the
    /// lease without making the owner mirror it. Once bound, later mutations update the shared
    /// ledger directly. Optional owners must call [`MemoryController::admit_committed`] at the same
    /// boundary where they previously settled their complete container.
    pub fn grow_committed(&mut self, bytes: u64) {
        if bytes == 0 {
            return;
        }
        self.bytes = self.bytes.checked_add(bytes).expect("memory charge overflow");
        if let Some(ledger) = &self.ledger {
            ledger.add(
                self.category,
                bytes,
                matches!(self.category.tier(), Tier::Acceleration | Tier::Scratch),
            );
        }
    }

    pub fn shrink_committed(&mut self, bytes: u64) {
        assert!(bytes <= self.bytes);
        if bytes == 0 {
            return;
        }
        if let Some(ledger) = &self.ledger {
            ledger.release(
                self.category,
                bytes,
                matches!(self.category.tier(), Tier::Acceleration | Tier::Scratch),
            );
        }
        self.bytes -= bytes;
    }

    /// Attach mutation-fed capacity to this document's ledger.
    pub fn settle_committed(&mut self, memory: &mut MemoryController) -> bool {
        if self.ledger.is_some() {
            self.bind(memory);
            return memory.is_category_over_limit(self.category);
        }
        self.bind(memory);
        if self.bytes != 0 {
            memory.charges.add(
                self.category,
                self.bytes,
                matches!(self.category.tier(), Tier::Acceleration | Tier::Scratch),
            );
        }
        memory.is_category_over_limit(self.category)
    }

    pub fn shrink_to(&mut self, bytes: u64) {
        assert!(bytes <= self.bytes);
        let released = self.bytes - bytes;
        if released != 0 {
            self.ledger
                .as_ref()
                .expect("a charged memory lease has a ledger")
                .release(
                    self.category,
                    released,
                    matches!(self.category.tier(), Tier::Acceleration | Tier::Scratch),
                );
            self.bytes = bytes;
        }
    }

    pub fn release(&mut self) {
        if self.ledger.is_some() {
            self.shrink_to(0);
        } else {
            self.bytes = 0;
        }
    }
}

impl Drop for MemoryLease {
    fn drop(&mut self) {
        self.release();
    }
}

pub struct CapacityGuard<'a, T> {
    value: &'a mut T,
    memory: &'a mut MemoryLease,
    capacity_before: u64,
    capacity_bytes: fn(&T) -> u64,
}

impl<'a, T> CapacityGuard<'a, T> {
    #[must_use]
    pub fn new(value: &'a mut T, memory: &'a mut MemoryLease, capacity_bytes: fn(&T) -> u64) -> Self {
        let capacity_before = capacity_bytes(value);
        Self {
            value,
            memory,
            capacity_before,
            capacity_bytes,
        }
    }
}

impl<T> std::ops::Deref for CapacityGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        self.value
    }
}

impl<T> std::ops::DerefMut for CapacityGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.value
    }
}

impl<T> Drop for CapacityGuard<'_, T> {
    fn drop(&mut self) {
        let capacity_after = (self.capacity_bytes)(self.value);
        if capacity_after > self.capacity_before {
            self.memory.grow_committed(capacity_after - self.capacity_before);
        } else {
            self.memory.shrink_committed(self.capacity_before - capacity_after);
        }
    }
}

#[must_use]
pub struct ScratchCharge {
    category: MemoryCategory,
    ledger: Rc<ChargeLedger>,
    bytes: u64,
}

impl Drop for ScratchCharge {
    fn drop(&mut self) {
        self.ledger.release(self.category, self.bytes, true);
    }
}

impl ReservationOutcome {
    #[must_use]
    pub fn is_granted(self) -> bool {
        matches!(self, Self::Granted)
    }
}

/// Per-document controller tracking exact bytes by category and tier, and answering whether a
/// capacity charge fits before it is allocated.
pub struct MemoryController {
    inputs: BudgetInputs,
    charges: Rc<ChargeLedger>,
    refusals: [u64; MEMORY_CATEGORY_COUNT],
    benefit_hits: [u64; MEMORY_CATEGORY_COUNT],
    benefit_observations: [u64; MEMORY_CATEGORY_COUNT],
    observed_hit_totals: [u64; MEMORY_CATEGORY_COUNT],
    observed_miss_totals: [u64; MEMORY_CATEGORY_COUNT],
    last_refused_bytes: [u64; MEMORY_CATEGORY_COUNT],
    recording_policy_enabled: bool,
    #[cfg(test)]
    tier3_limit_override: Option<u64>,
    #[cfg(test)]
    tier4_limit_override: Option<u64>,
}

impl MemoryController {
    #[must_use]
    pub fn new(_device_class: DeviceClass) -> Self {
        Self {
            inputs: BudgetInputs::default(),
            charges: Rc::new(ChargeLedger::new()),
            refusals: [0; MEMORY_CATEGORY_COUNT],
            benefit_hits: [0; MEMORY_CATEGORY_COUNT],
            benefit_observations: [0; MEMORY_CATEGORY_COUNT],
            observed_hit_totals: [0; MEMORY_CATEGORY_COUNT],
            observed_miss_totals: [0; MEMORY_CATEGORY_COUNT],
            last_refused_bytes: [0; MEMORY_CATEGORY_COUNT],
            recording_policy_enabled: false,
            #[cfg(test)]
            tier3_limit_override: None,
            #[cfg(test)]
            tier4_limit_override: None,
        }
    }

    pub(crate) fn verification_copy(&self) -> Self {
        Self {
            inputs: self.inputs,
            charges: Rc::new(ChargeLedger::new()),
            refusals: [0; MEMORY_CATEGORY_COUNT],
            benefit_hits: [0; MEMORY_CATEGORY_COUNT],
            benefit_observations: [0; MEMORY_CATEGORY_COUNT],
            observed_hit_totals: [0; MEMORY_CATEGORY_COUNT],
            observed_miss_totals: [0; MEMORY_CATEGORY_COUNT],
            last_refused_bytes: [0; MEMORY_CATEGORY_COUNT],
            recording_policy_enabled: self.recording_policy_enabled,
            #[cfg(test)]
            tier3_limit_override: self.tier3_limit_override,
            #[cfg(test)]
            tier4_limit_override: self.tier4_limit_override,
        }
    }

    #[cfg(test)]
    pub fn set_tier3_limit_for_test(&mut self, limit: u64) {
        self.tier3_limit_override = Some(limit);
    }

    #[cfg(test)]
    pub fn set_tier4_limit_for_test(&mut self, limit: u64) {
        self.tier4_limit_override = Some(limit);
    }

    pub fn set_budget_inputs(&mut self, inputs: BudgetInputs) {
        self.inputs = inputs;
    }

    pub fn enable_recording_policy(&mut self) {
        self.recording_policy_enabled = true;
    }

    pub fn disable_recording_policy(&mut self) {
        self.recording_policy_enabled = false;
    }

    /// `min(DeviceCap, BaseAllowance + NodeAllowance * ConnectedElementCount +
    /// StylesheetAllowance * CompactStyleProgramBytes)`.
    #[must_use]
    pub fn tier3_limit(&self) -> u64 {
        #[cfg(test)]
        if let Some(limit) = self.tier3_limit_override {
            return limit;
        }
        let limit = BASE_ALLOWANCE
            .saturating_add(PER_CONNECTED_NODE.saturating_mul(u64::from(self.inputs.connected_element_count)))
            .saturating_add(PER_PROGRAM_BYTE.saturating_mul(self.inputs.compact_style_program_bytes))
            .min(DEVICE_CAP);
        if self.recording_policy_enabled {
            return DEVICE_CAP;
        }
        limit
    }

    /// `min(DeviceScratchCap, max(4 MiB, Tier3Limit,
    /// TransactionNodeAllowance * ConnectedElementCount))`.
    ///
    /// The node term is what lets a broad transaction hold one row per element: the fact batch a
    /// broad match reads is one row per element by construction, and without the term a document of
    /// a few thousand elements is refused one and matches every element from scratch instead. The
    /// device scratch cap still binds, so a large enough document goes without, which is the
    /// degradation the design intends rather than a floor deciding it by accident.
    #[must_use]
    pub fn tier4_limit(&self) -> u64 {
        #[cfg(test)]
        if let Some(limit) = self.tier4_limit_override {
            return limit;
        }
        (4 * MIB)
            .max(self.tier3_limit())
            .max(PER_TRANSACTION_NODE.saturating_mul(u64::from(self.inputs.connected_element_count)))
            .min(SCRATCH_CAP)
    }

    fn limit_for(&self, tier: Tier) -> Option<u64> {
        match tier {
            Tier::Authoritative | Tier::Live | Tier::Program => None,
            Tier::Acceleration => Some(self.tier3_limit()),
            Tier::Scratch => Some(self.tier4_limit()),
        }
    }

    fn is_category_over_limit(&self, category: MemoryCategory) -> bool {
        self.limit_for(category.tier())
            .is_some_and(|limit| self.charges.tier_bytes[category.tier().index()].get() > limit)
    }

    /// Admit capacity which an optional owner accumulated since its previous settlement.
    pub fn admit_committed(&mut self, category: MemoryCategory) -> bool {
        let Some(limit) = self.limit_for(category.tier()) else {
            return true;
        };
        let current = self.charges.tier_bytes[category.tier().index()].get();
        if current <= limit {
            self.last_refused_bytes[category as usize] = 0;
            return true;
        }
        self.refusals[category as usize] += 1;
        self.last_refused_bytes[category as usize] = current - limit;
        if category.tier() == Tier::Acceleration {
            self.record_benefit_lookup(category, false);
        }
        false
    }

    /// Charge `bytes` of capacity to `category` before it is allocated or grown.
    pub fn reserve(&mut self, category: MemoryCategory, bytes: u64) -> ReservationOutcome {
        let tier = category.tier();
        assert!(
            tier != Tier::Authoritative,
            "authoritative input is referenced, never charged: {}",
            category.name()
        );

        if let Some(limit) = self.limit_for(tier) {
            let projected = self.charges.tier_bytes[tier.index()].get().saturating_add(bytes);
            if projected > limit {
                self.refusals[category as usize] += 1;
                self.last_refused_bytes[category as usize] = bytes;
                if tier == Tier::Acceleration {
                    self.record_benefit_lookup(category, false);
                }
                return ReservationOutcome::Refused {
                    shortfall_bytes: projected - limit,
                };
            }
        }

        self.last_refused_bytes[category as usize] = 0;
        self.charges.add(category, bytes, self.limit_for(tier).is_some());
        ReservationOutcome::Granted
    }

    /// Record one lookup that acceleration in `category` did or did not answer.
    fn record_benefit_lookup(&mut self, category: MemoryCategory, hit: bool) {
        self.record_benefit_lookups(category, u64::from(hit), u64::from(!hit));
    }

    /// Import cumulative instrumentation without counting the same observations twice on later
    /// admissions. StyleEngine already counts the useful work each view serves, so admission reads
    /// those totals only when the budget is under pressure instead of adding bookkeeping to hot
    /// lookup paths.
    pub(super) fn record_benefit_totals(&mut self, category: MemoryCategory, hits: u64, misses: u64) {
        let index = category as usize;
        let new_hits = hits.saturating_sub(self.observed_hit_totals[index]);
        let new_misses = misses.saturating_sub(self.observed_miss_totals[index]);
        self.observed_hit_totals[index] = hits;
        self.observed_miss_totals[index] = misses;
        self.record_benefit_lookups(category, new_hits, new_misses);
    }

    pub(super) fn record_benefit_lookups(&mut self, category: MemoryCategory, mut hits: u64, mut misses: u64) {
        debug_assert_eq!(category.tier(), Tier::Acceleration);
        let index = category as usize;
        let incoming = hits.saturating_add(misses);
        if incoming >= 4096 {
            hits = hits.saturating_mul(2048) / incoming;
            misses = misses.saturating_mul(2048) / incoming;
            self.benefit_hits[index] = 0;
            self.benefit_observations[index] = 0;
        }
        while self.benefit_observations[index]
            .saturating_add(hits)
            .saturating_add(misses)
            >= 4096
        {
            self.benefit_hits[index] /= 2;
            self.benefit_observations[index] /= 2;
        }
        self.benefit_hits[index] += hits;
        self.benefit_observations[index] += hits.saturating_add(misses);
    }

    /// Pick the first resident zero-hit Tier-3 category when all zero-hit categories together can
    /// satisfy the last refused reservation. Requiring a requester hit prevents a first-time scan
    /// from displacing the working set, and refusing partial reclamation preserves resident state
    /// when the request cannot fit.
    #[must_use]
    pub(super) fn eviction_candidate(&self, requester: MemoryCategory) -> Option<MemoryCategory> {
        debug_assert_eq!(requester.tier(), Tier::Acceleration);
        let requested_bytes = self.last_refused_bytes[requester as usize];
        if requested_bytes == 0
            || self.benefit_hits[requester as usize] == 0
            || self.charges.category_bytes[requester as usize]
                .get()
                .saturating_add(requested_bytes)
                > self.tier3_limit()
        {
            return None;
        }

        let mut candidate = None;
        let mut reclaimable = 0u64;
        for (index, &category) in MEMORY_CATEGORIES.iter().enumerate() {
            if category == requester
                || category.tier() != Tier::Acceleration
                || !category.is_controller_evictable()
                || self.charges.category_bytes[index].get() == 0
                || self.benefit_hits[index] != 0
            {
                continue;
            }
            candidate.get_or_insert(category);
            reclaimable = reclaimable.saturating_add(self.charges.category_bytes[index].get());
        }

        let projected = self.charges.tier_bytes[Tier::Acceleration.index()]
            .get()
            .saturating_add(requested_bytes);
        if projected.saturating_sub(reclaimable) <= self.tier3_limit() {
            candidate
        } else {
            None
        }
    }

    /// Charge capacity that is already committed and therefore cannot be refused: required live
    /// state, and reconciliation of scratch whose exact capacity is only known after the container
    /// has grown. Returns whether the charge pushed its tier over the limit.
    ///
    /// This is not a way around the reserve-first rule. Tier 3 never uses it - acceleration state
    /// asks first and goes unbuilt if the answer is no. What this preserves is the stronger
    /// invariant: bytes that exist are always accounted, and an overrun is reported rather than
    /// becoming untracked allocation.
    pub fn reserve_required(&mut self, category: MemoryCategory, bytes: u64) -> bool {
        let tier = category.tier();
        assert!(
            tier != Tier::Authoritative,
            "authoritative input is referenced, never charged: {}",
            category.name()
        );
        assert!(
            tier != Tier::Acceleration,
            "acceleration state must reserve before allocating: {}",
            category.name()
        );

        let Some(limit) = self.limit_for(tier) else {
            self.charges.add(category, bytes, false);
            return false;
        };
        self.charges.add(category, bytes, true);
        self.charges.tier_bytes[tier.index()].get() > limit
    }

    pub fn charge_scratch(&mut self, category: MemoryCategory, bytes: u64) -> ScratchCharge {
        assert_eq!(category.tier(), Tier::Scratch);
        self.reserve_required(category, bytes);
        ScratchCharge {
            category,
            ledger: Rc::clone(&self.charges),
            bytes,
        }
    }

    /// Release capacity previously reserved for `category`.
    pub fn release(&mut self, category: MemoryCategory, bytes: u64) {
        self.charges
            .release(category, bytes, self.limit_for(category.tier()).is_some());
    }

    #[must_use]
    pub fn bytes_in_category(&self, category: MemoryCategory) -> u64 {
        self.charges.category_bytes[category as usize].get()
    }

    #[must_use]
    pub fn bytes_in_tier(&self, tier: Tier) -> u64 {
        self.charges.tier_bytes[tier.index()].get()
    }

    /// Reservations refused because the tier limit was already reached.
    #[must_use]
    pub fn refusals(&self, category: MemoryCategory) -> u64 {
        self.refusals[category as usize]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn controller(device_class: DeviceClass, elements: u32, program_bytes: u64) -> MemoryController {
        let mut controller = MemoryController::new(device_class);
        controller.set_budget_inputs(BudgetInputs {
            connected_element_count: elements,
            compact_style_program_bytes: program_bytes,
        });
        controller
    }

    #[test]
    fn every_category_has_a_chargeable_tier() {
        for index in 0..MEMORY_CATEGORY_COUNT {
            let tier = MEMORY_CATEGORY_TIERS[index];
            assert_ne!(
                tier,
                Tier::Authoritative,
                "{} must not be Tier 0",
                MEMORY_CATEGORY_NAMES[index]
            );
        }
    }

    #[test]
    fn an_empty_desktop_document_gets_the_base_allowance() {
        let controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        assert_eq!(controller.tier3_limit(), MIB);
        // The 4 MiB floor beats a 1 MiB Tier-3 limit.
        assert_eq!(controller.tier4_limit(), 4 * MIB);
    }

    #[test]
    fn recording_policy_uses_the_device_cap() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        controller.enable_recording_policy();
        assert_eq!(controller.tier3_limit(), DEVICE_CAP);
    }

    #[test]
    fn ending_recording_restores_the_document_budget() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        let document_limit = controller.tier3_limit();
        controller.enable_recording_policy();
        controller.disable_recording_policy();
        assert_eq!(controller.tier3_limit(), document_limit);
    }

    #[test]
    fn the_node_coefficient_reaches_the_device_cap_where_the_model_says_it_does() {
        let below = controller(DeviceClass::ForegroundDesktop, 32_255, 0);
        assert!(below.tier3_limit() < 64 * MIB);
        let at = controller(DeviceClass::ForegroundDesktop, 32_256, 0);
        assert_eq!(at.tier3_limit(), 64 * MIB);
    }

    #[test]
    fn a_shrinking_tier_three_budget_preserves_existing_charges() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 10_000, 4096);
        assert!(
            controller
                .reserve(MemoryCategory::FeaturePosting, 64 * KIB)
                .is_granted()
        );

        controller.set_tier3_limit_for_test(0);
        assert_eq!(controller.tier3_limit(), 0);

        // Existing charges survive a shrinking budget; the next reservation reports what eviction
        // would have to free, which includes the state that is now over budget.
        assert_eq!(
            controller.reserve(MemoryCategory::FeaturePosting, KIB),
            ReservationOutcome::Refused {
                shortfall_bytes: 65 * KIB
            }
        );
        assert_eq!(controller.refusals(MemoryCategory::FeaturePosting), 1);
    }

    #[test]
    fn tier_three_reserves_before_allocating_rather_than_evicting_afterwards() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        assert!(controller.reserve(MemoryCategory::CascadeWinnerGroup, MIB).is_granted());
        assert_eq!(controller.bytes_in_tier(Tier::Acceleration), MIB);

        assert_eq!(
            controller.reserve(MemoryCategory::FeaturePosting, 4 * KIB),
            ReservationOutcome::Refused {
                shortfall_bytes: 4 * KIB
            }
        );
        // The refused view was never charged.
        assert_eq!(controller.bytes_in_category(MemoryCategory::FeaturePosting), 0);
        assert_eq!(controller.bytes_in_tier(Tier::Acceleration), MIB);

        controller.release(MemoryCategory::CascadeWinnerGroup, 8 * KIB);
        assert!(controller.reserve(MemoryCategory::FeaturePosting, 4 * KIB).is_granted());
    }

    #[test]
    fn a_memory_lease_releases_optional_capacity_on_shrink_and_drop() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        {
            let mut lease = MemoryLease::new(MemoryCategory::SpecifiedValueTable);
            assert!(lease.grow(&mut controller, 64 * KIB).is_granted());
            assert_eq!(lease.bytes(), 64 * KIB);
            assert_eq!(
                controller.bytes_in_category(MemoryCategory::SpecifiedValueTable),
                64 * KIB
            );

            lease.shrink_to(16 * KIB);
            assert_eq!(
                controller.bytes_in_category(MemoryCategory::SpecifiedValueTable),
                16 * KIB
            );
        }
        assert_eq!(controller.bytes_in_category(MemoryCategory::SpecifiedValueTable), 0);
        assert_eq!(controller.bytes_in_tier(Tier::Acceleration), 0);
    }

    #[test]
    fn a_memory_lease_releases_required_capacity_on_settle_and_drop() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        {
            let mut lease = MemoryLease::new(MemoryCategory::BatchScratch);
            assert!(!lease.resize_required_to(&mut controller, 64 * KIB));
            assert_eq!(controller.bytes_in_category(MemoryCategory::BatchScratch), 64 * KIB);

            assert!(!lease.resize_required_to(&mut controller, 16 * KIB));
            assert_eq!(controller.bytes_in_category(MemoryCategory::BatchScratch), 16 * KIB);
        }
        assert_eq!(controller.bytes_in_category(MemoryCategory::BatchScratch), 0);
    }

    #[test]
    fn a_capacity_guard_tracks_growth_and_shrinkage() {
        fn bytes(values: &Vec<u64>) -> u64 {
            (values.capacity() * size_of::<u64>()) as u64
        }

        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        let mut lease = MemoryLease::new(MemoryCategory::BatchScratch);
        let mut values = Vec::new();
        {
            let mut guarded = CapacityGuard::new(&mut values, &mut lease, bytes);
            guarded.extend(0..128);
        }
        assert_eq!(lease.bytes(), bytes(&values));
        assert_eq!(controller.bytes_in_category(MemoryCategory::BatchScratch), 0);

        lease.settle_committed(&mut controller);
        {
            let mut guarded = CapacityGuard::new(&mut values, &mut lease, bytes);
            guarded.clear();
            guarded.shrink_to_fit();
        }
        assert_eq!(lease.bytes(), 0);
        assert_eq!(controller.bytes_in_category(MemoryCategory::BatchScratch), 0);
    }

    #[test]
    fn a_scratch_charge_releases_on_drop() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        {
            let _charge = controller.charge_scratch(MemoryCategory::BatchScratch, 64 * KIB);
            assert_eq!(controller.bytes_in_category(MemoryCategory::BatchScratch), 64 * KIB);
        }
        assert_eq!(controller.bytes_in_category(MemoryCategory::BatchScratch), 0);
        assert_eq!(controller.bytes_in_tier(Tier::Scratch), 0);
    }

    #[test]
    fn a_mutation_fed_lease_updates_the_ledger_without_owner_accounting() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        {
            let mut lease = MemoryLease::new(MemoryCategory::BatchScratch);
            lease.grow_committed(64 * KIB);
            assert_eq!(controller.bytes_in_category(MemoryCategory::BatchScratch), 0);
            assert!(!lease.settle_committed(&mut controller));
            assert_eq!(controller.bytes_in_category(MemoryCategory::BatchScratch), 64 * KIB);

            lease.grow_committed(16 * KIB);
            lease.shrink_committed(32 * KIB);
            assert_eq!(lease.bytes(), 48 * KIB);
            assert_eq!(controller.bytes_in_category(MemoryCategory::BatchScratch), 48 * KIB);
        }
        assert_eq!(controller.bytes_in_category(MemoryCategory::BatchScratch), 0);
    }

    #[test]
    fn committed_acceleration_is_refused_at_its_existing_settlement_gate() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        let mut lease = MemoryLease::new(MemoryCategory::CascadeWinnerGroup);
        lease.grow_committed(MIB + 1);
        assert!(lease.settle_committed(&mut controller));
        assert!(!controller.admit_committed(MemoryCategory::CascadeWinnerGroup));
        assert_eq!(controller.refusals(MemoryCategory::CascadeWinnerGroup), 1);
        lease.release();
        assert_eq!(controller.bytes_in_tier(Tier::Acceleration), 0);
    }

    #[test]
    fn tier_three_selects_the_first_resident_cold_category_for_eviction() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        assert!(
            controller
                .reserve(MemoryCategory::FeaturePosting, 256 * KIB)
                .is_granted()
        );
        assert!(
            controller
                .reserve(MemoryCategory::CascadeWinnerGroup, 256 * KIB)
                .is_granted()
        );
        assert!(
            controller
                .reserve(MemoryCategory::PrefixTransitionCache, 512 * KIB)
                .is_granted()
        );

        for _ in 0..16 {
            controller.record_benefit_lookup(MemoryCategory::FeaturePosting, true);
        }
        controller.record_benefit_lookup(MemoryCategory::RetainedMatchAnswer, true);
        assert!(matches!(
            controller.reserve(MemoryCategory::RetainedMatchAnswer, 64 * KIB),
            ReservationOutcome::Refused { .. }
        ));
        assert_eq!(
            controller.eviction_candidate(MemoryCategory::RetainedMatchAnswer),
            Some(MemoryCategory::CascadeWinnerGroup),
            "the hot posting survives and declaration order breaks the cold tie"
        );
    }

    #[test]
    fn tier_three_does_not_admit_a_requester_without_a_hit() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        assert!(controller.reserve(MemoryCategory::CascadeWinnerGroup, MIB).is_granted());
        assert!(matches!(
            controller.reserve(MemoryCategory::RetainedMatchAnswer, 4 * KIB),
            ReservationOutcome::Refused { .. }
        ));
        assert_eq!(controller.eviction_candidate(MemoryCategory::RetainedMatchAnswer), None);
    }

    #[test]
    fn tier_three_does_not_evict_when_eligible_views_cannot_make_room() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        assert!(
            controller
                .reserve(MemoryCategory::PrefixTransitionCache, MIB)
                .is_granted()
        );
        for _ in 0..64 {
            controller.record_benefit_lookup(MemoryCategory::PrefixTransitionCache, true);
        }
        controller.record_benefit_lookup(MemoryCategory::RetainedMatchAnswer, true);
        assert!(matches!(
            controller.reserve(MemoryCategory::RetainedMatchAnswer, 64 * KIB),
            ReservationOutcome::Refused { .. }
        ));
        assert_eq!(controller.eviction_candidate(MemoryCategory::RetainedMatchAnswer), None);
    }

    #[test]
    fn tier_three_does_not_select_externally_owned_cache_for_eviction() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        assert!(
            controller
                .reserve(MemoryCategory::ParsedSubstitutionCache, MIB)
                .is_granted()
        );
        controller.record_benefit_lookup(MemoryCategory::RetainedMatchAnswer, true);
        assert!(matches!(
            controller.reserve(MemoryCategory::RetainedMatchAnswer, 64 * KIB),
            ReservationOutcome::Refused { .. }
        ));
        assert_eq!(controller.eviction_candidate(MemoryCategory::RetainedMatchAnswer), None);
    }

    #[test]
    fn live_state_is_never_refused() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        assert!(
            controller
                .reserve(MemoryCategory::RelationColumns, 64 * MIB)
                .is_granted()
        );
        assert_eq!(controller.bytes_in_category(MemoryCategory::RelationColumns), 64 * MIB);
    }

    #[test]
    fn released_scratch_does_not_accumulate() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        assert!(controller.reserve(MemoryCategory::BatchScratch, 3 * MIB).is_granted());
        controller.release(MemoryCategory::BatchScratch, 3 * MIB);
        assert!(controller.reserve(MemoryCategory::BridgeBuffer, 2 * MIB).is_granted());

        assert_eq!(controller.bytes_in_tier(Tier::Scratch), 2 * MIB);
    }
}
