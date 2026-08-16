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
//! * Tier 4 is transaction scratch. Its ceiling is reported, never refused, and its capacity is
//!   released or shrunk at transaction boundaries rather than accumulated across transactions.
//!
//! Tier 3 owners reconcile exact capacity at coarse container boundaries. Limit-crossing growth
//! remains usable for the current quota period, closes later admission for that category, and is
//! followed by whole-category eviction at a flush boundary.

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
    /// Externally owned acceleration, such as the parsed-substitution cache, instead acknowledges
    /// the boundary request after its owner clears the complete category.
    #[must_use]
    pub(super) fn is_controller_evictable(self) -> bool {
        matches!(
            self,
            Self::RetainedWitness
                | Self::FeaturePosting
                | Self::CascadeWinnerGroup
                | Self::RetainedSelectorIncidence
                | Self::RetainedMatchAnswer
                | Self::PrefixTransitionCache
                | Self::PrefixAnswerCache
        )
    }

    /// Whether this category can be released as one correctness-neutral working set at a flush
    /// boundary. Specified values are program-owned identities, so dropping their payload table
    /// would permanently make the attached program's identities unresolvable.
    #[must_use]
    fn is_boundary_evictable(self) -> bool {
        self.is_controller_evictable() || self == Self::ParsedSubstitutionCache
    }
}

/// Acceleration categories whose admission closures are exposed in memory-pressure reports.
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
const TIER3_CATEGORY_COUNT: usize = TIER3_REFUSAL_CATEGORIES.len();
fn tier3_period_index(category: MemoryCategory) -> usize {
    if category == MemoryCategory::ParsedSubstitutionCache {
        return TIER3_CATEGORY_COUNT - 1;
    }
    debug_assert!((MemoryCategory::RetainedWitness..=MemoryCategory::PrefixAnswerCache).contains(&category));
    category as usize - MemoryCategory::RetainedWitness as usize
}

/// The sole document memory class exposed by the browser.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeviceClass {
    ForegroundDesktop,
}

const DEVICE_CAP: u64 = 64 * MIB;
const BASE_ALLOWANCE: u64 = MIB;
const PER_CONNECTED_NODE: u64 = 2048;
const SCRATCH_CAP: u64 = 32 * MIB;
/// What one element of a broad transaction may cost in scratch. A fact batch is one row per
/// element by construction, so the reported ceiling scales with the document rather than making
/// every sufficiently large broad transaction appear over-limit.
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
/// Growth records capacity at an owner's coarse mutation boundary. Shrinking and dropping release
/// the charge directly. The retained container remains owned by its caller; this type owns only
/// its accounting lifetime.
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

    /// Account capacity which already exists and therefore cannot be refused.
    pub fn resize_required_to(&mut self, memory: &mut MemoryController, bytes: u64) {
        assert_ne!(self.category.tier(), Tier::Acceleration);
        if bytes > self.bytes {
            self.bind(memory);
            memory.reserve_required(self.category, bytes - self.bytes);
            self.bytes = bytes;
        } else {
            self.shrink_to(bytes);
        }
    }

    /// Add capacity which already exists and therefore cannot be refused.
    pub fn grow_required(&mut self, memory: &mut MemoryController, bytes: u64) {
        let new_bytes = self.bytes.checked_add(bytes).expect("memory charge overflow");
        self.resize_required_to(memory, new_bytes);
    }

    /// Record capacity which has changed at an owner's existing settlement boundary.
    ///
    /// This is the mutation-side half of a coarse lease: it keeps the exact retained total in the
    /// lease without making the owner mirror it. Once bound, later mutations update the shared
    /// ledger directly. Optional owners reconcile their complete container at the same coarse
    /// boundary where they previously settled it.
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

    /// Reconcile capacity already committed by an owner at one coarse growth boundary.
    pub fn reconcile_committed(&mut self, memory: &mut MemoryController, bytes: u64) {
        if bytes >= self.bytes {
            self.grow_committed(bytes - self.bytes);
        } else {
            self.shrink_committed(self.bytes - bytes);
        }
        if self.ledger.is_some() {
            self.bind(memory);
            return;
        }
        self.bind(memory);
        if self.bytes != 0 {
            memory.charges.add(
                self.category,
                self.bytes,
                matches!(self.category.tier(), Tier::Acceleration | Tier::Scratch),
            );
        }
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

/// Per-document controller tracking exact bytes by category and tier.
pub struct MemoryController {
    inputs: BudgetInputs,
    charges: Rc<ChargeLedger>,
    refusals: [u64; MEMORY_CATEGORY_COUNT],
    benefit_hits: [u64; MEMORY_CATEGORY_COUNT],
    benefit_observations: [u64; MEMORY_CATEGORY_COUNT],
    observed_hit_totals: [u64; MEMORY_CATEGORY_COUNT],
    observed_miss_totals: [u64; MEMORY_CATEGORY_COUNT],
    last_refused_bytes: [u64; MEMORY_CATEGORY_COUNT],
    tier3_period_start_bytes: [u64; TIER3_CATEGORY_COUNT],
    tier3_admitting: [bool; MEMORY_CATEGORY_COUNT],
    external_tier3_drop_pending: [bool; MEMORY_CATEGORY_COUNT],
    tier3_quota_period_active: bool,
    recording_policy_enabled: bool,
    #[cfg(test)]
    tier3_limit_override: Option<u64>,
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
            tier3_period_start_bytes: [0; TIER3_CATEGORY_COUNT],
            tier3_admitting: [true; MEMORY_CATEGORY_COUNT],
            external_tier3_drop_pending: [false; MEMORY_CATEGORY_COUNT],
            tier3_quota_period_active: false,
            recording_policy_enabled: false,
            #[cfg(test)]
            tier3_limit_override: None,
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
            tier3_period_start_bytes: [0; TIER3_CATEGORY_COUNT],
            tier3_admitting: [true; MEMORY_CATEGORY_COUNT],
            external_tier3_drop_pending: [false; MEMORY_CATEGORY_COUNT],
            tier3_quota_period_active: false,
            recording_policy_enabled: self.recording_policy_enabled,
            #[cfg(test)]
            tier3_limit_override: self.tier3_limit_override,
        }
    }

    #[cfg(test)]
    pub fn set_tier3_limit_for_test(&mut self, limit: u64) {
        self.tier3_limit_override = Some(limit);
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

    /// Start one flush interval with every Tier-3 category admitting. Growth that crosses the
    /// global limit closes only the category that grew until the next interval.
    pub(super) fn begin_tier3_quota_period(&mut self) {
        for (position, &category) in TIER3_REFUSAL_CATEGORIES.iter().enumerate() {
            let index = category as usize;
            self.tier3_period_start_bytes[position] = self.charges.category_bytes[index].get();
            self.tier3_admitting[index] = true;
        }
        self.tier3_quota_period_active = true;
    }

    #[must_use]
    pub(super) fn is_tier3_admitting(&self, category: MemoryCategory) -> bool {
        debug_assert_eq!(category.tier(), Tier::Acceleration);
        self.tier3_admitting[category as usize]
    }

    /// End the current quota period and select complete working sets in increasing benefit order
    /// until their current residency covers the actual Tier-3 overage.
    pub(super) fn finish_tier3_quota_period(&mut self) -> [bool; MEMORY_CATEGORY_COUNT] {
        let mut selected = [false; MEMORY_CATEGORY_COUNT];
        if !self.tier3_quota_period_active {
            return selected;
        }
        self.tier3_quota_period_active = false;
        let overage = self
            .bytes_in_tier(Tier::Acceleration)
            .saturating_sub(self.tier3_limit());
        if overage == 0 {
            return selected;
        }
        let growth_crossed_limit = TIER3_REFUSAL_CATEGORIES.iter().any(|&category| {
            let index = category as usize;
            let period_index = tier3_period_index(category);
            !self.tier3_admitting[index]
                && self.charges.category_bytes[index].get() > self.tier3_period_start_bytes[period_index]
        });
        if !growth_crossed_limit {
            return selected;
        }

        let mut candidates = TIER3_REFUSAL_CATEGORIES;
        candidates.sort_by(|left, right| {
            let left_index = *left as usize;
            let right_index = *right as usize;
            let left_hits = self.benefit_hits[left_index];
            let right_hits = self.benefit_hits[right_index];
            match (left_hits == 0).cmp(&(right_hits == 0)).reverse() {
                std::cmp::Ordering::Equal => {
                    let left_observations = self.benefit_observations[left_index].max(1);
                    let right_observations = self.benefit_observations[right_index].max(1);
                    (u128::from(left_hits) * u128::from(right_observations))
                        .cmp(&(u128::from(right_hits) * u128::from(left_observations)))
                        .then_with(|| left_index.cmp(&right_index))
                }
                ordering => ordering,
            }
        });
        let mut candidate_selection = [false; MEMORY_CATEGORY_COUNT];
        let mut selected_bytes = 0_u64;
        for category in candidates {
            let index = category as usize;
            if !category.is_boundary_evictable() || self.charges.category_bytes[index].get() == 0 {
                continue;
            }
            selected_bytes = selected_bytes.saturating_add(self.charges.category_bytes[index].get());
            candidate_selection[index] = true;
            if selected_bytes >= overage {
                break;
            }
        }
        if selected_bytes < overage {
            return selected;
        }
        for category in TIER3_REFUSAL_CATEGORIES {
            let index = category as usize;
            if !candidate_selection[index] {
                continue;
            }
            if category.is_controller_evictable() {
                selected[index] = true;
            } else {
                self.external_tier3_drop_pending[index] = true;
            }
        }
        selected
    }

    /// Record acceleration capacity that an owner has already committed at its coarse settlement
    /// boundary. Only growth by this category can close its admission for the current period; the
    /// boundary independently chooses victims from the final, actual overage.
    pub(super) fn finish_committed_acceleration_growth(&mut self, category: MemoryCategory) {
        debug_assert_eq!(category.tier(), Tier::Acceleration);
        let index = category as usize;
        let overage = self
            .bytes_in_tier(Tier::Acceleration)
            .saturating_sub(self.tier3_limit());
        if overage == 0 {
            self.last_refused_bytes[index] = 0;
            return;
        }
        let category_grew = !self.tier3_quota_period_active
            || self.charges.category_bytes[index].get() > self.tier3_period_start_bytes[tier3_period_index(category)];
        if !category_grew {
            self.last_refused_bytes[index] = 0;
            return;
        }
        if self.tier3_admitting[index] {
            self.refusals[index] += 1;
            self.record_benefit_lookup(category, false);
        }
        self.tier3_admitting[index] = false;
        self.last_refused_bytes[index] = overage;
    }

    #[must_use]
    pub(super) fn external_tier3_drop_is_pending(&self, category: MemoryCategory) -> bool {
        debug_assert_eq!(category.tier(), Tier::Acceleration);
        debug_assert!(!category.is_controller_evictable());
        self.external_tier3_drop_pending[category as usize]
    }

    pub(super) fn complete_external_tier3_drop(&mut self, category: MemoryCategory) {
        debug_assert!(self.external_tier3_drop_is_pending(category));
        self.external_tier3_drop_pending[category as usize] = false;
    }

    /// `min(DeviceCap, BaseAllowance + NodeAllowance * ConnectedElementCount)`.
    #[must_use]
    pub fn tier3_limit(&self) -> u64 {
        #[cfg(test)]
        if let Some(limit) = self.tier3_limit_override {
            return limit;
        }
        let limit = BASE_ALLOWANCE
            .saturating_add(PER_CONNECTED_NODE.saturating_mul(u64::from(self.inputs.connected_element_count)))
            .min(DEVICE_CAP);
        if self.recording_policy_enabled {
            return DEVICE_CAP;
        }
        limit
    }

    /// `min(DeviceScratchCap, max(4 MiB, Tier3Limit,
    /// TransactionNodeAllowance * ConnectedElementCount))`.
    ///
    /// The node term accounts for a broad transaction's one fact row per element. The device
    /// scratch ceiling makes an unusually broad transaction visible in pressure reports; it does
    /// not refuse scratch required to complete the flush.
    #[must_use]
    pub fn tier4_limit(&self) -> u64 {
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

    #[cfg(test)]
    pub(crate) fn reserve(&mut self, category: MemoryCategory, bytes: u64) -> bool {
        assert_eq!(category.tier(), Tier::Acceleration);
        if self.tier3_quota_period_active {
            let index = category as usize;
            let projected_tier = self.bytes_in_tier(Tier::Acceleration).saturating_add(bytes);
            if !self.tier3_admitting[index] || projected_tier > self.tier3_limit() {
                self.tier3_admitting[index] = false;
                self.refusals[index] += 1;
                self.last_refused_bytes[index] = bytes;
                self.record_benefit_lookup(category, false);
                return false;
            }
        } else {
            let projected = self.bytes_in_tier(Tier::Acceleration).saturating_add(bytes);
            let limit = self.tier3_limit();
            if projected > limit {
                self.refusals[category as usize] += 1;
                self.last_refused_bytes[category as usize] = bytes;
                self.record_benefit_lookup(category, false);
                return false;
            }
        }

        self.last_refused_bytes[category as usize] = 0;
        self.charges.add(category, bytes, true);
        true
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

    /// Charge capacity that is already committed: required live state and scratch whose exact
    /// capacity is known at a container boundary. Tier 4 remains a reported ceiling, so crossing
    /// it changes no behavior.
    pub fn reserve_required(&mut self, category: MemoryCategory, bytes: u64) {
        let tier = category.tier();
        assert!(
            tier != Tier::Authoritative,
            "authoritative input is referenced, never charged: {}",
            category.name()
        );
        assert!(
            tier != Tier::Acceleration,
            "acceleration state reconciles at its category boundary: {}",
            category.name()
        );

        self.charges.add(category, bytes, self.limit_for(tier).is_some());
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

    /// Admission closures recorded when category growth crosses the Tier-3 limit.
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
    fn tier_four_reports_its_ceiling_without_refusing_scratch() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        controller.reserve_required(MemoryCategory::BatchScratch, controller.tier4_limit() + 1);
        assert_eq!(controller.bytes_in_tier(Tier::Scratch), controller.tier4_limit() + 1);
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
    fn tier_three_pressure_closes_admission_until_the_next_period() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        controller.begin_tier3_quota_period();
        let category = MemoryCategory::FeaturePosting;
        assert!(controller.reserve(category, controller.tier3_limit()));
        assert!(!controller.reserve(category, 1));

        controller.release(category, 1);
        assert!(!controller.reserve(category, 1));
        controller.begin_tier3_quota_period();
        assert!(controller.reserve(category, 1));
    }

    #[test]
    fn tier_three_quota_boundary_selects_a_complete_cold_overage() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        controller.set_tier3_limit_for_test(100);
        controller.begin_tier3_quota_period();
        let mut lease = MemoryLease::new(MemoryCategory::FeaturePosting);
        lease.reconcile_committed(&mut controller, 101);
        controller.finish_committed_acceleration_growth(MemoryCategory::FeaturePosting);

        let selected = controller.finish_tier3_quota_period();

        assert!(selected[MemoryCategory::FeaturePosting as usize]);
        assert_eq!(selected.iter().filter(|&&candidate| candidate).count(), 1);
    }

    #[test]
    fn tier_three_boundary_keeps_working_sets_that_cannot_cover_the_overage() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        controller.set_tier3_limit_for_test(100);
        let mut answers = MemoryLease::new(MemoryCategory::RetainedMatchAnswer);
        answers.reconcile_committed(&mut controller, 50);
        controller.begin_tier3_quota_period();
        let mut program_values = MemoryLease::new(MemoryCategory::SpecifiedValueTable);
        program_values.reconcile_committed(&mut controller, 101);
        controller.finish_committed_acceleration_growth(MemoryCategory::SpecifiedValueTable);

        assert!(!controller.finish_tier3_quota_period().iter().any(|&selected| selected));
        assert_eq!(controller.bytes_in_category(MemoryCategory::RetainedMatchAnswer), 50);
    }

    #[test]
    fn committed_external_state_requests_owner_eviction_after_the_boundary() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        controller.set_tier3_limit_for_test(0);
        controller.begin_tier3_quota_period();
        let mut lease = MemoryLease::new(MemoryCategory::ParsedSubstitutionCache);
        lease.reconcile_committed(&mut controller, 1);
        controller.finish_committed_acceleration_growth(MemoryCategory::ParsedSubstitutionCache);

        assert!(!controller.finish_tier3_quota_period().iter().any(|&selected| selected));
        assert!(controller.external_tier3_drop_is_pending(MemoryCategory::ParsedSubstitutionCache));
        lease.reconcile_committed(&mut controller, 0);
        controller.complete_external_tier3_drop(MemoryCategory::ParsedSubstitutionCache);
        assert_eq!(controller.bytes_in_tier(Tier::Acceleration), 0);
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
        assert!(controller.reserve(MemoryCategory::FeaturePosting, 64 * KIB));

        controller.set_tier3_limit_for_test(0);
        assert_eq!(controller.tier3_limit(), 0);

        // Existing charges survive a shrinking budget, and test admission closes for new state.
        assert!(!controller.reserve(MemoryCategory::FeaturePosting, KIB));
        assert_eq!(controller.refusals(MemoryCategory::FeaturePosting), 1);
    }

    #[test]
    fn a_memory_lease_releases_optional_capacity_on_shrink_and_drop() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        {
            let mut lease = MemoryLease::new(MemoryCategory::SpecifiedValueTable);
            lease.reconcile_committed(&mut controller, 64 * KIB);
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
    fn a_memory_lease_releases_required_capacity_on_resize_and_drop() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        {
            let mut lease = MemoryLease::new(MemoryCategory::BatchScratch);
            lease.resize_required_to(&mut controller, 64 * KIB);
            assert_eq!(controller.bytes_in_category(MemoryCategory::BatchScratch), 64 * KIB);

            lease.resize_required_to(&mut controller, 16 * KIB);
            assert_eq!(controller.bytes_in_category(MemoryCategory::BatchScratch), 16 * KIB);
        }
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
            let bytes = lease.bytes();
            lease.reconcile_committed(&mut controller, bytes);
            assert_eq!(controller.bytes_in_category(MemoryCategory::BatchScratch), 64 * KIB);

            lease.grow_committed(16 * KIB);
            lease.shrink_committed(32 * KIB);
            assert_eq!(lease.bytes(), 48 * KIB);
            assert_eq!(controller.bytes_in_category(MemoryCategory::BatchScratch), 48 * KIB);
        }
        assert_eq!(controller.bytes_in_category(MemoryCategory::BatchScratch), 0);
    }

    #[test]
    fn committed_acceleration_is_dropped_at_the_next_quota_boundary() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        controller.begin_tier3_quota_period();
        let mut lease = MemoryLease::new(MemoryCategory::CascadeWinnerGroup);
        lease.grow_committed(MIB + 1);
        let bytes = lease.bytes();
        lease.reconcile_committed(&mut controller, bytes);
        controller.finish_committed_acceleration_growth(MemoryCategory::CascadeWinnerGroup);
        assert_eq!(controller.refusals(MemoryCategory::CascadeWinnerGroup), 1);
        assert!(controller.finish_tier3_quota_period()[MemoryCategory::CascadeWinnerGroup as usize]);
        lease.release();
        assert_eq!(controller.bytes_in_tier(Tier::Acceleration), 0);
    }

    #[test]
    fn an_over_limit_steady_state_does_not_repeat_boundary_evictions() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        controller.set_tier3_limit_for_test(0);
        let mut program_values = MemoryLease::new(MemoryCategory::SpecifiedValueTable);
        program_values.reconcile_committed(&mut controller, 1);
        controller.begin_tier3_quota_period();
        let mut answers = MemoryLease::new(MemoryCategory::RetainedMatchAnswer);
        answers.reconcile_committed(&mut controller, 1);
        controller.finish_committed_acceleration_growth(MemoryCategory::RetainedMatchAnswer);
        assert!(!controller.finish_tier3_quota_period().iter().any(|&selected| selected));
        answers.release();

        controller.begin_tier3_quota_period();
        controller.finish_committed_acceleration_growth(MemoryCategory::SpecifiedValueTable);
        assert!(!controller.finish_tier3_quota_period().iter().any(|&selected| selected));
    }

    #[test]
    fn discarded_committed_growth_does_not_condemn_resident_state() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        controller.set_tier3_limit_for_test(10);
        let mut resident = MemoryLease::new(MemoryCategory::FeaturePosting);
        resident.reconcile_committed(&mut controller, 10);
        controller.begin_tier3_quota_period();
        {
            let mut temporary = MemoryLease::new(MemoryCategory::FeaturePosting);
            temporary.reconcile_committed(&mut controller, 1);
            controller.finish_committed_acceleration_growth(MemoryCategory::FeaturePosting);
        }

        assert_eq!(controller.bytes_in_category(MemoryCategory::FeaturePosting), 10);
        assert!(!controller.finish_tier3_quota_period()[MemoryCategory::FeaturePosting as usize]);
    }

    #[test]
    fn live_state_is_never_refused() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        controller.reserve_required(MemoryCategory::RelationColumns, 64 * MIB);
        assert_eq!(controller.bytes_in_category(MemoryCategory::RelationColumns), 64 * MIB);
    }

    #[test]
    fn released_scratch_does_not_accumulate() {
        let mut controller = controller(DeviceClass::ForegroundDesktop, 0, 0);
        controller.reserve_required(MemoryCategory::BatchScratch, 3 * MIB);
        controller.release(MemoryCategory::BatchScratch, 3 * MIB);
        controller.reserve_required(MemoryCategory::BridgeBuffer, 2 * MIB);

        assert_eq!(controller.bytes_in_tier(Tier::Scratch), 2 * MIB);
    }
}
