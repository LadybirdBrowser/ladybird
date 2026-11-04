/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The facts the host publishes of each element, which style reads beside its records.
//!
//! The host states them with each element's arrival and whenever they change. They are not style:
//! no record holds them, and an element keeps them while its record is replaced or dropped. Few
//! elements differ in them (the box they ask for and what they are, as the adjustments and the
//! layout rows read it), so each distinct set is held once, and each element's projection row
//! holds a small identity of its set.

use super::HashMap;
use super::bridge::ElementBoxKind;

/// The facts the host publishes of one element.
#[derive(Clone, Copy, Default, PartialEq, Eq, Hash)]
pub(in crate::css::style) struct HostFactSet {
    /// The element facts the style computation's adjustments read; see
    /// `bridge::element_adjustment_fact`.
    pub(in crate::css::style) adjustment_facts: u32,
    /// The element facts a layout row built for the element records; see
    /// `bridge::element_construction_fact`.
    pub(in crate::css::style) construction_facts: u32,
    /// The principal box the element asks for.
    pub(in crate::css::style) box_kind: ElementBoxKind,
    /// One plus the kind of the element-backed pseudo-element of its shadow host the element stands for, or zero
    /// where it stands for none.
    pub(in crate::css::style) associated_pseudo_kind_plus_one: u8,
}

impl HostFactSet {
    #[must_use]
    pub(in crate::css::style) fn associated_pseudo_kind(&self) -> Option<u8> {
        self.associated_pseudo_kind_plus_one.checked_sub(1)
    }
}

/// The identity of a distinct set of host facts. Identity zero is the empty set.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub(in crate::css::style) struct HostFactSetID(u16);

/// The distinct sets of host facts of a document, which every element holding one shares.
#[derive(Clone)]
pub(in crate::css::style) struct HostFactSets {
    sets: Vec<HostFactSet>,
    ids: HashMap<HostFactSet, HostFactSetID>,
}

impl Default for HostFactSets {
    fn default() -> Self {
        let mut ids = HashMap::default();
        ids.insert(HostFactSet::default(), HostFactSetID::default());
        Self {
            sets: vec![HostFactSet::default()],
            ids,
        }
    }
}

impl HostFactSets {
    #[must_use]
    pub(in crate::css::style) fn get(&self, id: HostFactSetID) -> HostFactSet {
        self.sets[usize::from(id.0)]
    }

    /// The identity of `set`, where it is among the sets held.
    #[must_use]
    pub(in crate::css::style) fn id_of(&self, set: &HostFactSet) -> Option<HostFactSetID> {
        self.ids.get(set).copied()
    }

    /// The identity of `set`, among the sets held so far, or a new one.
    pub(in crate::css::style) fn intern(&mut self, set: HostFactSet) -> HostFactSetID {
        if let Some(&id) = self.ids.get(&set) {
            return id;
        }
        // NB: A set is a combination of the bits of host enums, never of web content, so a document holds few.
        let id = HostFactSetID(u16::try_from(self.sets.len()).expect("distinct element host fact sets fit in u16"));
        self.sets.push(set);
        self.ids.insert(set, id);
        id
    }

    pub(in crate::css::style) fn heap_size(&self) -> usize {
        crate::heap_size::vec_bytes(&self.sets) + crate::heap_size::map_bytes(&self.ids)
    }
}
