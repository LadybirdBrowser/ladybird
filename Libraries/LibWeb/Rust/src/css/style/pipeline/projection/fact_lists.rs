/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The facts of an element beside its tag and states: its id, namespace, language,
//! directionality, tree scope and assigned slot, and its lists of classes, attributes, parts and
//! custom states. They are packed into one allocation behind one pointer, which holds only what the
//! element has and which a row holding none of them leaves null. The process interns them by
//! content: siblings and cousins published alike point at one allocation, in every generation of
//! the rows.

use super::super::super::index::AttributeFact;
use super::super::super::index::StyleAtomID;
use crate::css::style::tree::{StyleNodeID, TreeScopeID};
use smallvec::SmallVec;
use std::alloc::Layout;
use std::ptr::NonNull;
use std::sync::atomic::AtomicU32;
use std::sync::atomic::Ordering;
use std::sync::atomic::fence;

/// Which optional atoms and lists an allocation holds.
#[derive(Clone, Copy, Default, PartialEq, Eq)]
struct Present(u16);

impl Present {
    // Atoms, in the order their words follow the header.
    const FOLDED_TAG: u16 = 1 << 0;
    const PART_EXPOSURE: u16 = 1 << 1;
    const NAMESPACE: u16 = 1 << 2;
    const ID: u16 = 1 << 3;
    const LANGUAGE: u16 = 1 << 4;
    const DIRECTIONALITY: u16 = 1 << 5;
    const TREE_SCOPE: u16 = 1 << 6;
    const ASSIGNED_SLOT: u16 = 1 << 7;
    const ATOMS: u16 = 0xff;
    // Lists, whose counts follow the atoms' words, in this order, and whose payloads follow the counts.
    const CLASSES: u16 = 1 << 8;
    const ATTRIBUTES: u16 = 1 << 9;
    const PARTS: u16 = 1 << 10;
    const CUSTOM_STATES: u16 = 1 << 11;
    const LISTS: [u16; 4] = [Self::CLASSES, Self::ATTRIBUTES, Self::PARTS, Self::CUSTOM_STATES];

    fn has(self, bit: u16) -> bool {
        self.0 & bit != 0
    }

    /// How many words of `bits` that come before `bit` the allocation holds.
    fn words_before(self, bits: u16, bit: u16) -> usize {
        (self.0 & bits & (bit - 1)).count_ones() as usize
    }

    fn atom_words(self) -> usize {
        (self.0 & Self::ATOMS).count_ones() as usize
    }

    fn count_words(self) -> usize {
        (self.0 & !Self::ATOMS).count_ones() as usize
    }
}

/// The start of a packed allocation: a word per atom the element has, then a count per list it has, then the lists'
/// entries, classes, attributes (two words each), parts and custom states.
#[repr(C)]
struct Header {
    references: AtomicU32,
    present: Present,
    // NB: Two bytes stay free here.
    _free: u16,
    /// The Bloom summary of the dispatch keys the lists carry, low half first.
    bloom: [u32; 2],
}

const _: () = assert!(size_of::<Header>() == 16 && align_of::<Header>() == 4);
// NB: Every word that follows the header is a u32.
const _: () = assert!(align_of::<StyleAtomID>() == 4 && size_of::<StyleAtomID>() == 4);
const _: () = assert!(align_of::<AttributeFact>() == 4 && size_of::<AttributeFact>() == 8);

/// The rare facts of one element, shared by every copy of its row.
#[derive(Default)]
pub(super) struct FactLists(Option<NonNull<Header>>);

// SAFETY: The allocation is immutable once made, and its reference count is atomic.
unsafe impl Send for FactLists {}
// SAFETY: As above.
unsafe impl Sync for FactLists {}

/// The identity of one allocation of lists, which interning makes one per content.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(in crate::css::style) struct FactListsIdentity(Option<NonNull<Header>>);

/// The words of every list's entries, by `Present::LISTS` order.
fn entry_words(counts: [u32; 4]) -> [usize; 4] {
    [
        counts[0] as usize,
        2 * counts[1] as usize,
        counts[2] as usize,
        counts[3] as usize,
    ]
}

fn layout(present: Present, counts: [u32; 4]) -> Layout {
    let words = present.atom_words() + present.count_words() + entry_words(counts).iter().sum::<usize>();
    Layout::from_size_align(size_of::<Header>() + 4 * words, align_of::<Header>())
        .expect("an element's fact lists fit in memory")
}

impl FactLists {
    fn new(edit: &FactListsEdit) -> Self {
        if edit.is_empty() {
            return Self(None);
        }
        let count = |length: usize| u32::try_from(length).expect("an element's fact list fits in u32");
        let counts = [
            count(edit.classes.len()),
            count(edit.attributes.len()),
            count(edit.parts.len()),
            count(edit.custom_states.len()),
        ];
        let atoms: [(u16, u32); 8] = [
            (Present::FOLDED_TAG, edit.folded_tag.0),
            (Present::PART_EXPOSURE, edit.part_exposure.0),
            (Present::NAMESPACE, edit.namespace.0),
            (Present::ID, edit.id.0),
            (Present::LANGUAGE, edit.language.0),
            (Present::DIRECTIONALITY, edit.directionality.0),
            (Present::TREE_SCOPE, edit.tree_scope.0),
            (Present::ASSIGNED_SLOT, edit.assigned_slot.map_or(0, StyleNodeID::raw)),
        ];
        let mut present = Present::default();
        for &(bit, word) in &atoms {
            if word != 0 {
                present.0 |= bit;
            }
        }
        for (bit, &count) in Present::LISTS.iter().zip(&counts) {
            if count != 0 {
                present.0 |= bit;
            }
        }
        let layout = layout(present, counts);
        // SAFETY: The layout is never zero-sized, as it holds the header.
        let pointer = unsafe { std::alloc::alloc(layout) }.cast::<Header>();
        let Some(header) = NonNull::new(pointer) else {
            std::alloc::handle_alloc_error(layout);
        };
        // SAFETY: The allocation holds the header and the words after it, and every write stays within it.
        unsafe {
            header.write(Header {
                references: AtomicU32::new(1),
                present,
                _free: 0,
                bloom: [edit.bloom as u32, (edit.bloom >> 32) as u32],
            });
            let mut cursor = header.as_ptr().add(1).cast::<u32>();
            for &(_, word) in atoms.iter().filter(|&&(_, word)| word != 0) {
                cursor.write(word);
                cursor = cursor.add(1);
            }
            for &count in counts.iter().filter(|&&count| count != 0) {
                cursor.write(count);
                cursor = cursor.add(1);
            }
            let mut cursor = cursor.cast::<StyleAtomID>();
            cursor.copy_from_nonoverlapping(edit.classes.as_ptr(), edit.classes.len());
            cursor = cursor.add(edit.classes.len());
            cursor
                .cast::<AttributeFact>()
                .copy_from_nonoverlapping(edit.attributes.as_ptr(), edit.attributes.len());
            cursor = cursor.add(2 * edit.attributes.len());
            cursor.copy_from_nonoverlapping(edit.parts.as_ptr(), edit.parts.len());
            cursor = cursor.add(edit.parts.len());
            cursor.copy_from_nonoverlapping(edit.custom_states.as_ptr(), edit.custom_states.len());
        }
        Self(Some(header))
    }

    fn header(&self) -> Option<&Header> {
        // SAFETY: The allocation lives while this reference to it does.
        self.0.map(|header| unsafe { header.as_ref() })
    }

    /// The words that follow the header.
    fn words(header: &Header) -> *const u32 {
        // SAFETY: The words follow the header in its allocation.
        unsafe { std::ptr::from_ref(header).add(1).cast::<u32>() }
    }

    /// The word of the atom `bit`, where the allocation holds one.
    fn atom(&self, bit: u16) -> Option<u32> {
        let header = self.header()?;
        if !header.present.has(bit) {
            return None;
        }
        // SAFETY: A present atom's word is among the atom words that follow the header.
        Some(unsafe {
            Self::words(header)
                .add(header.present.words_before(Present::ATOMS, bit))
                .read()
        })
    }

    /// The counts of every list, by `Present::LISTS` order.
    fn counts(header: &Header) -> [u32; 4] {
        let present = header.present;
        let first_count = present.atom_words();
        Present::LISTS.map(|bit| match present.has(bit) {
            // SAFETY: A present list's count is among the count words that follow the atom words.
            true => unsafe {
                Self::words(header)
                    .add(first_count + present.words_before(!Present::ATOMS, bit))
                    .read()
            },
            false => 0,
        })
    }

    /// The first atom of list `list`, an index into `Present::LISTS`, and its length.
    fn list(&self, list: usize) -> Option<(*const StyleAtomID, usize)> {
        let header = self.header()?;
        if !header.present.has(Present::LISTS[list]) {
            return None;
        }
        let counts = Self::counts(header);
        let offset = header.present.atom_words()
            + header.present.count_words()
            + entry_words(counts).iter().take(list).sum::<usize>();
        // SAFETY: The offset stays within the allocation, which holds every list.
        let first = unsafe { Self::words(header).add(offset).cast::<StyleAtomID>() };
        Some((first, counts[list] as usize))
    }

    fn atoms(&self, list: usize) -> &[StyleAtomID] {
        match self.list(list) {
            // SAFETY: The list was written when the allocation was made, and lives with it.
            Some((first, length)) => unsafe { ak::slice_from_raw_parts(first, length) },
            None => &[],
        }
    }

    /// Sorted.
    pub(super) fn classes(&self) -> &[StyleAtomID] {
        self.atoms(0)
    }

    /// Sorted by name, each with the atom of its value.
    pub(super) fn attributes(&self) -> &[AttributeFact] {
        match self.list(1) {
            // SAFETY: The attributes were written as such when the allocation was made, and live
            //         with it.
            Some((first, length)) => unsafe { ak::slice_from_raw_parts(first.cast::<AttributeFact>(), length) },
            None => &[],
        }
    }

    pub(super) fn parts(&self) -> &[StyleAtomID] {
        self.atoms(2)
    }

    pub(super) fn custom_states(&self) -> &[StyleAtomID] {
        self.atoms(3)
    }

    pub(super) fn folded_tag(&self) -> StyleAtomID {
        StyleAtomID(self.atom(Present::FOLDED_TAG).unwrap_or(0))
    }

    pub(super) fn part_exposure(&self) -> StyleAtomID {
        StyleAtomID(self.atom(Present::PART_EXPOSURE).unwrap_or(0))
    }

    pub(super) fn namespace(&self) -> StyleAtomID {
        StyleAtomID(self.atom(Present::NAMESPACE).unwrap_or(0))
    }

    pub(super) fn id(&self) -> StyleAtomID {
        StyleAtomID(self.atom(Present::ID).unwrap_or(0))
    }

    pub(super) fn language(&self) -> StyleAtomID {
        StyleAtomID(self.atom(Present::LANGUAGE).unwrap_or(0))
    }

    pub(super) fn directionality(&self) -> StyleAtomID {
        StyleAtomID(self.atom(Present::DIRECTIONALITY).unwrap_or(0))
    }

    pub(super) fn tree_scope(&self) -> TreeScopeID {
        self.atom(Present::TREE_SCOPE)
            .map_or(TreeScopeID::DOCUMENT, TreeScopeID)
    }

    pub(super) fn assigned_slot(&self) -> Option<StyleNodeID> {
        self.atom(Present::ASSIGNED_SLOT).and_then(StyleNodeID::from_raw)
    }

    /// The Bloom summary of the dispatch keys the lists carry.
    pub(super) fn identity(&self) -> FactListsIdentity {
        FactListsIdentity(self.0)
    }

    pub(super) fn bloom(&self) -> u64 {
        self.header().map_or(0, |header| {
            u64::from(header.bloom[0]) | (u64::from(header.bloom[1]) << 32)
        })
    }

    /// The facts to edit, whose lists stay borrowed until an edit changes them.
    pub(super) fn edit(&self) -> FactListsEdit<'_> {
        FactListsEdit {
            classes: EditedList::Held(self.classes()),
            attributes: EditedList::Held(self.attributes()),
            parts: EditedList::Held(self.parts()),
            custom_states: EditedList::Held(self.custom_states()),
            folded_tag: self.folded_tag(),
            part_exposure: self.part_exposure(),
            namespace: self.namespace(),
            id: self.id(),
            language: self.language(),
            directionality: self.directionality(),
            bloom: self.bloom(),
            tree_scope: self.tree_scope(),
            assigned_slot: self.assigned_slot(),
        }
    }

    fn holds_the_same_as(&self, other: &Self) -> bool {
        self.classes() == other.classes()
            && self.attributes() == other.attributes()
            && self.parts() == other.parts()
            && self.custom_states() == other.custom_states()
            && self.folded_tag() == other.folded_tag()
            && self.part_exposure() == other.part_exposure()
            && self.namespace() == other.namespace()
            && self.id() == other.id()
            && self.language() == other.language()
            && self.directionality() == other.directionality()
            && self.bloom() == other.bloom()
            && self.tree_scope() == other.tree_scope()
            && self.assigned_slot() == other.assigned_slot()
    }

    /// Whether the two hold the same facts of their elements, leaving aside their tree scopes and assigned slots, which
    /// are their elements' links.
    pub(super) fn says_the_same_as(&self, other: &Self) -> bool {
        self.0 == other.0
            || (self.classes() == other.classes()
                && self.attributes() == other.attributes()
                && self.parts() == other.parts()
                && self.custom_states() == other.custom_states()
                && self.folded_tag() == other.folded_tag()
                && self.part_exposure() == other.part_exposure()
                && self.namespace() == other.namespace()
                && self.id() == other.id()
                && self.language() == other.language()
                && self.directionality() == other.directionality())
    }

    /// Whether these are the facts `edit` holds.
    fn holds(&self, edit: &FactListsEdit) -> bool {
        self.classes() == &*edit.classes
            && self.attributes() == &*edit.attributes
            && self.parts() == &*edit.parts
            && self.custom_states() == &*edit.custom_states
            && self.folded_tag() == edit.folded_tag
            && self.part_exposure() == edit.part_exposure
            && self.namespace() == edit.namespace
            && self.id() == edit.id
            && self.language() == edit.language
            && self.directionality() == edit.directionality
            && self.bloom() == edit.bloom
            && self.tree_scope() == edit.tree_scope
            && self.assigned_slot() == edit.assigned_slot
    }

    /// The hash of what the lists hold, as [`FactListsEdit::content_hash`] gives it for an edit holding the same.
    fn content_hash(&self) -> u64 {
        self.edit().content_hash()
    }

    /// How many references to the allocation are live, the interning table's among them.
    fn references(&self) -> u32 {
        self.header()
            .map_or(0, |header| header.references.load(Ordering::Acquire))
    }

    pub(super) fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        match self.header() {
            // NB: The process's table holds an entry for every list, which the first document whose rows reach the
            //     list counts with it.
            Some(header) if sizer.first_sight(std::ptr::from_ref(header)) => {
                layout(header.present, Self::counts(header)).size()
                    + crate::heap_size::live_hash_table_bytes(1, size_of::<Self>())
            }
            _ => 0,
        }
    }
}

impl Clone for FactLists {
    fn clone(&self) -> Self {
        if let Some(header) = self.header() {
            let references = header.references.fetch_add(1, Ordering::Relaxed);
            assert!(references < u32::MAX / 2, "fact lists reference count overflow");
        }
        Self(self.0)
    }
}

impl Drop for FactLists {
    fn drop(&mut self) {
        let Some(header) = self.header() else {
            return;
        };
        if header.references.fetch_sub(1, Ordering::Release) != 1 {
            return;
        }
        fence(Ordering::Acquire);
        let layout = layout(header.present, Self::counts(header));
        // SAFETY: This was the last reference, so nothing reads the allocation after this.
        unsafe { std::alloc::dealloc(self.0.expect("the header was read").as_ptr().cast(), layout) };
    }
}

impl PartialEq for FactLists {
    fn eq(&self, other: &Self) -> bool {
        self.0 == other.0 || (self.0.is_some() && other.0.is_some() && self.holds_the_same_as(other))
    }
}

/// One list of an edit: the list as it stands until the edit changes it.
pub(super) enum EditedList<'a, T: Copy, const INLINE: usize> {
    Held(&'a [T]),
    Changed(SmallVec<[T; INLINE]>),
}

impl<T: Copy, const INLINE: usize> EditedList<'_, T, INLINE> {
    pub(super) fn to_mut(&mut self) -> &mut SmallVec<[T; INLINE]> {
        if let Self::Held(list) = *self {
            *self = Self::Changed(SmallVec::from_slice(list));
        }
        match self {
            Self::Changed(list) => list,
            Self::Held(_) => unreachable!("the list was just copied out"),
        }
    }
}

impl<T: Copy, const INLINE: usize> std::ops::Deref for EditedList<'_, T, INLINE> {
    type Target = [T];

    fn deref(&self) -> &[T] {
        match self {
            Self::Held(list) => list,
            Self::Changed(list) => list,
        }
    }
}

/// The rare facts of an element, to edit.
pub(super) struct FactListsEdit<'a> {
    pub(super) classes: EditedList<'a, StyleAtomID, 4>,
    pub(super) attributes: EditedList<'a, AttributeFact, 4>,
    pub(super) parts: EditedList<'a, StyleAtomID, 2>,
    pub(super) custom_states: EditedList<'a, StyleAtomID, 2>,
    pub(super) folded_tag: StyleAtomID,
    pub(super) part_exposure: StyleAtomID,
    pub(super) namespace: StyleAtomID,
    pub(super) id: StyleAtomID,
    pub(super) language: StyleAtomID,
    pub(super) directionality: StyleAtomID,
    /// The Bloom summary of the dispatch keys the lists carry, which the writer computes before it makes them.
    pub(super) bloom: u64,
    pub(super) tree_scope: TreeScopeID,
    pub(super) assigned_slot: Option<StyleNodeID>,
}

impl FactListsEdit<'_> {
    fn is_empty(&self) -> bool {
        self.classes.is_empty()
            && self.attributes.is_empty()
            && self.parts.is_empty()
            && self.custom_states.is_empty()
            && self.folded_tag.is_none()
            && self.part_exposure.is_none()
            && self.namespace.is_none()
            && self.id.is_none()
            && self.language.is_none()
            && self.directionality.is_none()
            && self.tree_scope == TreeScopeID::DOCUMENT
            && self.assigned_slot.is_none()
    }

    fn content_hash(&self) -> u64 {
        use std::hash::{Hash, Hasher};
        let mut hasher = crate::fast_hash::fast_hasher();
        self.classes.hash(&mut hasher);
        for attribute in self.attributes.iter() {
            (attribute.name, attribute.value).hash(&mut hasher);
        }
        self.parts.hash(&mut hasher);
        self.custom_states.hash(&mut hasher);
        (
            self.folded_tag,
            self.part_exposure,
            self.namespace,
            self.id,
            self.language,
            self.directionality,
            self.bloom,
            self.tree_scope,
            self.assigned_slot,
        )
            .hash(&mut hasher);
        hasher.finish()
    }
}

/// The fact lists of the process, by content: one allocation per content while any row of any
/// document holds it, so that rows holding the same facts hold the same pointer. Each entry is
/// the lists' pointer alone, found by the hash of their content, which the table recomputes from
/// the lists themselves as it grows.
pub(crate) struct FactListsTable {
    lists: hashbrown::HashTable<FactLists>,
    /// How many lists the table holds before it drops those only it holds: twice as many as the
    /// last drop kept, so dropping costs a constant per list interned.
    drop_threshold: usize,
}

const _: () = assert!(size_of::<FactLists>() == 8);

/// How many lists the table holds before it first drops those only it holds.
const MINIMUM_DROP_THRESHOLD: usize = 1024;

impl Default for FactListsTable {
    fn default() -> Self {
        Self {
            lists: hashbrown::HashTable::new(),
            drop_threshold: MINIMUM_DROP_THRESHOLD,
        }
    }
}

impl FactListsTable {
    /// The fact lists holding what `edit` holds, whose content hashes to `hash`.
    fn intern(&mut self, edit: &FactListsEdit, hash: u64) -> FactLists {
        if let Some(lists) = self.lists.find(hash, |lists| lists.holds(edit)) {
            return lists.clone();
        }
        if self.lists.len() >= self.drop_threshold {
            self.drop_unheld();
        }
        let lists = FactLists::new(edit);
        self.lists.insert_unique(hash, lists.clone(), FactLists::content_hash);
        lists
    }

    /// Drops the lists no row holds any more.
    fn drop_unheld(&mut self) {
        self.lists.retain(|lists| lists.references() > 1);
        self.drop_threshold = (2 * self.lists.len()).max(MINIMUM_DROP_THRESHOLD);
    }
}

/// How many fact lists a writer keeps to share without asking the process's table, each in the slot its content
/// hashes to.
const RECENT_FACT_LISTS: usize = 64;

/// The fact lists a writer made last, by the hash of what they hold, which an element made the same facts again shares
/// without taking the process's table's lock, as siblings and cousins published alike do.
#[derive(Clone)]
pub(super) struct RecentFactLists(Box<[FactLists; RECENT_FACT_LISTS]>);

impl Default for RecentFactLists {
    fn default() -> Self {
        Self(Box::new(std::array::from_fn(|_| FactLists::default())))
    }
}

impl RecentFactLists {
    /// Fact lists holding what `edit` holds: one made recently where one holds it, or the ones the
    /// process's table interned.
    pub(super) fn make(&mut self, edit: &FactListsEdit, table: &std::sync::Mutex<FactListsTable>) -> FactLists {
        if edit.is_empty() {
            return FactLists(None);
        }
        let hash = edit.content_hash();
        let slot = &mut self.0[(hash % RECENT_FACT_LISTS as u64) as usize];
        if slot.0.is_some() && slot.holds(edit) {
            return slot.clone();
        }
        let lists = table
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .intern(edit, hash);
        *slot = lists.clone();
        lists
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lists_hash_as_the_edit_they_were_made_from() {
        let edit = FactListsEdit {
            classes: EditedList::Held(&[StyleAtomID(1), StyleAtomID(2)]),
            attributes: EditedList::Held(&[AttributeFact {
                name: StyleAtomID(3),
                value: StyleAtomID(4),
            }]),
            parts: EditedList::Held(&[StyleAtomID(5)]),
            custom_states: EditedList::Held(&[StyleAtomID(6)]),
            folded_tag: StyleAtomID(7),
            part_exposure: StyleAtomID(8),
            namespace: StyleAtomID(9),
            id: StyleAtomID(10),
            language: StyleAtomID(11),
            directionality: StyleAtomID(12),
            bloom: 0x1234_5678_9abc_def0,
            tree_scope: TreeScopeID(13),
            assigned_slot: StyleNodeID::from_raw(14),
        };
        let lists = FactLists::new(&edit);
        assert_eq!(lists.content_hash(), edit.content_hash());
        assert!(lists.holds(&edit));
    }

    #[test]
    fn every_combination_of_atoms_reads_back() {
        for mask in 0_u32..256 {
            let atom = |bit: u32, value: u32| if mask & (1 << bit) != 0 { value } else { 0 };
            let empty = FactLists::default();
            let mut edit = empty.edit();
            edit.folded_tag = StyleAtomID(atom(0, 11));
            edit.part_exposure = StyleAtomID(atom(1, 12));
            edit.namespace = StyleAtomID(atom(2, 13));
            edit.id = StyleAtomID(atom(3, 14));
            edit.language = StyleAtomID(atom(4, 15));
            edit.directionality = StyleAtomID(atom(5, 16));
            edit.tree_scope = TreeScopeID(atom(6, 17));
            edit.assigned_slot = StyleNodeID::from_raw(atom(7, 18));
            edit.classes = EditedList::Changed(SmallVec::from_slice(&[StyleAtomID(20 + mask)]));
            edit.parts = EditedList::Held(&[StyleAtomID(21), StyleAtomID(22)]);
            let lists = FactLists::new(&edit);
            assert!(lists.holds(&edit), "mask {mask:#x}");
            assert_eq!(lists.classes(), &*edit.classes);
            assert_eq!(lists.parts(), &*edit.parts);
            assert!(lists.attributes().is_empty() && lists.custom_states().is_empty());
        }
    }

    #[test]
    fn long_lists_read_back() {
        for length in [0_u32, 1, 70000] {
            let empty = FactLists::default();
            let mut edit = empty.edit();
            edit.classes = EditedList::Changed((0..length).map(StyleAtomID).collect());
            edit.attributes = EditedList::Changed(
                (0..length)
                    .map(|index| AttributeFact {
                        name: StyleAtomID(index),
                        value: StyleAtomID(index + 1),
                    })
                    .collect(),
            );
            edit.custom_states = EditedList::Held(&[StyleAtomID(9)]);
            let lists = FactLists::new(&edit);
            assert_eq!(lists.classes(), &*edit.classes);
            assert_eq!(lists.attributes(), &*edit.attributes);
            assert_eq!(lists.custom_states(), &*edit.custom_states);
        }
    }

    #[test]
    fn the_table_hands_out_one_lists_per_content() {
        let mut table = FactListsTable::default();
        let empty = FactLists::default();
        let mut edit = empty.edit();
        edit.id = StyleAtomID(1);
        let first = table.intern(&edit, edit.content_hash());
        let second = table.intern(&edit, edit.content_hash());
        assert_eq!(first.0, second.0);
        edit.id = StyleAtomID(2);
        assert_ne!(table.intern(&edit, edit.content_hash()).0, first.0);
    }
}
