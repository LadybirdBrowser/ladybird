/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The rule index: every attached style rule entry of a tree scope, bucketed by the most selective
//! key its subject compound requires, with the ancestor keys its combinator chain requires.
//!
//! An entry keeps its number for as long as the index lives. A change to some rules indexes those
//! rules again alone: the entries of a rule that left or changed die, leaving every bucket, and the
//! entries of a rule that arrived or changed are appended. What the index indexed is kept by the
//! rule store's identities of the rules and sheets, so the rules a change does not name, such as
//! the user agent's, are never indexed again for it.
//!
//! Shadow trees whose ordered sheets and layers are the same decide with the same entries, which
//! they share as a group: a page with many instances of one component indexes its rules once.
//!
//! NB: An update works at the granularity of the rules a change names: an appended sheet's rules
//!     join the buckets the index has, and nothing else is indexed again.

use super::super::index::dispatch_bloom_bit;
use super::super::selector_analysis::{EntryFacts, collect_tested_ids, state_dispatch_key, subject_dispatch_keys};
use super::super::weak_pool::WeakPool;
use super::super::*;
use super::bloom::AncestorHashes;
use super::extent_map::{Extent, ExtentMap, FrozenExtentMap, PagedExtentMap};
use super::rule_changes::{Regrouped, RuleIndexChanges, StayedEntry};
use super::rule_store::UserAgentVariant;
use super::shared_pages::{PagePool, SharedPages};
use crate::css::container_conditions::ContainerConditionsData;
use crate::css::selector::Specificity;
use smallvec::SmallVec;
use std::hash::Hash;
use std::sync::Arc;

/// The container conditions a rule applies under, in the rule index's gates.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(super) struct ContainerGateID(std::num::NonZeroU32);

/// The number of a document's first entry of its own. The numbers below it are those of the user
/// agent index the document shares, so a document's own entries keep their numbers whichever user
/// agent index it shares.
pub(super) const FIRST_DOCUMENT_ENTRY: u32 = 1024;

/// The tree scopes whose sheets an entry is of: the document, or a group of shadow trees deciding
/// with the same ordered sheets and layers, which share the entries of those sheets.
#[derive(Clone, Copy, Debug, Hash, PartialEq, Eq, PartialOrd, Ord)]
pub(super) struct ScopeGroupID(u32);

impl ScopeGroupID {
    /// The document's own sheets.
    pub(super) const DOCUMENT: Self = Self(0);
}

/// The entries a group of shadow trees shares, with what the trees have alike.
#[derive(Clone)]
pub(super) struct ScopeGroup {
    pub(super) index: ScopeIndex,
    /// The trees of the group, in the order they joined it. The first ranks the layers of the
    /// entries, which are the same in every tree of the group.
    scopes: SmallVec<[TreeScopeID; 1]>,
    /// The sheets attached to each tree, in cascade order, the entries of which rank by their
    /// place here.
    sheets: Box<[SheetID]>,
    /// The ranks of the layers of each tree.
    layer_ranks: HashMap<CascadeLayerID, u32>,
}

/// The rule an entry is of: a rule of the document's rule store, or of the process's user agent
/// rules, as every document names those.
#[derive(Clone, Copy, PartialEq, Eq)]
struct EntryRule(u32);

impl EntryRule {
    fn document(rule: RuleID) -> Self {
        assert!(rule.in_user_agent_rules().is_none(), "rule identity space exhausted");
        Self(rule.0)
    }

    fn user_agent(rule: RuleID) -> Self {
        Self(RuleID::of_user_agent(rule).0)
    }
}

/// The number of an [`EntryPlacement`] among those of the entries it numbers.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(super) struct PlacementID(u32);

/// Where the entries of the rules of one layer of one sheet deciding in one group of scopes
/// decide: every such entry shares the origin, group and ranks it holds, which move here alone
/// when the order of the sheets or layers does.
///
/// NB: An entry holds its rule's position alone, and leaves the sheet's origin, scope and layer
///     order to its placement, which every entry of the sheet and layer shares.
#[derive(Clone, Copy)]
pub(super) struct EntryPlacement {
    pub(super) origin: CascadeOrigin,
    pub(super) group: ScopeGroupID,
    pub(super) layer_rank: u64,
    pub(super) sheet_rank: u64,
    layer: CascadeLayerID,
}

/// Where the selector of an entry is: the list of its rule, and where it starts in the list's arena.
#[derive(Clone)]
pub(in crate::css::style) struct EntrySource {
    selectors: crate::css::selector::SelectorListRef,
    at: crate::css::selector_arena::ComponentOffset,
}

impl EntrySource {
    pub(in crate::css::style) fn selector(&self) -> crate::css::selector::Selector<'_> {
        self.selectors.selector_at(self.at)
    }

    /// What tells the selector from another for as long as the process lives.
    fn identity(&self) -> crate::css::selector_arena::SelectorKey {
        crate::css::selector_arena::SelectorKey {
            arena: self.selectors.serial(),
            at: self.at,
        }
    }
}

/// What an entry's pseudo-element kind is for an entry styling the element itself.
pub(super) const ELEMENT_PSEUDO_KIND: u8 = u8::MAX;

/// The entries the selector list `selectors` makes: one for each selector, and a second for a
/// selector styling every search text match, for the current one; with the specificity and the
/// kind of pseudo-element each styles, or `ELEMENT_PSEUDO_KIND` for the element itself.
pub(in crate::css::style) fn entries_of(
    selectors: &crate::css::selector::SelectorListRef,
) -> impl Iterator<Item = (crate::css::selector::Selector<'_>, Specificity, u8)> + '_ {
    use crate::css::selector::PseudoElementType;
    selectors.iter().flat_map(move |selector| {
        let specificity = selector.specificity();
        let kind = |pseudo_element: Option<PseudoElementType>| {
            pseudo_element.map_or(ELEMENT_PSEUDO_KIND, |kind| {
                u8::try_from(kind as u16).unwrap_or(ELEMENT_PSEUDO_KIND - 1)
            })
        };
        let current = selector
            .styles_every_search_text_match()
            .then(|| (selector, specificity, kind(Some(PseudoElementType::SearchTextCurrent))));
        std::iter::once((selector, specificity, kind(selector.target_pseudo_element()))).chain(current)
    })
}

/// One top-level complex selector of one attached style rule.
#[derive(Clone, Copy)]
pub(super) struct IndexedEntry {
    rule: EntryRule,
    /// Where the entry decides, among the placements of the entries numbering it.
    placement: PlacementID,
    /// The entry's selector, in the space of the index holding the entry.
    selector: SelectorRef,
    specificity: PackedSpecificity,
    pub(super) rule_rank: u64,
    /// The kind of pseudo-element the entry styles, or `ELEMENT_PSEUDO_KIND` for the element itself.
    pub(super) pseudo_kind: u8,
    flags: EntryFlags,
}

// NB: Every attached style rule's selector has one, in every document and group of scopes it decides
//     in, so what grows it grows every index. Its selector is a reference into the index's space,
//     and the container conditions of the few gated rules are the index's to say, by rule.
const _: () = assert!(size_of::<IndexedEntry>() == 32);

/// The specificity of a selector packed into one word: its ids, classes and types counted in eight
/// bits each, from the most significant down, so that packed specificities compare as the
/// specificities do.
///
/// INTEROP: Other engines count each part of a specificity in eight bits too, saturating at 255.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
struct PackedSpecificity(u32);

impl PackedSpecificity {
    fn pack(specificity: Specificity) -> Self {
        Self(specificity.packed())
    }

    fn unpacked(self) -> Specificity {
        let part = |shift: u32| ((self.0 >> shift) & 0xff) as u16;
        Specificity {
            ids: part(16),
            classes: part(8),
            types: part(0),
        }
    }
}

/// The yes-or-no facts of an [`IndexedEntry`], one bit each.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct EntryFlags(u8);

impl EntryFlags {
    const SCOPED: u8 = 1 << 0;
    /// The number was of a segment the index let go of, which took the entry's selector with it.
    const SELECTOR_GONE: u8 = 1 << 7;
    /// The entry can match one of two elements and not the other where the style sharing key
    /// holds for both.
    const REVALIDATES: u8 = 1 << 1;
    /// The rule declares custom properties.
    const DECLARES_CUSTOM_PROPERTIES: u8 = 1 << 2;
    /// The rule declares something important.
    const DECLARES_IMPORTANT: u8 = 1 << 3;
    /// The entry is indexed: one whose rule left or changed is in no bucket, and keeps its number
    /// until the index is built again.
    const LIVE: u8 = 1 << 4;
    /// The rule applies under container conditions, which the index holds by rule.
    const GATED: u8 = 1 << 5;
    /// The entry stands for the number of an entry a segment the index binds holds, or for a
    /// number before or after such entries that no entry has.
    const BOUND: u8 = 1 << 6;

    fn has(self, flag: u8) -> bool {
        self.0 & flag != 0
    }
}

impl IndexedEntry {
    /// What tells the entry from another: its selector by its reference in the index's space, and
    /// the rest. Two indexes holding equal entries hold them alike for their own spaces.
    fn identity(&self) -> impl Eq + Hash + '_ {
        let Self {
            rule,
            placement,
            selector,
            specificity,
            rule_rank,
            pseudo_kind,
            flags,
        } = self;
        (
            rule.0,
            *selector,
            (*specificity, *placement, *rule_rank, *pseudo_kind, *flags),
        )
    }

    /// What stands in the document's entries for the number of an entry a bound segment holds, alike
    /// in every document, so that the pages of a segment's numbers are one page of the process.
    const BOUND_NUMBER: Self = Self {
        rule: EntryRule(0),
        placement: PlacementID(0),
        selector: SelectorRef(0),
        specificity: PackedSpecificity(0),
        rule_rank: 0,
        pseudo_kind: ELEMENT_PSEUDO_KIND,
        flags: EntryFlags(EntryFlags::BOUND),
    };

    pub(super) fn declares_important(&self) -> bool {
        self.flags.has(EntryFlags::DECLARES_IMPORTANT)
    }

    pub(super) fn live(&self) -> bool {
        self.flags.has(EntryFlags::LIVE)
    }

    fn kill(&mut self) {
        self.flags.0 &= !EntryFlags::LIVE;
    }
}

/// An entry read in place where it is held: in the entries of the index numbering it or of the
/// user agent index, or in the segment the index binds, whose binding says the rest of it.
#[derive(Clone, Copy)]
pub(super) enum EntryView<'a> {
    /// An entry of the index's own entries, or of the user agent index's where `user_agent`.
    Held { entry: &'a IndexedEntry, user_agent: bool },
    /// An entry a bound segment holds.
    Bound {
        bound: &'a BoundSegment,
        entry: &'a SegmentEntry,
        /// Whether the entry died for the index binding the segment, which the segment knows nothing of.
        dead: bool,
    },
}

/// What `entry`, held by the document or by a segment, says alike in both of its shapes.
macro_rules! of_either_entry {
    ($view:expr, $entry:ident => $read:expr) => {
        match $view {
            EntryView::Held { entry: $entry, .. } => $read,
            EntryView::Bound { entry: $entry, .. } => $read,
        }
    };
}

impl EntryView<'_> {
    fn flags(&self) -> EntryFlags {
        match self {
            Self::Held { entry, .. } => entry.flags,
            Self::Bound { entry, dead: false, .. } => entry.flags,
            Self::Bound { entry, dead: true, .. } => EntryFlags(entry.flags.0 & !EntryFlags::LIVE),
        }
    }

    /// The kind of pseudo-element the entry styles, or `ELEMENT_PSEUDO_KIND` for the element itself.
    pub(super) fn pseudo_kind(&self) -> u8 {
        of_either_entry!(self, entry => entry.pseudo_kind)
    }

    pub(super) fn rule_rank(&self) -> u64 {
        of_either_entry!(self, entry => entry.rule_rank)
    }

    /// The entry's selector, in the space of the index or segment holding the entry.
    fn selector(&self) -> SelectorRef {
        of_either_entry!(self, entry => entry.selector)
    }

    fn rule(&self) -> EntryRule {
        match self {
            Self::Held { entry, .. } => entry.rule,
            Self::Bound { bound, entry, .. } => EntryRule::document(RuleID(bound.first_rule.0 + entry.rule)),
        }
    }

    pub(super) fn specificity(&self) -> Specificity {
        of_either_entry!(self, entry => entry.specificity.unpacked())
    }

    /// Whether the entry styles a pseudo-element.
    pub(super) fn pseudo_element(&self) -> bool {
        self.pseudo_kind() != ELEMENT_PSEUDO_KIND
    }

    pub(super) fn scoped(&self) -> bool {
        self.flags().has(EntryFlags::SCOPED)
    }

    pub(super) fn revalidates(&self) -> bool {
        self.flags().has(EntryFlags::REVALIDATES)
    }

    pub(super) fn declares_custom_properties(&self) -> bool {
        self.flags().has(EntryFlags::DECLARES_CUSTOM_PROPERTIES)
    }

    pub(super) fn declares_important(&self) -> bool {
        self.flags().has(EntryFlags::DECLARES_IMPORTANT)
    }

    pub(super) fn live(&self) -> bool {
        self.flags().has(EntryFlags::LIVE)
    }

    pub(super) fn gated(&self) -> bool {
        self.flags().has(EntryFlags::GATED)
    }

    /// The entry as the document has it, but for a bound entry's selector, which is in the
    /// segment's space.
    fn indexed(&self) -> IndexedEntry {
        match *self {
            Self::Held { entry, .. } => *entry,
            Self::Bound { bound, entry, .. } => IndexedEntry {
                rule: self.rule(),
                placement: bound.placements[usize::from(entry.placement)],
                selector: entry.selector,
                specificity: entry.specificity,
                rule_rank: entry.rule_rank,
                pseudo_kind: entry.pseudo_kind,
                flags: self.flags(),
            },
        }
    }
}

impl PartialEq for IndexedEntry {
    fn eq(&self, other: &Self) -> bool {
        self.identity() == other.identity()
    }
}

impl Hash for IndexedEntry {
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        self.identity().hash(state);
    }
}

/// One entry of a rule bucket, on its way into [`Buckets`], with the keys some ancestor of a matching
/// subject necessarily carries, and its selector.
#[derive(Clone, Copy)]
pub(super) struct BucketEntry {
    ancestor_hashes: AncestorHashes,
    /// The entry's number, from the first of the segment where the buckets are a segment's, with
    /// [`Self::IN_OTHER_BUCKETS`], [`Self::SCOPED`], [`Self::COVERED`] and
    /// [`Self::HAS_SIBLING_KEY`].
    entry: u32,
    /// The entry's selector, in the space of the buckets' part.
    selector: SelectorRef,
    /// The key every earlier sibling of a subject right of `~` carries one of, where
    /// [`Self::HAS_SIBLING_KEY`] says the entry has one.
    sibling_key: u32,
    /// The dispatch bloom bits of the ids and classes the subject compound tests, which an element
    /// lacking any of cannot match.
    subject_bloom: u64,
}

const _: () = assert!(size_of::<BucketEntry>() == 32);

impl BucketEntry {
    /// The entry is in more buckets than this one, which an element can reach it by.
    const IN_OTHER_BUCKETS: u32 = 1 << 31;
    /// The entry's rule is inside `@scope`, whose levels matching reads from the entry.
    const SCOPED: u32 = 1 << 30;
    /// The bucket's key alone proves the selector: an element reaching the entry by its key matches.
    const COVERED: u32 = 1 << 29;
    /// The entry has a [`Self::sibling_key`].
    const HAS_SIBLING_KEY: u32 = 1 << 28;
    /// The bucket's key alone proves the subject compound of the selector, which an element reaching
    /// the entry by its key is matched left of, as it is the only thing the compound tests.
    const SUBJECT_COVERED: u32 = 1 << 26;
    /// [`Self::SUBJECT_COVERED`], where the element's tag is its own ASCII-lowercase form.
    const SUBJECT_COVERED_FOR_A_LOWERCASE_TAG: u32 = 1 << 25;
    /// The bucket's key proves the selector for an element whose tag is its own ASCII-lowercase
    /// form: the selector is a type selector alone, written in ASCII lowercase.
    const COVERED_FOR_A_LOWERCASE_TAG: u32 = 1 << 27;
    const FLAGS: u32 = Self::IN_OTHER_BUCKETS
        | Self::SCOPED
        | Self::COVERED
        | Self::HAS_SIBLING_KEY
        | Self::COVERED_FOR_A_LOWERCASE_TAG
        | Self::SUBJECT_COVERED
        | Self::SUBJECT_COVERED_FOR_A_LOWERCASE_TAG;

    /// The slot of the entry `entry` of `selector`, inside `@scope` where `scoped`.
    fn new(selector: crate::css::selector::Selector<'_>, entry: u32, selector_ref: SelectorRef, scoped: bool) -> Self {
        assert!(entry & Self::FLAGS == 0, "rule entry space exhausted");
        let sibling_key = super::bloom::earlier_sibling_key(selector);
        let mut flags = 0;
        if scoped {
            flags |= Self::SCOPED;
        }
        match is_covered_by_its_key(selector) {
            _ if scoped => {}
            Covered::No => {}
            Covered::Always => flags |= Self::COVERED,
            Covered::ForALowercaseTag => flags |= Self::COVERED_FOR_A_LOWERCASE_TAG,
        }
        match subject_is_covered_by_its_key(selector) {
            _ if scoped => {}
            Covered::No => {}
            Covered::Always => flags |= Self::SUBJECT_COVERED,
            Covered::ForALowercaseTag => flags |= Self::SUBJECT_COVERED_FOR_A_LOWERCASE_TAG,
        }
        if sibling_key.is_some() {
            flags |= Self::HAS_SIBLING_KEY;
        }
        Self {
            ancestor_hashes: AncestorHashes::required_left_of(super::super::selector_analysis::subject_compound(
                selector,
            )),
            entry: entry | flags,
            selector: selector_ref,
            sibling_key: sibling_key.unwrap_or(0),
            subject_bloom: selector.required_dispatch_bloom(),
        }
    }
}

/// The entries reached by each dispatch key.
///
/// A slot is one word for an entry its key proves, its number and flags, which no element reaching
/// it is rejected from. Any other is [`BUCKET_SLOT_WORDS`] words at fixed places, so a scan rejecting
/// candidates reads what it rejects on with nothing to decode.
///
/// NB: The buckets of a sheet segment never change once it is built, and take the smaller shape.
#[derive(Clone)]
pub(super) enum Buckets {
    Growing(ExtentMap<DispatchKey, u32>),
    Frozen(FrozenExtentMap<DispatchKey, u32>),
}

impl Default for Buckets {
    fn default() -> Self {
        Self::Growing(ExtentMap::default())
    }
}

impl Buckets {
    /// The slots of `key`, where it has any.
    #[inline]
    pub(super) fn get(&self, key: &DispatchKey) -> Option<BucketSlots<'_>> {
        let words = match self {
            Self::Growing(map) => map.get(key),
            Self::Frozen(map) => map.get(key),
        };
        words.map(|words| BucketSlots { words })
    }

    pub(super) fn is_empty(&self) -> bool {
        match self {
            Self::Growing(map) => map.is_empty(),
            Self::Frozen(map) => map.is_empty(),
        }
    }

    pub(super) fn keys(&self) -> Box<dyn Iterator<Item = &DispatchKey> + '_> {
        match self {
            Self::Growing(map) => Box::new(map.keys()),
            Self::Frozen(map) => Box::new(map.keys()),
        }
    }

    fn heap_size(&self) -> usize {
        match self {
            Self::Growing(map) => map.heap_size(),
            Self::Frozen(map) => map.heap_size(),
        }
    }

    fn shrink_to_fit(&mut self) {
        if let Self::Growing(map) = self {
            map.shrink_to_fit();
        }
    }

    fn freeze(&mut self) {
        if let Self::Growing(map) = self {
            *self = Self::Frozen(FrozenExtentMap::of(std::mem::take(map)));
        }
    }

    fn has_slack(&self) -> bool {
        matches!(self, Self::Growing(map) if map.has_slack())
    }

    fn growing(&mut self) -> &mut ExtentMap<DispatchKey, u32> {
        match self {
            Self::Growing(map) => map,
            Self::Frozen(_) => unreachable!("the buckets of a segment do not change"),
        }
    }

    fn push(&mut self, key: DispatchKey, slot: &BucketEntry) {
        let words = self.growing();
        if slot.entry & BucketEntry::COVERED != 0 {
            words.push(key, slot.entry);
            return;
        }
        // NB: A key proving the subject leaves nothing an element reaching the slot could lack.
        let subject_bloom = match slot.entry & BucketEntry::SUBJECT_COVERED != 0 {
            true => 0,
            false => slot.subject_bloom,
        };
        let [first_hash, second_hash, third_hash] = slot.ancestor_hashes.packed_words();
        for word in [
            slot.entry,
            slot.selector.0,
            subject_bloom as u32,
            (subject_bloom >> 32) as u32,
            first_hash,
            second_hash,
            third_hash,
            slot.sibling_key,
        ] {
            words.push(key, word);
        }
    }

    /// Take the slots of the entry `entry` out of the list of `key`.
    fn remove(&mut self, key: &DispatchKey, entry: u32) {
        let Some(kept) = self.get(key).map(|slots| {
            slots
                .filter(|slot| slot.entry() != entry)
                .map(|slot| slot.entry_to_hold())
                .collect::<Vec<_>>()
        }) else {
            return;
        };
        self.growing().remove(key);
        for slot in &kept {
            self.push(*key, slot);
        }
    }

    #[cfg(test)]
    fn count(&self) -> usize {
        let keys: Vec<DispatchKey> = self.keys().copied().collect();
        keys.iter().filter_map(|key| self.get(key)).map(Iterator::count).sum()
    }
}

/// How many words a slot of an entry its key does not prove takes, and where it holds what.
const BUCKET_SLOT_WORDS: usize = 8;
const BUCKET_SLOT_SELECTOR: usize = 1;
const BUCKET_SLOT_BLOOM_LOW: usize = 2;
const BUCKET_SLOT_BLOOM_HIGH: usize = 3;
const BUCKET_SLOT_HASHES: usize = 4;
const BUCKET_SLOT_SIBLING_KEY: usize = 7;

/// The slots of one bucket, as [`Buckets`] holds them.
#[derive(Clone)]
pub(super) struct BucketSlots<'a> {
    words: &'a [u32],
}

impl<'a> Iterator for BucketSlots<'a> {
    type Item = BucketSlot<'a>;

    #[inline(always)]
    fn next(&mut self) -> Option<BucketSlot<'a>> {
        let &first = self.words.first()?;
        if first & BucketEntry::COVERED != 0 {
            self.words = &self.words[1..];
            // NB: Nothing rejects an element reaching a proven entry: it requires no bit of an element's, and no key
            //     of its ancestors or earlier siblings.
            return Some(BucketSlot {
                entry: first,
                words: &[0; BUCKET_SLOT_WORDS],
            });
        }
        let (words, rest) = self
            .words
            .split_first_chunk::<BUCKET_SLOT_WORDS>()
            .expect("a slot holds the words of a slot");
        self.words = rest;
        Some(BucketSlot { entry: first, words })
    }
}

/// One entry of a rule bucket, read where [`Buckets`] holds it, so that a candidate rejected on its
/// subject's bloom loads that and nothing more.
#[derive(Clone, Copy)]
pub(super) struct BucketSlot<'a> {
    entry: u32,
    words: &'a [u32; BUCKET_SLOT_WORDS],
}

impl BucketSlot<'_> {
    /// The dispatch bloom bits of the ids and classes the subject compound tests, which an element
    /// lacking any of cannot match.
    #[inline(always)]
    pub(super) fn subject_bloom(&self) -> u64 {
        u64::from(self.words[BUCKET_SLOT_BLOOM_LOW]) | u64::from(self.words[BUCKET_SLOT_BLOOM_HIGH]) << 32
    }

    /// The keys some ancestor of a matching subject necessarily carries.
    #[inline(always)]
    pub(super) fn ancestor_hashes(&self) -> AncestorHashes {
        AncestorHashes::from_packed_words([
            self.words[BUCKET_SLOT_HASHES],
            self.words[BUCKET_SLOT_HASHES + 1],
            self.words[BUCKET_SLOT_HASHES + 2],
        ])
    }

    /// The entry's selector, in the space of the buckets' part, where the entry is not [`Self::covered`].
    #[inline(always)]
    pub(super) fn selector(&self) -> SelectorRef {
        SelectorRef(self.words[BUCKET_SLOT_SELECTOR])
    }

    /// The entry's number, from the first of the segment where the buckets are a segment's.
    #[inline(always)]
    pub(super) fn entry(&self) -> u32 {
        self.entry & !BucketEntry::FLAGS
    }

    #[inline(always)]
    pub(super) fn in_other_buckets(&self) -> bool {
        self.entry & BucketEntry::IN_OTHER_BUCKETS != 0
    }

    #[inline(always)]
    pub(super) fn scoped(&self) -> bool {
        self.entry & BucketEntry::SCOPED != 0
    }

    /// Whether the bucket's key proves the selector for an element reaching the entry by it, whose
    /// tag is its own ASCII-lowercase form where `tag_is_lowercase`.
    #[inline(always)]
    pub(super) fn covered(&self, tag_is_lowercase: bool) -> bool {
        self.entry & BucketEntry::COVERED != 0
            || (tag_is_lowercase && self.entry & BucketEntry::COVERED_FOR_A_LOWERCASE_TAG != 0)
    }

    /// Whether the bucket's key proves the subject compound for an element reaching the entry by it,
    /// whose tag is its own ASCII-lowercase form where `tag_is_lowercase`.
    #[inline(always)]
    pub(super) fn subject_covered(&self, tag_is_lowercase: bool) -> bool {
        self.entry & BucketEntry::SUBJECT_COVERED != 0
            || (tag_is_lowercase && self.entry & BucketEntry::SUBJECT_COVERED_FOR_A_LOWERCASE_TAG != 0)
    }

    /// The key an earlier sibling of a matching subject carries, where the entry has one.
    #[inline(always)]
    pub(super) fn sibling_key(&self) -> Option<u32> {
        (self.entry & BucketEntry::HAS_SIBLING_KEY != 0).then_some(self.words[BUCKET_SLOT_SIBLING_KEY])
    }

    /// The entry as [`Buckets`] takes it.
    fn entry_to_hold(&self) -> BucketEntry {
        BucketEntry {
            ancestor_hashes: self.ancestor_hashes(),
            entry: self.entry,
            selector: self.selector(),
            sibling_key: self.words[BUCKET_SLOT_SIBLING_KEY],
            subject_bloom: self.subject_bloom(),
        }
    }
}

/// Whether an element reaching an entry by the key of its bucket matches its selector.
enum Covered {
    No,
    Always,
    /// Where the element's tag is its own ASCII-lowercase form: the bucket of a type selector is
    /// reached by an element's tag and by its folding.
    ForALowercaseTag,
}

/// Whether an element reaching `selector`'s entry by the key of its bucket matches it: the selector
/// is one simple selector alone, which the bucket is the key of, or styles a pseudo-element of the
/// element it describes and nothing more, and is of a sheet declaring no default namespace, which
/// would constrain the type it implies. That simple selector is an id or a class written in ASCII
/// lowercase, `*` or a type selector in any namespace, `:root`, or a state.
fn is_covered_by_its_key(selector: crate::css::selector::Selector<'_>) -> Covered {
    use crate::css::selector::Combinator;
    if selector.names_the_scope() || selector.namespaces().default.is_some() {
        return Covered::No;
    }
    // A pseudo-element alone in its compound, other than `::part()` and `::slotted()`, is of the
    // element the compound left of it describes, which is matched for it.
    let subject = match selector.target_pseudo_element() {
        None => selector,
        Some(_) => {
            let target = selector.first_compound();
            if target.simple_selectors.len() != 1
                || target.combinator != Combinator::PseudoElement
                || !matches!(
                    super::super::component_matching::compound_target(&target),
                    super::super::component_matching::CompoundTarget::Transparent
                )
            {
                return Covered::No;
            }
            match selector.left() {
                Some((_, left)) if !left.is_relative_anchor() => left,
                _ => return Covered::No,
            }
        }
    };
    if subject.left().is_some() {
        return Covered::No;
    }
    compound_is_covered_by_its_key(subject)
}

/// Whether an element reaching `selector`'s entry by the key of its bucket matches it, whatever it
/// is otherwise.
pub(super) fn proves_the_whole_selector(selector: crate::css::selector::Selector<'_>) -> bool {
    matches!(is_covered_by_its_key(selector), Covered::Always)
}

/// Whether an element reaching `selector`'s entry by the key of its bucket matches its subject
/// compound, whatever it takes of the elements left of it: the selector has compounds left of a
/// subject that [`compound_is_covered_by_its_key`], and has no target pseudo-element.
fn subject_is_covered_by_its_key(selector: crate::css::selector::Selector<'_>) -> Covered {
    if selector.names_the_scope()
        || selector.namespaces().default.is_some()
        || !selector.describes_its_subject()
        || !selector.left().is_some_and(|(_, left)| !left.is_relative_anchor())
    {
        return Covered::No;
    }
    compound_is_covered_by_its_key(selector)
}

/// Whether the first compound of `subject` is one simple selector the key of a bucket proves.
fn compound_is_covered_by_its_key(subject: crate::css::selector::Selector<'_>) -> Covered {
    use crate::css::selector::{NamespaceType, PseudoClassType, SimpleSelector};
    let mut simple_selectors = subject.first_compound().simple_selectors.iter();
    let (Some(simple), None) = (simple_selectors.next(), simple_selectors.next()) else {
        return Covered::No;
    };
    let any_namespace = |namespace: NamespaceType| matches!(namespace, NamespaceType::Default | NamespaceType::Any);
    match simple {
        SimpleSelector::Id(name) | SimpleSelector::Class(name) if name.is_ascii_lowercase => Covered::Always,
        SimpleSelector::Universal(name) if any_namespace(name.namespace_type) => Covered::Always,
        SimpleSelector::TagName(name) if name.name_is_ascii_lowercase && any_namespace(name.namespace_type) => {
            Covered::ForALowercaseTag
        }
        SimpleSelector::PseudoClass(pseudo_class)
            if pseudo_class.pseudo_class == PseudoClassType::Root
                || super::super::compiler::state_fact_for(pseudo_class.pseudo_class).is_some() =>
        {
            Covered::Always
        }
        _ => Covered::No,
    }
}

/// A selector the buckets of one part of a rule index address: the page of the part's space it
/// is in, and where it starts in the page.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(super) struct SelectorRef(u32);

/// An arena of a selector space, by its place among the space's arenas.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct ArenaSlot(u32);

/// How many components a page of a selector space spans, as a power of two.
const SELECTOR_PAGE_BITS: u32 = 12;

/// The selectors one part of a rule index addresses, its own buckets', a segment's or the user
/// agent index's: the arenas they live in, mapped into one space of 32-bit references in pages of
/// 4096 components. An arena whose selectors span several pages takes several, which need not
/// stand together. A reference resolves through the space that made it alone.
#[derive(Clone, Default)]
pub(super) struct SelectorSpace {
    /// One handle to each arena, which keeps it.
    arenas: Vec<crate::css::selector::SelectorListRef>,
    /// Whose each page is, and where it starts in that arena.
    pages: Vec<(ArenaSlot, crate::css::selector_arena::ComponentOffset)>,
    /// The slot of each arena, by its serial.
    slots: HashMap<crate::css::selector_arena::SelectorArenaSerial, ArenaSlot>,
    /// The page of each page start of each arena.
    page_of: HashMap<(ArenaSlot, crate::css::selector_arena::ComponentOffset), u32>,
}

impl SelectorSpace {
    /// The reference of the selector of `selectors`' arena starting at `at`.
    fn add(
        &mut self,
        selectors: &crate::css::selector::SelectorListRef,
        at: crate::css::selector_arena::ComponentOffset,
    ) -> SelectorRef {
        let arenas = &mut self.arenas;
        let slot = *self.slots.entry(selectors.serial()).or_insert_with(|| {
            arenas.push(selectors.clone());
            ArenaSlot(u32::try_from(arenas.len() - 1).expect("selector space exhausted"))
        });
        let (start, offset) = at.in_page(SELECTOR_PAGE_BITS);
        let pages = &mut self.pages;
        let page = *self.page_of.entry((slot, start)).or_insert_with(|| {
            pages.push((slot, start));
            u32::try_from(pages.len() - 1).expect("selector space exhausted")
        });
        assert!(page < 1 << (32 - SELECTOR_PAGE_BITS), "selector space exhausted");
        SelectorRef(page << SELECTOR_PAGE_BITS | offset)
    }

    /// The reference of the selector of an entry.
    fn add_entry(&mut self, source: &EntrySource) -> SelectorRef {
        self.add(&source.selectors, source.at)
    }

    /// The selector `selector` names.
    pub(super) fn resolve(&self, selector: SelectorRef) -> crate::css::selector::Selector<'_> {
        let source = self.locate(selector);
        self.arenas[source.0.0 as usize].selector_at(source.1)
    }

    /// Where the selector `selector` names is, held by its arena.
    fn source(&self, selector: SelectorRef) -> EntrySource {
        let (slot, at) = self.locate(selector);
        EntrySource {
            selectors: self.arenas[slot.0 as usize].clone(),
            at,
        }
    }

    fn locate(&self, selector: SelectorRef) -> (ArenaSlot, crate::css::selector_arena::ComponentOffset) {
        let (slot, start) = self.pages[(selector.0 >> SELECTOR_PAGE_BITS) as usize];
        (slot, start.into_page(selector.0 & ((1 << SELECTOR_PAGE_BITS) - 1)))
    }

    fn heap_size(&self) -> usize {
        use crate::heap_size::{map_bytes, vec_bytes};
        vec_bytes(&self.arenas) + vec_bytes(&self.pages) + map_bytes(&self.slots) + map_bytes(&self.page_of)
    }

    fn shrink_to_fit(&mut self) {
        self.arenas.shrink_to_fit();
        self.pages.shrink_to_fit();
        self.slots.shrink_to_fit();
        self.page_of.shrink_to_fit();
    }
}

/// The entries of one tree scope, bucketed once for each kind of element they can match: the
/// elements of the scope, the host of its shadow root, the elements slotted into its slots, and
/// the parts exposed to it. An entry is in the families its subject allows besides the first.
#[derive(Clone, Default)]
pub(super) struct ScopeIndex {
    /// Every entry, which the elements of the scope match.
    pub(super) own: Buckets,
    /// The entries of `own` that revalidate.
    pub(super) own_revalidation: Buckets,
    /// The entries whose subject can be the host of the scope's shadow root, for `:host`.
    pub(super) host: Buckets,
    /// The entries whose subject is an element slotted into the scope, for `::slotted()`.
    pub(super) slotted: Buckets,
    /// The entries whose subject is a part exposed to the scope, for `::part()`.
    pub(super) part: Buckets,
}

/// Rule entries bucketed by the keys their subjects can be reached by, per tree scope. An entry whose
/// subject can be reached by any of several keys is in each of their buckets; one with no dispatchable
/// key set is in the universal bucket, which every element reaches.
///
/// NB: Where a scope's shadow root is, how deeply it is nested and whether it decides with the
///     document's author sheets are facts of the tree, not of the sheets, so the matcher reads
///     them where it matches rather than the index holding them.
#[derive(Clone, Default)]
pub(super) struct RuleIndex {
    /// Every entry, numbered across scopes, the dead ones among them.
    pub(super) entries: IndexEntries,
    /// How many of `entries` are dead.
    pub(super) dead_entries: usize,
    /// The entries of the document's sheets, but for the user agent's, which the user agent index
    /// holds.
    pub(super) document: ScopeIndex,
    /// The entries of the document's user sheets, which also decide in every shadow tree that does
    /// not decide with the document's author sheets, as the user agent's do. An index of the
    /// entries some change brought holds the user agent's that arrived here too.
    pub(super) document_non_author: ScopeIndex,
    /// The entries of the sheets of the shadow trees with sheets of their own, by the group of
    /// trees with the same sheets that shares them.
    pub(super) groups: HashMap<ScopeGroupID, ScopeGroup>,
    /// The group of each shadow tree with sheets of its own.
    group_of_scope: HashMap<TreeScopeID, ScopeGroupID>,
    /// The groups whose trees have each list of sheets, which their layers tell apart.
    groups_by_sheets: HashMap<Box<[SheetID]>, SmallVec<[ScopeGroupID; 1]>>,
    /// The number of the next group.
    next_group: u32,
    /// The ids an entry that does not revalidate tests for. No such entry tells an element with any
    /// other id from one with none.
    pub(super) tested_ids: HashSet<StyleAtomID>,
    /// The selector entries this build or update analyzed for its buckets, those of a user agent
    /// index it built among them.
    pub(super) compiled_selectors: u64,
    /// The entries this build or update indexed, those of a user agent index it built among them.
    pub(super) indexed_entries: u64,
    /// Whether this build or update built the user agent index it shares, which no document of the
    /// process had built before.
    pub(super) built_user_agent_index: bool,
    /// The container conditions of the gated rules, each rule's once, innermost first.
    gates: Vec<Box<[Arc<ContainerConditionsData>]>>,
    /// Whether one of the gates asks a `style()` question, which reads the custom properties of
    /// any element above its subject.
    pub(super) gates_ask_container_style: bool,
    /// What the index indexed, by the rule store's identities.
    indexed: Indexed,
    /// The space the selectors of the index's own buckets are in.
    pub(super) selectors: SelectorSpace,
    /// The `@scope` levels the rule of each entry inside any is inside.
    entry_scopes: HashMap<u32, Arc<super::super::bridge::BoundScopeChain>>,
}

/// The placement of the entries of each layer of one sheet deciding in one group.
type LayerPlacements = SmallVec<[(CascadeLayerID, PlacementID); 1]>;

/// The entries of a rule index, by their numbers: those of the user agent index the document
/// shares below [`FIRST_DOCUMENT_ENTRY`], and the document's own from there on.
#[derive(Clone, Default)]
pub(super) struct IndexEntries {
    user_agent: Option<Arc<UserAgentIndex>>,
    /// The document's entries, in pages documents numbering their entries alike share.
    own: SharedPages<IndexedEntry>,
    /// The number of the first of `own`: zero in a user agent index, which has no other entries.
    first: u32,
    /// Where the entries of `own` decide, which the tree scopes of the sheet attachment each is of
    /// name: a sheet attached to several scopes, as a constructed sheet adopted by several shadow
    /// roots is, has its entries indexed once for each group of them.
    placements: Vec<EntryPlacement>,
    /// The placement of the entries of each layer of each sheet deciding in each group.
    placements_of_sheet: HashMap<(SheetID, ScopeGroupID), LayerPlacements>,
    /// The sheets whose entries are bucketed and kept in a segment the process shares, rather than
    /// in the index's own buckets and entries, in the order of their numbers.
    segments: Vec<BoundSegment>,
    /// The place among `segments` of the segment holding the entries of each page of `own`, or
    /// `NO_SEGMENT`: a segment's numbers end a page, so no page holds two segments' entries.
    segment_of_page: Vec<u32>,
}

const NO_SEGMENT: u32 = u32::MAX;

impl IndexEntries {
    /// Where the entry `id` decides.
    pub(super) fn placement(&self, id: usize) -> &EntryPlacement {
        self.placement_of(self.view(id))
    }

    /// Where the entry `view` decides.
    pub(super) fn placement_of(&self, view: EntryView<'_>) -> &EntryPlacement {
        match view {
            EntryView::Held {
                entry,
                user_agent: false,
            } => &self.placements[entry.placement.0 as usize],
            EntryView::Held {
                entry,
                user_agent: true,
            } => {
                &self
                    .user_agent
                    .as_ref()
                    .expect("a user agent entry has its index")
                    .index
                    .entries
                    .placements[entry.placement.0 as usize]
            }
            EntryView::Bound { bound, entry, .. } => {
                &self.placements[bound.placements[usize::from(entry.placement)].0 as usize]
            }
        }
    }

    /// The entry numbered `id`.
    pub(super) fn entry(&self, id: usize) -> IndexedEntry {
        self.get(id).expect("an entry has its number")
    }

    /// The entry numbered `id`, read in place.
    pub(super) fn view(&self, id: usize) -> EntryView<'_> {
        self.try_view(id).expect("an entry has its number")
    }

    /// The segment holding the entry `entry`, by its place among the index's.
    fn segment_holding(&self, entry: u32) -> Option<usize> {
        let own = entry.checked_sub(self.first)? as usize;
        let place = *self.segment_of_page.get(own / super::shared_pages::VALUES_PER_PAGE)?;
        (place != NO_SEGMENT && self.segments[place as usize].holds(entry)).then_some(place as usize)
    }

    /// Note which segment holds the entries of each page of `own`, once the segments moved.
    fn note_segment_pages(&mut self) {
        let pages = self.own.len().div_ceil(super::shared_pages::VALUES_PER_PAGE);
        self.segment_of_page.clear();
        self.segment_of_page.resize(pages, NO_SEGMENT);
        for (place, bound) in self.segments.iter().enumerate() {
            let start = (bound.first - self.first) as usize;
            let end = start + bound.segment.len() as usize;
            for page in start / super::shared_pages::VALUES_PER_PAGE..end.div_ceil(super::shared_pages::VALUES_PER_PAGE)
            {
                self.segment_of_page[page] = u32::try_from(place).expect("segment space exhausted");
            }
        }
    }

    /// Have the numbers from here on start a page of `own`, which the numbers of entries a bound
    /// segment holds fill.
    fn pad_to_page(&mut self) {
        while !self.own.len().is_multiple_of(super::shared_pages::VALUES_PER_PAGE) {
            self.own.push(IndexedEntry::BOUND_NUMBER);
        }
    }

    /// Where an entry of a rule of `layer` of `sheet` deciding in `group` decides, which `placement`
    /// says where none such was placed before.
    fn place(
        &mut self,
        sheet: SheetID,
        group: ScopeGroupID,
        layer: CascadeLayerID,
        placement: impl FnOnce() -> EntryPlacement,
    ) -> PlacementID {
        let of_sheet = self.placements_of_sheet.entry((sheet, group)).or_default();
        if let Some(&(_, id)) = of_sheet.iter().find(|(placed, _)| *placed == layer) {
            return id;
        }
        let id = PlacementID(u32::try_from(self.placements.len()).expect("entry placement space exhausted"));
        self.placements.push(placement());
        of_sheet.push((layer, id));
        id
    }

    /// Have `rank` rank the placements of the entries of `sheet` deciding in `group` anew.
    fn rank_placements_of(&mut self, sheet: SheetID, group: ScopeGroupID, mut rank: impl FnMut(&mut EntryPlacement)) {
        for &(_, id) in self.placements_of_sheet.get(&(sheet, group)).into_iter().flatten() {
            rank(&mut self.placements[id.0 as usize]);
        }
    }

    /// How many numbers there are: one past the greatest.
    pub(super) fn len(&self) -> usize {
        self.first as usize + self.own.len()
    }

    /// How many entries there are, the dead ones among them.
    pub(super) fn count(&self) -> usize {
        self.user_agent_count() + self.own.len()
    }

    pub(super) fn get(&self, id: usize) -> Option<IndexedEntry> {
        self.try_view(id).map(|view| view.indexed())
    }

    /// The entry numbered `id`, read in place where it is held, if any has the number.
    pub(super) fn try_view(&self, id: usize) -> Option<EntryView<'_>> {
        match id.checked_sub(self.first as usize) {
            Some(own) => {
                let entry = self.own.get(own)?;
                let held = EntryView::Held {
                    entry,
                    user_agent: false,
                };
                if !entry.flags.has(EntryFlags::BOUND) {
                    return Some(held);
                }
                // NB: The padding after a segment's entries is of its last page, and is no entry.
                let id = u32::try_from(id).expect("rule entry space exhausted");
                Some(self.segment_holding(id).map_or(held, |place| {
                    let bound = &self.segments[place];
                    EntryView::Bound {
                        bound,
                        entry: &bound.segment.entries[(id - bound.first) as usize],
                        dead: bound.is_dead(id),
                    }
                }))
            }
            None => self
                .user_agent
                .as_ref()
                .and_then(|user_agent| user_agent.index.entries.own.get(id))
                .map(|entry| EntryView::Held {
                    entry,
                    user_agent: true,
                }),
        }
    }

    /// The entry numbered `id`, if any has the number, as it was before the update that let go of
    /// the segments `let_go`, which took what their entries were with them.
    pub(super) fn try_view_before<'a>(&'a self, let_go: &'a [BoundSegment], id: usize) -> Option<EntryView<'a>> {
        let number = u32::try_from(id).expect("rule entry space exhausted");
        match let_go.iter().find(|bound| bound.holds(number)) {
            Some(bound) => Some(EntryView::Bound {
                bound,
                entry: &bound.segment.entries[(number - bound.first) as usize],
                dead: true,
            }),
            None => self.try_view(id),
        }
    }

    /// How many entries the user agent index the document shares has.
    fn user_agent_count(&self) -> usize {
        self.user_agent
            .as_ref()
            .map_or(0, |user_agent| user_agent.index.entries.own.len())
    }

    /// The number of every entry, in order.
    #[cfg(test)]
    pub(super) fn numbers(&self) -> impl Iterator<Item = usize> {
        (0..self.user_agent_count()).chain(self.first as usize..self.len())
    }

    /// The numbers of the document's own entries.
    pub(super) fn own_numbers(&self) -> std::ops::Range<usize> {
        self.first as usize..self.len()
    }

    fn next_number(&self) -> u32 {
        u32::try_from(self.len()).expect("rule entry space exhausted")
    }

    fn own_mut(&mut self, id: u32) -> &mut IndexedEntry {
        debug_assert!(self.segment_holding(id).is_none(), "a bound segment holds its entries");
        self.own.get_mut((id - self.first) as usize)
    }
}

/// The entries of the user agent's rules deciding in a document, which every document of the
/// process whose user agent rules are the same shares: their buckets, selectors and
/// invalidation map are built once.
///
/// NB: The index is keyed by the user agent rules that decide, so documents whose media select
///     the same rules share one.
pub(in crate::css::style) struct UserAgentIndex {
    /// The rules the index indexes.
    variant: UserAgentVariant,
    /// The entries, numbered from zero, bucketed in the document's buckets.
    index: RuleIndex,
    /// The invalidation map of the entries, which a document's own map starts from.
    pub(super) invalidation: super::invalidation::InvalidationPart,
}

impl UserAgentIndex {
    /// Index the rules of `variant`, which `rules`, the process's user agent rules, hold.
    fn build(rules: &RuleStore, variant: &UserAgentVariant) -> Self {
        let mut index = RuleIndex::default();
        let mut added = Vec::new();
        for (rank, &sheet) in variant.sheets().iter().enumerate() {
            let placement = SheetPlacement {
                origin: CascadeOrigin::UserAgent,
                sheet_rank: rank as u64,
            };
            for rule in rules.rules_in_sheet(sheet) {
                if variant.leaves_out(rule) {
                    continue;
                }
                let first = index.entries.own.len();
                let first_added = added.len();
                index.index_rule(rules, rule, ScopeGroupID::DOCUMENT, placement, &mut added);
                for &id in &added[first_added..] {
                    index.bucket(id);
                }
                for number in first..index.entries.own.len() {
                    index.entries.own.get_mut(number).rule = EntryRule::user_agent(rule);
                }
            }
        }
        assert!(
            index.entries.own.len() <= FIRST_DOCUMENT_ENTRY as usize,
            "the user agent's entries fit below the document's"
        );
        assert!(
            index.gates.is_empty(),
            "no user agent rule applies under container conditions"
        );
        // NB: The identities the rules were indexed by are the process's, which no document names.
        index.indexed = Indexed::default();
        index.document_non_author = ScopeIndex::default();
        index.shrink_to_fit();
        // NB: Every document's map starts from this one, sharing what it holds.
        let mut invalidation = super::invalidation::InvalidationPart::default();
        invalidation.extend(0..index.entries.len(), |entry| {
            let indexed = index.entries.entry(entry as usize);
            indexed
                .live()
                .then(|| (index.entry_selector(entry), index.arena_of_entry(entry)))
        });
        invalidation.freeze();
        Self {
            variant: variant.clone(),
            index,
            invalidation,
        }
    }
}

/// The fewest entries a sheet has for its entries to be bucketed in a segment the process shares.
/// Matching probes the buckets of each segment apart, which costs more than a few entries of the
/// document's own buckets save.
pub(super) const SEGMENT_MIN_ENTRIES: usize = 64;

/// One entry of a sheet segment, which every document binding the segment shares: what the entry
/// is relative to its sheet. The binding says the rest: the rule the offsets count from, and the
/// placement of each slot.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(super) struct SegmentEntry {
    /// The entry's rule, as an offset from the binding's first rule.
    rule: u32,
    /// The entry's selector, in the segment's space.
    selector: SelectorRef,
    specificity: PackedSpecificity,
    /// The slot of the entry's placement among the binding's placements.
    placement: u16,
    pseudo_kind: u8,
    flags: EntryFlags,
    /// The rank of the entry's rule in its sheet, which the documents sharing the sheet's rule
    /// order share.
    rule_rank: u64,
}

const _: () = assert!(size_of::<SegmentEntry>() == 24);

/// What a document asks a segment to hold for one entry: the entry relative to its sheet, where its
/// selector is, and the `@scope` levels its rule is inside, if any.
struct HeldEntry {
    entry: SegmentEntry,
    source: EntrySource,
    scope: Option<Arc<super::super::bridge::BoundScopeChain>>,
}

/// What tells a segment entry from another for as long as the process lives: the entry but for its
/// selector, the selector by its arena's serial and offset, and the address of its scope chain.
type HeldIdentity = (
    (u32, PackedSpecificity, u16, u8, EntryFlags, u64),
    crate::css::selector_arena::SelectorKey,
    usize,
);

fn held_identity(
    entry: &SegmentEntry,
    selector: crate::css::selector_arena::SelectorKey,
    scope: Option<&Arc<super::super::bridge::BoundScopeChain>>,
) -> HeldIdentity {
    (
        (
            entry.rule,
            entry.specificity,
            entry.placement,
            entry.pseudo_kind,
            entry.flags,
            entry.rule_rank,
        ),
        selector,
        scope.map_or(0, |scope| Arc::as_ptr(scope) as usize),
    )
}

impl HeldEntry {
    fn selector(&self) -> super::super::component_matching::EntrySelector<'_> {
        super::super::component_matching::EntrySelector::new(self.source.selector(), self.scope.as_deref())
    }

    fn identity(&self) -> HeldIdentity {
        held_identity(&self.entry, self.source.identity(), self.scope.as_ref())
    }
}

/// The entries of one sheet's rules deciding in a document, bucketed, which every
/// document of the process indexing the same selectors for one of its sheets shares: the frames of
/// a site loading one sheet bucket its rules once, and hold none of its entries. Each document
/// numbers the entries from a first number of its own, and binds the rest of what an entry is to
/// it: the rule the entries' rules count from, and the placements of their layers.
///
/// NB: A document matches against its own buckets and then the segment of each sheet it binds,
///     in turn.
pub(in crate::css::style) struct SheetSegment {
    /// Each entry, in the order of their numbers.
    entries: Box<[SegmentEntry]>,
    /// The `@scope` levels of the rules of the entries inside any, by entry, in the order of the
    /// entries.
    scopes: Box<[(u32, Arc<super::super::bridge::BoundScopeChain>)]>,
    /// The space the entries' and the buckets' selectors are in.
    selector_space: SelectorSpace,
    /// The entries, numbered from zero, bucketed as the document's entries of a scope are.
    index: ScopeIndex,
    /// The ids an entry that does not revalidate tests for.
    tested_ids: HashSet<StyleAtomID>,
    /// The invalidation map of the entries, numbered from zero, which the maps of the documents
    /// binding the segment share.
    pub(super) invalidation: super::invalidation::InvalidationPart,
}

impl SheetSegment {
    fn build(held: Vec<HeldEntry>) -> Self {
        let mut index = ScopeIndex::default();
        let mut tested_ids = HashSet::default();
        let mut selector_space = SelectorSpace::default();
        let mut entries = Vec::with_capacity(held.len());
        let mut scopes = Vec::new();
        for (number, held) in held.iter().enumerate() {
            let number = u32::try_from(number).expect("rule entry space exhausted");
            let entry_selector = held.selector();
            let facts = EntryFacts::of(entry_selector);
            let selector = selector_space.add_entry(&held.source);
            let slot = BucketEntry::new(entry_selector.selector, number, selector, held.scope.is_some());
            let (keys, families) = placement_in_buckets(entry_selector.selector, facts);
            index.insert(slot, &keys, families);
            if !facts.never_matches && !facts.revalidates {
                let mut ids = Vec::new();
                collect_tested_ids(entry_selector.selector, &mut ids);
                tested_ids.extend(ids);
            }
            entries.push(SegmentEntry { selector, ..held.entry });
            if let Some(scope) = &held.scope {
                scopes.push((number, Arc::clone(scope)));
            }
        }
        index.shrink_to_fit();
        tested_ids.shrink_to_fit();
        selector_space.shrink_to_fit();
        let mut invalidation = super::invalidation::InvalidationPart::default();
        for buckets in [
            &mut index.own,
            &mut index.own_revalidation,
            &mut index.host,
            &mut index.slotted,
            &mut index.part,
        ] {
            buckets.freeze();
        }
        invalidation.leave_lone_entries_to_buckets();
        invalidation.extend(0..held.len(), |entry| {
            let held = &held[entry as usize];
            Some((held.selector(), &held.source.selectors))
        });
        invalidation.freeze();
        Self {
            entries: entries.into(),
            scopes: scopes.into(),
            selector_space,
            index,
            tested_ids,
            invalidation,
        }
    }

    pub(super) fn len(&self) -> u32 {
        self.entries.len() as u32
    }

    /// The entries of the segment the bucket of `key` holds that its key proves.
    pub(super) fn entries_proven_by(&self, key: DispatchKey) -> impl Iterator<Item = u32> + '_ {
        self.index
            .own
            .get(&key)
            .into_iter()
            .flatten()
            .filter(|slot| slot.covered(false))
            .map(|slot| slot.entry())
    }

    /// Whether the buckets of the segment hold any entry under `key`.
    pub(super) fn buckets_hold(&self, key: DispatchKey) -> bool {
        self.index.own.get(&key).is_some()
    }

    /// The `@scope` levels the rule of entry `entry` of the segment is inside, if any.
    fn scope_of(&self, entry: u32) -> Option<&Arc<super::super::bridge::BoundScopeChain>> {
        self.scopes
            .binary_search_by_key(&entry, |&(number, _)| number)
            .ok()
            .map(|place| &self.scopes[place].1)
    }

    fn hash_of(held: &[HeldEntry]) -> u64 {
        use std::hash::{Hash, Hasher};
        let mut hasher = super::super::fast_hash::fast_hasher();
        for held in held {
            held.identity().hash(&mut hasher);
        }
        hasher.finish()
    }

    fn holds(&self, held: &[HeldEntry]) -> bool {
        self.entries.len() == held.len()
            && self
                .entries
                .iter()
                .zip(held)
                .enumerate()
                .all(|(number, (entry, other))| {
                    let number = number as u32;
                    held_identity(
                        entry,
                        self.selector_space.resolve(entry.selector).key(),
                        self.scope_of(number),
                    ) == other.identity()
                })
    }

    /// The bytes of the segment, which the documents binding it reach through it.
    fn heap_size(&self) -> usize {
        use crate::heap_size::set_bytes;
        let buckets = |buckets: &Buckets| buckets.heap_size();
        size_of_val::<[SegmentEntry]>(&self.entries)
            + size_of_val::<[(u32, Arc<super::super::bridge::BoundScopeChain>)]>(&self.scopes)
            + [
                &self.index.own,
                &self.index.own_revalidation,
                &self.index.host,
                &self.index.slotted,
                &self.index.part,
            ]
            .into_iter()
            .map(buckets)
            .sum::<usize>()
            + set_bytes(&self.tested_ids)
            + self.selector_space.heap_size()
            + self.invalidation.heap_size()
            + self.invalidation.shared_heap_size()
    }
}

/// A segment the entries of one sheet deciding in one group are bucketed and kept in, numbered in
/// the document from `first`.
#[derive(Clone)]
pub(super) struct BoundSegment {
    segment: Arc<SheetSegment>,
    first: u32,
    sheet: SheetID,
    group: ScopeGroupID,
    origin: CascadeOrigin,
    /// The rule the segment's entries count their rules from.
    first_rule: RuleID,
    /// The placement of each slot the segment's entries name.
    placements: SmallVec<[PlacementID; 1]>,
    /// One bit for each of the segment's entries that died for this index, whose buckets, shared
    /// with the other documents binding the segment, go on holding them. Empty until one dies.
    dead: Vec<u64>,
}

impl BoundSegment {
    /// Whether the entry `id`, which the segment holds, died for this index.
    pub(super) fn is_dead(&self, id: u32) -> bool {
        let at = (id - self.first) as usize;
        self.dead.get(at / 64).is_some_and(|word| word & (1 << (at % 64)) != 0)
    }

    /// Note that the entry `id`, which the segment holds, died for this index.
    fn note_dead(&mut self, id: u32) {
        let at = (id - self.first) as usize;
        if self.dead.is_empty() {
            self.dead = vec![0; (self.segment.len() as usize).div_ceil(64)];
        }
        self.dead[at / 64] |= 1 << (at % 64);
    }

    fn holds(&self, entry: u32) -> bool {
        (self.first..self.first + self.segment.len()).contains(&entry)
    }

    /// The entry `id` of the document, which the segment holds, as the document has it but for its
    /// selector, which is in the segment's space.
    fn entry(&self, id: u32) -> IndexedEntry {
        EntryView::Bound {
            bound: self,
            entry: &self.segment.entries[(id - self.first) as usize],
            dead: self.is_dead(id),
        }
        .indexed()
    }
}

/// The buckets of one part of an index, and the number their entries are numbered from.
#[derive(Clone, Copy)]
pub(super) struct BucketSource<'a> {
    pub(super) buckets: &'a ScopeIndex,
    pub(super) first: u32,
    /// The space the buckets' selectors are in.
    pub(super) space: &'a SelectorSpace,
    /// The bound segment the buckets are of, which holds their entries, where they are a segment's.
    bound: Option<&'a BoundSegment>,
}

/// What a match of an entry takes from it.
#[derive(Clone, Copy)]
pub(super) struct MatchedEntry {
    pub(super) rule: RuleID,
    /// The group of trees the entry decides in.
    pub(super) group: ScopeGroupID,
    pub(super) specificity: Specificity,
    pub(super) pseudo_kind: u8,
}

impl<'a> BucketSource<'a> {
    /// The binding of the segment the buckets are of, where entries of its died for the index.
    pub(super) fn binding_with_dead_entries(&self) -> Option<&'a BoundSegment> {
        self.bound.filter(|bound| !bound.dead.is_empty())
    }

    /// What a match of the entry numbered `id` of `index`, which the buckets hold, takes from it:
    /// read in place from the segment the buckets are of, which decides in its binding's group,
    /// where they are a segment's.
    pub(super) fn matched(&self, index: &RuleIndex, id: u32) -> MatchedEntry {
        match self.bound {
            Some(bound) => {
                let entry = &bound.segment.entries[(id - bound.first) as usize];
                MatchedEntry {
                    rule: RuleID(bound.first_rule.0 + entry.rule),
                    group: bound.group,
                    specificity: entry.specificity.unpacked(),
                    pseudo_kind: entry.pseudo_kind,
                }
            }
            None => {
                let view = index.entries.view(id as usize);
                MatchedEntry {
                    rule: index.rule_of_view(view),
                    group: index.entries.placement_of(view).group,
                    specificity: view.specificity(),
                    pseudo_kind: view.pseudo_kind(),
                }
            }
        }
    }
}

/// The rule index data the process shares between documents: the user agent indexes, one for each
/// set of user agent rules a document decided with, and the segments of the sheets documents
/// index alike.
#[derive(Default)]
pub(in crate::css::style) struct ProcessRuleIndexes {
    indexes: std::sync::Mutex<Vec<Arc<UserAgentIndex>>>,
    /// The segments the process built, by the hash of their selectors, which go once no document
    /// binds them and `kept_segments` lets go of them.
    segments: std::sync::Mutex<HashMap<u64, SmallVec<[std::sync::Weak<SheetSegment>; 1]>>>,
    /// The segments a page of the same site finds as it binds them again.
    kept_segments: std::sync::Mutex<crate::css::style::kept_past_release::KeptPastRelease<SheetSegment>>,
    /// The pages of the documents' entries, which documents numbering their entries alike share.
    entry_pages: PagePool<IndexedEntry>,
    /// The pages of the lists of the entries of each rule, which documents numbering their rules
    /// and entries alike share.
    entry_extents: PagePool<Extent>,
    entry_lists: PagePool<u32>,
    /// The top-level rules and rule orders of the documents' sheets, which documents whose sheets
    /// hold their rules alike share.
    sheet_rules: std::sync::Mutex<WeakPool<Vec<RuleID>>>,
    sheet_rule_orders: std::sync::Mutex<WeakPool<crate::css::style::order::OrderMaintenance>>,
    /// The pages of the documents' rule records, rule versions and native rule targets, which
    /// documents holding their rules alike share.
    pub(super) rule_record_pages: super::rule_store::RuleRecordPages,
    pub(super) rule_version_pages: super::rule_store::RuleVersionPages,
    pub(super) target_pages: super::rule_store::TargetPages,
}

impl ProcessRuleIndexes {
    /// Share the top-level rules and rule order of a sheet with the documents of the process
    /// whose sheets hold their rules alike.
    pub(super) fn share_sheet_rules(
        &self,
        rules: &mut Arc<Vec<RuleID>>,
        rule_order: &mut Arc<crate::css::style::order::OrderMaintenance>,
    ) {
        fn share<T: PartialEq + Hash>(pool: &std::sync::Mutex<WeakPool<T>>, value: &mut Arc<T>) {
            let mut hasher = super::super::fast_hash::fast_hasher();
            value.hash(&mut hasher);
            let hash = std::hash::Hasher::finish(&hasher);
            let mut pool = pool.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
            match pool.find(hash, |candidate| candidate == &**value) {
                Some(found) => *value = found,
                None => pool.insert(hash, value),
            }
        }
        share(&self.sheet_rules, rules);
        share(&self.sheet_rule_orders, rule_order);
    }

    /// The segment of `selectors`, built where no document of the process binds one, and whether
    /// it was.
    fn segment_of(&self, held: Vec<HeldEntry>) -> (Arc<SheetSegment>, bool) {
        let hash = SheetSegment::hash_of(&held);
        let mut segments = self.segments.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        if let Some(found) = segments
            .get(&hash)
            .into_iter()
            .flatten()
            .filter_map(std::sync::Weak::upgrade)
            .find(|segment| segment.holds(&held))
        {
            self.kept_segments
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner)
                .note_use(&found);
            return (found, false);
        }
        // NB: Building under the lock has a document indexing the same sheet beside this one wait
        //     for the segment and share it, rather than build another.
        let segment = Arc::new(SheetSegment::build(held));
        segments.retain(|_, held| {
            held.retain(|segment| segment.strong_count() > 0);
            !held.is_empty()
        });
        segments.entry(hash).or_default().push(Arc::downgrade(&segment));
        let left = self
            .kept_segments
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .keep(&segment, size_of::<SheetSegment>() + segment.heap_size());
        drop(segments);
        drop(left);
        (segment, true)
    }

    /// Let go of the segments only the process keeps and nothing asked for lately.
    pub(in crate::css::style) fn prune_kept_segments(&self) {
        let left = self
            .kept_segments
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .prune();
        drop(left);
    }

    /// Let go of the segments only the process keeps.
    pub(in crate::css::style) fn release_kept_segments(&self) {
        let left = self
            .kept_segments
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .release_all();
        drop(left);
    }

    /// Build the index of `variant` where no document of the process built it yet.
    pub(in crate::css::style) fn index_ahead(&self, rules: &RuleStore, variant: &UserAgentVariant) {
        self.get_or_build(rules, variant);
    }

    /// The index of `variant`, built from `rules`, the process's user agent rules, where no document
    /// of the process built it yet, and whether it was.
    fn get_or_build(&self, rules: &RuleStore, variant: &UserAgentVariant) -> (Arc<UserAgentIndex>, bool) {
        let mut indexes = self.indexes.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        if let Some(index) = indexes.iter().find(|index| index.variant == *variant) {
            return (Arc::clone(index), false);
        }
        let index = Arc::new(UserAgentIndex::build(rules, variant));
        indexes.push(Arc::clone(&index));
        (index, true)
    }
}

/// The selectors `rule` indexes entries from, if it indexes any.
fn indexed_selectors(program: &RuleStore, rule: RuleID) -> Option<crate::css::selector::SelectorListRef> {
    if !program.rule_can_decide(rule) {
        return None;
    }
    if !program.rule_version(rule).kind.matches_elements() {
        return None;
    }
    program.rule_selectors(rule)
}

/// Whether the rules of `sheet` decide through the user agent index: those of a user agent sheet of
/// the document.
fn is_user_agent_sheet(program: &RuleStore, sheet: SheetID) -> bool {
    program.sheet_origin(sheet) == CascadeOrigin::UserAgent
}

/// What a rule index indexed, by the identities of the rules and sheets in the rule store, which a
/// change names the rules and sheets it moved by.
#[derive(Clone, Default)]
struct Indexed {
    /// The live entries of each rule, in every scope.
    entries_of_rule: PagedExtentMap<u32>,
    /// The groups each contributing sheet's rules are indexed in, with the rank of the sheet in
    /// each that its entries hold.
    sheets: HashMap<SheetID, SmallVec<[(ScopeGroupID, u64); 1]>>,
    /// How many times the rule order of each sheet was relabeled, as the rule ranks of its entries
    /// follow.
    rule_order_relabels: HashMap<SheetID, u64>,
    /// The gate of each rule under container conditions.
    gate_of_rule: HashMap<RuleID, ContainerGateID>,
    /// The gate of each list of container conditions, by the addresses of the conditions: the
    /// rules of one container rule share its gate.
    gate_of_conditions: HashMap<Box<[usize]>, ContainerGateID>,
}

/// What a change to the rules names: the rules and sheets that moved, and the scopes whose sheet
/// or layer order moved.
pub(super) struct IndexChanges<'a> {
    /// The rules whose contents, conditions, layering or existence moved.
    pub(super) rules: &'a HashSet<RuleID>,
    /// Those of `rules` where more than their conditions moved.
    pub(super) rule_contents: &'a HashSet<RuleID>,
    /// The sheets attached to or detached from a scope, or whose activation moved.
    pub(super) sheets: &'a HashSet<SheetID>,
    /// The scopes whose sheets or layers were reordered.
    pub(super) reordered_scopes: &'a HashSet<TreeScopeID>,
    /// The scopes whose layer order moved, which moves the layer ranks of their entries.
    pub(super) layered_scopes: &'a HashSet<TreeScopeID>,
}

/// Where a sheet decides in one group of scopes: its origin and rank there.
#[derive(Clone, Copy)]
struct SheetPlacement {
    origin: CascadeOrigin,
    sheet_rank: u64,
}

impl SheetPlacement {
    /// Where `sheet` decides in the document.
    fn in_document(program: &RuleStore, sheet: SheetID) -> Self {
        Self {
            origin: program.sheet_origin(sheet),
            sheet_rank: program
                .attachment_in_scope(sheet, TreeScopeID::DOCUMENT)
                .map_or(0, |attachment| program.sheet_rank(attachment)),
        }
    }
}

fn is_non_author(origin: CascadeOrigin) -> bool {
    matches!(origin, CascadeOrigin::UserAgent | CascadeOrigin::User)
}

/// Whether elements report carrying the key when they are probed.
pub(super) fn is_probed(key: &DispatchKey) -> bool {
    matches!(
        key,
        DispatchKey::Universal
            | DispatchKey::State(_)
            | DispatchKey::Root
            | DispatchKey::Heading
            | DispatchKey::TagName(_)
            | DispatchKey::Id(_)
            | DispatchKey::Class(_)
            | DispatchKey::Directionality(_)
            | DispatchKey::CustomState(_)
            | DispatchKey::Part(_)
            | DispatchKey::AttributeName(_)
    )
}

impl RuleIndex {
    /// The bytes of the index's entries and buckets, without the user agent index the process shares between documents.
    pub(in crate::css::style) fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        use crate::heap_size::{map_bytes, set_bytes, vec_bytes};
        fn buckets_heap(buckets: &Buckets) -> usize {
            buckets.heap_size()
        }
        fn scope_heap(scope: &ScopeIndex) -> usize {
            [
                &scope.own,
                &scope.own_revalidation,
                &scope.host,
                &scope.slotted,
                &scope.part,
            ]
            .into_iter()
            .map(buckets_heap)
            .sum()
        }
        let entries = self.entries.own.heap_size(sizer, |_, _| 0);
        let indexed = &self.indexed;
        let indexed_bytes = indexed.entries_of_rule.heap_size(sizer)
            + map_bytes(&indexed.sheets)
            + indexed
                .sheets
                .values()
                .map(crate::heap_size::small_vec_bytes)
                .sum::<usize>()
            + map_bytes(&indexed.rule_order_relabels)
            + map_bytes(&indexed.gate_of_rule)
            + map_bytes(&indexed.gate_of_conditions)
            + indexed
                .gate_of_conditions
                .keys()
                .map(|key| size_of_val::<[usize]>(key))
                .sum::<usize>();
        entries
            + self.selectors.heap_size()
            + map_bytes(&self.entry_scopes)
            + vec_bytes(&self.entries.segments)
            + scope_heap(&self.document)
            + scope_heap(&self.document_non_author)
            + map_bytes(&self.groups)
            + self
                .groups
                .values()
                .map(|group| {
                    scope_heap(&group.index)
                        + crate::heap_size::small_vec_bytes(&group.scopes)
                        + size_of_val::<[SheetID]>(&group.sheets)
                        + map_bytes(&group.layer_ranks)
                })
                .sum::<usize>()
            + map_bytes(&self.group_of_scope)
            + map_bytes(&self.groups_by_sheets)
            + self
                .groups_by_sheets
                .iter()
                .map(|(sheets, groups)| size_of_val::<[SheetID]>(sheets) + crate::heap_size::small_vec_bytes(groups))
                .sum::<usize>()
            + set_bytes(&self.tested_ids)
            + vec_bytes(&self.gates)
            + self
                .gates
                .iter()
                .map(|gate| size_of_val::<[Arc<ContainerConditionsData>]>(gate))
                .sum::<usize>()
            + indexed_bytes
    }

    /// The bytes of the user agent index the process shares between documents, the first time
    /// `sizer` reaches it.
    pub(in crate::css::style) fn user_agent_heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        let Some(user_agent) = &self.entries.user_agent else {
            return 0;
        };
        if !sizer.first_sight(Arc::as_ptr(user_agent)) {
            return 0;
        }
        crate::heap_size::ARC_HEADER_BYTES
            + size_of::<UserAgentIndex>()
            + user_agent.variant.heap_size()
            + user_agent.index.heap_size(sizer)
            + user_agent.invalidation.heap_size()
            + user_agent.invalidation.shared_heap_size()
    }

    /// The bytes of the sheet segments the index binds, which the process shares between
    /// documents, each the first time `sizer` reaches it.
    pub(in crate::css::style) fn segments_heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        let mut bytes = 0;
        for bound in &self.entries.segments {
            bytes += crate::heap_size::vec_bytes(&bound.dead);
            if sizer.first_sight(Arc::as_ptr(&bound.segment)) {
                bytes += crate::heap_size::ARC_HEADER_BYTES + size_of::<SheetSegment>() + bound.segment.heap_size();
            }
        }
        bytes
    }

    /// The entry of this index that stands for each entry of `previous`, the same selector of the
    /// same rule deciding in the same scope, or none where the rule left it or the entry is dead.
    pub(super) fn renumbering_from(&self, previous: &RuleIndex) -> Vec<Option<u32>> {
        // NB: The groups of the scopes are numbered anew, and named here by their first scope.
        #[allow(clippy::type_complexity)]
        let mut by_key: HashMap<
            (
                RuleID,
                (crate::css::selector_arena::ComponentOffset, u8),
                Option<TreeScopeID>,
            ),
            u32,
        > = HashMap::default();
        for id in self.entries.own_numbers() {
            let entry = self.entries.entry(id);
            if !entry.live() {
                continue;
            }
            by_key.insert(
                (
                    self.rule_of(id as u32),
                    (self.selector_of_entry(id as u32).offset(), entry.pseudo_kind),
                    self.first_scope_of(self.entries.placement(id).group),
                ),
                u32::try_from(id).expect("rule entry space exhausted"),
            );
        }
        // The user agent's entries stand for themselves where the same selector has the same
        // number in both.
        let user_agent_stays = stays_in_user_agent_index(previous, self);
        (0..previous.entries.len())
            .map(|id| {
                let entry = previous.entries.get(id).filter(|entry| entry.live())?;
                let number = u32::try_from(id).expect("rule entry space exhausted");
                match id < previous.entries.first as usize {
                    true => user_agent_stays(id).then_some(number),
                    false => by_key
                        .get(&(
                            previous.rule_of(number),
                            (previous.selector_of_entry(number).offset(), entry.pseudo_kind),
                            previous.first_scope_of(previous.entries.placement(id).group),
                        ))
                        .copied(),
                }
            })
            .collect()
    }

    /// The group whose entries decide in `scope`, where it has sheets.
    /// Whether every entry decides in the document's tree: no shadow tree has sheets of its own.
    pub(super) fn decides_only_in_the_document(&self) -> bool {
        self.groups.is_empty()
            && self
                .entries
                .segments
                .iter()
                .all(|bound| bound.group == ScopeGroupID::DOCUMENT)
    }

    pub(super) fn group_of(&self, scope: TreeScopeID) -> Option<ScopeGroupID> {
        match scope {
            TreeScopeID::DOCUMENT => Some(ScopeGroupID::DOCUMENT),
            _ => self.group_of_scope.get(&scope).copied(),
        }
    }

    /// The buckets of the entries deciding in the shadow tree `scope`, where it has sheets.
    #[cfg(test)]
    pub(super) fn scope_index(&self, scope: TreeScopeID) -> Option<&ScopeIndex> {
        let group = self.group_of_scope.get(&scope)?;
        self.groups.get(group).map(|group| &group.index)
    }

    /// The tree scopes deciding with the entries of `group`: none for a group that is gone.
    pub(super) fn scopes_of(&self, group: ScopeGroupID) -> &[TreeScopeID] {
        match group {
            ScopeGroupID::DOCUMENT => &[TreeScopeID::DOCUMENT],
            _ => self.groups.get(&group).map_or(&[], |group| group.scopes.as_slice()),
        }
    }

    /// The least of the tree scopes of `group`, which names it across builds.
    fn first_scope_of(&self, group: ScopeGroupID) -> Option<TreeScopeID> {
        self.scopes_of(group).iter().copied().min()
    }

    /// The tree scope the layers of the entries of `group` rank in.
    fn layer_scope_of(&self, group: ScopeGroupID) -> TreeScopeID {
        match group {
            ScopeGroupID::DOCUMENT => TreeScopeID::DOCUMENT,
            _ => self.groups[&group].scopes[0],
        }
    }

    /// The sheets the entries of `group` are of, in cascade order.
    fn sheets_of_group<'a>(&'a self, program: &'a RuleStore, group: ScopeGroupID) -> &'a [SheetID] {
        match group {
            ScopeGroupID::DOCUMENT => program.sheets_in_scope(TreeScopeID::DOCUMENT),
            _ => &self.groups[&group].sheets,
        }
    }

    /// Where `sheet` decides in `group`: in a group of shadow trees, the sheet ranks by its place
    /// among their sheets, which are the same in each.
    fn placement(&self, program: &RuleStore, sheet: SheetID, group: ScopeGroupID) -> SheetPlacement {
        match group {
            ScopeGroupID::DOCUMENT => SheetPlacement::in_document(program, sheet),
            _ => SheetPlacement {
                origin: program.sheet_origin(sheet),
                sheet_rank: self.groups[&group]
                    .sheets
                    .iter()
                    .position(|&held| held == sheet)
                    .map_or(0, |position| position as u64),
            },
        }
    }

    /// The group of the shadow trees whose sheets and layers are those of the shadow tree `scope`,
    /// if one exists.
    fn group_alike(&self, program: &RuleStore, scope: TreeScopeID) -> Option<ScopeGroupID> {
        self.groups_by_sheets
            .get(program.sheets_in_scope(scope))?
            .iter()
            .copied()
            .find(|group| program.layer_ranks_match(scope, &self.groups[group].layer_ranks))
    }

    /// Have the shadow tree `scope`, which is in no group, join the group of the trees alike, or a
    /// new one of its own. Answers the group, and whether it is new, which holds no entries yet.
    fn join_group(&mut self, program: &RuleStore, scope: TreeScopeID) -> (ScopeGroupID, bool) {
        debug_assert!(!self.group_of_scope.contains_key(&scope));
        if let Some(group) = self.group_alike(program, scope) {
            self.groups
                .get_mut(&group)
                .expect("a group alike is held")
                .scopes
                .push(scope);
            self.group_of_scope.insert(scope, group);
            return (group, false);
        }
        self.next_group += 1;
        let group = ScopeGroupID(self.next_group);
        let sheets: Box<[SheetID]> = program.sheets_in_scope(scope).into();
        self.groups_by_sheets.entry(sheets.clone()).or_default().push(group);
        self.groups.insert(
            group,
            ScopeGroup {
                index: ScopeIndex::default(),
                scopes: smallvec::smallvec![scope],
                sheets,
                layer_ranks: program.layer_ranks(scope),
            },
        );
        self.group_of_scope.insert(scope, group);
        (group, true)
    }

    /// Have the shadow tree `scope` leave its group, which dies with its last tree: its entries
    /// die, noted in `died`. Answers the group it left.
    fn leave_group(&mut self, program: &RuleStore, scope: TreeScopeID, died: &mut Vec<u32>) -> Option<ScopeGroupID> {
        let group = self.group_of_scope.remove(&scope)?;
        let held = self.groups.get_mut(&group).expect("a scope's group is held");
        held.scopes.retain(|&mut member| member != scope);
        if !held.scopes.is_empty() {
            return Some(group);
        }
        let held = self.groups.remove(&group).expect("a scope's group is held");
        if let Some(groups) = self.groups_by_sheets.get_mut(&held.sheets) {
            groups.retain(|&mut other| other != group);
            if groups.is_empty() {
                self.groups_by_sheets.remove(&held.sheets);
            }
        }
        self.drop_segments(None, group, died);
        for &sheet in &held.sheets {
            let Some(groups) = self.indexed.sheets.get_mut(&sheet) else {
                continue;
            };
            let before = groups.len();
            groups.retain(|&mut (indexed, _)| indexed != group);
            if groups.len() == before {
                continue;
            }
            if groups.is_empty() {
                self.indexed.sheets.remove(&sheet);
            }
            self.entries.placements_of_sheet.remove(&(sheet, group));
            for rule in program.all_rules_in_sheet(sheet) {
                self.kill_rule(rule, Some(group), died);
            }
        }
        Some(group)
    }

    /// Have the group of the shadow tree `scope`, its only tree, take the sheets and layers the
    /// tree has now, where no other group has them.
    fn regroup_alone(&mut self, program: &RuleStore, scope: TreeScopeID) -> bool {
        let Some(&group) = self.group_of_scope.get(&scope) else {
            return false;
        };
        if self.groups[&group].scopes.len() != 1 || self.group_alike(program, scope).is_some() {
            return false;
        }
        let held = self.groups.get_mut(&group).expect("a scope's group is held");
        let sheets: Box<[SheetID]> = program.sheets_in_scope(scope).into();
        let previous = std::mem::replace(&mut held.sheets, sheets.clone());
        held.layer_ranks = program.layer_ranks(scope);
        if let Some(groups) = self.groups_by_sheets.get_mut(&previous) {
            groups.retain(|&mut other| other != group);
            if groups.is_empty() {
                self.groups_by_sheets.remove(&previous);
            }
        }
        self.groups_by_sheets.entry(sheets).or_default().push(group);
        true
    }

    /// Each entry as itself where it lives, and none where it is dead.
    pub(super) fn live_numbering(&self) -> Vec<Option<u32>> {
        (0..self.entries.len())
            .map(|id| {
                self.entries
                    .get(id)
                    .is_some_and(|entry| entry.live())
                    .then(|| u32::try_from(id).expect("rule entry space exhausted"))
            })
            .collect()
    }

    /// The rule of the document `entry` is of.
    pub(super) fn rule_of(&self, entry: u32) -> RuleID {
        self.rule_of_view(self.entries.view(entry as usize))
    }

    /// The rule of the entry `view`.
    pub(super) fn rule_of_view(&self, view: EntryView<'_>) -> RuleID {
        RuleID(view.rule().0)
    }

    /// Whether an entry that does not revalidate tests for the id `id`.
    pub(super) fn tests_id(&self, id: StyleAtomID) -> bool {
        self.tested_ids.contains(&id)
            || self
                .entries
                .user_agent
                .as_ref()
                .is_some_and(|user_agent| user_agent.index.tested_ids.contains(&id))
            || self
                .entries
                .segments
                .iter()
                .any(|bound| bound.segment.tested_ids.contains(&id))
    }

    /// The buckets of the user agent's entries, which decide in every tree.
    pub(super) fn user_agent_buckets(&self) -> Option<BucketSource<'_>> {
        self.entries.user_agent.as_ref().map(|user_agent| BucketSource {
            buckets: &user_agent.index.document,
            first: 0,
            space: &user_agent.index.selectors,
            bound: None,
        })
    }

    /// The buckets of the entries of `group`: those of its scope index where it has one, and
    /// those of the segments of its sheets, or of only those of a user agent or user origin
    /// where `non_author`, in which case the scope index is the document's non-author one.
    pub(super) fn bucket_sources(
        &self,
        group: ScopeGroupID,
        non_author: bool,
    ) -> impl Iterator<Item = BucketSource<'_>> + '_ {
        let own = match (group, non_author) {
            (ScopeGroupID::DOCUMENT, false) => Some(&self.document),
            (ScopeGroupID::DOCUMENT, true) => Some(&self.document_non_author),
            (group, _) => self.groups.get(&group).map(|group| &group.index),
        };
        own.map(|buckets| BucketSource {
            buckets,
            first: 0,
            space: &self.selectors,
            bound: None,
        })
        .into_iter()
        .chain(
            self.entries
                .segments
                .iter()
                .filter(move |bound| bound.group == group && (!non_author || is_non_author(bound.origin)))
                .map(|bound| BucketSource {
                    buckets: &bound.segment.index,
                    first: bound.first,
                    space: &bound.segment.selector_space,
                    bound: Some(bound),
                }),
        )
    }

    /// The dispatch bloom bits of the keys of the buckets the index holds itself, those an element
    /// is matched by among them: its own, and those of the other trees reaching it.
    pub(super) fn bucket_key_bits(&self) -> u64 {
        let of_scope = |scope: &ScopeIndex| {
            [&scope.own, &scope.host, &scope.slotted, &scope.part]
                .into_iter()
                .flat_map(|buckets| buckets.keys())
                .fold(0, |bits, key| bits | dispatch_bloom_bit(*key))
        };
        self.groups
            .values()
            .map(|group| &group.index)
            .chain([&self.document, &self.document_non_author])
            .fold(0, |bits, scope| bits | of_scope(scope))
    }

    /// Whether any entry is of a rule inside `@scope`.
    pub(super) fn has_scoped_entries(&self) -> bool {
        !self.entry_scopes.is_empty()
    }

    /// Whether the entry `entry`, which may have died, still has its selector.
    pub(super) fn holds_selector_of(&self, entry: u32) -> bool {
        !self.entries.view(entry as usize).flags().has(EntryFlags::SELECTOR_GONE)
    }

    /// The selector entry `entry` matches by, inside the `@scope` levels of its rule.
    pub(super) fn entry_selector(&self, entry: u32) -> super::super::component_matching::EntrySelector<'_> {
        let indexed = self.entries.view(entry as usize);
        // NB: Only the entries of rules inside `@scope` have levels to look up.
        let scope = match indexed.scoped() {
            true => self.entry_scopes.get(&entry).map(|scope| &**scope),
            false => None,
        };
        super::super::component_matching::EntrySelector::new(
            self.space_of_entry(entry).resolve(indexed.selector()),
            scope,
        )
    }

    /// The space the selector of entry `entry` is in: the user agent index's for its entries, and
    /// the index's own for the rest.
    fn space_of_entry(&self, entry: u32) -> &SelectorSpace {
        if entry < self.entries.first {
            return &self
                .entries
                .user_agent
                .as_ref()
                .expect("a user agent entry has its index")
                .index
                .selectors;
        }
        match self.entries.segment_holding(entry) {
            Some(place) => &self.entries.segments[place].segment.selector_space,
            None => &self.selectors,
        }
    }

    /// The selector of entry `entry`.
    pub(super) fn selector_of_entry(&self, entry: u32) -> crate::css::selector::Selector<'_> {
        self.space_of_entry(entry)
            .resolve(self.entries.view(entry as usize).selector())
    }

    /// The arena holding the selector of entry `entry`.
    pub(super) fn arena_of_entry(&self, entry: u32) -> &crate::css::selector::SelectorListRef {
        let space = self.space_of_entry(entry);
        let (slot, _) = space.locate(self.entries.view(entry as usize).selector());
        &space.arenas[slot.0 as usize]
    }

    /// Where the selector of entry `entry` is, held by its arena.
    pub(super) fn source_of_entry(&self, entry: u32) -> EntrySource {
        self.space_of_entry(entry)
            .source(self.entries.view(entry as usize).selector())
    }

    /// The container conditions the rule of entry `entry` applies under, where it is gated.
    pub(super) fn gate_of_entry(&self, entry: u32) -> Option<ContainerGateID> {
        if !self.entries.view(entry as usize).gated() {
            return None;
        }
        let gate = self.indexed.gate_of_rule.get(&self.rule_of(entry));
        Some(*gate.expect("a gated entry's rule has its gate"))
    }

    /// Whether `entry` is bucketed in a segment the index binds.
    pub(super) fn in_segment(&self, entry: u32) -> bool {
        self.segment_holding(entry).is_some()
    }

    /// The segments the index binds, with the number of the first entry of each.
    pub(super) fn bound_segments(&self) -> impl Iterator<Item = (&Arc<SheetSegment>, u32)> {
        self.entries.segments.iter().map(|bound| (&bound.segment, bound.first))
    }

    /// The segment holding `entry`, by its place among the index's.
    fn segment_holding(&self, entry: u32) -> Option<usize> {
        self.entries.segment_holding(entry)
    }

    /// The user agent index the document shares, if its user agent sheets have rules.
    pub(super) fn user_agent_index(&self) -> Option<&Arc<UserAgentIndex>> {
        self.entries.user_agent.as_ref()
    }

    /// Whether so many entries are dead that building the index again is worth it.
    pub(super) fn wants_compaction(&self) -> bool {
        self.dead_entries > 256 && self.dead_entries * 2 > self.entries.own.len()
    }

    /// The container conditions of `gate`.
    pub(super) fn gate(&self, gate: ContainerGateID) -> &[Arc<ContainerConditionsData>] {
        &self.gates[gate.0.get() as usize - 1]
    }

    /// Whether a rule indexed applies under container conditions.
    pub(super) fn has_gates(&self) -> bool {
        !self.gates.is_empty()
    }

    /// Index every rule entry attached to any tree scope, sharing the user agent index of
    /// `process_indexes` where it has one for the document's user agent rules, or building one
    /// of the document's own where it is none.
    pub(super) fn build(program: &RuleStore, process_indexes: Option<&ProcessRuleIndexes>) -> Self {
        let mut index = Self::default();
        index.entries.first = FIRST_DOCUMENT_ENTRY;
        index.bind_user_agent(program, process_indexes);
        let mut added = Vec::new();
        for scope in program.scopes_with_sheets() {
            let group = match scope {
                TreeScopeID::DOCUMENT => ScopeGroupID::DOCUMENT,
                _ => match index.join_group(program, scope) {
                    (group, true) => group,
                    // A shadow tree alike to one before shares its group's entries.
                    (_, false) => continue,
                },
            };
            index.index_sheets_of_group(program, group, process_indexes, &mut added);
        }
        index.indexed_entries += added.len() as u64;
        index.shrink_to_fit();
        index.share_entries(process_indexes);
        index
    }

    /// Share the pages of the index's entries with the documents of the process holding equal ones.
    fn share_entries(&mut self, process_indexes: Option<&ProcessRuleIndexes>) {
        if let Some(process_indexes) = process_indexes {
            self.entries.own.share(&process_indexes.entry_pages);
            self.indexed
                .entries_of_rule
                .share(&process_indexes.entry_extents, &process_indexes.entry_lists);
        }
    }

    /// Releases what building the index reserved beyond what it holds. An update grows it again as
    /// it needs, and the next build gives the room back.
    fn shrink_to_fit(&mut self) {
        self.entries.own.shrink_to_fit();
        self.entries.placements.shrink_to_fit();
        self.entries.placements_of_sheet.shrink_to_fit();
        self.entries.segments.shrink_to_fit();
        self.document.shrink_to_fit();
        self.document_non_author.shrink_to_fit();
        for group in self.groups.values_mut() {
            group.index.shrink_to_fit();
        }
        self.groups.shrink_to_fit();
        self.group_of_scope.shrink_to_fit();
        self.groups_by_sheets.shrink_to_fit();
        self.tested_ids.shrink_to_fit();
        self.gates.shrink_to_fit();
        self.selectors.shrink_to_fit();
        self.entry_scopes.shrink_to_fit();
        let indexed = &mut self.indexed;
        indexed.entries_of_rule.shrink_to_fit();
        indexed.sheets.shrink_to_fit();
        indexed.rule_order_relabels.shrink_to_fit();
        indexed.gate_of_rule.shrink_to_fit();
        indexed.gate_of_conditions.shrink_to_fit();
    }

    /// Share the user agent index of the user agent rules deciding in the document of `program`,
    /// and answer the one shared before.
    fn bind_user_agent(
        &mut self,
        program: &RuleStore,
        process_indexes: Option<&ProcessRuleIndexes>,
    ) -> Option<Arc<UserAgentIndex>> {
        let (index, built) = match (
            program.user_agent_variant(),
            program.user_agent_rules(),
            process_indexes,
        ) {
            (Some(variant), Some(rules), Some(indexes)) => {
                let (index, built) = indexes.get_or_build(rules.store(), &variant);
                (Some(index), built)
            }
            (Some(variant), Some(rules), None) => {
                (Some(Arc::new(UserAgentIndex::build(rules.store(), &variant))), true)
            }
            _ => (None, false),
        };
        if built && let Some(index) = &index {
            self.built_user_agent_index = true;
            self.indexed_entries += index.index.entries.own.len() as u64;
            self.compiled_selectors += index.index.compiled_selectors;
        }
        std::mem::replace(&mut self.entries.user_agent, index)
    }

    /// Index the rules of the contributing sheets of `group`, which holds no entries, but for the
    /// user agent's.
    fn index_sheets_of_group(
        &mut self,
        program: &RuleStore,
        group: ScopeGroupID,
        process_indexes: Option<&ProcessRuleIndexes>,
        added: &mut Vec<u32>,
    ) {
        let sheets: SmallVec<[SheetID; 8]> = self.sheets_of_group(program, group).iter().copied().collect();
        for sheet in sheets {
            if !program.sheet_contributes(sheet) || is_user_agent_sheet(program, sheet) {
                continue;
            }
            self.index_sheet(program, sheet, group, process_indexes, added);
        }
    }

    /// Index the rules of `sheet` deciding in `group`, which the index had none of. Their entries
    /// are bucketed in the segment of the process indexing the same selectors where they are many
    /// enough, and in the index's own buckets otherwise.
    fn index_sheet(
        &mut self,
        program: &RuleStore,
        sheet: SheetID,
        group: ScopeGroupID,
        process_indexes: Option<&ProcessRuleIndexes>,
        added: &mut Vec<u32>,
    ) {
        let placement = self.placement(program, sheet, group);
        let groups = self.indexed.sheets.entry(sheet).or_default();
        debug_assert!(groups.iter().all(|&(indexed, _)| indexed != group));
        groups.push((group, placement.sheet_rank));
        self.indexed
            .rule_order_relabels
            .insert(sheet, program.rule_order_relabels(sheet));
        let rules = program.rules_in_sheet(sheet);
        // NB: Whether the sheet's entries are kept in a segment is decided before they are numbered,
        //     so that the segment's numbers start a page of the document's entries.
        let process_indexes = process_indexes.filter(|_| {
            rules
                .iter()
                .filter_map(|&rule| indexed_selectors(program, rule))
                .map(|selectors| selectors.len())
                .sum::<usize>()
                >= SEGMENT_MIN_ENTRIES
        });
        if process_indexes.is_some() {
            self.entries.pad_to_page();
        }
        let first = self.entries.next_number();
        let first_added = added.len();
        for &rule in &rules {
            self.index_rule(program, rule, group, placement, added);
        }
        let Some(process_indexes) = process_indexes else {
            for &id in &added[first_added..] {
                self.bucket(id);
            }
            return;
        };
        let end = self.entries.next_number();
        if end == first {
            return;
        }
        self.bind_segment(process_indexes, first..end, sheet, group, placement.origin);
    }

    /// Have the segment of the process holding the entries `numbers`, those of `sheet` deciding in
    /// `group` the index just numbered, hold them for the index, which keeps none of them itself.
    fn bind_segment(
        &mut self,
        process_indexes: &ProcessRuleIndexes,
        numbers: std::ops::Range<u32>,
        sheet: SheetID,
        group: ScopeGroupID,
        origin: CascadeOrigin,
    ) {
        let entries: Vec<IndexedEntry> = numbers.clone().map(|id| self.entries.entry(id as usize)).collect();
        let first_rule = entries
            .iter()
            .map(|entry| entry.rule.0)
            .min()
            .expect("a segment holds entries");
        let mut placements: SmallVec<[PlacementID; 1]> = SmallVec::new();
        let held = numbers
            .clone()
            .zip(&entries)
            .map(|(id, entry)| {
                let placement = match placements.iter().position(|&placed| placed == entry.placement) {
                    Some(slot) => slot,
                    None => {
                        placements.push(entry.placement);
                        placements.len() - 1
                    }
                };
                HeldEntry {
                    entry: SegmentEntry {
                        rule: entry.rule.0 - first_rule,
                        selector: SelectorRef(0),
                        specificity: entry.specificity,
                        placement: u16::try_from(placement).expect("entry placement space exhausted"),
                        pseudo_kind: entry.pseudo_kind,
                        flags: entry.flags,
                        rule_rank: entry.rule_rank,
                    },
                    source: self.selectors.source(entry.selector),
                    scope: self.entry_scopes.get(&id).cloned(),
                }
            })
            .collect();
        let (segment, _) = process_indexes.segment_of(held);
        // NB: The segment tells the entries of its rules, which the document need not list.
        for entry in &entries {
            let rule = entry.rule.0 as usize;
            let left = self.indexed.entries_of_rule.retain(rule, |id| !numbers.contains(id));
            if left == 0 {
                self.indexed.entries_of_rule.remove(rule);
            }
        }
        self.entries.own.truncate((numbers.start - self.entries.first) as usize);
        for _ in numbers.clone() {
            self.entries.own.push(IndexedEntry::BOUND_NUMBER);
        }
        self.entries.pad_to_page();
        self.entries.segments.push(BoundSegment {
            segment,
            first: numbers.start,
            sheet,
            group,
            origin,
            first_rule: RuleID(first_rule),
            placements,
            dead: Vec::new(),
        });
        self.entries.note_segment_pages();
    }

    /// Put the entry `id` in the index's own buckets.
    fn bucket(&mut self, id: u32) {
        let placement = self.entries.placement(id as usize);
        let (group, origin) = (placement.group, placement.origin);
        let source = self.source_of_entry(id);
        let scope = self.entry_scopes.get(&id).cloned();
        self.bucket_as(id, &source, scope.as_deref(), group, origin);
    }

    /// Put the entry `id`, of the selector `entry_index` of `program`, deciding in `group` from a
    /// sheet of `origin`, in the index's own buckets.
    fn bucket_as(
        &mut self,
        id: u32,
        source: &EntrySource,
        scope: Option<&super::super::bridge::BoundScopeChain>,
        group: ScopeGroupID,
        origin: CascadeOrigin,
    ) {
        let entry_selector = super::super::component_matching::EntrySelector::new(source.selector(), scope);
        let facts = EntryFacts::of(entry_selector);
        let selector = self.selectors.add_entry(source);
        let scoped = scope.is_some();
        let slot = BucketEntry::new(entry_selector.selector, id, selector, scoped);
        let (keys, families) = placement_in_buckets(entry_selector.selector, facts);
        insert_entry(self, group, origin, slot, &keys, families);
        if !facts.never_matches && !facts.revalidates {
            let mut tested_ids = Vec::new();
            collect_tested_ids(entry_selector.selector, &mut tested_ids);
            self.tested_ids.extend(tested_ids);
        }
    }

    /// Have the entries the segment at `place` buckets kept and bucketed in the index's own entries
    /// and buckets instead, before one of them dies: a sheet whose rules change has buckets of its
    /// own from then on.
    fn copy_segment_out(&mut self, place: usize) {
        let bound = self.entries.segments.remove(place);
        self.entries.note_segment_pages();
        for id in bound.first..bound.first + bound.segment.len() {
            if bound.is_dead(id) {
                *self.entries.own_mut(id) = IndexedEntry {
                    placement: bound.entry(id).placement,
                    flags: EntryFlags(EntryFlags::SELECTOR_GONE),
                    ..IndexedEntry::BOUND_NUMBER
                };
                continue;
            }
            let mut entry = bound.entry(id);
            entry.selector = self
                .selectors
                .add_entry(&bound.segment.selector_space.source(entry.selector));
            self.indexed.entries_of_rule.push(entry.rule.0 as usize, id);
            *self.entries.own_mut(id) = entry;
            self.bucket(id);
        }
    }

    /// Let go of the segment of the entries of `sheet` deciding in `group`, or of every sheet
    /// deciding in it where `sheet` is none, whose entries die, noting each in `died`. Their numbers
    /// keep only where they decided, which what changed tells the trees of. Answers the segments,
    /// which alone still tell the rest of what their entries were.
    fn drop_segments(&mut self, sheet: Option<SheetID>, group: ScopeGroupID, died: &mut Vec<u32>) -> Vec<BoundSegment> {
        let mut dropped = Vec::new();
        for place in (0..self.entries.segments.len()).rev() {
            let bound = &self.entries.segments[place];
            if bound.group != group || sheet.is_some_and(|sheet| bound.sheet != sheet) {
                continue;
            }
            let bound = self.entries.segments.remove(place);
            self.entries.note_segment_pages();
            for id in bound.first..bound.first + bound.segment.len() {
                *self.entries.own_mut(id) = IndexedEntry {
                    placement: bound.entry(id).placement,
                    flags: EntryFlags(EntryFlags::SELECTOR_GONE),
                    ..IndexedEntry::BOUND_NUMBER
                };
                if !bound.is_dead(id) {
                    self.dead_entries += 1;
                    died.push(id);
                }
            }
            dropped.push(bound);
        }
        dropped
    }

    /// Index the entries of `rule` deciding in `group`, appending their numbers to `added`, which
    /// the caller buckets.
    fn index_rule(
        &mut self,
        program: &RuleStore,
        rule: RuleID,
        group: ScopeGroupID,
        placement: SheetPlacement,
        added: &mut Vec<u32>,
    ) {
        let Some(selectors) = indexed_selectors(program, rule) else {
            return;
        };
        let version = program.rule_version(rule);
        // The conditions of the container rules around the rule are what it applies under,
        // whatever else says it is gated. A rule attached to several scopes, or with several
        // entries, has its gate once.
        let gate = match self.indexed.gate_of_rule.get(&rule) {
            Some(&gate) => Some(gate),
            None => program
                .native
                .targets
                .get(&rule)
                .map(|target| target.containers())
                .filter(|containers| !containers.is_empty())
                .map(|containers| {
                    let key: Box<[usize]> = containers
                        .iter()
                        .map(|container| Arc::as_ptr(container).addr())
                        .collect();
                    let id = match self.indexed.gate_of_conditions.get(&key) {
                        Some(&id) => id,
                        None => {
                            self.gates_ask_container_style |=
                                super::super::container_queries::conditions_ask_container_style(containers);
                            let id = ContainerGateID(
                                u32::try_from(self.gates.len() + 1)
                                    .ok()
                                    .and_then(std::num::NonZeroU32::new)
                                    .expect("container gate space exhausted"),
                            );
                            self.gates.push(containers.into());
                            self.indexed.gate_of_conditions.insert(key, id);
                            id
                        }
                    };
                    // NB: Only the rules under container conditions are noted, which most rules are
                    //     not: one with none finds that again as cheaply as it would be looked up.
                    self.indexed.gate_of_rule.insert(rule, id);
                    id
                }),
        };
        let layer_scope = self.layer_scope_of(group);
        let sheet = program.rule_sheet(rule);
        let placement = self.entries.place(sheet, group, version.layer, || EntryPlacement {
            origin: placement.origin,
            group,
            layer_rank: program.layer_rank(layer_scope, version.layer),
            sheet_rank: placement.sheet_rank,
            layer: version.layer,
        });
        let declared = program.declared_properties_of(rule);
        let declares_custom_properties = !program.custom_declarations_of(rule).is_empty();
        let declares_important = declared.iter().any(|declaration| declaration.important())
            || program
                .custom_declarations_of(rule)
                .iter()
                .any(|declaration| declaration.important());
        let rule_rank = program.rule_rank(rule);
        let scope = program.scope_chain(version.scope);
        for (of_rule, specificity, pseudo_kind) in entries_of(&selectors) {
            self.compiled_selectors += 1;
            let compiled = EntryFacts::of(super::super::component_matching::EntrySelector::new(
                of_rule,
                scope.map(|scope| &**scope),
            ));
            if compiled.never_matches {
                continue;
            }
            let id = self.entries.next_number();
            let selector = self.selectors.add(&selectors, of_rule.offset());
            self.entries.own.push(IndexedEntry {
                rule: EntryRule::document(rule),
                placement,
                selector,
                specificity: PackedSpecificity::pack(specificity),
                rule_rank,
                pseudo_kind,
                flags: EntryFlags(
                    [
                        (compiled.scoped, EntryFlags::SCOPED),
                        (compiled.revalidates, EntryFlags::REVALIDATES),
                        (declares_custom_properties, EntryFlags::DECLARES_CUSTOM_PROPERTIES),
                        (declares_important, EntryFlags::DECLARES_IMPORTANT),
                        (true, EntryFlags::LIVE),
                        (gate.is_some(), EntryFlags::GATED),
                    ]
                    .into_iter()
                    .filter(|&(holds, _)| holds)
                    .fold(0, |flags, (_, flag)| flags | flag),
                ),
            });
            self.indexed.entries_of_rule.push(rule.0 as usize, id);
            if let Some(scope) = scope {
                self.entry_scopes.insert(id, Arc::clone(scope));
            }
            added.push(id);
        }
    }

    /// Kill the live entries of `rule`, or those deciding in `group` where it is some, noting the
    /// number of each in `died`.
    fn kill_rule(&mut self, rule: RuleID, group: Option<ScopeGroupID>, died: &mut Vec<u32>) {
        // https://drafts.csswg.org/css-conditional-3/#conditional-group-rules
        // NB: A rule of a sheet the process keeps in a segment leaves this document alone, so its
        //     entries stay in the segment's shared buckets and invalidation map, marked dead here.
        for bound in &mut self.entries.segments {
            if !group.is_none_or(|group| bound.group == group) {
                continue;
            }
            let Some(offset) = rule.0.checked_sub(bound.first_rule.0) else {
                continue;
            };
            let dying: SmallVec<[u32; 4]> = (bound.first..bound.first + bound.segment.len())
                .filter(|&id| bound.segment.entries[(id - bound.first) as usize].rule == offset && !bound.is_dead(id))
                .collect();
            for id in dying {
                bound.note_dead(id);
                self.dead_entries += 1;
                died.push(id);
            }
        }
        let mut dying: SmallVec<[u32; 4]> = SmallVec::new();
        let entries = &self.entries;
        let left = self.indexed.entries_of_rule.retain(rule.0 as usize, |&id| {
            let dies = group.is_none_or(|group| entries.placement(id as usize).group == group);
            if dies {
                dying.push(id);
            }
            !dies
        });
        if left == 0 {
            self.indexed.entries_of_rule.remove(rule.0 as usize);
        }
        for id in dying {
            self.kill(id);
            died.push(id);
        }
    }

    /// Take the entry `id` out of every bucket it is in.
    fn kill(&mut self, id: u32) {
        let placement = *self.entries.placement(id as usize);
        let entry = self.entries.own_mut(id);
        debug_assert!(entry.live());
        entry.kill();
        self.dead_entries += 1;
        let (group, origin) = (placement.group, placement.origin);
        let source = self.source_of_entry(id);
        let scope = self.entry_scopes.get(&id).cloned();
        let entry_selector = super::super::component_matching::EntrySelector::new(source.selector(), scope.as_deref());
        let (keys, families) = placement_in_buckets(entry_selector.selector, EntryFacts::of(entry_selector));
        let remove = |scope_index: &mut ScopeIndex| scope_index.remove(id, &keys, families);
        match group {
            ScopeGroupID::DOCUMENT => {
                remove(&mut self.document);
                if is_non_author(origin) {
                    remove(&mut self.document_non_author);
                }
            }
            _ => {
                if let Some(held) = self.groups.get_mut(&group) {
                    remove(&mut held.index);
                }
            }
        }
    }

    /// Index again what `changes` moved alone: the entries of the rules and sheets it names that
    /// left die, and those that arrived are appended. The entries that stayed take the ranks of
    /// their sheets and layers where those moved. Answers what changed, for the elements it can
    /// restyle.
    pub(super) fn update(
        &mut self,
        program: &RuleStore,
        process_indexes: Option<&ProcessRuleIndexes>,
        changes: &IndexChanges<'_>,
    ) -> RuleIndexChanges {
        let first_added = self.entries.len();
        self.compiled_selectors = 0;
        self.built_user_agent_index = false;
        let mut died: Vec<u32> = Vec::new();
        let mut added: Vec<u32> = Vec::new();

        // The rules that moved, and those inside them, lose their entries, and their
        // declarations and gates are read again. Those of the user agent decide through the user
        // agent index, which the document shares again, and is bound to again.
        let mut rules: Vec<RuleID> = changes
            .rules
            .iter()
            .flat_map(|&rule| program.rule_subtree(rule))
            .collect();
        rules.sort_unstable();
        rules.dedup();
        let user_agent_moved = rules
            .iter()
            .any(|&rule| is_user_agent_sheet(program, program.rule_sheet(rule)))
            || changes.sheets.iter().any(|&sheet| is_user_agent_sheet(program, sheet));
        rules.retain(|&rule| !is_user_agent_sheet(program, program.rule_sheet(rule)));
        if user_agent_moved {
            let previous = self.bind_user_agent(program, process_indexes);
            let current = self.entries.user_agent.clone();
            let entries =
                |index: &Option<Arc<UserAgentIndex>>| index.as_ref().map_or(0, |index| index.index.entries.own.len());
            let same = |number: usize| match (&previous, &current) {
                (Some(previous), Some(current)) => same_user_agent_selector(previous, current, number),
                _ => false,
            };
            for number in 0..entries(&previous).max(entries(&current)) {
                if same(number) {
                    continue;
                }
                let number_u32 = u32::try_from(number).expect("rule entry space exhausted");
                if number < entries(&previous) {
                    died.push(number_u32);
                }
                if number < entries(&current) {
                    added.push(number_u32);
                }
            }
        }
        // A sheet every rule of which moved, as one whose text was replaced, is indexed whole
        // again where it decides, as a sheet arriving is: in the segment of the process indexing
        // the same selectors where it has enough of them, rather than in the index's own buckets.
        let mut replaced: Vec<SheetID> = rules.iter().map(|&rule| program.rule_sheet(rule)).collect();
        replaced.sort_unstable();
        replaced.dedup();
        replaced.retain(|sheet| {
            if changes.sheets.contains(sheet) || !self.indexed.sheets.contains_key(sheet) {
                return false;
            }
            let live = program.rules_in_sheet(*sheet);
            !live.is_empty() && live.iter().all(|rule| rules.binary_search(rule).is_ok())
        });
        let mut let_go: Vec<BoundSegment> = Vec::new();
        for &sheet in &replaced {
            let groups: SmallVec<[ScopeGroupID; 4]> =
                self.indexed.sheets[&sheet].iter().map(|&(group, _)| group).collect();
            for group in groups {
                let_go.extend(self.drop_segments(Some(sheet), group, &mut died));
            }
        }
        for &rule in &rules {
            self.kill_rule(rule, None, &mut died);
            self.indexed.gate_of_rule.remove(&rule);
        }

        // A shadow tree whose sheets or layers moved stops deciding with the entries of the trees
        // it was alike to, and decides with those of the trees it is alike to now, which are
        // indexed where no tree had them. A group whose only tree moved takes what the tree has
        // instead, where no other group has it, and its entries change as the tree's sheets did.
        let mut sheets: Vec<SheetID> = changes
            .sheets
            .iter()
            .copied()
            .filter(|&sheet| !is_user_agent_sheet(program, sheet))
            .collect();
        sheets.sort_unstable();
        let mut moved_scopes: Vec<TreeScopeID> = changes
            .reordered_scopes
            .iter()
            .chain(changes.layered_scopes.iter())
            .copied()
            .chain(sheets.iter().flat_map(|&sheet| program.sheet_scopes(sheet)))
            .chain(sheets.iter().flat_map(|sheet| {
                self.indexed
                    .sheets
                    .get(sheet)
                    .into_iter()
                    .flatten()
                    .flat_map(|&(group, _)| self.scopes_of(group).iter().copied())
            }))
            .filter(|&scope| scope != TreeScopeID::DOCUMENT)
            .collect();
        moved_scopes.sort_unstable();
        moved_scopes.dedup();
        let mut regrouped: Vec<Regrouped> = Vec::new();
        let mut reordered_groups: Vec<ScopeGroupID> = Vec::new();
        for scope in moved_scopes {
            let reordered = changes.reordered_scopes.contains(&scope) || changes.layered_scopes.contains(&scope);
            let sheets_in_scope = program.sheets_in_scope(scope);
            if let Some(group) = self.group_of_scope.get(&scope).copied() {
                let held = &self.groups[&group];
                if *held.sheets == *sheets_in_scope && program.layer_ranks_match(scope, &held.layer_ranks) {
                    if reordered {
                        reordered_groups.push(group);
                    }
                    continue;
                }
                if !sheets_in_scope.is_empty() && self.regroup_alone(program, scope) {
                    reordered_groups.push(group);
                    continue;
                }
            }
            let left = self.leave_group(program, scope, &mut died);
            let joined = match sheets_in_scope.is_empty() {
                true => None,
                false => match self.join_group(program, scope) {
                    (group, true) => {
                        self.index_sheets_of_group(program, group, process_indexes, &mut added);
                        // NB: The entries of a new group arrived, and restyle what they match.
                        None
                    }
                    (group, false) => Some(group),
                },
            };
            regrouped.push(Regrouped { scope, left, joined });
        }

        // A sheet that stopped deciding in a group loses its entries there; one that started is
        // indexed there.
        let mut ranked_groups: Vec<ScopeGroupID> = changes
            .reordered_scopes
            .iter()
            .chain(changes.layered_scopes.iter())
            .filter_map(|&scope| self.group_of(scope))
            .chain(reordered_groups.iter().copied())
            .collect();
        if changes.reordered_scopes.contains(&TreeScopeID::DOCUMENT)
            || changes.layered_scopes.contains(&TreeScopeID::DOCUMENT)
        {
            reordered_groups.push(ScopeGroupID::DOCUMENT);
        }
        let mut arriving: Vec<(SheetID, ScopeGroupID)> = Vec::new();
        for sheet in sheets {
            let mut attached: Vec<ScopeGroupID> = program
                .sheet_scopes(sheet)
                .into_iter()
                .filter_map(|scope| self.group_of(scope))
                .collect();
            attached.sort_unstable();
            attached.dedup();
            ranked_groups.extend(attached.iter().copied());
            let deciding = match program.sheet_contributes(sheet) {
                true => attached,
                false => Vec::new(),
            };
            let indexed = self.indexed.sheets.remove(&sheet).unwrap_or_default();
            let leaving: Vec<ScopeGroupID> = indexed
                .iter()
                .map(|&(group, _)| group)
                .filter(|group| !deciding.contains(group))
                .collect();
            ranked_groups.extend(indexed.iter().map(|&(group, _)| group));
            if !leaving.is_empty() {
                for &group in &leaving {
                    self.drop_segments(Some(sheet), group, &mut died);
                    // NB: Where it decides next it stands where it stands then.
                    self.entries.placements_of_sheet.remove(&(sheet, group));
                }
                for rule in program.all_rules_in_sheet(sheet) {
                    for &group in &leaving {
                        self.kill_rule(rule, Some(group), &mut died);
                    }
                }
            }
            let kept: SmallVec<[(ScopeGroupID, u64); 1]> = indexed
                .into_iter()
                .filter(|(group, _)| deciding.contains(group))
                .collect();
            arriving.extend(
                deciding
                    .iter()
                    .filter(|&&group| kept.iter().all(|&(indexed, _)| indexed != group))
                    .map(|&group| (sheet, group)),
            );
            if !kept.is_empty() {
                self.indexed.sheets.insert(sheet, kept);
            }
        }

        // The rules that moved are indexed again where their sheets stayed, and the sheets that
        // arrived are indexed whole.
        for &rule in &rules {
            let sheet = program.rule_sheet(rule);
            if replaced.binary_search(&sheet).is_ok() {
                continue;
            }
            let Some(groups) = self.indexed.sheets.get(&sheet) else {
                continue;
            };
            let groups: SmallVec<[ScopeGroupID; 4]> = groups.iter().map(|&(group, _)| group).collect();
            for group in groups {
                let placement = self.placement(program, sheet, group);
                let first_added = added.len();
                self.index_rule(program, rule, group, placement, &mut added);
                for &id in &added[first_added..] {
                    self.bucket(id);
                }
            }
        }
        for sheet in replaced {
            for (group, _) in self.indexed.sheets.remove(&sheet).unwrap_or_default() {
                self.entries.placements_of_sheet.remove(&(sheet, group));
                self.index_sheet(program, sheet, group, process_indexes, &mut added);
            }
        }
        for (sheet, group) in arriving {
            self.index_sheet(program, sheet, group, process_indexes, &mut added);
        }

        // The entries that stayed take the ranks their rules moved to, which keeps their order
        // where the rule order of their sheet was only relabeled.
        let mut relabeled: Vec<SheetID> = rules.iter().map(|&rule| program.rule_sheet(rule)).collect();
        relabeled.sort_unstable();
        relabeled.dedup();
        let mut any_relabeled = false;
        for sheet in relabeled {
            let relabels = program.rule_order_relabels(sheet);
            if self.indexed.rule_order_relabels.insert(sheet, relabels) == Some(relabels) {
                continue;
            }
            any_relabeled = true;
            // NB: The documents sharing a segment share the rule order its entries hold.
            for place in (0..self.entries.segments.len()).rev() {
                let bound = &self.entries.segments[place];
                if bound.sheet == sheet
                    && (bound.first..bound.first + bound.segment.len()).any(|id| {
                        let entry = bound.entry(id);
                        entry.rule_rank != program.rule_rank(RuleID(entry.rule.0))
                    })
                {
                    self.copy_segment_out(place);
                }
            }
            for rule in program.all_rules_in_sheet(sheet) {
                let rule_rank = program.rule_rank(rule);
                let ids: SmallVec<[u32; 4]> = self.indexed.entries_of_rule.get(rule.0 as usize).collect();
                for id in ids {
                    if self.entries.view(id as usize).rule_rank() != rule_rank {
                        self.entries.own_mut(id).rule_rank = rule_rank;
                    }
                }
            }
        }

        // And the ranks their sheets and layers moved to, where an entry that moved against
        // another in the cascade order restyles the elements it matched.
        ranked_groups.sort_unstable();
        ranked_groups.dedup();
        let mut moved: Vec<u32> = Vec::new();
        for group in ranked_groups {
            // NB: A group that died has no sheets to rank.
            if group != ScopeGroupID::DOCUMENT && !self.groups.contains_key(&group) {
                continue;
            }
            let reordered = reordered_groups.contains(&group);
            let layer_scope = self.layer_scope_of(group);
            let mut stayed: Vec<StayedEntry> = Vec::new();
            let group_sheets: SmallVec<[SheetID; 8]> = self.sheets_of_group(program, group).iter().copied().collect();
            for sheet in group_sheets {
                let sheet_rank = self.placement(program, sheet, group).sheet_rank;
                let Some(slot) = self
                    .indexed
                    .sheets
                    .get_mut(&sheet)
                    .and_then(|groups| groups.iter_mut().find(|(indexed, _)| *indexed == group))
                else {
                    continue;
                };
                let sheet_moved = std::mem::replace(&mut slot.1, sheet_rank) != sheet_rank;
                if !sheet_moved && !reordered {
                    continue;
                }
                for bound in &self.entries.segments {
                    if bound.sheet == sheet && bound.group == group {
                        stayed.extend(
                            (bound.first..bound.first + bound.segment.len())
                                .filter(|&id| (id as usize) < first_added)
                                .map(|id| StayedEntry::of(id, &self.entries)),
                        );
                    }
                }
                for rule in program.rules_in_sheet(sheet) {
                    for id in self.indexed.entries_of_rule.get(rule.0 as usize) {
                        if self.entries.placement(id as usize).group != group || id as usize >= first_added {
                            continue;
                        }
                        stayed.push(StayedEntry::of(id, &self.entries));
                    }
                }
                // The entries take the ranks of their sheet and layers where they are placed.
                self.entries.rank_placements_of(sheet, group, |placement| {
                    placement.sheet_rank = sheet_rank;
                    placement.layer_rank = program.layer_rank(layer_scope, placement.layer);
                });
            }
            // NB: A sheet whose rank was only relabeled keeps its order against the others.
            if reordered {
                super::rule_changes::note_moved(&self.entries, &stayed, &mut moved);
            }
        }
        // An update that added at least as many entries as the index had gives back the room its
        // growth reserved, as a build does: each entry pays for that once, however the rules
        // arrive. One that moved many rules' entry lists gives back the room they left.
        if self.entries.len() - first_added >= first_added.saturating_sub(self.entries.own_numbers().start) {
            self.shrink_to_fit();
        } else if self.indexed.entries_of_rule.has_slack() {
            self.indexed.entries_of_rule.shrink_to_fit();
        }
        self.share_entries(process_indexes);

        // NB: An entry that died where only the conditions of its rule moved declared what the rule
        //     declares now. So nothing but those conditions moved the entries of an update where the
        //     rule of each is one of them, and no sheet or layer moved. A rule order relabeled gives
        //     new ranks to the entries that stayed alone, which those that died no longer compare to.
        let contents_moved: HashSet<RuleID> = changes
            .rule_contents
            .iter()
            .flat_map(|&rule| program.rule_subtree(rule))
            .collect();
        let conditions_moved_alone = changes.sheets.is_empty()
            && changes.reordered_scopes.is_empty()
            && changes.layered_scopes.is_empty()
            && !user_agent_moved
            && !any_relabeled
            && moved.is_empty()
            && regrouped.is_empty()
            && died.iter().all(|&id| {
                self.entries
                    .try_view_before(&let_go, id as usize)
                    .is_some_and(|view| !contents_moved.contains(&self.rule_of_view(view)))
            });
        let mut rule_index_changes = RuleIndexChanges::new(
            self,
            died,
            &moved,
            &added,
            user_agent_moved,
            regrouped,
            conditions_moved_alone,
        );
        rule_index_changes.let_go = let_go;
        rule_index_changes
    }

    /// The buckets holding the entries `entries` alone, where they are live.
    pub(super) fn buckets_of(&self, entries: &[u32]) -> RuleIndex {
        let mut buckets = RuleIndex::default();
        for &id in entries {
            let entry = self.entries.entry(id as usize);
            if !entry.live() {
                continue;
            }
            let placement = self.entries.placement(id as usize);
            buckets.bucket_as(
                id,
                &self.source_of_entry(id),
                self.entry_scopes.get(&id).map(|scope| &**scope),
                placement.group,
                placement.origin,
            );
        }
        // The shadow trees reach the entries of their groups.
        buckets.group_of_scope = self
            .group_of_scope
            .iter()
            .filter(|(_, group)| buckets.groups.contains_key(group))
            .map(|(&scope, &group)| (scope, group))
            .collect();
        buckets
    }

    /// The live entries of `group`.
    pub(super) fn live_entries_of(&self, group: ScopeGroupID) -> Vec<u32> {
        self.entries
            .own_numbers()
            .filter(|&id| {
                let view = self.entries.view(id);
                view.live() && self.entries.placement_of(view).group == group
            })
            .map(|id| u32::try_from(id).expect("rule entry space exhausted"))
            .collect()
    }
}

/// Whether the entry `number` of the user agent indexes `before` and `after` is of the same selector.
fn same_user_agent_selector(before: &UserAgentIndex, after: &UserAgentIndex, number: usize) -> bool {
    let (before, after) = (&before.index, &after.index);
    let number_u32 = u32::try_from(number).expect("rule entry space exhausted");
    number < before.entries.own.len().min(after.entries.own.len())
        && before.selector_of_entry(number_u32).key() == after.selector_of_entry(number_u32).key()
}

/// Whether each number of the user agent's entries of `previous` stands for the same selector in
/// `current`.
fn stays_in_user_agent_index<'a>(previous: &'a RuleIndex, current: &'a RuleIndex) -> impl Fn(usize) -> bool + 'a {
    move |number| match (&previous.entries.user_agent, &current.entries.user_agent) {
        (Some(before), Some(after)) if Arc::ptr_eq(before, after) => true,
        (Some(before), Some(after)) => same_user_agent_selector(before, after, number),
        _ => false,
    }
}

/// The keys an entry's subject can be reached by, and the families of its scope's buckets it is
/// in besides its own elements'.
fn placement_in_buckets(
    selector: crate::css::selector::Selector<'_>,
    compiled: EntryFacts,
) -> (SmallVec<[DispatchKey; 4]>, Families) {
    let mut keys = subject_dispatch_keys(selector);
    if keys.is_empty()
        && let Some(key) = state_dispatch_key(selector)
    {
        keys.push(key);
    }
    let families = Families {
        revalidates: compiled.revalidates,
        host: compiled.host,
        slotted: compiled.slotted,
        part: compiled.part,
    };
    (keys, families)
}

/// Put `slot` in the buckets of `group` in `index`, and in the document's non-author buckets
/// where it is of a user agent or user sheet of the document.
fn insert_entry(
    index: &mut RuleIndex,
    group: ScopeGroupID,
    origin: CascadeOrigin,
    slot: BucketEntry,
    keys: &[DispatchKey],
    families: Families,
) {
    let scope_index = match group {
        ScopeGroupID::DOCUMENT => &mut index.document,
        _ => {
            &mut index
                .groups
                .entry(group)
                .or_insert_with(|| ScopeGroup {
                    index: ScopeIndex::default(),
                    scopes: SmallVec::new(),
                    sheets: Box::default(),
                    layer_ranks: HashMap::default(),
                })
                .index
        }
    };
    scope_index.insert(slot, keys, families);
    if group == ScopeGroupID::DOCUMENT && is_non_author(origin) {
        index.document_non_author.insert(slot, keys, families);
    }
}

/// The families of a scope's buckets an entry is in besides its own elements'.
#[derive(Clone, Copy)]
struct Families {
    revalidates: bool,
    host: bool,
    slotted: bool,
    part: bool,
}

impl ScopeIndex {
    /// Releases what the buckets reserved beyond the entries they hold.
    fn shrink_to_fit(&mut self) {
        for buckets in [
            &mut self.own,
            &mut self.own_revalidation,
            &mut self.host,
            &mut self.slotted,
            &mut self.part,
        ] {
            buckets.shrink_to_fit();
        }
    }

    fn families_mut(&mut self, families: Families) -> [(bool, &mut Buckets); 5] {
        [
            (true, &mut self.own),
            (families.revalidates, &mut self.own_revalidation),
            (families.host, &mut self.host),
            (families.slotted, &mut self.slotted),
            (families.part, &mut self.part),
        ]
    }

    fn insert(&mut self, slot: BucketEntry, keys: &[DispatchKey], families: Families) {
        for (member, buckets) in self.families_mut(families) {
            if member {
                insert(buckets, slot, keys);
            }
        }
    }

    fn remove(&mut self, entry: u32, keys: &[DispatchKey], families: Families) {
        for (member, buckets) in self.families_mut(families) {
            if member {
                remove(buckets, entry, keys);
            }
        }
    }
}

/// The keys of the buckets an entry reached by `keys` is in.
fn bucket_keys(keys: &[DispatchKey]) -> &[DispatchKey] {
    if keys.is_empty() || !keys.iter().all(is_probed) {
        return &[DispatchKey::Universal];
    }
    keys
}

fn insert(buckets: &mut Buckets, mut bucket_entry: BucketEntry, keys: &[DispatchKey]) {
    let keys = bucket_keys(keys);
    if keys.len() > 1 {
        bucket_entry.entry |= BucketEntry::IN_OTHER_BUCKETS;
    }
    for &key in keys {
        buckets.push(key, &bucket_entry);
    }
}

fn remove(buckets: &mut Buckets, entry: u32, keys: &[DispatchKey]) {
    for key in bucket_keys(keys) {
        buckets.remove(key, entry);
    }
    // NB: Packing the lists again once half the backing is room no list holds keeps the room the
    //     removals and moved lists leave bounded, at an amortized constant cost per removal.
    if buckets.has_slack() {
        buckets.shrink_to_fit();
    }
}

#[cfg(test)]
mod tests {
    use super::super::rule_store::RuleKind;
    use super::*;

    /// The selectors `text` writes.
    fn sourced(text: &str) -> crate::css::selector::SelectorListRef {
        let parsed = crate::css::selector_parser::parse_selector_list(
            &text.encode_utf16().collect::<Vec<u16>>()[..],
            &[],
            crate::css::selector_parser::SelectorType::Standalone,
            crate::css::selector_parser::SelectorParsingMode::Standard,
        )
        .expect("a test writes valid selectors");
        crate::css::selector::SelectorListRef::pack(&parsed.selectors)
    }

    /// Append a style rule with the selectors `text` writes to `sheet`.
    fn add_rule(program: &mut RuleStore, sheet: SheetID, text: &str) -> RuleID {
        let rule = program.append_rule(sheet, None, RuleKind::Style);
        let mut version = program.rule_version(rule).clone();
        version.selectors = program.start_of_selectors(sheet, &sourced(text));
        program.replace_rule_version(rule, version);
        rule
    }

    fn count(buckets: &Buckets) -> usize {
        buckets.count()
    }

    /// Have a sheet of `program` stand for the user agent sheet of `process` whose rules select
    /// `selectors`, which the process publishes from the native sheet `source`.
    fn attach_user_agent_sheet(
        program: &mut RuleStore,
        process: &Arc<crate::css::style::ProcessStyle>,
        source: u64,
        selectors: &[&crate::css::selector::SelectorListRef],
    ) -> SheetID {
        process.publish_user_agent_sheet(source, |engine, sheet| {
            for &selectors in selectors {
                let rule = engine.rules.append_rule(sheet, None, RuleKind::Style);
                let mut version = engine.rules.rule_version(rule).clone();
                version.selectors = engine.rules.start_of_selectors(sheet, selectors);
                engine.rules.replace_rule_version(rule, version);
            }
        });
        let rules = process.user_agent_rules();
        let published = rules.sheet_of_source(source).expect("the sheet was published");
        let sheet = program.add_sheet(CascadeOrigin::UserAgent);
        program.bind_user_agent_sheet(sheet, rules, published);
        program.attach_sheet(sheet, TreeScopeID::DOCUMENT);
        sheet
    }

    #[test]
    fn rules_are_indexed_per_tree_scope_and_attachment() {
        let mut program = RuleStore::new();
        let first_scope = TreeScopeID(1);
        let second_scope = TreeScopeID(2);

        // The document: a user agent rule, an author rule and an author `::part()` rule.
        let process = Arc::<crate::css::style::ProcessStyle>::default();
        attach_user_agent_sheet(&mut program, &process, 1, &[&sourced(&format!(".c{}", 100))]);
        let document = program.add_sheet(CascadeOrigin::Author);
        program.attach_sheet(document, TreeScopeID::DOCUMENT);
        add_rule(&mut program, document, &format!(".c{}", 101));
        add_rule(&mut program, document, "::part(p102)");

        // The first shadow tree's own sheet: `:host` and `::slotted(.x)`.
        let own = program.add_sheet(CascadeOrigin::Author);
        program.attach_sheet(own, first_scope);
        add_rule(&mut program, own, ":host");
        add_rule(&mut program, own, "::slotted(.c103)");

        // A constructed sheet both shadow trees adopt.
        let shared = program.add_sheet(CascadeOrigin::Author);
        program.attach_sheet(shared, first_scope);
        program.attach_sheet(shared, second_scope);
        let shared_rule = add_rule(&mut program, shared, &format!(".c{}", 104));

        let index = RuleIndex::build(&program, None);
        let entries = || {
            index
                .entries
                .numbers()
                .map(|number| (number as u32, index.entries.entry(number)))
        };

        // One entry per rule and attachment.
        assert_eq!(index.entries.count(), 7);
        for (scope, expected) in [(TreeScopeID::DOCUMENT, 3), (first_scope, 3), (second_scope, 1)] {
            let group = index.group_of(scope).expect("the scope has sheets");
            assert_eq!(
                entries()
                    .filter(|&(number, _)| index.entries.placement(number as usize).group == group)
                    .count(),
                expected
            );
        }
        // The adopted sheet's rule is indexed for each scope, sharing its compiled selector and
        // declarations.
        let shared_entries: Vec<_> = entries()
            .filter(|&(number, _)| index.rule_of(number) == shared_rule)
            .collect();
        assert_eq!(shared_entries.len(), 2);
        assert!(
            index.selector_of_entry(shared_entries[0].0).key() == index.selector_of_entry(shared_entries[1].0).key()
        );
        assert_ne!(
            index.entries.placement(shared_entries[0].0 as usize).group,
            index.entries.placement(shared_entries[1].0 as usize).group
        );

        // The user agent rule is in the user agent index alone.
        assert_eq!(count(&index.document.own), 2);
        assert_eq!(count(&index.document.part), 1);
        assert_eq!(count(&index.document.host), 0);
        assert_eq!(count(&index.document.slotted), 0);
        assert_eq!(count(&index.document_non_author.own), 0);
        let user_agent = index.user_agent_buckets().expect("the user agent sheet has a rule");
        assert_eq!(count(&user_agent.buckets.own), 1);
        assert_eq!(count(&user_agent.buckets.part), 0);

        let first = index.scope_index(first_scope).expect("the first scope has sheets");
        assert_eq!(count(&first.own), 3);
        assert_eq!(count(&first.host), 1);
        assert_eq!(count(&first.slotted), 1);
        assert_eq!(count(&first.part), 0);
        let second = index.scope_index(second_scope).expect("the second scope has sheets");
        assert_eq!(count(&second.own), 1);
        assert_eq!(count(&second.host) + count(&second.slotted) + count(&second.part), 0);
        assert!(index.scope_index(TreeScopeID(3)).is_none());
    }

    #[test]
    fn shadow_trees_with_the_same_sheets_share_their_entries() {
        let mut program = RuleStore::new();
        let scopes: Vec<TreeScopeID> = (1..=3).map(TreeScopeID).collect();
        let shared = program.add_sheet(CascadeOrigin::Author);
        for &scope in &scopes {
            program.attach_sheet(shared, scope);
        }
        add_rule(&mut program, shared, &format!(".c{}", 100));
        add_rule(&mut program, shared, &format!(".c{}", 101));
        let mut index = RuleIndex::build(&program, None);
        let update = |index: &mut RuleIndex, program: &RuleStore, sheet: SheetID| {
            index.update(
                program,
                None,
                &IndexChanges {
                    rules: &HashSet::default(),
                    rule_contents: &HashSet::default(),
                    sheets: &[sheet].into_iter().collect(),
                    reordered_scopes: &HashSet::default(),
                    layered_scopes: &HashSet::default(),
                },
            )
        };

        // The three trees are one group, whose entries are indexed once.
        assert_eq!(index.entries.count(), 2);
        assert_eq!(index.indexed_entries, 2);
        let group = index.group_of(scopes[0]).expect("the scope has sheets");
        assert_eq!(index.scopes_of(group), &scopes[..]);

        // A tree taking another sheet leaves the group for one of its own, and the elements of
        // that tree alone are restyled.
        let own = program.add_sheet(CascadeOrigin::Author);
        program.attach_sheet(own, scopes[1]);
        add_rule(&mut program, own, &format!(".c{}", 102));
        let changes = update(&mut index, &program, own);
        let diverged = index.group_of(scopes[1]).expect("the scope has sheets");
        assert_ne!(diverged, group);
        assert_eq!(index.scopes_of(group), [scopes[0], scopes[2]]);
        assert_eq!(index.scopes_of(diverged), [scopes[1]]);
        assert_eq!(changes.arrived_entries, 3);
        assert!(changes.died().is_empty());
        assert_eq!(changes.scopes, [scopes[1]]);

        // Giving the sheet up has it join the group again, and the group of its own dies.
        program.detach_sheet(own, scopes[1]);
        let changes = update(&mut index, &program, own);
        assert_eq!(index.group_of(scopes[1]), Some(group));
        assert!(index.scopes_of(diverged).is_empty());
        assert_eq!(changes.arrived_entries, 0);
        assert_eq!(changes.died().len(), 3);
        assert_eq!(changes.scopes, [scopes[1]]);
        assert_eq!(index.groups.len(), 1);
    }

    #[test]
    fn documents_with_the_same_user_agent_rules_share_one_user_agent_index() {
        // The process compiles one selector program for each user agent rule, which every
        // document's rule attaches.
        let programs: Vec<crate::css::selector::SelectorListRef> =
            (0..3).map(|atom| sourced(&format!(".c{}", 100 + atom))).collect();
        let process = Arc::<crate::css::style::ProcessStyle>::default();
        let document = |user_agent_sheets: &[&[usize]]| {
            let mut program = RuleStore::new();
            for rules in user_agent_sheets {
                let source = rules.iter().fold(1, |source, &rule| source * 10 + rule as u64);
                let selectors: Vec<_> = rules.iter().map(|&rule| &programs[rule]).collect();
                attach_user_agent_sheet(&mut program, &process, source, &selectors);
            }
            let author = program.add_sheet(CascadeOrigin::Author);
            program.attach_sheet(author, TreeScopeID::DOCUMENT);
            add_rule(&mut program, author, &format!(".c{}", 200));
            program
        };
        let indexes = ProcessRuleIndexes::default();

        // The first document builds the user agent index, and the second shares it.
        let first = RuleIndex::build(&document(&[&[0, 1]]), Some(&indexes));
        assert!(first.built_user_agent_index);
        let second = RuleIndex::build(&document(&[&[0, 1]]), Some(&indexes));
        assert!(!second.built_user_agent_index);
        assert!(Arc::ptr_eq(
            first.user_agent_index().unwrap(),
            second.user_agent_index().unwrap()
        ));
        // Their invalidation maps start from the user agent index's, sharing what it holds rather
        // than copying it, and hold their own selectors' alone.
        use super::super::invalidation::InvalidationMap;
        let (first_map, second_map) = (InvalidationMap::build(&first), InvalidationMap::build(&second));
        let (first_map, second_map) = (first_map.base(), second_map.base());
        assert!(first_map.starts_from(&first.user_agent_index().unwrap().invalidation));
        assert!(second_map.starts_from(&first.user_agent_index().unwrap().invalidation));
        assert_eq!(
            second_map.by_trigger.own_keys().count() + second_map.lone_by_trigger.own_keys().count(),
            1
        );
        // Its entries are numbered below the document's own, which count only those.
        assert_eq!(second.indexed_entries, 1);
        assert_eq!(
            second.entries.numbers().collect::<Vec<_>>(),
            [0, 1, FIRST_DOCUMENT_ENTRY as usize]
        );

        // A document with another user agent sheet decides with another index, in which the
        // rules both have keep their numbers.
        let third = RuleIndex::build(&document(&[&[0, 1], &[2]]), Some(&indexes));
        assert!(third.built_user_agent_index);
        let stays = stays_in_user_agent_index(&first, &third);
        assert!(stays(0) && stays(1) && !stays(2));
        assert_eq!(first.renumbering_from(&first)[..2], [Some(0), Some(1)]);

        // Each document's entries name its own rules and declarations.
        let fourth_program = document(&[&[0, 1]]);
        let fourth = RuleIndex::build(&fourth_program, Some(&indexes));
        assert!(!fourth.built_user_agent_index);
        let user_agent_rules: Vec<RuleID> = fourth_program
            .rules_in_sheet(fourth_program.sheets_in_scope(TreeScopeID::DOCUMENT)[0])
            .into_iter()
            .collect();
        assert_eq!([fourth.rule_of(0), fourth.rule_of(1)], user_agent_rules[..]);

        // A document attaching the process's sheets in another order than the process published
        // them in has them decide in its order.
        let reversed = RuleIndex::build(&document(&[&[2], &[0, 1]]), Some(&indexes));
        assert!(reversed.built_user_agent_index);
        let rank = |entry: usize| reversed.entries.placement(entry).sheet_rank;
        assert!(rank(0) < rank(1) && rank(1) == rank(2));
        assert_eq!(reversed.selector_of_entry(0).key(), third.selector_of_entry(2).key());
    }

    #[test]
    fn documents_indexing_the_same_sheet_share_its_segment() {
        // The process compiles one selector program for each selector of a sheet whose parsed
        // contents documents share, which every document's rule attaches.
        let programs: Vec<crate::css::selector::SelectorListRef> = (0..SEGMENT_MIN_ENTRIES as u32)
            .map(|atom| sourced(&format!(".c{}", 300 + atom)))
            .collect();
        let document = || {
            let mut program = RuleStore::new();
            let sheet = program.add_sheet(CascadeOrigin::Author);
            program.attach_sheet(sheet, TreeScopeID::DOCUMENT);
            for selectors in &programs {
                let id = program.append_rule(sheet, None, RuleKind::Style);
                let mut version = program.rule_version(id).clone();
                version.selectors = program.start_of_selectors(sheet, selectors);
                program.replace_rule_version(id, version);
            }
            (program, sheet)
        };
        let indexes = ProcessRuleIndexes::default();
        let (first_program, sheet) = document();
        let (second_program, _) = document();
        let mut first = RuleIndex::build(&first_program, Some(&indexes));
        let second = RuleIndex::build(&second_program, Some(&indexes));

        // Both bucket the sheet's entries in one segment, and none in buckets of their own.
        assert_eq!(first.entries.segments.len(), 1);
        assert!(Arc::ptr_eq(
            &first.entries.segments[0].segment,
            &second.entries.segments[0].segment
        ));
        assert_eq!(count(&first.document.own), 0);
        assert_eq!(first.bucket_sources(ScopeGroupID::DOCUMENT, false).count(), 2);

        // A rule of the sheet that dies is dead for that document alone, which keeps the segment
        // and buckets nothing itself.
        let rule = first_program.rules_in_sheet(sheet).into_iter().next().unwrap();
        let mut died = Vec::new();
        first.kill_rule(rule, None, &mut died);
        assert!(!died.is_empty());
        assert_eq!(first.entries.segments.len(), 1);
        assert_eq!(count(&first.document.own), 0);
        assert!(died.iter().all(|&id| !first.entries.view(id as usize).live()));
        assert!(died.iter().all(|&id| second.entries.view(id as usize).live()));
        // NB: The process keeps it too, for a document of a page yet to come.
        assert_eq!(Arc::strong_count(&second.entries.segments[0].segment), 3);
    }

    #[test]
    fn a_sheet_whose_rules_were_all_replaced_binds_a_segment() {
        let indexes = ProcessRuleIndexes::default();
        let mut program = RuleStore::new();
        let sheet = program.add_sheet(CascadeOrigin::Author);
        program.attach_sheet(sheet, TreeScopeID::DOCUMENT);
        let replaced: Vec<RuleID> = (0..SEGMENT_MIN_ENTRIES as u32)
            .map(|atom| add_rule(&mut program, sheet, &format!(".c{}", 500 + atom)))
            .collect();
        let mut index = RuleIndex::build(&program, Some(&indexes));
        assert_eq!(index.entries.segments.len(), 1);

        // The sheet's text is replaced: every rule leaves, and as many others arrive.
        let mut moved: HashSet<RuleID> = replaced.iter().copied().collect();
        for &rule in &replaced {
            program.remove_rule(rule);
        }
        for atom in 0..SEGMENT_MIN_ENTRIES as u32 {
            moved.insert(add_rule(&mut program, sheet, &format!(".c{}", 600 + atom)));
        }
        index.update(
            &program,
            Some(&indexes),
            &IndexChanges {
                rules: &moved,
                rule_contents: &moved,
                sheets: &HashSet::default(),
                reordered_scopes: &HashSet::default(),
                layered_scopes: &HashSet::default(),
            },
        );

        // The arriving rules are bucketed in a segment, as those of an arriving sheet are, and the
        // document buckets none of the sheet's entries itself.
        assert_eq!(index.entries.segments.len(), 1);
        assert_eq!(index.entries.segments[0].segment.len(), SEGMENT_MIN_ENTRIES as u32);
        assert_eq!(count(&index.document.own), 0);
    }

    #[test]
    fn animation_names_are_declared_in_the_scope_of_their_attachment() {
        use super::super::super::animations::DeclarationScope;
        use super::super::cascade::declaration_scope;
        let mut program = RuleStore::new();
        let first_scope = TreeScopeID(1);
        let second_scope = TreeScopeID(2);

        let process = Arc::<crate::css::style::ProcessStyle>::default();
        let user_agent = attach_user_agent_sheet(&mut program, &process, 1, &[&sourced(&format!(".c{}", 100))]);
        let user_agent_rule = program.rules_in_sheet(user_agent)[0];
        let document = program.add_sheet(CascadeOrigin::Author);
        program.attach_sheet(document, TreeScopeID::DOCUMENT);
        let document_rule = add_rule(&mut program, document, &format!(".c{}", 101));
        // A constructed sheet the document and two shadow trees adopt.
        let shared = program.add_sheet(CascadeOrigin::Author);
        program.attach_sheet(shared, TreeScopeID::DOCUMENT);
        program.attach_sheet(shared, first_scope);
        program.attach_sheet(shared, second_scope);
        let shared_rule = add_rule(&mut program, shared, &format!(".c{}", 102));

        let index = RuleIndex::build(&program, None);
        let scope_of = |rule: RuleID, scope: TreeScopeID| {
            let entry = index
                .entries
                .numbers()
                .find(|&number| {
                    index.rule_of(number as u32) == rule
                        && Some(index.entries.placement(number).group) == index.group_of(scope)
                })
                .expect("the rule is indexed for the scope");
            declaration_scope(&index.entries, entry as u32, scope)
        };
        assert_eq!(
            scope_of(user_agent_rule, TreeScopeID::DOCUMENT),
            DeclarationScope::Unscoped
        );
        assert_eq!(
            scope_of(document_rule, TreeScopeID::DOCUMENT),
            DeclarationScope::Unscoped
        );
        // Each attachment of the adopted sheet declares in its own scope, though the rule is one.
        assert_eq!(scope_of(shared_rule, TreeScopeID::DOCUMENT), DeclarationScope::Unscoped);
        assert_eq!(
            scope_of(shared_rule, first_scope),
            DeclarationScope::Shadow(first_scope)
        );
        assert_eq!(
            scope_of(shared_rule, second_scope),
            DeclarationScope::Shadow(second_scope)
        );
        // An element's own declarations have no scope.
        assert_eq!(
            declaration_scope(&index.entries, u32::MAX, TreeScopeID::DOCUMENT),
            DeclarationScope::Unscoped
        );
    }
}
