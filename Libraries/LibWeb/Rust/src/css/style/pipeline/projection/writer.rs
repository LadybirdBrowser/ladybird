/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The host's side of the projection: the rows, which the host writes as its DOM changes and nothing else writes, and
//! the catalogs that spell their atoms. At each barrier the host publishes them to the render owner's [`Projection`]:
//! a generation of the rows that shares every chunk the host does not write again (the copy-on-write spine of a
//! [`CowColumn`]), with the elements written since the last barrier. The render owner reads only what was published,
//! so the host's writes beside a transaction in flight stay out of it without a copy.
//!
//! An element that left the tree is retired and forgotten here as its departure is published: the generations
//! published before keep its facts, which is where the render owner reads what a departed element carried.

use super::super::super::fast_hash::FastMap as HashMap;
use super::super::super::fast_hash::FastSet as HashSet;
use super::super::super::index::AttributeFact;
use super::super::super::index::AttributeNameForms;
use super::super::super::index::StyleAtomID;
use super::super::super::input_types::StateFact;
use super::super::super::input_types::TreeRelations;
use super::super::super::instrumentation::Counter;
use super::super::super::instrumentation::Counters;
use super::super::super::tree::StyleNodeID;

use super::super::super::element_host_facts::HostFactSet;
use super::super::super::element_host_facts::HostFactSets;
use super::ArrivalFacts;
use super::Catalogs;
use super::ElementFacts;
use super::Links;
use super::ROWS_PER_CHUNK;
use super::Row;
use super::ShadowColumn;
use super::ShadowLinks;
use super::ShadowSnapshot;
use super::SharedList;
use super::WrittenRows;
use super::fact_lists::FactLists;
use super::fact_lists::RecentFactLists;
use super::fact_lists::{EditedList, FactListsEdit};
use super::list_bloom_of;
use super::relations_of;
use crate::cow_column::ColumnSnapshot;
use crate::cow_column::CowColumn;
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use smallvec::SmallVec;
use std::sync::Arc;

/// The counters a host's writes count, which a publication carries to the render owner's.
const WRITE_COUNTERS: [Counter; 8] = [
    Counter::RawMutationRecords,
    Counter::TreeDeltas,
    Counter::LocalFeatureDeltas,
    Counter::StateDeltas,
    Counter::ArrivingNodeFactsFolded,
    Counter::TreeDepartureDeltas,
    Counter::SlotAssignedNodeListsPublished,
    Counter::LanguageTextsPublished,
];

/// The rows of a document's elements as its host writes them.
#[derive(Clone, Default)]
pub(in crate::css::style) struct ProjectionWriter {
    rows: CowColumn<Row, ROWS_PER_CHUNK>,
    /// The elements' shadow relations, made only as far as the last element that held any.
    shadows: ShadowColumn,
    /// The rows as the host last published them.
    published: ColumnSnapshot<Row, ROWS_PER_CHUNK>,
    /// One past the greatest element slot written.
    slot_count: usize,
    catalogs: Catalogs,
    /// The distinct sets of host facts the rows name.
    host_fact_sets: Arc<HostFactSets>,
    recent_lists: RecentFactLists,
    /// A slotted text node's slot. Text nodes have no rows, and a slotted one is rare.
    text_assigned_slots: Arc<HashMap<StyleNodeID, StyleNodeID>>,
    /// The elements whose facts say they are disabled, which only the writes of their states and their departures move.
    disabled: Arc<HashSet<StyleNodeID>>,
    /// The elements whose rows were written since the last publication.
    written: WrittenRows,
    /// Of those, the elements whose links were written.
    links_written: WrittenRows,
    /// Of those, the elements whose own relations were written, rather than only their first element child.
    relations_written: WrittenRows,
    /// Whether the catalogs or the slots of text nodes were written since the last publication, which no row says.
    catalogs_written: bool,
    /// Whether the host facts of an element were written since the last publication, which the rows written do not
    /// list: no transaction takes them.
    host_facts_written: bool,
    /// The elements the host took out of the tree since the last publication.
    departures: Vec<StyleNodeID>,
    /// What the writes since the last publication counted.
    pub(in crate::css::style) counters: Counters,
}

/// What the host published of its rows at a barrier: the rows as written, which share every chunk with the host's
/// until it writes there again, with the catalogs, and the elements written since the barrier before.
pub(in crate::css::style) struct ProjectionPublication {
    pub(super) rows: ColumnSnapshot<Row, ROWS_PER_CHUNK>,
    pub(super) shadows: ShadowSnapshot,
    pub(super) slot_count: usize,
    pub(super) catalogs: Catalogs,
    pub(super) host_fact_sets: Arc<HostFactSets>,
    pub(super) text_assigned_slots: Arc<HashMap<StyleNodeID, StyleNodeID>>,
    pub(super) disabled: Arc<HashSet<StyleNodeID>>,
    pub(super) written: Vec<StyleNodeID>,
    pub(super) links_written: Vec<StyleNodeID>,
    pub(super) relations_written: Vec<StyleNodeID>,
    /// What the writes counted.
    pub(in crate::css::style) counts: SmallVec<[(Counter, u64); 4]>,
}

impl ProjectionPublication {
    /// How many elements' links the publication wrote, each of which may deepen the tree by a level.
    #[must_use]
    pub(in crate::css::style) fn link_rows_written(&self) -> usize {
        self.links_written.len()
    }

    /// Whether the publication moves elements in the tree, or their shadow relations.
    #[must_use]
    pub(in crate::css::style) fn moves_links(&self) -> bool {
        !self.links_written.is_empty()
    }
}

impl ProjectionWriter {
    /// Counts what the host's rows hold beside the projection: the chunks, shadow relations and
    /// catalogs it wrote since the barrier, as those it shares with the published projection are
    /// counted where they are seen first, and the lists of what it wrote. Like the generations a
    /// transaction in flight holds, how much that is depends on when the report runs.
    pub(in crate::css::style) fn report_memory(&self, report: &mut crate::css::style::memory_report::MemoryReport) {
        use crate::heap_size::map_bytes;
        let sizer = &mut report.sizer;
        let mut bytes = self.rows.heap_size(sizer, Row::heap_size)
            + self.shadows.heap_size(sizer, |shadow, sizer| {
                super::shadow_links_heap_size(shadow.as_ref(), sizer)
            })
            + self.published.heap_size(sizer, Row::heap_size);
        if sizer.arc_shallow(&self.host_fact_sets) != 0 {
            bytes += crate::heap_size::ARC_HEADER_BYTES + size_of::<HostFactSets>() + self.host_fact_sets.heap_size();
        }
        let catalogs = &self.catalogs;
        bytes += catalogs.name_forms.heap_size(sizer);
        bytes += catalogs.value_texts.heap_size(sizer);
        for texts in [&catalogs.substitution_names, &catalogs.language_texts] {
            bytes += texts.heap_size(sizer);
            bytes += texts.values().map(|text| size_of_val::<[u16]>(text)).sum::<usize>();
        }
        bytes += sizer.arc_shallow(&self.text_assigned_slots) + map_bytes(&self.text_assigned_slots);
        bytes += sizer.arc_shallow(&self.disabled) + crate::heap_size::set_bytes(&self.disabled);
        bytes += [&self.written, &self.links_written, &self.relations_written]
            .iter()
            .map(|rows| rows.heap_size())
            .sum::<usize>();
        bytes += crate::heap_size::vec_bytes(&self.departures);
        report.add(
            crate::css::style::memory_report::MemoryOwner::ProjectionGenerations,
            bytes,
        );
    }

    /// The writes the host makes, with what they mean.
    pub(in crate::css::style) fn writes(&mut self) -> super::super::super::projection_writes::ProjectionWrites<'_> {
        super::super::super::projection_writes::ProjectionWrites { projection: self }
    }

    /// Whether anything was written since the last publication.
    #[must_use]
    pub(in crate::css::style) fn has_unpublished_writes(&self) -> bool {
        !self.written.nodes.is_empty() || self.catalogs_written || self.host_facts_written
    }

    /// Publish the rows as written: the barrier the render owner takes them at. The elements that left the tree are
    /// retired and forgotten as their departure is published.
    pub(in crate::css::style) fn publish(&mut self) -> ProjectionPublication {
        let mut departed = std::mem::take(&mut self.departures);
        departed.retain(|&node| {
            self.row(node)
                .is_some_and(|row| row.live() && relations_of(row).is_none())
        });
        // NB: Nothing routes from a departed element's facts but the generations published before, which keep them.
        for &node in &departed {
            self.retire(node);
            self.note_disabled(node, false);
            if let Some(mut row) = self.rows.row_mut(node.element_slot()) {
                row.facts = row.facts.forgotten();
            }
        }
        let rows = self.rows.publish();
        self.catalogs_written = false;
        self.host_facts_written = false;
        let counts = WRITE_COUNTERS
            .into_iter()
            .filter_map(|counter| {
                let count = self.counters.get(counter);
                self.counters.set(counter, 0);
                (count != 0).then_some((counter, count))
            })
            .collect();
        let publication = ProjectionPublication {
            rows: rows.clone(),
            shadows: self.shadows.publish(),
            slot_count: self.slot_count,
            catalogs: self.catalogs.clone(),
            host_fact_sets: Arc::clone(&self.host_fact_sets),
            text_assigned_slots: Arc::clone(&self.text_assigned_slots),
            disabled: Arc::clone(&self.disabled),
            written: self.written.take(),
            links_written: self.links_written.take(),
            relations_written: self.relations_written.take(),
            counts,
        };
        self.published = rows;
        publication
    }

    /// How many times the rows were written where an edit changed them, which a test reads to see that an edit that
    /// changes nothing copies no chunk.
    #[cfg(test)]
    pub(super) fn rows_version(&self) -> u64 {
        self.rows.version()
    }

    fn row(&self, node: StyleNodeID) -> Option<&Row> {
        self.rows.get(node.element_slot())
    }

    fn note_written(&mut self, node: StyleNodeID, slot: usize) {
        self.written.insert(node, slot);
    }

    fn note_links_written(&mut self, node: StyleNodeID) {
        self.links_written.insert(node, node.element_slot());
    }

    /// Note whether the facts of `node` say it is disabled.
    fn note_disabled(&mut self, node: StyleNodeID, disabled: bool) {
        if self.disabled.contains(&node) == disabled {
            return;
        }
        let elements = Arc::make_mut(&mut self.disabled);
        if disabled {
            elements.insert(node);
        } else {
            elements.remove(&node);
        }
    }

    /// The element's facts as written, or none where nothing was written of it.
    #[must_use]
    pub(in crate::css::style) fn written_facts(&self, node: StyleNodeID) -> Option<&ElementFacts> {
        self.row(node).map(|row| &row.facts).filter(|facts| facts.resident())
    }

    /// How the element stands in the tree as written, or none where it stands in no tree.
    #[must_use]
    pub(in crate::css::style) fn written_relations(&self, node: StyleNodeID) -> Option<TreeRelations> {
        self.row(node).and_then(relations_of)
    }

    /// Whether the relations of `node` were written since the last publication.
    #[must_use]
    pub(in crate::css::style) fn relations_were_written(&self, node: StyleNodeID) -> bool {
        node.element_index().is_some() && self.relations_written.contains(node.element_slot())
    }

    /// Whether `node` arrives in the publication to come: it stood in no tree as last published, and stands in one as
    /// written.
    #[must_use]
    pub(in crate::css::style) fn arrival_is_pending(&self, node: StyleNodeID) -> bool {
        node.element_index().is_some()
            && self.published.get(node.element_slot()).and_then(relations_of).is_none()
            && self.written_relations(node).is_some()
    }

    #[must_use]
    pub(in crate::css::style) fn shadow_root_of(&self, host: StyleNodeID) -> Option<StyleNodeID> {
        self.shadow(host)?.shadow_root
    }

    #[must_use]
    pub(in crate::css::style) fn host_of(&self, shadow_root: StyleNodeID) -> Option<StyleNodeID> {
        self.shadow(shadow_root)?.host
    }

    fn assigned_nodes_of(&self, slot: StyleNodeID) -> &[StyleNodeID] {
        self.shadow(slot).map_or(&[], |shadow| shadow.assigned_nodes.as_slice())
    }

    /// Edit the row of an element, making room for it first.
    fn edit<R>(&mut self, node: StyleNodeID, edit: impl FnOnce(&mut Row) -> R) -> R {
        let slot = node.element_slot();
        assert!(node.element_index().is_some(), "only an element has a projection row");
        self.note_written(node, slot);
        self.rows.grow_to(slot + 1);
        self.slot_count = self.slot_count.max(slot + 1);
        let mut row = self.rows.row_mut(slot).expect("the row was just made room for");
        let answer = edit(&mut row);
        drop(row);
        answer
    }

    fn edit_facts(&mut self, node: StyleNodeID, edit: impl FnOnce(&mut ElementFacts)) {
        let slot = node.element_slot();
        assert!(node.element_index().is_some(), "only an element has a projection row");
        self.note_written(node, slot);
        self.rows.grow_to(slot + 1);
        self.slot_count = self.slot_count.max(slot + 1);
        let mut row = self.rows.row_mut(slot).expect("the row was just made room for");
        row.facts.set_flag(ElementFacts::RESIDENT, true);
        edit(&mut row.facts);
        row.facts.refresh_dispatch_bloom();
        drop(row);
    }

    /// Edit the fact lists of an element as its links move, which `edit` says whether it changed, without publishing a
    /// fact of it: its tree scope and assigned slot live in its lists, but say nothing of the element itself.
    fn edit_lists_quietly(&mut self, node: StyleNodeID, edit: impl FnOnce(&mut FactListsEdit) -> bool) {
        let empty = FactLists::default();
        let mut lists = self
            .rows
            .get(node.element_slot())
            .map_or_else(|| empty.edit(), |row| row.facts.lists.edit());
        if !edit(&mut lists) {
            return;
        }
        let lists = Self::make_lists(&mut self.recent_lists, &self.catalogs.name_forms, lists);
        self.edit(node, |row| row.facts.lists = lists);
    }

    /// Edit the fact lists of an element, which `edit` says whether it changed. Lists made the same
    /// as ones made recently share their allocation.
    fn edit_lists(&mut self, node: StyleNodeID, edit: impl FnOnce(&mut FactListsEdit) -> bool) {
        let empty = FactLists::default();
        let mut lists = self
            .rows
            .get(node.element_slot())
            .map_or_else(|| empty.edit(), |row| row.facts.lists.edit());
        if !edit(&mut lists) {
            self.edit_facts(node, |_| {});
            return;
        }
        let lists = Self::make_lists(&mut self.recent_lists, &self.catalogs.name_forms, lists);
        self.edit_facts(node, |facts| facts.lists = lists);
    }

    /// Fact lists holding what `lists` holds, with the Bloom summary of their dispatch keys, shared with any made the
    /// same.
    fn make_lists(
        recent_lists: &mut RecentFactLists,
        name_forms: &super::Catalog<AttributeNameForms>,
        mut lists: FactListsEdit,
    ) -> FactLists {
        lists.bloom = list_bloom_of(&lists, name_forms);
        recent_lists.make(&lists, &crate::render_state::process_style().fact_lists)
    }

    fn shadow(&self, node: StyleNodeID) -> Option<&ShadowLinks> {
        self.shadows.get(node.element_slot())?.as_deref()
    }

    fn edit_shadow<R>(&mut self, node: StyleNodeID, edit: impl FnOnce(&mut ShadowLinks) -> R) -> R {
        // NB: The element's row is written too, so that the publication lists the element.
        self.edit(node, |_| ());
        let slot = node.element_slot();
        let mut shadow = self.shadow(node).cloned().unwrap_or_default();
        let result = edit(&mut shadow);
        let shadow = (shadow != ShadowLinks::default()).then(|| Arc::new(shadow));
        if shadow.is_some() || self.shadows.get(slot).is_some_and(Option::is_some) {
            self.shadows.grow_to(slot + 1);
            self.shadows.set(slot, shadow);
        }
        result
    }

    /// Take the element's shadow relations, leaving it none.
    fn take_shadow(&mut self, node: StyleNodeID) -> Option<Arc<ShadowLinks>> {
        let slot = node.element_slot();
        let shadow = self.shadows.get(slot).cloned().flatten()?;
        self.shadows.set(slot, None);
        Some(shadow)
    }

    /// Make the element identity `node` live, with no links yet.
    pub(in crate::css::style) fn mint(&mut self, node: StyleNodeID) {
        self.edit(node, |row| {
            assert!(!row.live(), "minting an element identity that is live");
            row.links = Links::default();
            row.facts.set_flag(ElementFacts::LIVE, true);
            row.facts.set_flag(ElementFacts::CONNECTED, false);
            // NB: A departed element keeps its host facts until its identity is minted again, which may hand it to a
            //     shadow root or the document, which publish none of their own.
            row.facts.host_facts = Default::default();
        });
    }

    /// Retire the element identity `node`, dropping its links and every shadow relation that names
    /// it. Its facts stay until [`Self::forget`].
    pub(in crate::css::style) fn retire(&mut self, node: StyleNodeID) {
        let slot = self.edit(node, |row| {
            assert!(row.live(), "retiring an identity that is not live");
            row.links = Links::default();
            row.facts.set_flag(ElementFacts::LIVE, false);
            row.facts.set_flag(ElementFacts::CONNECTED, false);
            row.facts.lists.assigned_slot()
        });
        if slot.is_some() {
            self.edit_lists_quietly(node, |lists| std::mem::take(&mut lists.assigned_slot).is_some());
        }
        let shadow = self.take_shadow(node);
        if let Some(slot) = slot {
            self.remove_assigned_node(slot, node);
        }
        let Some(shadow) = shadow else {
            return;
        };
        for &assigned in shadow.assigned_nodes.as_slice() {
            if assigned.text_index().is_some() {
                if self.text_assigned_slots.get(&assigned) == Some(&node) {
                    Arc::make_mut(&mut self.text_assigned_slots).remove(&assigned);
                }
            } else if self.row(assigned).and_then(|row| row.facts.lists.assigned_slot()) == Some(node) {
                self.edit_lists_quietly(assigned, |lists| std::mem::take(&mut lists.assigned_slot).is_some());
            }
        }
        if let Some(root) = shadow.shadow_root {
            self.edit_shadow(root, |links| links.host = None);
        }
        if let Some(host) = shadow.host {
            self.edit_shadow(host, |links| links.shadow_root = None);
        }
    }

    /// Retire text identities, dropping them from the slot each was assigned to.
    pub(in crate::css::style) fn retire_texts(&mut self, nodes: impl IntoIterator<Item = StyleNodeID>) {
        for node in nodes {
            let Some(slot) = Arc::make_mut(&mut self.text_assigned_slots).remove(&node) else {
                continue;
            };
            self.remove_assigned_node(slot, node);
        }
    }

    fn remove_assigned_node(&mut self, slot: StyleNodeID, node: StyleNodeID) {
        if !self.assigned_nodes_of(slot).contains(&node) {
            return;
        }
        self.edit_shadow(slot, |links| {
            let remaining: Vec<_> = links
                .assigned_nodes
                .as_slice()
                .iter()
                .copied()
                .filter(|&assigned| assigned != node)
                .collect();
            links.assigned_nodes = SharedList::new(&remaining);
        });
    }

    #[cfg(test)]
    /// Drop the facts of an element that departed, now that nothing can still route from them.
    /// They leave what style reads when the transaction is released.
    pub(in crate::css::style) fn forget(&mut self, node: StyleNodeID) {
        if self.row(node).is_some_and(|row| row.facts.resident()) {
            self.note_disabled(node, false);
            self.edit(node, |row| row.facts = row.facts.forgotten());
        }
    }

    /// Give a node a row without publishing a fact of it, as a shadow root, which is featureless,
    /// still has to be readable.
    pub(in crate::css::style) fn ensure_facts(&mut self, node: StyleNodeID) {
        self.edit_facts(node, |_| {});
    }

    pub(in crate::css::style) fn set_tag(&mut self, node: StyleNodeID, tag: StyleAtomID) {
        self.edit_facts(node, |facts| facts.tag = tag);
    }

    /// A folding equal to the name itself carries no information and is dropped.
    pub(in crate::css::style) fn set_folded_tag(&mut self, node: StyleNodeID, folded: StyleAtomID) {
        let tag = self.row(node).map_or(StyleAtomID::NONE, |row| row.facts.tag);
        self.edit_lists(node, |lists| {
            let folded = if folded == tag { StyleAtomID::NONE } else { folded };
            std::mem::replace(&mut lists.folded_tag, folded) != folded
        });
    }

    #[cfg(test)]
    pub(in crate::css::style) fn set_namespace(&mut self, node: StyleNodeID, namespace: StyleAtomID) {
        self.edit_lists(node, |lists| {
            std::mem::replace(&mut lists.namespace, namespace) != namespace
        });
    }

    pub(in crate::css::style) fn set_id(&mut self, node: StyleNodeID, id: StyleAtomID) {
        self.edit_lists(node, |lists| std::mem::replace(&mut lists.id, id) != id);
    }

    pub(in crate::css::style) fn set_state(&mut self, node: StyleNodeID, fact: StateFact, value: bool) {
        if fact == StateFact::Disabled {
            self.note_disabled(node, value);
        }
        self.edit_facts(node, |facts| match value {
            true => facts.states.insert(fact),
            false => facts.states.remove(fact),
        });
    }

    pub(in crate::css::style) fn set_language(&mut self, node: StyleNodeID, language: StyleAtomID) {
        self.edit_lists(node, |lists| {
            std::mem::replace(&mut lists.language, language) != language
        });
    }

    pub(in crate::css::style) fn set_directionality(&mut self, node: StyleNodeID, directionality: StyleAtomID) {
        self.edit_lists(node, |lists| {
            std::mem::replace(&mut lists.directionality, directionality) != directionality
        });
    }

    pub(in crate::css::style) fn set_part_exposure(&mut self, node: StyleNodeID, exposure: StyleAtomID) {
        self.edit_lists(node, |lists| {
            std::mem::replace(&mut lists.part_exposure, exposure) != exposure
        });
    }

    /// The host facts of the element as written.
    #[must_use]
    pub(in crate::css::style) fn written_host_facts(&self, node: StyleNodeID) -> HostFactSet {
        let id = self.row(node).map(|row| row.facts.host_facts).unwrap_or_default();
        self.host_fact_sets.get(id)
    }

    /// Change the host facts of the element with `edit`, without noting its row written for a transaction, but where
    /// the element-backed pseudo-element it stands for moves, which is a key of the element: the host facts change
    /// nothing a transaction reads. Answers the facts as they were.
    pub(in crate::css::style) fn edit_host_facts(
        &mut self,
        node: StyleNodeID,
        edit: impl FnOnce(&mut HostFactSet),
    ) -> HostFactSet {
        assert!(node.element_index().is_some(), "only an element has host facts");
        let previous = self.written_host_facts(node);
        let mut set = previous;
        edit(&mut set);
        if set == previous {
            return previous;
        }
        let id = match self.host_fact_sets.id_of(&set) {
            Some(id) => id,
            None => Arc::make_mut(&mut self.host_fact_sets).intern(set),
        };
        let slot = node.element_slot();
        self.rows.grow_to(slot + 1);
        self.slot_count = self.slot_count.max(slot + 1);
        self.rows
            .row_mut(slot)
            .expect("the row was just made room for")
            .facts
            .host_facts = id;
        self.host_facts_written = true;
        if previous.associated_pseudo_kind() != set.associated_pseudo_kind() {
            self.note_written(node, slot);
        }
        previous
    }

    pub(in crate::css::style) fn set_heading_level(&mut self, node: StyleNodeID, level: u8) {
        self.edit_facts(node, |facts| facts.heading_level = level);
    }

    pub(in crate::css::style) fn set_has_text_content(&mut self, node: StyleNodeID, has_text_content: bool) {
        self.edit_facts(node, |facts| {
            facts.set_flag(ElementFacts::HAS_TEXT_CONTENT, has_text_content);
        });
    }

    pub(in crate::css::style) fn set_class(&mut self, node: StyleNodeID, class: StyleAtomID, present: bool) {
        self.edit_lists(node, |lists| {
            match (present, lists.classes.binary_search(&class)) {
                (true, Err(index)) => lists.classes.to_mut().insert(index, class),
                (false, Ok(index)) => {
                    lists.classes.to_mut().remove(index);
                }
                _ => return false,
            }
            true
        });
    }

    pub(in crate::css::style) fn set_attribute(
        &mut self,
        node: StyleNodeID,
        name: StyleAtomID,
        value: StyleAtomID,
        present: bool,
    ) {
        self.edit_lists(node, |lists| {
            let attributes = &mut lists.attributes;
            match (
                present,
                attributes.binary_search_by_key(&name, |attribute| attribute.name),
            ) {
                (true, Ok(index)) if attributes[index].value == value => return false,
                (true, Ok(index)) => attributes.to_mut()[index].value = value,
                (true, Err(index)) => attributes.to_mut().insert(index, AttributeFact { name, value }),
                (false, Ok(index)) => {
                    attributes.to_mut().remove(index);
                }
                (false, Err(_)) => return false,
            }
            true
        });
    }

    /// Write every fact an element arrives with in one edit of its row: the facts it names are
    /// set, its classes and attributes join any its row holds, and its states join those set.
    pub(in crate::css::style) fn write_arrival_facts(&mut self, node: StyleNodeID, arrival: &ArrivalFacts<'_>) {
        let empty = FactLists::default();
        let mut lists = self
            .rows
            .get(node.element_slot())
            .map_or_else(|| empty.edit(), |row| row.facts.lists.edit());
        for &class in arrival.classes {
            if let Err(index) = lists.classes.binary_search(&class) {
                lists.classes.to_mut().insert(index, class);
            }
        }
        for &attribute in arrival.attributes {
            match lists
                .attributes
                .binary_search_by_key(&attribute.name, |written| written.name)
            {
                Ok(index) => lists.attributes.to_mut()[index].value = attribute.value,
                Err(index) => lists.attributes.to_mut().insert(index, attribute),
            }
        }
        lists.custom_states = EditedList::Held(arrival.custom_states);
        lists.namespace = arrival.namespace;
        if !arrival.id.is_none() {
            lists.id = arrival.id;
        }
        if !arrival.language.is_none() {
            lists.language = arrival.language;
        }
        if !arrival.directionality.is_none() {
            lists.directionality = arrival.directionality;
        }
        lists.folded_tag = match arrival.folded_tag == arrival.tag {
            true => StyleAtomID::NONE,
            false => arrival.folded_tag,
        };
        let lists = Self::make_lists(&mut self.recent_lists, &self.catalogs.name_forms, lists);
        if arrival.states.contains(StateFact::Disabled) {
            self.note_disabled(node, true);
        }
        self.edit_facts(node, |facts| {
            facts.tag = arrival.tag;
            facts.set_flag(ElementFacts::IS_SLOT, arrival.is_slot);
            facts.set_flag(ElementFacts::HAS_TEXT_CONTENT, arrival.has_text_content);
            facts.states.0 |= arrival.states.0;
            if arrival.heading_level != 0 {
                facts.heading_level = arrival.heading_level;
            }
            facts.lists = lists;
        });
    }

    pub(in crate::css::style) fn set_parts(&mut self, node: StyleNodeID, parts: &[StyleAtomID]) {
        self.edit_lists(node, |lists| {
            if &*lists.parts == parts {
                return false;
            }
            lists.parts = EditedList::Changed(SmallVec::from_slice(parts));
            true
        });
    }

    pub(in crate::css::style) fn set_custom_states(&mut self, node: StyleNodeID, states: &[StyleAtomID]) {
        self.edit_lists(node, |lists| {
            if &*lists.custom_states == states {
                return false;
            }
            lists.custom_states = EditedList::Changed(SmallVec::from_slice(states));
            true
        });
    }

    /// Record the other names an attribute name answers to, where the two atoms are minted.
    pub(in crate::css::style) fn note_attribute_name_forms(&mut self, name: StyleAtomID, forms: AttributeNameForms) {
        if self.catalogs.name_forms.get(&name) != Some(&forms) {
            self.catalogs.name_forms.insert(name, forms);
            self.catalogs_written = true;
        }
    }

    /// Record the local name an `attr()` reads the attribute `name` by, which is in no namespace.
    pub(in crate::css::style) fn note_attribute_substitution_name(&mut self, name: StyleAtomID, local_name: &[u16]) {
        if self.catalogs.substitution_names.get(&name).map(AsRef::as_ref) != Some(local_name) {
            self.catalogs.substitution_names.insert(name, local_name.into());
            self.catalogs_written = true;
        }
    }

    /// Record what one attribute-value atom spells. The first spelling stands for as long as the
    /// atom does.
    pub(in crate::css::style) fn set_attribute_value_text(&mut self, value: StyleAtomID, text: RetainedUtf16FlyString) {
        if !value.is_none() && !self.catalogs.value_texts.contains_key(&value) {
            self.catalogs.value_texts.insert(value, text);
            self.catalogs_written = true;
        }
    }

    /// Record what one language atom spells. The first spelling stands for as long as the atom
    /// does.
    pub(in crate::css::style) fn set_language_text(&mut self, language: StyleAtomID, text: &[u16]) {
        if !language.is_none() && !self.catalogs.language_texts.contains_key(&language) {
            self.catalogs.language_texts.insert(language, text.into());
            self.catalogs_written = true;
        }
    }

    /// Drop everything the catalogs hold of atoms that are reclaimed, before an identity can be
    /// reused to spell something else.
    pub(in crate::css::style) fn forget_atoms(&mut self, atoms: &[StyleAtomID]) {
        self.catalogs.name_forms.forget(atoms);
        self.catalogs.substitution_names.forget(atoms);
        self.catalogs.value_texts.forget(atoms);
        self.catalogs.language_texts.forget(atoms);
    }

    /// Edit the relations of an element, which are all its links but its first element child.
    fn edit_links(&mut self, node: StyleNodeID, edit: impl FnOnce(&mut Row)) {
        self.relations_written.insert(node, node.element_slot());
        self.edit_any_links(node, edit);
    }

    fn edit_any_links(&mut self, node: StyleNodeID, edit: impl FnOnce(&mut Row)) {
        self.note_links_written(node);
        self.edit(node, |row| {
            assert!(row.live(), "linking an identity that is not live");
            edit(row);
        });
    }

    #[cfg(test)]
    pub(in crate::css::style) fn set_parent(&mut self, node: StyleNodeID, parent: Option<StyleNodeID>) {
        self.edit_links(node, |row| row.links.parent = parent);
    }

    pub(in crate::css::style) fn set_first_element_child(&mut self, node: StyleNodeID, child: Option<StyleNodeID>) {
        self.edit_any_links(node, |row| row.links.first_element_child = child);
    }

    #[cfg(test)]
    pub(in crate::css::style) fn set_previous_element_sibling(
        &mut self,
        node: StyleNodeID,
        sibling: Option<StyleNodeID>,
    ) {
        self.edit_links(node, |row| row.links.previous_element_sibling = sibling);
    }

    #[cfg(test)]
    pub(in crate::css::style) fn set_next_element_sibling(&mut self, node: StyleNodeID, sibling: Option<StyleNodeID>) {
        self.edit_links(node, |row| row.links.next_element_sibling = sibling);
    }

    #[cfg(test)]
    pub(in crate::css::style) fn set_connected(&mut self, node: StyleNodeID, connected: bool) {
        self.edit_links(node, |row| row.facts.set_flag(ElementFacts::CONNECTED, connected));
    }

    #[cfg(test)]
    /// Assign an element to `slot`, or to no slot.
    pub(in crate::css::style) fn set_assigned_slot(&mut self, node: StyleNodeID, slot: Option<StyleNodeID>) {
        self.note_links_written(node);
        self.relations_written.insert(node, node.element_slot());
        self.edit_lists_quietly(node, |lists| std::mem::replace(&mut lists.assigned_slot, slot) != slot);
    }

    /// Write how an element stands in the tree, or that it stands in none, leaving the links of
    /// its neighbors to their own writes. An element leaving keeps its tree scope.
    ///
    /// The write is noted even where the row holds these relations already: the host moved them,
    /// and an element it took out of a tree has to be settled as departed.
    pub(in crate::css::style) fn write_relations(&mut self, node: StyleNodeID, relations: Option<TreeRelations>) {
        let mut departed = false;
        self.edit_links(node, |row| {
            let links = &mut row.links;
            match relations {
                Some(relations) => {
                    links.parent = relations.parent;
                    links.previous_element_sibling = relations.previous_element_sibling;
                    links.next_element_sibling = relations.next_element_sibling;
                }
                None => {
                    departed = true;
                    links.parent = None;
                    links.previous_element_sibling = None;
                    links.next_element_sibling = None;
                }
            }
            row.facts.set_flag(ElementFacts::CONNECTED, relations.is_some());
        });
        // NB: An element leaving keeps its tree scope.
        let (tree_scope, assigned_slot) = match relations {
            Some(relations) => (Some(relations.tree_scope), relations.assigned_slot),
            None => (None, None),
        };
        self.edit_lists_quietly(node, |lists| {
            let scope_moved = tree_scope.is_some_and(|scope| std::mem::replace(&mut lists.tree_scope, scope) != scope);
            let slot_moved = std::mem::replace(&mut lists.assigned_slot, assigned_slot) != assigned_slot;
            scope_moved || slot_moved
        });
        if departed {
            self.departures.push(node);
        }
    }

    /// Record that `host` hosts `shadow_root`, unpairing whichever root or host either had before.
    pub(in crate::css::style) fn set_shadow_root(&mut self, host: StyleNodeID, shadow_root: StyleNodeID) {
        let previous_root = self.edit_shadow(host, |links| links.shadow_root.replace(shadow_root));
        if let Some(previous_root) = previous_root
            && previous_root != shadow_root
            && self.host_of(previous_root) == Some(host)
        {
            self.edit_shadow(previous_root, |links| links.host = None);
        }
        let previous_host = self.edit_shadow(shadow_root, |links| links.host.replace(host));
        if let Some(previous_host) = previous_host
            && previous_host != host
            && self.shadow_root_of(previous_host) == Some(shadow_root)
        {
            self.edit_shadow(previous_host, |links| links.shadow_root = None);
        }
    }

    /// Replace the ordered list of nodes `slot` has assigned to it. An element's own slot arrives
    /// with its links; a text node has none, so this is where its slot comes from.
    pub(in crate::css::style) fn set_assigned_nodes(&mut self, slot: StyleNodeID, nodes: &[StyleNodeID]) {
        if nodes == self.assigned_nodes_of(slot) && nodes.is_empty() {
            return;
        }
        let previous: Vec<_> = self.assigned_nodes_of(slot).to_vec();
        let text_slots = Arc::make_mut(&mut self.text_assigned_slots);
        // A tree-wide assignment can have moved a departing text node to another slot already,
        // and this slot must not take that newer assignment away again.
        for node in &previous {
            if text_slots.get(node) == Some(&slot) {
                text_slots.remove(node);
            }
        }
        for &node in nodes {
            if node.text_index().is_some() {
                text_slots.insert(node, slot);
            }
        }
        self.edit_shadow(slot, |links| links.assigned_nodes = SharedList::new(nodes));
    }

    pub(in crate::css::style) fn set_part_hosts(&mut self, node: StyleNodeID, pairs: &[(StyleAtomID, StyleNodeID)]) {
        self.edit_shadow(node, |links| links.part_hosts = SharedList::new(pairs));
    }
}
