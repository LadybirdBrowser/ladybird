/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Canonical specified-value identities retained as an evictable dense table.

use crate::css::style_value::RetainedStyleValueData;
use crate::css::style_value::StyleValueData;
use crate::css::style_value::retain_style_value;

use super::capacity::capacity_bytes;
use super::cascade::SpecifiedValueID;
use super::fast_hash::FastMap as HashMap;
use super::memory::MemoryCategory;
use super::memory::MemoryController;
use super::memory::MemoryLease;
use super::partial_view::Lookup;

struct SpecifiedValueEntry {
    id: SpecifiedValueID,
    value: RetainedStyleValueData,
}

define_id! { struct SpecifiedValueEntryIndex(); }

impl super::intern_table::InternIdentity for SpecifiedValueEntryIndex {
    fn index(self) -> usize {
        self.0 as usize
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SpecifiedValueCoverage {
    Complete,
    Partial { eviction_generation: u64 },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum SpecifiedValueGap {
    RetiredPayloads { eviction_generation: u64 },
}

#[derive(Clone, Copy)]
struct SpecifiedValueProbe<'a> {
    pointer: *const StyleValueData,
    value: &'a StyleValueData,
}

/// An identity is never reused, so evicting the values only makes future equality checks
/// conservative. Existing rule and winner rows can keep comparing their opaque identities without
/// retaining old CSSOM values forever.
pub(super) struct SpecifiedValues {
    entries: super::intern_table::InternTable<SpecifiedValueEntryIndex, SpecifiedValueEntry>,
    entries_by_pointer: HashMap<usize, SpecifiedValueID>,
    entries_by_id: HashMap<SpecifiedValueID, u32>,
    coverage: SpecifiedValueCoverage,
    next_id: u64,
    residency: MemoryLease,
}

impl SpecifiedValues {
    pub(super) fn new() -> Self {
        Self {
            entries: super::intern_table::InternTable::default(),
            entries_by_pointer: HashMap::default(),
            entries_by_id: HashMap::default(),
            coverage: SpecifiedValueCoverage::Complete,
            next_id: 1,
            residency: MemoryLease::new(MemoryCategory::SpecifiedValueTable),
        }
    }

    #[must_use]
    fn lookup(&self, probe: SpecifiedValueProbe<'_>) -> Lookup<SpecifiedValueID, SpecifiedValueGap> {
        if let Some(id) = self.entries_by_pointer.get(&(probe.pointer as usize)) {
            return Lookup::Known(*id);
        }
        if let Some(index) = self.entries.find(probe.value.content_hash(), |_index, entry| {
            entry.value.data() == probe.value
        }) {
            return Lookup::Known(self.entries[index].id);
        }
        match self.coverage() {
            SpecifiedValueCoverage::Complete => Lookup::KnownAbsent,
            SpecifiedValueCoverage::Partial { eviction_generation } => {
                Lookup::Missing(SpecifiedValueGap::RetiredPayloads {
                    eviction_generation: *eviction_generation,
                })
            }
        }
    }

    #[must_use]
    fn coverage(&self) -> &SpecifiedValueCoverage {
        &self.coverage
    }

    fn mark_partial(&mut self) -> SpecifiedValueGap {
        let eviction_generation = match self.coverage {
            SpecifiedValueCoverage::Complete => 1,
            SpecifiedValueCoverage::Partial { eviction_generation } => eviction_generation,
        };
        self.coverage = SpecifiedValueCoverage::Partial { eviction_generation };
        SpecifiedValueGap::RetiredPayloads { eviction_generation }
    }

    /// Return the retained payload behind one maintained declaration identity.
    #[must_use]
    pub(super) fn value(&self, id: SpecifiedValueID) -> Lookup<&StyleValueData, SpecifiedValueGap> {
        if let Some(index) = self.entries_by_id.get(&id) {
            return Lookup::Known(self.entries[*index as usize].value.data());
        }
        match self.coverage() {
            SpecifiedValueCoverage::Complete => Lookup::KnownAbsent,
            SpecifiedValueCoverage::Partial { eviction_generation } => {
                Lookup::Missing(SpecifiedValueGap::RetiredPayloads {
                    eviction_generation: *eviction_generation,
                })
            }
        }
    }

    /// # Safety
    /// `value` must point at live `StyleValueData`.
    pub(super) unsafe fn intern(
        &mut self,
        value: *const StyleValueData,
        memory: &mut MemoryController,
    ) -> (SpecifiedValueID, Lookup<(), SpecifiedValueGap>) {
        let probe = SpecifiedValueProbe {
            pointer: value,
            value: unsafe { &*value },
        };
        let lookup = match self.lookup(probe) {
            Lookup::Known(id) => return (id, Lookup::Known(())),
            Lookup::KnownAbsent => Lookup::KnownAbsent,
            Lookup::Missing(gap) => Lookup::Missing(gap),
        };

        let id = SpecifiedValueID(self.next_id);
        self.next_id = self
            .next_id
            .checked_add(1)
            .expect("specified value identity space exhausted");
        if !memory.is_tier3_admitting(MemoryCategory::SpecifiedValueTable) {
            return (id, Lookup::Missing(self.mark_partial()));
        }
        let retained = unsafe { RetainedStyleValueData::from_retained_pointer(retain_style_value(value)) };
        self.push_entry(id, retained, value as usize);
        self.settle_memory(memory);
        (id, lookup)
    }

    /// Restore a program-owned identity from its still-live declaration payload.
    ///
    /// # Safety
    /// `value` must point at live `StyleValueData`.
    pub(super) unsafe fn ensure_identity(
        &mut self,
        value: *const StyleValueData,
        id: SpecifiedValueID,
        memory: &mut MemoryController,
    ) -> bool {
        if self.entries_by_pointer.get(&(value as usize)) == Some(&id) {
            return true;
        }
        let probe = SpecifiedValueProbe {
            pointer: value,
            value: unsafe { &*value },
        };
        if let Some(index) = self.entries_by_id.get(&id) {
            return matches!(self.lookup(probe), Lookup::Known(existing) if self.entries_by_id.get(&existing) == Some(index));
        }
        if !memory.is_tier3_admitting(MemoryCategory::SpecifiedValueTable) {
            self.mark_partial();
            return false;
        }
        if let Lookup::Known(existing) = self.lookup(probe) {
            let index = self.entries_by_id[&existing];
            self.entries_by_id.insert(id, index);
        } else {
            let retained = unsafe { RetainedStyleValueData::from_retained_pointer(retain_style_value(value)) };
            self.push_entry(id, retained, value as usize);
        }
        self.settle_memory(memory);
        true
    }

    /// Associate another immutable spelling with an existing canonical identity.
    ///
    /// # Safety
    /// `value` must point at live `StyleValueData`.
    pub(super) unsafe fn alias(
        &mut self,
        value: *const StyleValueData,
        id: SpecifiedValueID,
        memory: &mut MemoryController,
    ) {
        let probe = SpecifiedValueProbe {
            pointer: value,
            value: unsafe { &*value },
        };
        if let Lookup::Known(existing) = self.lookup(probe) {
            debug_assert_eq!(existing, id);
            return;
        }
        if !memory.is_tier3_admitting(MemoryCategory::SpecifiedValueTable) {
            return;
        }
        let retained = unsafe { RetainedStyleValueData::from_retained_pointer(retain_style_value(value)) };
        self.push_entry(id, retained, value as usize);
        self.settle_memory(memory);
    }

    fn push_entry(&mut self, id: SpecifiedValueID, value: RetainedStyleValueData, pointer: usize) {
        let index = SpecifiedValueEntryIndex(
            u32::try_from(self.entries.len()).expect("specified value table exceeds u32 indexing"),
        );
        self.entries
            .insert(value.data().content_hash(), index, SpecifiedValueEntry { id, value });
        self.entries_by_pointer.insert(pointer, id);
        self.entries_by_id.entry(id).or_insert(index.0);
    }

    #[must_use]
    fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.entries, self.entries_by_pointer, self.entries_by_id];
            cached [];
            nested [];
            skip [self.coverage, self.next_id, self.residency];
        }
    }

    fn settle_memory(&mut self, memory: &mut MemoryController) {
        let current = self.capacity_bytes();
        self.residency.reconcile_committed(memory, current);
        memory.finish_committed_acceleration_growth(MemoryCategory::SpecifiedValueTable);
    }

    pub(super) fn evict(&mut self) {
        self.residency.release();
        if !self.entries.is_empty() {
            let eviction_generation = match self.coverage {
                SpecifiedValueCoverage::Complete => 1,
                SpecifiedValueCoverage::Partial { eviction_generation } => eviction_generation
                    .checked_add(1)
                    .expect("specified value eviction generation exhausted"),
            };
            self.coverage = SpecifiedValueCoverage::Partial { eviction_generation };
        }
        self.entries = super::intern_table::InternTable::default();
        self.entries_by_pointer = HashMap::default();
        self.entries_by_id = HashMap::default();
    }
}

#[cfg(test)]
mod tests {
    use super::super::memory::DeviceClass;
    use super::*;

    #[test]
    fn evicted_specified_values_are_missing_instead_of_absent() {
        fn probe(value: &StyleValueData) -> SpecifiedValueProbe<'_> {
            SpecifiedValueProbe { pointer: value, value }
        }

        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut values = SpecifiedValues::new();
        let value = std::sync::Arc::new(StyleValueData::Number { value: 42.0 });
        let equal_value = std::sync::Arc::new(StyleValueData::Number { value: 42.0 });

        assert!(matches!(values.lookup(probe(&value)), Lookup::KnownAbsent));
        let (first, first_lookup) = unsafe { values.intern(std::sync::Arc::as_ptr(&value), &mut memory) };
        assert!(matches!(first_lookup, Lookup::KnownAbsent));
        assert_eq!(values.entries_by_pointer.len(), 1);
        assert_eq!(
            memory.bytes_in_category(MemoryCategory::SpecifiedValueTable),
            values.capacity_bytes()
        );
        assert!(matches!(values.lookup(probe(&value)), Lookup::Known(id) if id == first));
        assert!(matches!(values.value(first), Lookup::Known(retained) if retained == value.as_ref()));
        let (reused, reused_lookup) = unsafe { values.intern(std::sync::Arc::as_ptr(&equal_value), &mut memory) };
        assert_eq!(reused, first);
        assert!(matches!(reused_lookup, Lookup::Known(())));
        // A content hit does not retain a one-use duplicate spelling. Looking it up again repeats
        // the collision-safe content lookup against the canonical table.
        assert_eq!(values.entries_by_pointer.len(), 1);
        assert!(matches!(values.lookup(probe(&equal_value)), Lookup::Known(id) if id == first));

        values.evict();
        assert!(matches!(
            values.value(first),
            Lookup::Missing(SpecifiedValueGap::RetiredPayloads { eviction_generation: 1 })
        ));
        assert!(matches!(
            values.lookup(probe(&value)),
            Lookup::Missing(SpecifiedValueGap::RetiredPayloads { eviction_generation: 1 })
        ));
        let (second, second_lookup) = unsafe { values.intern(std::sync::Arc::as_ptr(&value), &mut memory) };
        assert_ne!(second, first, "retired specified-value identities are never reused");
        assert!(matches!(
            second_lookup,
            Lookup::Missing(SpecifiedValueGap::RetiredPayloads { eviction_generation: 1 })
        ));
    }

    #[test]
    fn equal_content_dedups_across_distinct_spellings() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut values = SpecifiedValues::new();
        let originals: Vec<_> = (0..64)
            .map(|i| std::sync::Arc::new(StyleValueData::Number { value: f64::from(i) }))
            .collect();
        let ids: Vec<_> = originals
            .iter()
            .map(|value| unsafe { values.intern(std::sync::Arc::as_ptr(value), &mut memory) }.0)
            .collect();
        for (left, right) in ids.iter().zip(ids.iter().skip(1)) {
            assert_ne!(left, right);
        }
        let respellings: Vec<_> = (0..64)
            .map(|i| std::sync::Arc::new(StyleValueData::Number { value: f64::from(i) }))
            .collect();
        for (respelling, id) in respellings.iter().zip(ids.iter()) {
            let (reused, _) = unsafe { values.intern(std::sync::Arc::as_ptr(respelling), &mut memory) };
            assert_eq!(reused, *id);
        }
    }

    #[test]
    fn committed_specified_values_remain_resolvable_across_quota_boundaries() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        memory.set_tier3_limit_for_test(0);
        memory.begin_tier3_quota_period();
        let mut values = SpecifiedValues::new();
        let value = std::sync::Arc::new(StyleValueData::Number { value: 42.0 });

        let (identity, _) = unsafe { values.intern(std::sync::Arc::as_ptr(&value), &mut memory) };

        assert!(matches!(values.value(identity), Lookup::Known(retained) if retained == value.as_ref()));
        assert!(!memory.finish_tier3_quota_period()[MemoryCategory::SpecifiedValueTable as usize]);
        assert!(matches!(values.value(identity), Lookup::Known(retained) if retained == value.as_ref()));
    }

    #[test]
    fn closed_specified_value_admission_keeps_program_values_resolvable() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        memory.set_tier3_limit_for_test(0);
        memory.begin_tier3_quota_period();
        let mut values = SpecifiedValues::new();
        let resident = std::sync::Arc::new(StyleValueData::Number { value: 1.0 });
        let refused = std::sync::Arc::new(StyleValueData::Number { value: 2.0 });

        let (resident_id, _) = unsafe { values.intern(std::sync::Arc::as_ptr(&resident), &mut memory) };
        let bytes = memory.bytes_in_category(MemoryCategory::SpecifiedValueTable);
        let (refused_id, refused_lookup) = unsafe { values.intern(std::sync::Arc::as_ptr(&refused), &mut memory) };

        assert!(matches!(values.value(resident_id), Lookup::Known(value) if value == resident.as_ref()));
        assert!(matches!(refused_lookup, Lookup::Missing(_)));
        assert!(matches!(values.value(refused_id), Lookup::Missing(_)));
        assert_eq!(memory.bytes_in_category(MemoryCategory::SpecifiedValueTable), bytes);
        assert!(!unsafe { values.ensure_identity(std::sync::Arc::as_ptr(&refused), resident_id, &mut memory) });

        let _ = memory.finish_tier3_quota_period();
        memory.begin_tier3_quota_period();
        assert!(unsafe { values.ensure_identity(std::sync::Arc::as_ptr(&refused), refused_id, &mut memory) });
        assert!(matches!(values.value(refused_id), Lookup::Known(value) if value == refused.as_ref()));
    }

    #[test]
    fn authored_spellings_can_alias_a_canonical_identity() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut values = SpecifiedValues::new();
        let canonical = std::sync::Arc::new(StyleValueData::Number { value: 42.0 });
        let authored = std::sync::Arc::new(StyleValueData::Number { value: 43.0 });

        let (canonical_id, _) = unsafe { values.intern(std::sync::Arc::as_ptr(&canonical), &mut memory) };
        unsafe { values.alias(std::sync::Arc::as_ptr(&authored), canonical_id, &mut memory) };
        assert_eq!(values.entries_by_pointer.len(), 2);
        assert!(matches!(
            values.lookup(SpecifiedValueProbe {
                pointer: std::sync::Arc::as_ptr(&authored),
                value: &authored,
            }),
            Lookup::Known(id) if id == canonical_id
        ));
        let (authored_id, lookup) = unsafe { values.intern(std::sync::Arc::as_ptr(&authored), &mut memory) };

        assert_eq!(authored_id, canonical_id);
        assert!(matches!(lookup, Lookup::Known(())));
    }

    #[test]
    fn identity_restoration_indexes_only_retained_payload_pointers() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut values = SpecifiedValues::new();
        let canonical = std::sync::Arc::new(StyleValueData::Number { value: 42.0 });
        let duplicate = std::sync::Arc::new(StyleValueData::Number { value: 42.0 });

        let (canonical_id, _) = unsafe { values.intern(std::sync::Arc::as_ptr(&canonical), &mut memory) };
        let restored_id = SpecifiedValueID(canonical_id.0 + 1);
        assert!(unsafe { values.ensure_identity(std::sync::Arc::as_ptr(&duplicate), restored_id, &mut memory) });

        assert_eq!(values.entries_by_pointer.len(), 1);
        assert!(matches!(values.value(restored_id), Lookup::Known(value) if value == canonical.as_ref()));
    }
}
