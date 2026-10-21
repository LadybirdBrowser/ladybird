/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Canonical specified-value identities retained as an evictable dense table.

use crate::css::style_value::RetainedStyleValueData;
use crate::css::style_value::StyleValueData;
use std::sync::Arc;

use super::cascade::SpecifiedValueID;
use super::fast_hash::FastMap as HashMap;
use super::partial_view::Lookup;

#[derive(Clone)]

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
pub(super) enum SpecifiedValueGap {
    RetiredPayloads { eviction_generation: u64 },
}

/// An identity is never reused, so evicting the values only makes future equality checks
/// conservative. Existing rule and winner rows can keep comparing their opaque identities without
/// retaining old CSSOM values forever.
#[derive(Clone)]
pub(super) struct SpecifiedValues {
    entries: super::intern_table::InternTable<SpecifiedValueEntryIndex, SpecifiedValueEntry>,
    entries_by_pointer: HashMap<usize, SpecifiedValueID>,
    entries_by_id: HashMap<SpecifiedValueID, u32>,
    gap: Option<SpecifiedValueGap>,
    next_id: u64,
}

impl SpecifiedValues {
    pub(super) fn new() -> Self {
        Self {
            entries: super::intern_table::InternTable::default(),
            entries_by_pointer: HashMap::default(),
            entries_by_id: HashMap::default(),
            gap: None,
            next_id: 1,
        }
    }

    #[must_use]
    fn lookup(&self, value: &StyleValueData) -> Lookup<SpecifiedValueID, SpecifiedValueGap> {
        if let Some(id) = self.entries_by_pointer.get(&(std::ptr::from_ref(value) as usize)) {
            return Lookup::Known(*id);
        }
        // Equal URL text can refer to different resources in different declarations. The native
        // image keeps its stylesheet context outside StyleValueData, so only an existing pointer
        // identity can prove equality for values containing resource-dependent images.
        if !crate::css::style_compute::value_may_need_style_sheet_resource_context(value)
            && let Some(index) = self
                .entries
                .find(value.content_hash(), |_index, entry| entry.value.data() == value)
        {
            return Lookup::Known(self.entries[index].id);
        }
        self.gap.map_or(Lookup::KnownAbsent, Lookup::Missing)
    }

    fn mark_partial(&mut self) -> SpecifiedValueGap {
        *self
            .gap
            .get_or_insert(SpecifiedValueGap::RetiredPayloads { eviction_generation: 1 })
    }

    /// Return the retained payload behind one maintained declaration identity.
    #[must_use]
    pub(super) fn value(&self, id: SpecifiedValueID) -> Lookup<&StyleValueData, SpecifiedValueGap> {
        if let Some(index) = self.entries_by_id.get(&id) {
            return Lookup::Known(self.entries[*index as usize].value.data());
        }
        self.gap.map_or(Lookup::KnownAbsent, Lookup::Missing)
    }

    /// Return an owned reference to one maintained declaration payload.
    #[must_use]
    #[cfg(test)]
    pub(super) fn retained_value(&self, id: SpecifiedValueID) -> Lookup<RetainedStyleValueData, SpecifiedValueGap> {
        if let Some(index) = self.entries_by_id.get(&id) {
            return Lookup::Known(self.entries[*index as usize].value.clone_retained());
        }
        self.gap.map_or(Lookup::KnownAbsent, Lookup::Missing)
    }

    pub(super) fn intern(&mut self, value: &Arc<StyleValueData>) -> (SpecifiedValueID, Lookup<(), SpecifiedValueGap>) {
        let lookup = match self.lookup(value) {
            Lookup::Known(id) => return (id, Lookup::Known(())),
            Lookup::KnownAbsent => Lookup::KnownAbsent,
            Lookup::Missing(gap) => Lookup::Missing(gap),
        };

        let id = SpecifiedValueID(self.next_id);
        self.next_id = self
            .next_id
            .checked_add(1)
            .expect("specified value identity space exhausted");
        if !true {
            return (id, Lookup::Missing(self.mark_partial()));
        }
        self.push_entry(id, RetainedStyleValueData::from_arc(Arc::clone(value)));

        (id, lookup)
    }

    /// Associate another immutable spelling with an existing canonical identity.
    pub(super) fn alias(&mut self, value: &Arc<StyleValueData>, id: SpecifiedValueID) {
        if let Lookup::Known(existing) = self.lookup(value) {
            debug_assert_eq!(existing, id);
            return;
        }
        if !true {
            return;
        }
        self.push_entry(id, RetainedStyleValueData::from_arc(Arc::clone(value)));
    }

    fn push_entry(&mut self, id: SpecifiedValueID, value: RetainedStyleValueData) {
        let pointer = value.pointer() as usize;
        let index = SpecifiedValueEntryIndex(
            u32::try_from(self.entries.len()).expect("specified value table exceeds u32 indexing"),
        );
        self.entries
            .insert(value.data().content_hash(), index, SpecifiedValueEntry { id, value });
        self.entries_by_pointer.insert(pointer, id);
        self.entries_by_id.entry(id).or_insert(index.0);
    }
}

#[cfg(test)]
#[allow(clippy::arc_with_non_send_sync)]
mod tests {
    use super::*;

    #[test]
    fn equal_content_dedups_across_distinct_spellings() {
        let mut values = SpecifiedValues::new();
        let originals: Vec<_> = (0..64)
            .map(|i| std::sync::Arc::new(StyleValueData::Number { value: f64::from(i) }))
            .collect();
        let ids: Vec<_> = originals.iter().map(|value| values.intern(value).0).collect();
        for (left, right) in ids.iter().zip(ids.iter().skip(1)) {
            assert_ne!(left, right);
        }
        let respellings: Vec<_> = (0..64)
            .map(|i| std::sync::Arc::new(StyleValueData::Number { value: f64::from(i) }))
            .collect();
        for (respelling, id) in respellings.iter().zip(ids.iter()) {
            let (reused, _) = values.intern(respelling);
            assert_eq!(reused, *id);
        }
    }

    #[test]
    fn authored_spellings_can_alias_a_canonical_identity() {
        let mut values = SpecifiedValues::new();
        let canonical = std::sync::Arc::new(StyleValueData::Number { value: 42.0 });
        let authored = std::sync::Arc::new(StyleValueData::Number { value: 43.0 });

        let (canonical_id, _) = values.intern(&canonical);
        values.alias(&authored, canonical_id);
        assert_eq!(values.entries_by_pointer.len(), 2);
        assert!(matches!(
            values.lookup(&authored),
            Lookup::Known(id) if id == canonical_id
        ));
        let (authored_id, lookup) = values.intern(&authored);

        assert_eq!(authored_id, canonical_id);
        assert!(matches!(lookup, Lookup::Known(())));
    }
}
