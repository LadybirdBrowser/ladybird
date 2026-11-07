/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The matched declarations cache: an element that matched the same rules and declares the same
//! declarations as one computed before it, under a parent it inherits the same from, takes that
//! element's record without cascading or computing its own, and likewise for the pseudo-elements
//! of an element, which inherit from it as a child does.
//!
//! An element looks first by what it matched, which saves its cascade, and then by what its cascade
//! picked, which finds elements that matched other rules losing to the same declarations.
//!
//! The cache is the document's: every thread of a pass reads what any of them stored, and it stays
//! from one pass to the next, which is what finds the elements a small restyle adds to a list. A
//! pass holds it shared and only adds to it, each thread claiming a way of a bucket with one
//! compare-exchange. Only between passes, where the engine holds it exclusively, does it let go of
//! anything: as an input every entry may have read moves, and as it fills.
//!
//! What a computation reads beyond the key is in the conditions of the entry, which what the
//! computation recorded it read decides. A computation reading something of the element that
//! neither the key nor the conditions name is not stored at all.

use super::super::animations::DeclarationScope;
use super::super::*;
use super::cascade::Winner;
use super::compute::RootFontInputs;
use super::matching::ElementMatch;
use super::records::Record;
use crate::css::display::FfiDisplay;
use std::hash::{Hash, Hasher};
use std::sync::atomic::{AtomicPtr, AtomicU32, Ordering};
use std::sync::{Arc, OnceLock};

/// How many entries a bucket holds: as many as fit a cache line with their tags, so that an element
/// no entry answers for costs one load.
const WAYS: usize = 5;

/// The most buckets a cache holds, which bounds what it keeps alive.
const MAX_BUCKETS: usize = 512;

/// The fewest buckets a cache holds.
const MIN_BUCKETS: usize = 16;

/// How many elements of a document a cache makes room for one entry per.
const ELEMENTS_PER_ENTRY: usize = 2;

/// How many of the elements with declarations of their own a thread remembers the hashes of. One is
/// stored only once its hash recurs, so the many whose inline style is their own cost a hash
/// rather than an entry.
const SEEN_CAPACITY: usize = 64;

/// How many lookups the hit counts of elements with declarations of their own cover before they
/// start over.
const COUNT_PERIOD: u32 = 1024;

/// What a computation reads of its parent and of the element, besides what the element matched
/// and declares. The parent's groups and custom properties are compared by identity, which the
/// entry's reference to the parent's record keeps from going to another.
#[derive(Clone, Copy, PartialEq)]
pub(super) struct MatchedContext {
    pub(super) parent_inherited_groups: [usize; super::super::computed::ENGINE_INHERITED_GROUP_COUNT],
    pub(super) parent_color: u32,
    /// The inherited longhands the parent holds as bits, and its effective zoom.
    pub(super) parent_inherited_values: crate::css::computed_value_types::InheritedSetValues,
    pub(super) parent_dependency_flags: u8,
    /// The display of the parent, which is the box the element would be a child of.
    pub(super) parent_display: FfiDisplay,
    /// The custom properties the parent hands down and the ones it holds.
    pub(super) parent_custom_properties: [usize; 2],
    /// The kind of the pseudo-element styled, or `u8::MAX` for the element itself: only the matches
    /// for it are looked at.
    pub(super) pseudo_kind: u8,
    pub(super) adjustment_facts: u32,
    pub(super) language: StyleAtomID,
    pub(super) directionality: StyleAtomID,
    /// The tree scope of the element, which orders its own declarations among the rules it matched.
    pub(super) tree_scope: TreeScopeID,
    /// The identity of the counter-style registry of the element's style scope, which its
    /// `list-style-type` or `content` may resolve counter styles against.
    pub(super) counter_style_environment: u64,
}

/// What an element's declarations are told apart by: what it matched, which is known before its
/// cascade, or what its cascade picked.
#[derive(Clone, Copy)]
pub(super) enum Declared<'d> {
    Matched(&'d [ElementMatch]),
    Picked {
        winners: &'d [Winner],
        custom_winners: &'d [(CustomDeclaration, RetainedStyleValueData)],
        animation_name_scope: DeclarationScope,
    },
}

/// What an element is looked for by, beside what it matched and declares.
#[derive(Clone, Copy)]
pub(super) struct MatchedKey<'a> {
    hash: u64,
    /// The hash of everything but what the element matched.
    context_hash: u64,
    context: MatchedContext,
    parent: &'a Arc<Record>,
    declares_own: bool,
}

/// What the computation of a record read beyond its key, which its entry answers only where it
/// holds still.
#[derive(Clone, Copy, PartialEq)]
pub(super) struct MatchedConditions {
    /// The font of the document element, where the computation resolved a length against it.
    pub(super) root_font: Option<Option<RootFontInputs>>,
    /// Whether the computation read the parent's value of a property that does not inherit, which
    /// only the same parent's record tells.
    pub(super) reads_parent_reset_values: bool,
}

enum EntryDeclarations {
    Matched(Box<[ElementMatch]>),
    Picked {
        winners: Box<[Winner]>,
        custom_winners: Box<[(CustomDeclaration, RetainedStyleValueData)]>,
        animation_name_scope: DeclarationScope,
    },
}

struct Entry {
    hash: u64,
    context: MatchedContext,
    conditions: MatchedConditions,
    declarations: EntryDeclarations,
    /// What the element declared, which the entry keeps alive so that no other value takes an
    /// address it is compared by.
    declared: Box<[(ElementDeclarationKind, DeclarationEntry, RetainedStyleValueData)]>,
    custom_declared: Box<[(CustomDeclaration, RetainedStyleValueData)]>,
    record: Arc<Record>,
    /// The parent's record, which holds the groups and custom properties the context names.
    parent: Arc<Record>,
}

const _: fn() = || {
    fn assert_shared_between_threads<T: Send + Sync>() {}
    assert_shared_between_threads::<Entry>();
};

/// What `node` declares itself, in the order the cascade reads the kinds of its declarations.
fn declared_by(
    state: &RetainedState,
    node: StyleNodeID,
) -> impl Iterator<Item = (ElementDeclarationKind, &DeclarationEntry, &RetainedStyleValueData)> {
    ElementDeclarationKind::ALL.into_iter().flat_map(move |kind| {
        state
            .element_declarations
            .declarations(node, kind)
            .unwrap_or_default()
            .iter()
            .map(move |(declared, written)| (kind, declared, written))
    })
}

/// The custom properties `node` declares itself.
fn custom_declared_by(
    state: &RetainedState,
    node: StyleNodeID,
) -> impl Iterator<Item = (&CustomDeclaration, &RetainedStyleValueData)> {
    let custom = state.element_declarations.custom(node);
    custom
        .into_iter()
        .flat_map(|custom| custom.declared().iter().zip(custom.written_values()))
}

/// Whether `node` declares anything itself.
pub(super) fn declares_own(state: &RetainedState, node: StyleNodeID) -> bool {
    declared_by(state, node).next().is_some() || custom_declared_by(state, node).next().is_some()
}

impl<'a> MatchedKey<'a> {
    /// The key of `node`, which matched `matches`, under the parent whose record is `parent`.
    pub(super) fn new(
        state: &RetainedState,
        node: StyleNodeID,
        matches: &[ElementMatch],
        context: MatchedContext,
        parent: &'a Arc<Record>,
        declares_own: bool,
    ) -> Self {
        let mut hasher = crate::fast_hash::fast_hasher();
        if declares_own {
            for (kind, declared, written) in declared_by(state, node) {
                (kind as u8).hash(&mut hasher);
                declared.hash(&mut hasher);
                written.pointer().hash(&mut hasher);
            }
            for (declared, written) in custom_declared_by(state, node) {
                declared.hash(&mut hasher);
                written.pointer().hash(&mut hasher);
            }
        }
        context.parent_inherited_groups.hash(&mut hasher);
        context.parent_color.hash(&mut hasher);
        context.parent_custom_properties.hash(&mut hasher);
        context.adjustment_facts.hash(&mut hasher);
        context.pseudo_kind.hash(&mut hasher);
        let context_hash = hasher.finish();
        for matched in matches
            .iter()
            .filter(|matched| matched.pseudo_kind == context.pseudo_kind)
        {
            hasher.write_u32(matched.entry);
            matched.scope.hash(&mut hasher);
        }
        Self {
            hash: hasher.finish(),
            context_hash,
            context,
            parent,
            declares_own,
        }
    }

    pub(super) fn declares_own(&self) -> bool {
        self.declares_own
    }

    /// What tells this key from others, short of comparing what it names.
    pub(super) fn hash(&self) -> u64 {
        self.hash
    }

    /// This key, looking for what `winners` and `custom_winners` were picked by a cascade.
    pub(super) fn for_picked(
        &self,
        winners: &[Winner],
        custom_winners: &[(CustomDeclaration, RetainedStyleValueData)],
    ) -> Self {
        let mut hasher = crate::fast_hash::fast_hasher();
        self.context_hash.hash(&mut hasher);
        winners.hash(&mut hasher);
        for (declared, written) in custom_winners {
            declared.hash(&mut hasher);
            written.pointer().hash(&mut hasher);
        }
        Self {
            hash: hasher.finish(),
            ..*self
        }
    }

    /// The tag of the entries of this key, which is never that of an empty way.
    fn tag(&self) -> u32 {
        tag_of(self.hash)
    }
}

fn tag_of(hash: u64) -> u32 {
    (hash >> 32) as u32 | 1
}

/// What of a thread's lookups for elements with declarations of their own says whether they are
/// worth their cost, and which of those elements recur.
pub(super) struct OwnDeclarationLookups {
    seen: [u64; SEEN_CAPACITY],
    next_seen: usize,
    hits: u32,
    misses: u32,
    lookups: u32,
}

impl Default for OwnDeclarationLookups {
    fn default() -> Self {
        Self {
            seen: [0; SEEN_CAPACITY],
            next_seen: 0,
            hits: 0,
            misses: 0,
            lookups: 0,
        }
    }
}

impl OwnDeclarationLookups {
    /// Whether looking for the next element with declarations of its own is worth its cost.
    pub(super) fn worth_looking(&mut self) -> bool {
        self.lookups += 1;
        if self.lookups == COUNT_PERIOD {
            self.lookups = 0;
            self.hits = 0;
            self.misses = 0;
        }
        // A hit saves a cascade and a computation, which cost some twenty lookups that miss.
        self.misses < 64 || self.hits * 20 >= self.misses
    }

    pub(super) fn note_answer(&mut self, found: bool) {
        match found {
            true => self.hits += 1,
            false => self.misses += 1,
        }
    }

    /// Whether the element of `key` is worth an entry: one with declarations of its own is once
    /// its hash recurs.
    fn worth_storing(&mut self, key: &MatchedKey<'_>) -> bool {
        if !key.declares_own || self.seen.contains(&key.hash) {
            return true;
        }
        self.seen[self.next_seen] = key.hash;
        self.next_seen = (self.next_seen + 1) % SEEN_CAPACITY;
        false
    }
}

#[repr(C, align(64))]
struct Bucket {
    tags: [AtomicU32; WAYS],
    entries: [AtomicPtr<Entry>; WAYS],
}

const _: () = assert!(size_of::<Bucket>() == 64);

impl Bucket {
    fn new() -> Self {
        Self {
            tags: [const { AtomicU32::new(0) }; WAYS],
            entries: [const { AtomicPtr::new(std::ptr::null_mut()) }; WAYS],
        }
    }

    /// Take `entry` into a free way, or hand it back where none is.
    fn claim(&self, tag: u32, entry: *mut Entry) -> Result<(), *mut Entry> {
        for way in 0..WAYS {
            if self.entries[way]
                .compare_exchange(std::ptr::null_mut(), entry, Ordering::Release, Ordering::Relaxed)
                .is_ok()
            {
                self.tags[way].store(tag, Ordering::Release);
                return Ok(());
            }
        }
        Err(entry)
    }

    /// The entries in the ways tagged `tag`.
    fn tagged(&self, tag: u32) -> impl Iterator<Item = &Entry> {
        (0..WAYS).filter_map(move |way| {
            if self.tags[way].load(Ordering::Relaxed) != tag {
                return None;
            }
            // SAFETY: A way holds null or an entry nothing frees while the cache is shared, and the
            //         acquire load sees the writes that made it.
            unsafe { self.entries[way].load(Ordering::Acquire).as_ref() }
        })
    }

    fn is_full(&self) -> bool {
        self.entries
            .iter()
            .all(|entry| !entry.load(Ordering::Relaxed).is_null())
    }
}

/// The cache: buckets of ways, a table a pass reads and adds to through shared references. Nothing
/// leaves it but through an exclusive reference, so an entry a thread reads outlives its read.
#[derive(Default)]
pub(super) struct MatchedDeclarationsCache {
    buckets: OnceLock<Box<[Bucket]>>,
    entries: AtomicU32,
}

fn buckets_for(elements: usize) -> usize {
    (elements / ELEMENTS_PER_ENTRY / WAYS)
        .next_power_of_two()
        .clamp(MIN_BUCKETS, MAX_BUCKETS)
}

fn new_buckets(count: usize) -> Box<[Bucket]> {
    (0..count).map(|_| Bucket::new()).collect()
}

impl MatchedDeclarationsCache {
    /// The record of the element or pseudo-element of `key` with the declarations `declared`, against
    /// the document element's font `root_font`, with what its computation read, where an entry
    /// answers for it.
    pub(super) fn find(
        &self,
        state: &RetainedState,
        node: StyleNodeID,
        declared: Declared<'_>,
        key: &MatchedKey<'_>,
        root_font: Option<RootFontInputs>,
    ) -> Option<(&Arc<Record>, MatchedConditions)> {
        let buckets = self.buckets.get()?;
        let bucket = &buckets[key.hash as usize & (buckets.len() - 1)];
        bucket
            .tagged(key.tag())
            .find(|entry| entry.answers(state, node, declared, key, root_font))
            .map(|entry| (&entry.record, entry.conditions))
    }

    /// Keep `record` for the elements of `key`, where a way of its bucket is free. Styling goes on
    /// without it where none is, where the element is unlikely to recur, or where another thread
    /// that computed the same stored it first, which would only take a way from other keys. Threads
    /// storing at the very same time can still each take one.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn insert(
        &self,
        state: &RetainedState,
        node: StyleNodeID,
        declared_by_cascade: Declared<'_>,
        key: MatchedKey<'_>,
        conditions: MatchedConditions,
        record: Arc<Record>,
        lookups: &mut OwnDeclarationLookups,
    ) {
        if !lookups.worth_storing(&key) {
            return;
        }
        let buckets = self.buckets.get_or_init(|| new_buckets(MIN_BUCKETS));
        let bucket = &buckets[key.hash as usize & (buckets.len() - 1)];
        if bucket.is_full()
            || bucket
                .tagged(key.tag())
                .any(|entry| entry.stores(state, node, declared_by_cascade, &key, conditions))
        {
            return;
        }
        let (declared, custom_declared) = match key.declares_own {
            true => (
                declared_by(state, node)
                    .map(|(kind, declared, written)| (kind, *declared, written.clone()))
                    .collect(),
                custom_declared_by(state, node)
                    .map(|(declared, written)| (*declared, written.clone()))
                    .collect(),
            ),
            false => (Default::default(), Default::default()),
        };
        let entry = Box::into_raw(Box::new(Entry {
            hash: key.hash,
            context: key.context,
            conditions,
            declarations: match declared_by_cascade {
                Declared::Matched(matches) => EntryDeclarations::Matched(
                    matches
                        .iter()
                        .filter(|matched| matched.pseudo_kind == key.context.pseudo_kind)
                        .copied()
                        .collect(),
                ),
                Declared::Picked {
                    winners,
                    custom_winners,
                    animation_name_scope,
                } => EntryDeclarations::Picked {
                    winners: winners.into(),
                    custom_winners: custom_winners.into(),
                    animation_name_scope,
                },
            },
            declared,
            custom_declared,
            record,
            parent: key.parent.clone(),
        }));
        match bucket.claim(key.tag(), entry) {
            Ok(()) => {
                self.entries.fetch_add(1, Ordering::Relaxed);
            }
            // SAFETY: The entry was just made, and nothing else names it.
            Err(entry) => drop(unsafe { Box::from_raw(entry) }),
        }
    }

    /// Let go of every entry: an input they may have read moved.
    pub(super) fn forget(&mut self) {
        let Some(buckets) = self.buckets.take() else {
            return;
        };
        for bucket in &buckets {
            for entry in &bucket.entries {
                let entry = entry.load(Ordering::Relaxed);
                if !entry.is_null() {
                    // SAFETY: The cache holds the entry alone, and is held exclusively.
                    drop(unsafe { Box::from_raw(entry) });
                }
            }
        }
        *self.entries.get_mut() = 0;
    }

    /// Make room for the pass over a document of `elements` elements that follows: more buckets
    /// where the document grew or the cache filled, and none of its entries where it is as large
    /// as it goes and filled.
    pub(super) fn prepare(&mut self, elements: usize) {
        let entries = *self.entries.get_mut() as usize;
        let buckets = self.buckets.get_or_init(|| new_buckets(buckets_for(elements)));
        let count = buckets.len();
        if entries * 4 > count * WAYS * 3 && count == MAX_BUCKETS {
            self.forget();
            self.buckets.get_or_init(|| new_buckets(MAX_BUCKETS));
            return;
        }
        let buckets = self.buckets.get_mut().expect("the cache just made its buckets");
        let filling = entries * 2 > count * WAYS;
        let wanted = buckets_for(elements)
            .max(if filling { count * 2 } else { count })
            .min(MAX_BUCKETS);
        if wanted == count {
            return;
        }
        let bigger = new_buckets(wanted);
        let mut kept = 0;
        for entry in buckets.iter().flat_map(|bucket| &bucket.entries) {
            let entry = entry.load(Ordering::Relaxed);
            // SAFETY: A way holds null or an entry, and the cache is held exclusively.
            let Some(hash) = (unsafe { entry.as_ref() }).map(|entry| entry.hash) else {
                continue;
            };
            match bigger[hash as usize & (wanted - 1)].claim(tag_of(hash), entry) {
                Ok(()) => kept += 1,
                // SAFETY: The entry just left the buckets holding it, which go below.
                Err(entry) => drop(unsafe { Box::from_raw(entry) }),
            }
        }
        *buckets = bigger;
        *self.entries.get_mut() = kept;
    }

    /// Counts the cache's table and entries, and the records only it holds, toward the worker caches.
    pub(super) fn report_memory(&self, report: &mut crate::css::style::memory_report::MemoryReport) {
        use crate::css::style::memory_report::MemoryOwner;
        let Some(buckets) = self.buckets.get() else {
            return;
        };
        report.add(MemoryOwner::WorkerCaches, std::mem::size_of_val(&**buckets));
        for entry in buckets.iter().flat_map(|bucket| &bucket.entries) {
            // SAFETY: A way holds null or an entry nothing frees while the cache is shared.
            let Some(entry) = (unsafe { entry.load(Ordering::Acquire).as_ref() }) else {
                continue;
            };
            let bytes = size_of::<Entry>()
                + match &entry.declarations {
                    EntryDeclarations::Matched(matches) => std::mem::size_of_val(&**matches),
                    EntryDeclarations::Picked {
                        winners,
                        custom_winners,
                        ..
                    } => std::mem::size_of_val(&**winners) + std::mem::size_of_val(&**custom_winners),
                }
                + std::mem::size_of_val(&*entry.declared)
                + std::mem::size_of_val(&*entry.custom_declared);
            report.add(MemoryOwner::WorkerCaches, bytes);
            if report.sizer.first_sight(Arc::as_ptr(&entry.record)) {
                report.add(MemoryOwner::WorkerCaches, size_of::<Record>());
            }
        }
    }
}

impl Drop for MatchedDeclarationsCache {
    fn drop(&mut self) {
        self.forget();
    }
}

impl EntryDeclarations {
    fn are(&self, declared: Declared<'_>, pseudo_kind: u8) -> bool {
        match (self, declared) {
            (Self::Matched(held), Declared::Matched(matches)) => held
                .iter()
                .eq(matches.iter().filter(|matched| matched.pseudo_kind == pseudo_kind)),
            (
                Self::Picked {
                    winners: held_winners,
                    custom_winners: held_custom_winners,
                    animation_name_scope: held_scope,
                },
                Declared::Picked {
                    winners,
                    custom_winners,
                    animation_name_scope,
                },
            ) => {
                *held_scope == animation_name_scope
                    && **held_winners == *winners
                    && held_custom_winners
                        .iter()
                        .map(|(declared, written)| (*declared, written.pointer()))
                        .eq(custom_winners
                            .iter()
                            .map(|(declared, written)| (*declared, written.pointer())))
            }
            _ => false,
        }
    }
}

impl Entry {
    /// Whether this entry is one of the element of `key`, whatever its conditions.
    fn is_of(&self, state: &RetainedState, node: StyleNodeID, declared: Declared<'_>, key: &MatchedKey<'_>) -> bool {
        self.hash == key.hash
            && self.context == key.context
            && self.declarations.are(declared, key.context.pseudo_kind)
            && self.declares_as(state, node, key.declares_own)
    }

    /// Whether the record answers for the element of `key`.
    fn answers(
        &self,
        state: &RetainedState,
        node: StyleNodeID,
        declared: Declared<'_>,
        key: &MatchedKey<'_>,
        root_font: Option<RootFontInputs>,
    ) -> bool {
        self.is_of(state, node, declared, key)
            && self.conditions.root_font.is_none_or(|read| read == root_font)
            && (!self.conditions.reads_parent_reset_values || Arc::ptr_eq(&self.parent, key.parent))
    }

    /// Whether this entry answers wherever an entry of the element of `key` with `conditions` would.
    fn stores(
        &self,
        state: &RetainedState,
        node: StyleNodeID,
        declared: Declared<'_>,
        key: &MatchedKey<'_>,
        conditions: MatchedConditions,
    ) -> bool {
        self.conditions == conditions
            && (!conditions.reads_parent_reset_values || Arc::ptr_eq(&self.parent, key.parent))
            && self.is_of(state, node, declared, key)
    }

    /// Whether `node` declares what the element of this entry did.
    fn declares_as(&self, state: &RetainedState, node: StyleNodeID, declares_own: bool) -> bool {
        if !declares_own {
            return self.declared.is_empty() && self.custom_declared.is_empty();
        }
        self.declared
            .iter()
            .map(|(kind, declared, written)| (*kind, *declared, written.pointer()))
            .eq(declared_by(state, node).map(|(kind, declared, written)| (kind, *declared, written.pointer())))
            && self
                .custom_declared
                .iter()
                .map(|(declared, written)| (*declared, written.pointer()))
                .eq(custom_declared_by(state, node).map(|(declared, written)| (*declared, written.pointer())))
    }
}

#[cfg(test)]
impl MatchedDeclarationsCache {
    /// Have the entries of pseudo-elements, or those of elements, answer with `wrong`.
    pub(super) fn poison_with(&self, wrong: &Arc<Record>, pseudo_elements: bool) {
        let buckets = self.buckets.get().expect("the pass kept records");
        for entry in buckets.iter().flat_map(|bucket| &bucket.entries) {
            // SAFETY: A way holds null or an entry nothing frees while the cache is shared, and a
            //         test's pass on one thread reads none of them while this writes.
            let Some(entry) = (unsafe { entry.load(Ordering::Acquire).as_mut() }) else {
                continue;
            };
            if (entry.context.pseudo_kind != u8::MAX) == pseudo_elements {
                entry.record = wrong.clone();
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::counters::StyleCounter;
    use super::super::test_support::{
        add_class, add_rules, add_sheet, allocate_nodes, assert_live_matches_cold, document_with_children, live_record,
        records_equal, relations, remove_class, run_style_update, set_tag, start_live, start_live_on,
    };
    use super::*;
    use crate::css::style_value::{RetainedStyleValueData, StyleValueData};

    fn reused(engine: &StyleEngine) -> u64 {
        engine.retained.live_counters.get(StyleCounter::ReusedElements as u32)
    }

    fn matched_declarations(engine: &StyleEngine) -> &MatchedDeclarationsCache {
        engine
            .retained
            .live_style
            .as_ref()
            .expect("the engine runs")
            .matched_declarations()
    }

    impl MatchedDeclarationsCache {
        fn entry_count(&self) -> usize {
            self.buckets.get().map_or(0, |buckets| {
                buckets
                    .iter()
                    .flat_map(|bucket| &bucket.entries)
                    .filter(|entry| !entry.load(Ordering::Relaxed).is_null())
                    .count()
            })
        }
    }

    /// `root -> [first -> child, second -> child]`, with `source` for its rules.
    fn two_parents(source: &str) -> (StyleEngine, Vec<StyleNodeID>) {
        let (mut engine, nodes) = document_with_children(3);
        let children = allocate_nodes(&mut engine, 2);
        for (&child, parent) in children.iter().zip([nodes[1], nodes[2]]) {
            engine.record_tree_delta(child, None, Some(relations(Some(parent), None, None)));
            set_tag(&mut engine, child, "div");
        }
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(&mut engine, sheet, source);
        let mut all = nodes;
        all.extend(children);
        (engine, all)
    }

    #[test]
    fn an_element_takes_the_record_an_element_of_a_pass_before_it_kept() {
        let (mut engine, nodes) = document_with_children(3);
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(&mut engine, sheet, ".item { width: 1px; font-size: 12px; }");
        add_class(&mut engine, nodes[1], "item");
        start_live(&mut engine, nodes[0]);
        let before = reused(&engine);
        add_class(&mut engine, nodes[2], "item");
        run_style_update(&mut engine, nodes[0]);
        assert_eq!(reused(&engine), before + 1);
        assert!(Arc::ptr_eq(
            &live_record(&engine, nodes[1]),
            &live_record(&engine, nodes[2])
        ));
        assert_live_matches_cold(&mut engine, nodes[0]);
    }

    #[test]
    fn a_rule_change_leaves_no_record_computed_from_the_rules_before_it() {
        let (mut engine, nodes) = document_with_children(3);
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(&mut engine, sheet, ".item { width: 1px; }");
        add_class(&mut engine, nodes[1], "item");
        start_live(&mut engine, nodes[0]);
        assert_ne!(matched_declarations(&engine).entry_count(), 0);
        add_rules(&mut engine, sheet, ".item { width: 2px; }");
        add_class(&mut engine, nodes[2], "item");
        run_style_update(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
    }

    #[test]
    fn parents_inheriting_otherwise_keep_their_children_apart() {
        let (mut engine, nodes) = two_parents(
            ".small { font-size: 10px; color: rgb(1, 1, 1); } .large { font-size: 20px; color: rgb(2, 2, 2); } \
             .child { width: 2em; background-color: currentcolor; }",
        );
        add_class(&mut engine, nodes[1], "small");
        add_class(&mut engine, nodes[2], "large");
        add_class(&mut engine, nodes[3], "child");
        add_class(&mut engine, nodes[4], "child");
        start_live(&mut engine, nodes[0]);
        assert!(!Arc::ptr_eq(
            &live_record(&engine, nodes[3]),
            &live_record(&engine, nodes[4])
        ));
        assert_live_matches_cold(&mut engine, nodes[0]);
    }

    #[test]
    fn a_parent_of_the_same_inherited_style_takes_its_children_along() {
        let (mut engine, nodes) = two_parents(".narrow { width: 1px; } .wide { width: 2px; } .child { height: 3px; }");
        add_class(&mut engine, nodes[1], "narrow");
        add_class(&mut engine, nodes[2], "wide");
        add_class(&mut engine, nodes[3], "child");
        start_live(&mut engine, nodes[0]);
        add_class(&mut engine, nodes[4], "child");
        run_style_update(&mut engine, nodes[0]);
        assert!(Arc::ptr_eq(
            &live_record(&engine, nodes[3]),
            &live_record(&engine, nodes[4])
        ));
        assert_live_matches_cold(&mut engine, nodes[0]);
    }

    #[test]
    fn an_explicit_inherit_of_a_reset_property_reads_the_very_parent() {
        let (mut engine, nodes) =
            two_parents(".narrow { width: 1px; } .wide { width: 2px; } .child { width: inherit; }");
        add_class(&mut engine, nodes[1], "narrow");
        add_class(&mut engine, nodes[2], "wide");
        add_class(&mut engine, nodes[3], "child");
        start_live(&mut engine, nodes[0]);
        add_class(&mut engine, nodes[4], "child");
        run_style_update(&mut engine, nodes[0]);
        assert!(!Arc::ptr_eq(
            &live_record(&engine, nodes[3]),
            &live_record(&engine, nodes[4])
        ));
        assert_live_matches_cold(&mut engine, nodes[0]);
    }

    #[test]
    fn a_root_font_moving_leaves_no_length_resolved_against_it() {
        let (mut engine, nodes) = document_with_children(3);
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(
            &mut engine,
            sheet,
            ".small { font-size: 10px; } .large { font-size: 20px; } .item { width: 2rem; }",
        );
        add_class(&mut engine, nodes[0], "small");
        add_class(&mut engine, nodes[1], "item");
        start_live(&mut engine, nodes[0]);
        remove_class(&mut engine, nodes[0], "small");
        add_class(&mut engine, nodes[0], "large");
        add_class(&mut engine, nodes[2], "item");
        run_style_update(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
    }

    #[test]
    fn the_cache_holds_no_more_than_its_table_holds() {
        const ELEMENTS: usize = 6000;
        let (mut engine, nodes) = document_with_children(ELEMENTS);
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        let source: String = (0..ELEMENTS)
            .map(|index| format!(".class{index} {{ width: {index}px; }} "))
            .collect();
        add_rules(&mut engine, sheet, &source);
        for (index, &node) in nodes.iter().enumerate().skip(1) {
            add_class(&mut engine, node, &format!("class{index}"));
        }
        start_live(&mut engine, nodes[0]);
        let live = engine.retained.live_style.as_ref().expect("the engine runs");
        assert!(!live.has_pending());
        let entries = live.matched_declarations().entry_count();
        assert_ne!(entries, 0);
        assert!(entries <= MAX_BUCKETS * WAYS, "{entries} entries");
        // Compare computed styles without the reference matcher checking every rule against every element.
        let failures = engine.self_check_failures(nodes[0], None);
        assert!(failures.is_empty(), "{}", failures.join("\n"));
    }

    #[test]
    fn elements_with_one_key_resolve_a_font_once() {
        let (mut engine, nodes) = document_with_children(12);
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(&mut engine, sheet, ".item { font-size: 13px; }");
        start_live(&mut engine, nodes[0]);
        let requests = |engine: &StyleEngine| engine.retained.counters.get(Counter::FontResolutionRequests);
        let before = requests(&engine);
        for &node in &nodes[1..] {
            add_class(&mut engine, node, "item");
        }
        run_style_update(&mut engine, nodes[0]);
        assert_eq!(requests(&engine), before + 1);
        assert_live_matches_cold(&mut engine, nodes[0]);
    }

    #[test]
    fn a_full_bucket_hands_back_what_it_has_no_way_for() {
        let bucket = Bucket::new();
        let mut entries = [0_usize; WAYS + 1];
        let pointers: Vec<*mut Entry> = entries
            .iter_mut()
            .map(|entry| std::ptr::from_mut(entry).cast::<Entry>())
            .collect();
        for &pointer in &pointers[..WAYS] {
            assert!(bucket.claim(3, pointer).is_ok());
        }
        assert!(bucket.is_full());
        assert_eq!(bucket.claim(3, pointers[WAYS]), Err(pointers[WAYS]));
    }

    #[test]
    fn a_document_decides_how_many_buckets_a_cache_starts_with() {
        assert_eq!(buckets_for(0), MIN_BUCKETS);
        assert_eq!(buckets_for(1000), 128);
        assert_eq!(buckets_for(usize::MAX / 2), MAX_BUCKETS);
    }

    /// What a key of the stress test below is told apart by, and holds its element's parent to.
    fn stress_context(state: &RetainedState, node: StyleNodeID, parent: &Arc<Record>, index: u32) -> MatchedContext {
        let facts = state.projection.facts(node).expect("the node is an element");
        MatchedContext {
            parent_inherited_groups: [0; crate::css::style::computed::ENGINE_INHERITED_GROUP_COUNT],
            parent_color: parent.payloads.current_color(),
            parent_inherited_values: parent.payloads.inherited_values(),
            parent_dependency_flags: parent.facts().publication_dependency_flags(),
            parent_display: parent.facts().display(),
            parent_custom_properties: [0, 0],
            pseudo_kind: u8::MAX,
            adjustment_facts: index,
            language: facts.language(),
            directionality: facts.directionality(),
            tree_scope: TreeScopeID::DOCUMENT,
            counter_style_environment: 0,
        }
    }

    #[test]
    fn threads_adding_and_reading_at_once_find_only_what_was_stored_for_their_key() {
        const RECORDS: usize = 8;
        const KEYS: u32 = 512;
        const ROUNDS: u32 = 32;
        const WRITERS: u32 = 4;
        const READERS: u32 = 4;
        let (mut engine, nodes) = document_with_children(RECORDS + 3);
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        let source: String = (0..RECORDS)
            .map(|index| format!(".class{index} {{ width: {}px; }} ", index + 1))
            .collect();
        add_rules(&mut engine, sheet, &source);
        for (index, &node) in nodes[1..=RECORDS].iter().enumerate() {
            add_class(&mut engine, node, &format!("class{index}"));
        }
        // An element with declarations of its own, whose values every entry of its keys retains.
        let declaring = nodes[RECORDS + 1];
        let plain = nodes[RECORDS + 2];
        // NB: A value no other test holds, as tests run side by side and small numbers are shared.
        let written = RetainedStyleValueData::from_owned(StyleValueData::Number { value: 8675309.25 });
        engine.element_declarations.set_inline_for_test(
            declaring,
            vec![(
                DeclarationEntry::new(
                    crate::css::property_metadata::property_id::MARGIN_LEFT,
                    false,
                    CascadeOperator::Declared,
                ),
                written.clone_retained(),
            )],
        );
        start_live_on(&mut engine, nodes[0], 1);
        let parent = live_record(&engine, nodes[0]);
        let records: Vec<Arc<Record>> = nodes[1..=RECORDS]
            .iter()
            .map(|&node| live_record(&engine, node))
            .collect();
        let parent_count = Arc::strong_count(&parent);
        let record_counts: Vec<usize> = records.iter().map(Arc::strong_count).collect();
        let written_count =
            crate::css::style_value_arc::StyleValueArc::strong_count(&written.clone_retained().into_arc());

        let mut cache = MatchedDeclarationsCache::default();
        let state = &engine.retained;
        let key_of = |index: u32| {
            // Every eighth key names the element declaring its own, whose entries retain them.
            let declares_own = index.is_multiple_of(8);
            let node = if declares_own { declaring } else { plain };
            let key = MatchedKey::new(
                state,
                node,
                &[],
                stress_context(state, node, &parent, index),
                &parent,
                declares_own,
            );
            (node, key)
        };
        let record_of = |index: u32| &records[index as usize % RECORDS];
        let mut hits = 0_u64;
        for round in 0..ROUNDS {
            let writing = std::sync::atomic::AtomicU32::new(WRITERS);
            let start = std::sync::Barrier::new((WRITERS + READERS) as usize);
            let table = &cache;
            hits += std::thread::scope(|scope| {
                for writer in 0..WRITERS {
                    let (writing, start, key_of, record_of) = (&writing, &start, &key_of, &record_of);
                    scope.spawn(move || {
                        let mut lookups = OwnDeclarationLookups::default();
                        start.wait();
                        // Twice, as a key declaring its own is stored only once it recurs. Every
                        // writer goes through the keys from elsewhere, so that they meet everywhere.
                        for index in 0..KEYS * 2 {
                            let index = (index + writer * KEYS / WRITERS) % KEYS;
                            let (node, key) = key_of(index);
                            table.insert(
                                state,
                                node,
                                Declared::Matched(&[]),
                                key,
                                MatchedConditions {
                                    root_font: None,
                                    reads_parent_reset_values: false,
                                },
                                record_of(index).clone(),
                                &mut lookups,
                            );
                        }
                        writing.fetch_sub(1, Ordering::Release);
                    });
                }
                let readers: Vec<_> = (0..READERS)
                    .map(|reader| {
                        let (writing, start, key_of, record_of) = (&writing, &start, &key_of, &record_of);
                        scope.spawn(move || {
                            let mut hits = 0_u64;
                            start.wait();
                            let mut index = reader * KEYS / READERS;
                            // One sweep over all keys after the last writer is done, too.
                            let mut left = KEYS;
                            while left != 0 {
                                if writing.load(Ordering::Acquire) == 0 {
                                    left -= 1;
                                }
                                index = (index + 1) % KEYS;
                                let (node, key) = key_of(index);
                                if let Some((record, _)) = table.find(state, node, Declared::Matched(&[]), &key, None) {
                                    assert!(
                                        Arc::ptr_eq(record, record_of(index)),
                                        "key {index} found another's record"
                                    );
                                    // Have the refcount and the payloads of what was found raced on.
                                    let kept = record.clone();
                                    assert!(records_equal(&kept, record_of(index)));
                                    hits += 1;
                                }
                            }
                            hits
                        })
                    })
                    .collect();
                readers
                    .into_iter()
                    .map(|reader| reader.join().expect("a reader finishes"))
                    .sum::<u64>()
            });

            // What every thread left is whole: the count holds, and a tag names its entry.
            let held = cache.entry_count();
            assert_eq!(*cache.entries.get_mut() as usize, held);
            for bucket in cache.buckets.get().expect("the writers added") {
                for (tag, entry) in bucket.tags.iter().zip(&bucket.entries) {
                    let tag = tag.load(Ordering::Relaxed);
                    let entry = entry.load(Ordering::Relaxed);
                    // SAFETY: A way holds null or an entry, and the threads are done.
                    match unsafe { entry.as_ref() } {
                        Some(entry) => assert_eq!(tag, tag_of(entry.hash)),
                        None => assert_eq!(tag, 0),
                    }
                }
            }
            // Every way between passes: all entries going, more buckets, the largest table filled.
            match round % 4 {
                0 => cache.forget(),
                1 => cache.prepare(KEYS as usize),
                2 => cache.prepare(MAX_BUCKETS * WAYS * ELEMENTS_PER_ENTRY),
                _ => cache.prepare(0),
            }
        }
        assert_ne!(hits, 0);

        // Nothing but the entries held these, and no entry holds them now.
        cache.forget();
        assert_eq!(Arc::strong_count(&parent), parent_count);
        for (record, count) in records.iter().zip(record_counts) {
            assert_eq!(Arc::strong_count(record), count);
        }
        assert_eq!(
            crate::css::style_value_arc::StyleValueArc::strong_count(&written.clone_retained().into_arc()),
            written_count
        );
    }

    #[test]
    fn a_key_stored_already_takes_no_other_way() {
        let (mut engine, nodes) = document_with_children(3);
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(&mut engine, sheet, ".item { width: 1px; }");
        add_class(&mut engine, nodes[1], "item");
        start_live(&mut engine, nodes[0]);
        let parent = live_record(&engine, nodes[0]);
        let record = live_record(&engine, nodes[1]);
        let cache = MatchedDeclarationsCache::default();
        let state = &engine.retained;
        let mut lookups = OwnDeclarationLookups::default();
        let mut insert = |index: u32, root_font: Option<Option<RootFontInputs>>| {
            let key = MatchedKey::new(
                state,
                nodes[2],
                &[],
                stress_context(state, nodes[2], &parent, index),
                &parent,
                false,
            );
            let conditions = MatchedConditions {
                root_font,
                reads_parent_reset_values: false,
            };
            cache.insert(
                state,
                nodes[2],
                Declared::Matched(&[]),
                key,
                conditions,
                record.clone(),
                &mut lookups,
            );
            cache.entry_count()
        };
        assert_eq!(insert(0, None), 1);
        assert_eq!(insert(0, None), 1);
        // An entry answering under other conditions does not answer for this one.
        assert_eq!(insert(0, Some(None)), 2);
        assert_eq!(insert(1, None), 3);
    }
}
