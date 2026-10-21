/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Canonical specified-value identities retained as a dense table.

use crate::css::style_value::RetainedStyleValueData;
use crate::css::style_value::StyleValueData;
use std::sync::Arc;

use super::cascade::SpecifiedValueID;
use super::fast_hash::FastMap as HashMap;

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

/// An identity is never reused, so rule and winner rows can compare their opaque identities.
#[derive(Clone)]
pub(super) struct SpecifiedValues {
    entries: super::intern_table::InternTable<SpecifiedValueEntryIndex, SpecifiedValueEntry>,
    entries_by_pointer: HashMap<usize, SpecifiedValueID>,
    entries_by_id: HashMap<SpecifiedValueID, u32>,
    next_id: u64,
}

impl SpecifiedValues {
    pub(super) fn new() -> Self {
        Self {
            entries: super::intern_table::InternTable::default(),
            entries_by_pointer: HashMap::default(),
            entries_by_id: HashMap::default(),
            next_id: 1,
        }
    }

    #[must_use]
    fn lookup(&self, value: &StyleValueData) -> Option<SpecifiedValueID> {
        if let Some(id) = self.entries_by_pointer.get(&(std::ptr::from_ref(value) as usize)) {
            return Some(*id);
        }
        // Equal URL text can refer to different resources in different declarations. The native
        // image keeps its stylesheet context outside StyleValueData, so only an existing pointer
        // identity can prove equality for values containing resource-dependent images.
        if !crate::css::style_compute::value_may_need_style_sheet_resource_context(value)
            && let Some(index) = self
                .entries
                .find(value.content_hash(), |_index, entry| entry.value.data() == value)
        {
            return Some(self.entries[index].id);
        }
        None
    }

    /// Return the retained payload behind one maintained declaration identity.
    #[must_use]
    pub(super) fn value(&self, id: SpecifiedValueID) -> Option<&StyleValueData> {
        let index = self.entries_by_id.get(&id)?;
        Some(self.entries[*index as usize].value.data())
    }

    /// Return an owned reference to one maintained declaration payload.
    #[must_use]
    #[cfg(test)]
    pub(super) fn retained_value(&self, id: SpecifiedValueID) -> Option<RetainedStyleValueData> {
        let index = self.entries_by_id.get(&id)?;
        Some(self.entries[*index as usize].value.clone_retained())
    }

    /// The identity of `value`, and whether it already had one.
    pub(super) fn intern(&mut self, value: &Arc<StyleValueData>) -> (SpecifiedValueID, bool) {
        if let Some(id) = self.lookup(value) {
            return (id, true);
        }
        let id = SpecifiedValueID(self.next_id);
        self.next_id = self
            .next_id
            .checked_add(1)
            .expect("specified value identity space exhausted");
        self.push_entry(id, RetainedStyleValueData::from_arc(Arc::clone(value)));
        (id, false)
    }

    /// Associate another immutable spelling with an existing canonical identity.
    pub(super) fn alias(&mut self, value: &Arc<StyleValueData>, id: SpecifiedValueID) {
        if let Some(existing) = self.lookup(value) {
            debug_assert_eq!(existing, id);
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
        assert_eq!(values.lookup(&authored), Some(canonical_id));
        let (authored_id, reused) = values.intern(&authored);
        assert_eq!(authored_id, canonical_id);
        assert!(reused);
    }
}
