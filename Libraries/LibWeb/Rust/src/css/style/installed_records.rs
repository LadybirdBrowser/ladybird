/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The records the engine installed, by identity: each slot holds the record itself and what a view
//! of it says past its groups and its table, set once as it is installed. Installing a record
//! takes a slot and nothing else: no group, table or record is interned.
//!
//! A record stays in its slot while an element or pseudo-element is assigned it, a reader pins it
//! or anything beside the slot holds a reference to it, and its identity names it for as long: a slot freed and taken again names its next record
//! under a new generation.
//!
//! A record names one slot, but a fork of the render state and the state it forked each take slots
//! of their own, and share the records their passes kept. Where either takes a slot for a record
//! the other named after the fork, the name means nothing to it: it installs a copy of the record,
//! and the name is told from one of its own by whether its slot holds the record.
//!
//! An element holds the record it computed. The records of styles derived for what no element is, as
//! the anonymous boxes of a layout tree and the samples of animations, that hold the same values share
//! a slot: there is one of them per box, which nothing shares before they are made. They are found
//! by a hash of the longhands styles most often differ in, and told apart by comparing what they hold.

use super::fast_hash::FastSet as HashSet;
use super::pipeline::Record;
use std::hash::{Hash, Hasher};
use std::sync::Arc;

/// The slot of an installed record.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub(crate) struct InstalledRecordSlot(u32);

impl InstalledRecordSlot {
    fn index(self) -> usize {
        self.0 as usize
    }
}

/// An installed record: its slot, and the generation that slot is in, which no earlier or later
/// record held there shares.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) struct InstalledRecordID {
    slot: InstalledRecordSlot,
    generation: u32,
}

impl InstalledRecordID {
    /// The largest generation an identity carries: what fits in the header of the record the slot
    /// holds.
    pub(super) const MAX_GENERATION: u32 = u16::MAX as u32;
    /// The largest slot index an identity carries, as the header of a record holds it.
    pub(super) const MAX_SLOT: u32 = (1 << 24) - 2;

    pub(super) fn from_parts(slot: u32, generation: u32) -> Self {
        Self {
            slot: InstalledRecordSlot(slot),
            generation,
        }
    }

    pub(super) fn slot(self) -> u32 {
        self.slot.0
    }

    pub(super) fn generation(self) -> u32 {
        self.generation
    }
}

#[derive(Clone)]
pub(super) struct InstalledRecord {
    /// The record, which the host holds for the elements assigned it.
    record: Arc<Record>,
    /// The assignments and pins holding the record.
    holders: u32,
    /// The epoch of the sweeps' leases the record's last holder let go in.
    released_in: u32,
}

impl InstalledRecord {
    pub(super) fn record(&self) -> &Arc<Record> {
        &self.record
    }

    /// The identity of the custom-property environment the record was resolved in: that of the store it holds.
    pub(super) fn custom_property_environment(&self) -> u64 {
        self.record().custom_property_environment()
    }

    /// What a view of the record depends on.
    pub(super) fn dependency_flags(&self) -> u8 {
        self.record.view_dependency_flags()
    }
}

/// The slots of the installed records.
#[derive(Clone, Default)]
pub(super) struct InstalledRecords {
    slots: crate::fork_vec::ForkVec<Option<InstalledRecord>>,
    generations: crate::fork_vec::ForkVec<u32>,
    free: crate::fork_vec::ForkVec<InstalledRecordSlot>,
    /// The slots of the records derived for what no element is, by the key of what they hold.
    derived_by_content: crate::cow_map::CowMap<u64, InstalledRecordSlot>,
    /// The records whose last holder let go, which the next sweep frees unless one took them again.
    released: crate::fork::ForkShared<Vec<InstalledRecordID>>,
    /// How many leases on the records held as they are taken were taken.
    lease_epoch: u32,
}

impl InstalledRecords {
    /// Counts the slots and indexes toward the installed records, and the records they hold by
    /// owner.
    pub(super) fn report_memory(&self, report: &mut super::memory_report::MemoryReport) {
        use crate::heap_size::vec_bytes;
        for entry in self.slots.iter().flatten() {
            Record::report_memory(entry.record(), report);
        }
        report.add(
            super::memory_report::MemoryOwner::Installed,
            self.slots.row_bytes()
                + self.generations.row_bytes()
                + self.free.row_bytes()
                + self.derived_by_content.heap_size()
                + vec_bytes(&self.released),
        );
    }

    pub(super) fn get(&self, id: InstalledRecordID) -> Option<&InstalledRecord> {
        if self.generations.get(id.slot.index()) != Some(&id.generation) {
            return None;
        }
        self.slots[id.slot.index()].as_ref()
    }

    /// The identity `record` is installed under: that of the slot holding it, or a new slot, which
    /// the record names, or a copy of it where a fork's slot names it already. `dependency_flags`
    /// says what a new slot's view depends on.
    pub(super) fn install(&mut self, record: &Arc<Record>, dependency_flags: impl FnOnce() -> u8) -> InstalledRecordID {
        if let Some(id) = self.named_by(record) {
            return id;
        }
        let id = match self.free.pop() {
            Some(slot) => InstalledRecordID {
                slot,
                generation: self.generations[slot.index()],
            },
            None => {
                let slot = InstalledRecordSlot(
                    u32::try_from(self.slots.len())
                        .ok()
                        .filter(|&slot| slot <= InstalledRecordID::MAX_SLOT)
                        .expect("installed style record slot space exhausted"),
                );
                self.slots.push(None);
                self.generations.push(0);
                InstalledRecordID { slot, generation: 0 }
            }
        };
        let dependency_flags = dependency_flags();
        let record = match record.name_installed_record(id, dependency_flags) {
            true => record.clone(),
            false => {
                let copy = Arc::new(record.copy_without_a_name());
                assert!(copy.name_installed_record(id, dependency_flags));
                copy
            }
        };
        self.slots[id.slot.index()] = Some(InstalledRecord {
            record,
            holders: 0,
            released_in: self.lease_epoch,
        });
        // NB: A slot nothing holds yet is freed by the next sweep unless an assignment takes it.
        self.released.push(id);
        id
    }

    /// The identity the record `record`, derived for what no element is, is installed under: that of a slot holding
    /// the same, or a new slot as [`Self::install`] takes.
    pub(super) fn install_derived(
        &mut self,
        record: &Arc<Record>,
        dependency_flags: impl FnOnce() -> u8,
    ) -> InstalledRecordID {
        if let Some(id) = self.named_by(record) {
            return id;
        }
        // NB: The key reads a few longhands, so records that differ in others share it: each takes the next key free. A
        //     sweep can open a gap in such a run, which costs a record past it no more than a slot of its own.
        let mut content_key = Self::content_key(record);
        while let Some(&slot) = self.derived_by_content.get(&content_key) {
            if self.slots[slot.index()]
                .as_ref()
                .is_some_and(|entry| entry.record().holds_the_same_as(record))
            {
                return InstalledRecordID {
                    slot,
                    generation: self.generations[slot.index()],
                };
            }
            content_key = content_key.wrapping_add(1);
        }
        let id = self.install(record, dependency_flags);
        self.derived_by_content.insert(content_key, id.slot);
        id
    }

    /// The key a record is found under by what it holds: the content hashes of the values of the longhands styles most
    /// often differ in, which records holding the same values hash alike.
    fn content_key(record: &Record) -> u64 {
        use crate::css::computed_values::LonghandID;
        use crate::css::property_metadata::property_id as prop;
        let mut hasher = super::fast_hash::fast_hasher();
        for property in [
            prop::DISPLAY,
            prop::POSITION,
            prop::COLOR,
            prop::FONT_SIZE,
            prop::WIDTH,
            prop::HEIGHT,
            prop::MARGIN_TOP,
            prop::MARGIN_LEFT,
            prop::PADDING_TOP,
            prop::PADDING_LEFT,
            prop::BACKGROUND_COLOR,
            prop::TRANSFORM,
        ] {
            crate::css::style_value::style_value_content_hash(
                record.longhand(LonghandID::new(property)).optional_data(),
            )
            .hash(&mut hasher);
        }
        hasher.finish()
    }

    /// The identity of the slot holding `record`, where one here does. A record can name a slot of
    /// a fork instead, which here is free or holds another record.
    pub(super) fn named_by(&self, record: &Arc<Record>) -> Option<InstalledRecordID> {
        let id = record.installed_record()?;
        self.get(id)
            .is_some_and(|entry| Arc::ptr_eq(entry.record(), record))
            .then_some(id)
    }

    pub(super) fn hold(&mut self, id: InstalledRecordID) {
        let entry = self.slots[id.slot.index()]
            .as_mut()
            .filter(|_| self.generations[id.slot.index()] == id.generation)
            .expect("installed style record is live");
        entry.holders = entry
            .holders
            .checked_add(1)
            .expect("installed style record holder overflow");
    }

    pub(super) fn release(&mut self, id: InstalledRecordID) {
        let entry = self.slots[id.slot.index()]
            .as_mut()
            .filter(|_| self.generations[id.slot.index()] == id.generation)
            .expect("installed style record is live");
        entry.holders = entry
            .holders
            .checked_sub(1)
            .expect("installed style record holder underflow");
        if entry.holders == 0 {
            entry.released_in = self.lease_epoch;
            self.released.push(id);
        }
    }

    /// Whether anything holds the record.
    pub(super) fn is_held(&self, id: InstalledRecordID) -> bool {
        self.get(id).is_some_and(|entry| entry.holders != 0)
    }

    /// Begin the epoch of a lease on the records held now, which keeps those released from here on.
    pub(super) fn begin_lease_epoch(&mut self) -> u32 {
        // NB: One that saturates keeps what it released for as long as any lease of the epoch stands.
        self.lease_epoch = self.lease_epoch.saturating_add(1);
        self.lease_epoch
    }

    pub(super) fn released_count(&self) -> usize {
        self.released.len()
    }

    /// Free the records no holder took again since they were released, but for those of `kept`
    /// and those released in or after the epoch `oldest_lease`, and answer how many it freed.
    pub(super) fn sweep(&mut self, kept: &HashSet<InstalledRecordID>, oldest_lease: Option<u32>) -> usize {
        let mut freed = 0;
        // NB: The list keeps its room for the records released after the sweep.
        let mut released = std::mem::take(&mut self.released);
        released.retain(|&id| {
            let index = id.slot.index();
            if self.generations[index] != id.generation
                || self.slots[index].as_ref().is_none_or(|entry| entry.holders != 0)
            {
                return false;
            }
            // NB: A record the host, a reader or a fork sharing the slot still holds a reference to keeps its slot, so that
            //     its identity keeps naming it until the last reference beside the slot's goes.
            if kept.contains(&id)
                || self.slots.shares_row(index)
                || self.slots[index].as_ref().is_some_and(|entry| {
                    Arc::strong_count(entry.record()) > 1
                        || oldest_lease.is_some_and(|epoch| entry.released_in >= epoch)
                })
            {
                return true;
            }
            let entry = self.slots[index].take().expect("a released slot holds its record");
            entry.record().forget_installed_record();
            // NB: A slot whose generations ran out is never taken again.
            if self.generations[index] < InstalledRecordID::MAX_GENERATION {
                self.generations[index] += 1;
                self.free.push(id.slot);
            }
            freed += 1;
            false
        });
        self.released = released;
        if freed != 0 {
            let slots = &self.slots;
            self.derived_by_content.retain(|_, slot| slots[slot.index()].is_some());
        }
        freed
    }

    pub(super) fn iter(&self) -> impl Iterator<Item = &InstalledRecord> {
        self.slots.iter().flatten()
    }
}

#[cfg(test)]
mod tests {
    use super::super::pipeline::PseudoElementStyles;
    use super::*;

    /// A record of the initial values, told apart from others by `pseudo_element_styles`.
    fn record(pseudo_element_styles: u64) -> Arc<Record> {
        let initial = |property| crate::css::style_compute::initial_value_handle(property).clone();
        Arc::new(Record::of_test_values(initial))
            .with_pseudo_element_styles(PseudoElementStyles::from_bits(pseudo_element_styles))
    }

    fn holder_of(records: &InstalledRecords, id: InstalledRecordID) -> &Arc<Record> {
        records.get(id).expect("the slot holds a record").record()
    }

    #[test]
    fn derived_records_whose_content_shares_a_key_each_keep_a_slot() {
        let mut records = InstalledRecords::default();
        let first = records.install_derived(&record(1), || 0);
        let second = records.install_derived(&record(2), || 0);

        assert_ne!(first, second);
        assert_eq!(records.install_derived(&record(1), || 0), first);
        assert_eq!(records.install_derived(&record(2), || 0), second);
    }

    #[test]
    fn a_record_a_fork_named_first_takes_a_copy_in_a_slot_here() {
        let mut original = InstalledRecords::default();
        let shared = record(1);
        let mut fork = original.clone();

        let in_fork = fork.install(&shared, || 0);
        assert_eq!(original.named_by(&shared), None);
        let here = original.install(&shared, || 0);

        assert!(Arc::ptr_eq(holder_of(&fork, in_fork), &shared));
        assert_eq!(shared.installed_record(), Some(in_fork));
        let copy = holder_of(&original, here).clone();
        assert!(!Arc::ptr_eq(&copy, &shared));
        assert!(copy.holds_the_same_as(&shared));
        assert_eq!(original.named_by(&copy), Some(here));
    }

    #[test]
    fn a_record_the_original_named_after_a_fork_takes_a_copy_in_the_fork() {
        let mut original = InstalledRecords::default();
        let shared = record(1);
        let mut fork = original.clone();

        let here = original.install(&shared, || 0);
        let in_fork = fork.install(&shared, || 0);

        assert!(Arc::ptr_eq(holder_of(&original, here), &shared));
        assert_eq!(shared.installed_record(), Some(here));
        assert!(!Arc::ptr_eq(holder_of(&fork, in_fork), &shared));
    }

    #[test]
    fn a_name_from_a_fork_never_answers_for_the_record_in_that_slot_here() {
        let mut original = InstalledRecords::default();
        let shared = record(1);
        let mut fork = original.clone();
        let other = record(2);

        let in_fork = fork.install(&shared, || 0);
        let others = original.install(&other, || 0);
        assert_eq!(in_fork, others, "both sides take the same free slot");

        assert_eq!(original.named_by(&shared), None);
        let here = original.install(&shared, || 0);
        assert_ne!(here, others);
        assert!(Arc::ptr_eq(holder_of(&original, others), &other));
    }

    #[test]
    fn a_record_named_before_a_fork_keeps_its_slot_on_both_sides() {
        let mut original = InstalledRecords::default();
        let shared = record(1);
        let id = original.install(&shared, || 0);
        let mut fork = original.clone();

        assert_eq!(fork.install(&shared, || 0), id);
        assert_eq!(original.install(&shared, || 0), id);
        assert!(Arc::ptr_eq(holder_of(&fork, id), &shared));
    }
}
